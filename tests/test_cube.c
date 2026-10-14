/* Tests for the cube (step 7a): id and link cells in the assembler, the reference cube against
   plain C, and every engine against the reference cube, random programs with links included.
   Run from the project root. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "benes.h"
#include "build.h"
#include "check.h"
#include "enum.h"
#include "gen.h"
#include "jit.h"
#include "lanes.h"
#include "parallel.h"
#include "platform.h"
#include "ref.h"
#include "spec.h"

static int checks, failures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        checks++;                                            \
        if (!(cond)) {                                       \
            failures++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            putchar('\n');                                   \
        }                                                    \
    } while (0)

static uint64_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static int assemble(const char *src, Program *p)
{
    char err[512];
    if (asm_string(src, "<test>", ".", p, err, sizeof err))
        return 1;
    printf("FAIL assembling: %s\n", err);
    failures++;
    return 0;
}

/* ---- assembler ---------------------------------------------------------------------------- */

static void test_assembler(void)
{
    static const struct {
        const char *src, *expect;
    } errors[] = {
        {".link n = r @ 0\nSTO n\n", "cannot store to link cell n"},
        {"STO ID3\n", "cannot store to lane-id cell"},
        {".link y = x @ 16\n", "dimension must be 0 to 15"},
        {"STO ID15\n", "cannot store to lane-id cell"},
        {".link y = x\n", ".link expects"},
        {"NAND y\n.link y = x @ 1\n", "already a cell"},
        {".link y = y @ 1\n", "cannot read itself"},
        {".link ID3 = x @ 1\n", "lane-id cell"},
        {".in ID0\n", "is a lane-id cell"},
        {".link n = r @ 2\n.out n\n", "is a link cell"},
    };
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
        char err[512];
        Program p;
        int ok = asm_string(errors[i].src, "<test>", ".", &p, err, sizeof err);
        CHECK(!ok && strstr(err, errors[i].expect), "case %zu: expected \"%s\", got \"%s\"", i,
              errors[i].expect, ok ? "(no error)" : err);
        if (ok)
            program_free(&p);
    }

    Program p;
    if (assemble(".macro M x\n.link @n = x @ 2\nNAND @n\n.endm\nM a\nM a\nNAND id8\n", &p)) {
        unsigned mask;
        uint32_t a, base = program_resolve(&p, p.code[0].addr, &mask);
        program_find_cell(&p, "a", &a);
        CHECK(p.special && base == a && mask == 4 && p.code[0].addr != p.code[1].addr &&
                  p.special[p.code[2].addr].kind == CK_ID && p.special[p.code[2].addr].dim == 8,
              "links inside macros, one per call; id8 is an id cell");
        program_free(&p);
    }
    if (assemble("NAND a\nSTO b\n", &p)) {
        CHECK(!p.special, "a program without id or link cells has no cell table");
        program_free(&p);
    }

    /* printed and read back, links included */
    if (assemble(".in x\n.out o\n.link y = x @ 3\n.link z = y @ 5\nNAND z\nNAND ID1\nSTO o\n",
                 &p)) {
        FILE *f = fopen("build/cube_print.asm", "w");
        program_print(f, &p);
        fclose(f);
        Program q;
        char err[256];
        int ok = asm_file("build/cube_print.asm", &q, err, sizeof err);
        unsigned m1 = 0, m2 = 0;
        int same = ok && q.len == p.len;
        for (size_t i = 0; same && i < p.len; i++) {
            uint32_t b1 = program_resolve(&p, p.code[i].addr, &m1),
                     b2 = program_resolve(&q, q.code[i].addr, &m2);
            same = m1 == m2 && !strcmp(p.cell_names[b1], q.cell_names[b2]);
        }
        CHECK(same, "printed links read back the same: %s", ok ? "" : err);
        if (ok)
            program_free(&q);
        remove("build/cube_print.asm");
        program_free(&p);
    }
}

/* ---- the reference cube against plain C --------------------------------------------------- */

static void test_reference(void)
{
    Program p;
    if (!assemble(".link y = x @ 3\n.link z = y @ 5\n.link j = ID0 @ 0\n"
                  "NAND ZERO\nNAND ID4\nSTO a\n"  /* a = !bit 4 of k */
                  "NAND ZERO\nNAND z\nSTO b\n"    /* b = !x of k ^ 40 */
                  "NAND ZERO\nNAND j\nSTO c\n",   /* c = !bit 0 of k ^ 1 = bit 0 of k */
                  &p))
        return;
    RefMachine *cube = calloc(CUBE_SIZE, sizeof *cube);
    ref_cube_init(cube, &p);
    uint32_t x, a, b, c;
    program_find_cell(&p, "x", &x);
    program_find_cell(&p, "a", &a);
    program_find_cell(&p, "b", &b);
    program_find_cell(&p, "c", &c);
    uint64_t s = 99;
    uint8_t xs[CUBE_SIZE];
    for (unsigned k = 0; k < CUBE_SIZE; k++)
        ref_set(&cube[k], x, xs[k] = (uint8_t)(rnd(&s) & 1));
    ref_cube_tick(cube);
    int bad = 0;
    for (unsigned k = 0; k < CUBE_SIZE; k++) {
        bad += ref_get(&cube[k], a) != !((k >> 4) & 1);
        bad += ref_get(&cube[k], b) != !xs[k ^ 40];
        bad += ref_get(&cube[k], c) != (k & 1);
    }
    CHECK(bad == 0, "id cells, links and chains of links in the reference cube (%d wrong)", bad);
    ref_cube_free(cube);
    free(cube);
    program_free(&p);
}

/* ---- lanes across links ------------------------------------------------------------------- */

static void test_permute(void)
{
    Line in, out;
    uint64_t s = 1234;
    int bad = 0;
    for (unsigned mask = 0; mask < CUBE_SIZE; mask++) {
        for (int w = 0; w < 8; w++)
            in.w[w] = rnd(&s);
        lane_permute(&out, &in, mask);
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            unsigned from = k ^ mask;
            bad += ((out.w[k >> 6] >> (k & 63)) & 1) != ((in.w[from >> 6] >> (from & 63)) & 1);
        }
    }
    CHECK(bad == 0, "every one of the 512 masks moves lane k XOR mask to lane k (%d wrong)", bad);
}

static void expect_match(const char *what, const Program *p, uint64_t seed)
{
    CheckResult r;
    int steps = (uint64_t)p->len * p->ncells < 300000;  /* big programs: after every pass */
    check_lanes(p, NULL, seed, 2, steps, &r);
    CHECK(r.ok, "%s, interpreter after every %s: tick %llu pc %ld lane %u cell %ld", what,
          steps ? "instruction" : "pass", (unsigned long long)r.tick, r.pc, r.lane, r.cell);
    for (int literal = 0; literal < 2; literal++) {
        JitCode jc;
        char err[128];
        if (!jit_compile(p, literal, &jc, NULL, err, sizeof err)) {
            CHECK(0, "%s: %s", what, err);
            continue;
        }
        check_lanes(p, &jc, seed + 1, 4, 0, &r);
        CHECK(r.ok, "%s, %s compile: tick %llu lane %u cell %ld: reference %u, compiled %u", what,
              literal ? "literal" : "optimized", (unsigned long long)r.tick, r.lane, r.cell,
              r.want, r.got);
        jit_free(&jc);
    }
}

/* Random programs over plain, id and link cells (links to links included), with writes to
   read-only cells mixed in, which every engine must ignore. */
static void test_random(void)
{
    uint64_t s = 0xC0BE;
    for (int k = 0; k < 150; k++) {
        uint32_t plain = 1 + (uint32_t)(rnd(&s) % 30), ids = (uint32_t)(rnd(&s) % 4),
                 links = 1 + (uint32_t)(rnd(&s) % 20);
        uint32_t n = CELL_FIRST_FREE + plain + ids + links;
        Program p;
        memset(&p, 0, sizeof p);
        p.ncells = n;
        p.special = calloc(n, sizeof *p.special);
        p.cell_names = calloc(n, sizeof *p.cell_names);
        for (uint32_t c = 0; c < n; c++) {
            p.cell_names[c] = malloc(16);
            snprintf(p.cell_names[c], 16, "c%u", c);
        }
        uint32_t c = CELL_FIRST_FREE + plain;
        for (uint32_t i = 0; i < ids; i++, c++) {
            p.special[c].kind = CK_ID;
            p.special[c].dim = (uint8_t)(rnd(&s) % CUBE_DIMS);
        }
        for (uint32_t i = 0; i < links; i++, c++) {
            p.special[c].kind = CK_LINK;
            p.special[c].dim = (uint8_t)(rnd(&s) % CUBE_DIMS);
            p.special[c].src = (uint32_t)(rnd(&s) % c);  /* any older cell, links too */
        }
        p.len = 1 + (size_t)(rnd(&s) % 150);
        p.code = malloc(p.len * sizeof *p.code);
        for (size_t i = 0; i < p.len; i++) {
            p.code[i].op = (uint8_t)(rnd(&s) & 1);
            uint32_t a = (uint32_t)(rnd(&s) % n);
            if (p.code[i].op == OP_STO && rnd(&s) % 8)  /* mostly writes to plain cells */
                a = CELL_FIRST_FREE + (uint32_t)(rnd(&s) % plain);
            p.code[i].addr = a;
        }
        char what[96];
        snprintf(what, sizeof what, "random program %d (%zu instructions, %u plain, %u id, %u links)",
                 k, p.len, plain, ids, links);
        expect_match(what, &p, 700 + (uint64_t)k);
        program_free(&p);
    }
}

/* ---- the OR over all 512 machines, against plain C ---------------------------------------- */

static void test_any(void)
{
    Program p;
    char err[256];
    if (!asm_file("examples/cube_any.asm", &p, err, sizeof err)) {
        CHECK(0, "%s", err);
        return;
    }
    expect_match("cube_any.asm", &p, 5);
    uint32_t x, any;
    program_find_cell(&p, "x", &x);
    program_find_cell(&p, "any", &any);
    JitCode jc;
    jit_compile(&p, 0, &jc, NULL, err, sizeof err);
    LaneMachine m;
    lane_init(&m, &p);
    int bad = 0;
    for (int one = -1; one < CUBE_SIZE; one += 37) {  /* nobody, then one machine at a time */
        lane_reset(&m);
        if (one >= 0)
            lane_set(&m, x, (unsigned)one, 1);
        jit_tick(&jc, &m);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            bad += lane_get(&m, any, k) != (one >= 0);
    }
    CHECK(bad == 0, "every machine learns whether any machine has x = 1 (%d wrong)", bad);
    lane_free(&m);
    jit_free(&jc);

    Spec *spec = spec_parse("any = x", &p, 1, err, sizeof err);
    Enum e;
    memset(&e, 0, sizeof e);
    e.mode = EN_VERIFY;
    e.a = &p;
    e.spec = spec;
    e.ticks = 1;
    e.parts = 1;
    EnumResult r;
    CHECK(!enum_run(&e, &r, err, sizeof err) && strstr(err, "connect the lanes"),
          "verify refuses programs whose lanes read each other: %s", err);
    spec_free(spec);
    program_free(&p);
}

/* ---- collectives (step 7b): lib/cube.inc against plain C ---------------------------------- */

typedef struct {
    const char *name;  /* a vector: the cell `name`, or name0, name1, ... */
    int         idx[64], width;
    int         out;   /* in the program's outputs, else inputs */
} Vec;

static int vec(const Program *p, Vec *v, const char *name, int out)
{
    v->name = name;
    v->out = out;
    v->width = vec_resolve(p, out ? p->outputs : p->inputs, out ? p->noutputs : p->ninputs, name,
                           v->idx);
    return v->width > 0;
}

static uint32_t vec_cell(const Program *p, const Vec *v, int b)
{
    return (v->out ? p->outputs : p->inputs)[v->idx[b]];
}

/* Runs one pass of p with in[i][k] = value of input vector i in machine k, on the reference cube
   (engine 0) or compiled (engine 1); out[i][k] gets output vector i of machine k. */
static void run_cube(const Program *p, int engine, const Vec *in, int nin,
                     uint64_t (*inv)[CUBE_SIZE], const Vec *out, int nout,
                     uint64_t (*outv)[CUBE_SIZE])
{
    RefMachine *cube = NULL;
    LaneMachine m;
    JitCode jc;
    char err[128];
    if (engine == 0) {
        cube = calloc(CUBE_SIZE, sizeof *cube);
        ref_cube_init(cube, p);
    } else {
        lane_init(&m, p);
        jit_compile(p, 0, &jc, NULL, err, sizeof err);
    }
    for (int i = 0; i < nin; i++)
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            for (int b = 0; b < in[i].width; b++) {
                uint8_t v = (uint8_t)((inv[i][k] >> b) & 1);
                if (engine == 0)
                    ref_set(&cube[k], vec_cell(p, &in[i], b), v);
                else
                    lane_set(&m, vec_cell(p, &in[i], b), k, v);
            }
    if (engine == 0)
        ref_cube_tick(cube);
    else
        jit_tick(&jc, &m);
    for (int i = 0; i < nout; i++)
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            uint64_t v = 0;
            for (int b = 0; b < out[i].width; b++) {
                uint32_t c = vec_cell(p, &out[i], b);
                v |= (uint64_t)(engine == 0 ? ref_get(&cube[k], c) : lane_get(&m, c, k)) << b;
            }
            outv[i][k] = v;
        }
    if (engine == 0) {
        ref_cube_free(cube);
        free(cube);
    } else {
        lane_free(&m);
        jit_free(&jc);
    }
}

static uint64_t in_v[3][CUBE_SIZE], out_v[2][CUBE_SIZE], want_v[2][CUBE_SIZE];
static uint8_t  care[CUBE_SIZE];  /* with use_care, output 0 only counts where care[k] = 1 */
static int      use_care;

/* Runs p on both engines with in_v, compares out_v with want_v (filled by the caller), and the
   first time (engines = 1) also checks the engines bit for bit from random states. */
static void run_both(const char *what, const Program *p, const char **ins, int nin,
                     const char **outs, int nout, int engines)
{
    Vec in[3], out[2];
    int ok = 1;
    for (int i = 0; i < nin; i++)
        ok &= vec(p, &in[i], ins[i], 0);
    for (int i = 0; i < nout; i++)
        ok &= vec(p, &out[i], outs[i], 1);
    CHECK(ok, "%s: the inputs and outputs exist", what);
    for (int engine = 0; ok && engine < 2; engine++) {
        run_cube(p, engine, in, nin, in_v, out, nout, out_v);
        int bad = 0;
        unsigned first = 0;
        for (int i = 0; i < nout; i++)
            for (unsigned k = 0; k < CUBE_SIZE; k++)
                if (!(i == 0 && use_care && !care[k]) && out_v[i][k] != want_v[i][k] && !bad++)
                    first = k;
        CHECK(bad == 0, "%s on the %s: %d values wrong, first in machine %u", what,
              engine ? "compiled code" : "reference cube", bad, first);
    }
    if (engines)
        expect_match(what, p, 900);
}

/* run_both on `body`, built with the libraries */
static void collective(const char *what, const char *body, const char **ins, int nin,
                       const char **outs, int nout, int engines)
{
    char src[512];
    snprintf(src, sizeof src, ".include \"lib/std.inc\"\n.include \"lib/cube.inc\"\n%s\n", body);
    Program p;
    if (!assemble(src, &p))
        return;
    run_both(what, &p, ins, nin, outs, nout, engines);
    program_free(&p);
}

static void test_collectives(void)
{
    uint64_t s = 0x7B;
    const char *x[] = {"x"}, *r[] = {"r"}, *pt[] = {"p", "t"}, *v[] = {"v"}, *m[] = {"m"};
    const char *xs[] = {"x", "s"};
    static const int density[] = {0, 1, 2, 50, 99, 100};  /* % of machines with x = 1; 1 = one */
    for (int di = 0; di < 6; di++) {
        int any = 0, all = 1, par = 0, pre = 0, cnt = 0;
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            in_v[0][k] = density[di] == 1 ? k == 300 : (int)(rnd(&s) % 100) < density[di];
            any |= (int)in_v[0][k];
            all &= (int)in_v[0][k];
            par ^= (int)in_v[0][k];
        }
        char what[64];
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = (uint64_t)any;
        snprintf(what, sizeof what, "ALL_OR, %d%% ones", density[di]);
        collective(what, ".in x\n.out r\nALL_OR x r", x, 1, r, 1, di == 3);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = (uint64_t)all;
        snprintf(what, sizeof what, "ALL_AND, %d%% ones", density[di]);
        collective(what, ".in x\n.out r\nALL_AND x r", x, 1, r, 1, di == 3);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = (uint64_t)par;
        snprintf(what, sizeof what, "ALL_XOR, %d%% ones", density[di]);
        collective(what, ".in x\n.out r\nALL_XOR x r", x, 1, r, 1, di == 3);
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            pre ^= (int)in_v[0][k];
            want_v[0][k] = (uint64_t)pre;  /* inclusive */
            want_v[1][k] = (uint64_t)par;
        }
        snprintf(what, sizeof what, "PXOR, %d%% ones", density[di]);
        collective(what, ".in x\n.out p t\nPXOR x p t", x, 1, pt, 2, di == 3);
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            want_v[0][k] = (uint64_t)cnt;  /* exclusive */
            cnt += (int)in_v[0][k];
        }
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[1][k] = (uint64_t)cnt;
        snprintf(what, sizeof what, "PCOUNT, %d%% ones", density[di]);
        collective(what,
                   ".in x\n.out p0 p1 p2 p3 p4 p5 p6 p7 p8 p9 t0 t1 t2 t3 t4 t5 t6 t7 t8 t9\n"
                   "PCOUNT x p t",
                   x, 1, pt, 2, di == 3);
    }

    for (int trial = 0; trial < 3; trial++) {
        unsigned from = trial == 0 ? 0 : trial == 1 ? 511 : (unsigned)(rnd(&s) % CUBE_SIZE);
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            in_v[0][k] = rnd(&s) & 1;
            in_v[1][k] = from;
        }
        char what[64];
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = in_v[0][0];
        snprintf(what, sizeof what, "BCAST0, trial %d", trial);
        collective(what, ".in x\n.out r\nBCAST0 x r", x, 1, r, 1, trial == 0);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = in_v[0][from];
        snprintf(what, sizeof what, "BCAST from machine %u", from);
        collective(what, ".in x s0 s1 s2 s3 s4 s5 s6 s7 s8\n.out r\nBCAST x r s", xs, 2, r, 1,
                   trial == 0);

        uint64_t hi = 0, lo = 255;
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            in_v[0][k] = trial == 2 ? 77 : rnd(&s) & 255;  /* trial 2: all equal */
            hi = in_v[0][k] > hi ? in_v[0][k] : hi;
            lo = in_v[0][k] < lo ? in_v[0][k] : lo;
        }
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = hi;
        snprintf(what, sizeof what, "MAX8, trial %d", trial);
        collective(what, ".in v0 v1 v2 v3 v4 v5 v6 v7\n.out m0 m1 m2 m3 m4 m5 m6 m7\nMAX8 v m", v,
                   1, m, 1, trial == 0);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = lo;
        snprintf(what, sizeof what, "MIN8, trial %d", trial);
        collective(what, ".in v0 v1 v2 v3 v4 v5 v6 v7\n.out m0 m1 m2 m3 m4 m5 m6 m7\nMIN8 v m", v,
                   1, m, 1, trial == 0);
    }
}

/* ---- moving data (step 7c) ---------------------------------------------------------------- */

static unsigned rev9(unsigned k)
{
    unsigned r = 0;
    for (int b = 0; b < CUBE_DIMS; b++)
        r |= ((k >> b) & 1) << (CUBE_DIMS - 1 - b);
    return r;
}

static void permutation(int kind, uint64_t *s, uint16_t *dest)
{
    for (unsigned k = 0; k < CUBE_SIZE; k++)
        dest[k] = (uint16_t)(kind == 0 ? k
                           : kind == 1 ? CUBE_SIZE - 1 - k
                           : kind == 2 ? rev9(k)
                           : kind == 3 ? (k + 1) % CUBE_SIZE
                           : kind == 4 ? ((k << 1) | (k >> 8)) & (CUBE_SIZE - 1)  /* shuffle */
                           : kind == 5 ? (k % 32) * 16 + k / 32                   /* transpose */
                                       : k);
    if (kind >= 6)
        for (unsigned k = CUBE_SIZE - 1; k > 0; k--) {
            unsigned j = (unsigned)(rnd(s) % (k + 1));
            uint16_t t = dest[k];
            dest[k] = dest[j];
            dest[j] = t;
        }
}

static void test_benes(void)
{
    static uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE];
    uint64_t s = 0xBE5;
    int bad = 0, pairs = 0;
    for (int kind = 0; kind < 6 + 200; kind++) {
        uint16_t dest[CUBE_SIZE];
        uint64_t v[CUBE_SIZE];
        permutation(kind, &s, dest);
        benes_route(dest, ctrl);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            v[k] = k;
        benes_apply(ctrl, v);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            bad += v[dest[k]] != k;
        for (int r = 0; r < BENES_ROUNDS; r++)
            for (unsigned k = 0; k < CUBE_SIZE; k++)
                pairs += ctrl[r][k] != ctrl[r][k ^ (1u << benes_dim(r))];
    }
    CHECK(bad == 0, "Beneš routing of 206 permutations: %d values in the wrong place", bad);
    CHECK(pairs == 0, "both machines of a pair always get the same setting (%d differ)", pairs);
}

static void test_movement(void)
{
    uint64_t s = 0x7C;
    const char *v[] = {"v"}, *vh[] = {"v", "h"}, *vc[] = {"v", "c"};
    char what[64];

    /* sort: random, all equal, already sorted, reversed */
    for (int kind = 0; kind < 4; kind++) {
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            in_v[0][k] = kind == 0 ? rnd(&s) & 255 : kind == 1 ? 9 : kind == 2 ? k / 2 : 255 - k / 2;
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = in_v[0][k];
        for (unsigned a = 1; a < CUBE_SIZE; a++)  /* insertion sort, the plain C answer */
            for (unsigned b = a; b > 0 && want_v[0][b - 1] > want_v[0][b]; b--) {
                uint64_t t = want_v[0][b];
                want_v[0][b] = want_v[0][b - 1];
                want_v[0][b - 1] = t;
            }
        snprintf(what, sizeof what, "SORT8, input kind %d", kind);
        collective(what, ".in v0 v1 v2 v3 v4 v5 v6 v7\n.out v0 v1 v2 v3 v4 v5 v6 v7\nSORT8 v", v,
                   1, v, 1, kind == 0);
    }

    /* pack: no hits, one hit, few, half, all */
    static const int density[] = {0, 1, 10, 50, 100};
    for (int di = 0; di < 5; di++) {
        unsigned n = 0;
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            in_v[0][k] = rnd(&s) & 255;
            in_v[1][k] = density[di] == 1 ? k == 300 : (int)(rnd(&s) % 100) < density[di];
            if (in_v[1][k])
                want_v[0][n++] = in_v[0][k];
        }
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            want_v[1][k] = k < n;
            care[k] = k < n;  /* machines without a value may hold anything */
        }
        use_care = 1;
        snprintf(what, sizeof what, "PACK8, %d%% hits", density[di]);
        collective(what,
                   ".in v0 v1 v2 v3 v4 v5 v6 v7 h\n.out v0 v1 v2 v3 v4 v5 v6 v7 h\n"
                   "PCOUNT h p t\nPACK8 v h p",
                   vh, 2, vh, 2, di == 2);
        use_care = 0;
    }

    /* permutations: reverse, bit reversal, transpose, random */
    static uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE];
    static const int kinds[] = {1, 2, 5, 6, 7};
    for (int ki = 0; ki < 5; ki++) {
        uint16_t dest[CUBE_SIZE];
        permutation(kinds[ki], &s, dest);
        benes_route(dest, ctrl);
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            in_v[0][k] = rnd(&s) & 255;
            in_v[1][k] = 0;
            for (int r = 0; r < BENES_ROUNDS; r++)
                in_v[1][k] |= (uint64_t)ctrl[r][k] << r;
        }
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][dest[k]] = in_v[0][k];
        snprintf(what, sizeof what, "PERM8, permutation kind %d", kinds[ki]);
        collective(what,
                   ".in v0 v1 v2 v3 v4 v5 v6 v7 c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11 c12 c13 "
                   "c14 c15 c16\n.out v0 v1 v2 v3 v4 v5 v6 v7\nPERM8 v c",
                   vc, 2, v, 1, ki == 3);
    }
}

/* ---- transforms (step 7d) ----------------------------------------------------------------- */

static int parity(unsigned x)
{
    int p = 0;
    for (; x; x &= x - 1)
        p ^= 1;
    return p;
}

static void test_transforms(void)
{
    uint64_t s = 0x7D;
    const char *f[] = {"f"}, *fg[] = {"f", "g"}, *w[] = {"w"}, *h[] = {"h"};
    static const int density[] = {0, 100, 1, 50, 10};  /* % of machines with f = 1; 1 = machine 0 */
    char what[64];
    for (int di = 0; di < 5; di++) {
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            in_v[0][k] = density[di] == 1 ? k == 0 : (int)(rnd(&s) % 100) < density[di];

        /* the algebraic normal form: machine k gets the XOR of f(j) over every j inside k */
        int bad = 0;
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            uint64_t a = 0;
            for (unsigned j = 0; j < CUBE_SIZE; j++)
                if ((j & k) == j)
                    a ^= in_v[0][j];
            want_v[0][k] = a;
            bad += density[di] == 100 && a != (k == 0);  /* 1 is the empty monomial */
            bad += density[di] == 1 && a != 1;           /* [x = 0] has every monomial */
        }
        CHECK(bad == 0, "the plain C normal form of a constant and of [x = 0]");
        snprintf(what, sizeof what, "MOBIUS, %d%% ones", density[di]);
        collective(what, ".in f\n.out f\nMOBIUS f", f, 1, f, 1, di == 3);
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            want_v[0][k] = in_v[0][k];
        snprintf(what, sizeof what, "MOBIUS twice, %d%% ones", density[di]);
        collective(what, ".in f\n.out f\nMOBIUS f\nMOBIUS f", f, 1, f, 1, 0);

        /* the Walsh-Hadamard spectrum, mod 2^bits: 10 bits wrap 512 around to -512 */
        int64_t energy = 0;
        int spec[CUBE_SIZE];
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            spec[k] = 0;
            for (unsigned x = 0; x < CUBE_SIZE; x++)
                spec[k] += ((int)in_v[0][x] ^ parity(x & k)) ? -1 : 1;
            energy += (int64_t)spec[k] * spec[k];
        }
        CHECK(energy == (int64_t)CUBE_SIZE * CUBE_SIZE, "Parseval for the plain C spectrum");
        for (int bits = 12; bits >= 10; bits -= 2) {
            Program p;
            gen_wht(&p, bits);
            for (unsigned k = 0; k < CUBE_SIZE; k++)
                want_v[0][k] = (uint64_t)(int64_t)spec[k] & ((1u << bits) - 1);
            snprintf(what, sizeof what, "WHT, %d bits, %d%% ones", bits, density[di]);
            run_both(what, &p, f, 1, w, 1, di == 3 && bits == 12);
            program_free(&p);
        }
    }

    /* XOR convolution: h(k) = the number of x with f(x) = g(x ^ k) = 1 */
    Program p;
    gen_xorconv(&p);
    for (int trial = 0; trial < 5; trial++) {
        unsigned a = (unsigned)(rnd(&s) % CUBE_SIZE), b = (unsigned)(rnd(&s) % CUBE_SIZE);
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            switch (trial) {
            case 0: in_v[0][k] = in_v[1][k] = 1; break;                  /* h = 512 */
            case 1: in_v[0][k] = k == a; in_v[1][k] = k == b; break;     /* h = [k = a ^ b] */
            case 2: in_v[0][k] = rnd(&s) & 1; in_v[1][k] = rnd(&s) & 1; break;
            case 3: in_v[0][k] = rnd(&s) % 10 == 0; in_v[1][k] = rnd(&s) % 10 != 0; break;
            default: in_v[0][k] = 0; in_v[1][k] = rnd(&s) & 1; break;    /* h = 0 */
            }
        }
        int bad = 0;
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            uint64_t n = 0;
            for (unsigned x = 0; x < CUBE_SIZE; x++)
                n += in_v[0][x] & in_v[1][x ^ k];
            want_v[0][k] = n;
            bad += (trial == 0 && n != CUBE_SIZE) || (trial == 1 && n != (k == (a ^ b))) ||
                   (trial == 4 && n != 0);
        }
        CHECK(bad == 0, "the plain C convolution, trial %d", trial);
        snprintf(what, sizeof what, "XOR convolution, trial %d", trial);
        run_both(what, &p, fg, 2, h, 1, trial == 2);
    }
    program_free(&p);
}

/* c = a * b mod (x^512 + 1, 12289) by NTT on the cube, against schoolbook multiplication */
static void test_polymul(void)
{
    enum { NIN = 2 + 2 * CUBE_DIMS };
    static uint16_t zf[CUBE_DIMS][CUBE_SIZE], zi[CUBE_DIMS][CUBE_SIZE], a[CUBE_SIZE],
        b[CUBE_SIZE], c[CUBE_SIZE];
    static uint64_t inv[NIN][CUBE_SIZE], outv[1][CUBE_SIZE];
    static char names[NIN][8];
    Program p;
    gen_polymul(&p);
    ntt_twiddles(zf, zi);
    Vec in[NIN], out[1];
    int ok = vec(&p, &in[0], "a", 0) && vec(&p, &in[1], "b", 0) && vec(&p, &out[0], "c", 1);
    for (int d = 0; d < CUBE_DIMS; d++) {
        snprintf(names[2 + d], sizeof names[0], "zf%d_", d);
        snprintf(names[2 + CUBE_DIMS + d], sizeof names[0], "zi%d_", d);
        ok &= vec(&p, &in[2 + d], names[2 + d], 0) &&
              vec(&p, &in[2 + CUBE_DIMS + d], names[2 + CUBE_DIMS + d], 0);
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            inv[2 + d][k] = zf[d][k];
            inv[2 + CUBE_DIMS + d][k] = zi[d][k];
        }
    }
    CHECK(ok && in[0].width == 14 && out[0].width == 14 && in[NIN - 1].width == 14,
          "polymul: the inputs and outputs exist");
    uint64_t s = 0x9F;
    for (int trial = 0; ok && trial < 5; trial++) {
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            uint64_t r = rnd(&s), sparse = rnd(&s) % 8 == 0;
            a[k] = (uint16_t)(trial == 3 ? NTT_Q - 1 : rnd(&s) % NTT_Q);
            b[k] = (uint16_t)(trial == 0   ? k == 0  /* 1 */
                              : trial == 1 ? k == 1  /* x */
                              : trial == 3 ? NTT_Q - 1
                              : trial == 4 ? sparse * (r % NTT_Q)
                                           : r % NTT_Q);
            inv[0][k] = a[k];
            inv[1][k] = b[k];
        }
        polymul_scalar(a, b, c);
        int bad = 0;
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            bad += (trial == 0 && c[k] != a[k]) ||
                   (trial == 1 && c[k] != (k ? a[k - 1] : (NTT_Q - a[CUBE_SIZE - 1]) % NTT_Q));
        CHECK(bad == 0, "the plain C product, trial %d (times 1, times x: x^512 = -1)", trial);
        for (int engine = trial < 2 ? 0 : 1; engine < 2; engine++) {
            run_cube(&p, engine, in, NIN, inv, out, 1, outv);
            bad = 0;
            unsigned first = 0;
            for (unsigned k = 0; k < CUBE_SIZE; k++)
                if (outv[0][k] != c[k] && !bad++)
                    first = k;
            CHECK(bad == 0, "polymul, trial %d, on the %s: %d coefficients wrong, first %u", trial,
                  engine ? "compiled code" : "reference cube", bad, first);
        }
    }
    expect_match("polymul", &p, 900);
    program_free(&p);
}

/* ---- grids (step 7e) ---------------------------------------------------------------------- */

/* Life on one board of (2^rbits h) x (2^(9-rbits) w), tiles in the machines, for `gens`
   generations on the reference cube and on compiled code, against plain C on the whole board */
static void life_cube(int w, int h, int rbits, int gens, int density, uint64_t *s, int engines)
{
    Program p;
    uint32_t *board = malloc((size_t)(w * h) * sizeof *board);
    gen_life_cube(&p, w, h, rbits, CUBE_DIMS - rbits, board);
    int gw = w << (CUBE_DIMS - rbits), gh = h << rbits;
    uint8_t *big = malloc((size_t)(gw * gh));
    RefMachine *cube = calloc(CUBE_SIZE, sizeof *cube);
    ref_cube_init(cube, &p);
    LaneMachine m;
    lane_init(&m, &p);
    JitCode jc;
    char err[128];
    int ok = jit_compile(&p, 0, &jc, NULL, err, sizeof err);
    CHECK(ok, "life on the cube compiles: %s", ok ? "" : err);
    for (int R = 0; R < gh; R++)
        for (int C = 0; C < gw; C++) {
            uint8_t v = (uint8_t)((int)(rnd(s) % 100) < density);
            unsigned k = grid_machine((unsigned)(R / h), (unsigned)(C / w), CUBE_DIMS - rbits);
            big[R * gw + C] = v;
            ref_set(&cube[k], board[(R % h) * w + C % w], v);
            lane_set(&m, board[(R % h) * w + C % w], k, v);
        }
    for (int g = 0; g < gens; g++) {
        ref_cube_tick(cube);
        if (ok)
            jit_tick(&jc, &m);
        life_scalar(big, gw, gh);
    }
    int bad[2] = {0, 0}, alive = 0;
    for (int R = 0; R < gh; R++)
        for (int C = 0; C < gw; C++) {
            unsigned k = grid_machine((unsigned)(R / h), (unsigned)(C / w), CUBE_DIMS - rbits);
            uint32_t cell = board[(R % h) * w + C % w];
            bad[0] += ref_get(&cube[k], cell) != big[R * gw + C];
            bad[1] += ok && lane_get(&m, cell, k) != big[R * gw + C];
            alive += big[R * gw + C];
        }
    CHECK(bad[0] == 0 && bad[1] == 0,
          "life on one %d x %d board of %d x %d tiles, %d generations: %d cells wrong on the "
          "reference cube, %d compiled (%d alive)",
          gh, gw, h, w, gens, bad[0], bad[1], alive);
    if (engines) {
        char what[64];
        snprintf(what, sizeof what, "life on the cube, %d x %d tiles", h, w);
        expect_match(what, &p, 900);
    }
    if (ok)
        jit_free(&jc);
    lane_free(&m);
    ref_cube_free(cube);
    free(cube);
    free(big);
    free(board);
    program_free(&p);
}

static void test_grids(void)
{
    uint64_t s = 0x7E;
    unsigned pos[CUBE_SIZE], at[CUBE_SIZE];  /* machine -> ring position and back */
    for (unsigned i = 0; i < CUBE_SIZE; i++) {
        at[i] = grid_machine(0, i, CUBE_DIMS);
        pos[at[i]] = i;
    }
    int bad = 0;
    for (unsigned i = 0; i < CUBE_SIZE; i++) {  /* Gray code: neighbours one link apart */
        unsigned d = at[i] ^ at[(i + 1) % CUBE_SIZE];
        bad += d == 0 || (d & (d - 1)) != 0;
    }
    CHECK(bad == 0 && at[511] == 256, "the ring in Gray code order closes, one bit at a time");

    const char *x[] = {"x"}, *rl[] = {"r", "l"};
    char what[64];
    for (int trial = 0; trial < 4; trial++) {
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            in_v[0][k] = trial == 0 ? k == 0 : trial == 1 ? k == 256 : rnd(&s) & 1;
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            want_v[0][k] = in_v[0][at[(pos[k] + 1) % CUBE_SIZE]];
            want_v[1][k] = in_v[0][at[(pos[k] + CUBE_SIZE - 1) % CUBE_SIZE]];
        }
        snprintf(what, sizeof what, "RING_NEXT and RING_PREV, trial %d", trial);
        collective(what, ".in x\n.out r l\nRING_MASKS\nRING_NEXT x r\nRING_PREV x l", x, 1, rl, 2,
                   trial == 2);
        snprintf(what, sizeof what, "RING_NEXT in place, trial %d", trial);
        collective(what, ".in x\n.out x\nRING_MASKS\nRING_NEXT x x", x, 1, x, 1, 0);
    }

    /* Rule 110 (examples/rule110.asm) for a few steps, each against plain C on the ring */
    Program p;
    char err[512];
    if (!asm_file("examples/rule110.asm", &p, err, sizeof err)) {
        CHECK(0, "%s", err);
    } else {
        for (unsigned k = 0; k < CUBE_SIZE; k++)
            in_v[0][k] = rnd(&s) & 1;
        for (int step = 0; step < 6; step++) {
            for (unsigned i = 0; i < CUBE_SIZE; i++) {
                uint64_t l = in_v[0][at[(i + CUBE_SIZE - 1) % CUBE_SIZE]], c = in_v[0][at[i]],
                         r = in_v[0][at[(i + 1) % CUBE_SIZE]];
                want_v[0][at[i]] = (110 >> ((l << 2) | (c << 1) | r)) & 1;
            }
            snprintf(what, sizeof what, "Rule 110, step %d", step);
            run_both(what, &p, x, 1, x, 1, step == 0);
            memcpy(in_v[0], want_v[0], sizeof in_v[0]);
        }
        program_free(&p);
    }

    /* Life: one cell per machine, flat and tall grids, bigger tiles */
    static const int shape[][5] = {
        /* w, h, rbits, generations, % alive */
        {1, 1, 4, 6, 40}, {1, 1, 0, 4, 40}, {1, 1, 9, 4, 40}, {3, 2, 4, 5, 35},
        {2, 3, 1, 4, 35}, {4, 4, 8, 4, 30}, {5, 3, 5, 3, 50}, {8, 8, 4, 3, 35},
    };
    for (size_t i = 0; i < sizeof shape / sizeof shape[0]; i++)
        life_cube(shape[i][0], shape[i][1], shape[i][2], shape[i][3], shape[i][4], &s, i == 3);
}

/* ---- more than 512 machines (step 7f) ----------------------------------------------------- */

static uint64_t splitmix(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void random_state(LaneCube *c, uint64_t seed)
{
    for (unsigned g = 0; g < c->groups; g++) {
        for (uint32_t a = CELL_FIRST_FREE; a < c->prog->ncells; a++)
            for (int w = 0; w < 8; w++) {
                uint64_t v = splitmix(&seed);
                if (!program_readonly(c->prog, a))
                    c->g[g].mem[a].w[w] = v;
            }
        for (int w = 0; w < 8; w++)
            c->g[g].rr.w[w] = splitmix(&seed);
    }
}

/* The same passes on one thread and on `threads` threads, from the same random state: the
   barriers must give bit for bit the same machines. */
static void same_on_threads(const char *what, const Program *p, int threads, uint64_t passes)
{
    JitCode jc;
    char err[128];
    LaneCube a, b;
    if (!jit_compile(p, 0, &jc, NULL, err, sizeof err) || !lane_cube_init(&a, p) ||
        !lane_cube_init(&b, p)) {
        CHECK(0, "%s: %s", what, err);
        return;
    }
    random_state(&a, 77);
    random_state(&b, 77);
    jit_cube_run(&jc, &a, passes);
    par_cube_run(&jc, &b, threads, NULL, 0, passes, NULL);
    int bad = 0;
    for (unsigned g = 0; g < a.groups; g++) {
        Line d;
        lane_diff(&a.g[g], &b.g[g], &d);
        for (int w = 0; w < 8; w++)
            bad += d.w[w] != 0;
    }
    CHECK(bad == 0, "%s: %d threads give the same %u machines as one after %llu passes (%u "
          "segments)", what, threads, a.groups * LANES, (unsigned long long)passes, jc.nseg);
    lane_cube_free(&a);
    lane_cube_free(&b);
    jit_free(&jc);
}

/* Random programs on 1024 .. 4096 machines: links across all dimensions, chains of them, id cells
   up to the top, writes to read-only cells. Interpreter, both compilers and threads against the
   reference cube. */
static void test_big_random(void)
{
    uint64_t s = 0xB16;
    unsigned most = 0, total = 0;
    for (int k = 0; k < 60; k++) {
        unsigned dims = CUBE_DIMS + 1 + (unsigned)(rnd(&s) % 3);
        uint32_t plain = 1 + (uint32_t)(rnd(&s) % 20), ids = (uint32_t)(rnd(&s) % 4),
                 links = 1 + (uint32_t)(rnd(&s) % 16);
        uint32_t n = CELL_FIRST_FREE + plain + ids + links;
        Program p;
        memset(&p, 0, sizeof p);
        p.ncells = n;
        p.special = calloc(n, sizeof *p.special);
        p.cell_names = calloc(n, sizeof *p.cell_names);
        for (uint32_t c = 0; c < n; c++) {
            p.cell_names[c] = malloc(16);
            snprintf(p.cell_names[c], 16, "c%u", c);
        }
        uint32_t c = CELL_FIRST_FREE + plain;
        for (uint32_t i = 0; i < ids; i++, c++) {
            p.special[c].kind = CK_ID;
            p.special[c].dim = (uint8_t)(rnd(&s) % dims);
        }
        for (uint32_t i = 0; i < links; i++, c++) {
            p.special[c].kind = CK_LINK;  /* half of them to other groups */
            p.special[c].dim = (uint8_t)(rnd(&s) % 2 ? CUBE_DIMS + rnd(&s) % (dims - CUBE_DIMS)
                                                     : rnd(&s) % CUBE_DIMS);
            p.special[c].src = (uint32_t)(rnd(&s) % c);
        }
        p.special[n - 1].dim = (uint8_t)(dims - 1);  /* the cube is that big */
        p.special[n - 1].kind = CK_LINK;
        p.len = 1 + (size_t)(rnd(&s) % 120);
        p.code = malloc(p.len * sizeof *p.code);
        for (size_t i = 0; i < p.len; i++) {
            p.code[i].op = (uint8_t)(rnd(&s) & 1);
            uint32_t a = (uint32_t)(rnd(&s) % n);
            if (p.code[i].op == OP_STO && rnd(&s) % 8)
                a = CELL_FIRST_FREE + (uint32_t)(rnd(&s) % plain);
            p.code[i].addr = a;
        }
        CHECK(program_dims(&p) == dims, "random program %d needs %u dimensions", k, dims);
        CheckResult r;
        int steps = k % 4 == 0;
        check_lanes(&p, NULL, 900 + (uint64_t)k, 2, steps, &r);
        CHECK(r.ok, "big random program %d (%u machines, %zu instructions), interpreter after "
              "every %s: tick %llu pc %ld machine %u cell %ld", k, 1u << dims, p.len,
              steps ? "instruction" : "pass", (unsigned long long)r.tick, r.pc, r.lane, r.cell);
        for (int literal = 0; literal < 2; literal++) {
            JitCode jc;
            char err[128];
            if (!jit_compile(&p, literal, &jc, NULL, err, sizeof err)) {
                CHECK(0, "big random program %d: %s", k, err);
                continue;
            }
            most = jc.nseg > most ? jc.nseg : most;
            total += jc.nseg;
            check_lanes(&p, &jc, 950 + (uint64_t)k, 3, 0, &r);
            CHECK(r.ok, "big random program %d (%u machines, %zu instructions, %u segments), %s "
                  "compile: tick %llu machine %u cell %ld: reference %u, compiled %u", k,
                  1u << dims, p.len, jc.nseg, literal ? "literal" : "optimized",
                  (unsigned long long)r.tick, r.lane, r.cell, r.want, r.got);
            jit_free(&jc);
        }
        if (k % 3 == 0) {
            char what[64];
            snprintf(what, sizeof what, "big random program %d", k);
            same_on_threads(what, &p, 2 + k % 5, 4);
        }
        program_free(&p);
    }
    CHECK(most >= 5 && total >= 300, "the random programs needed cuts: up to %u segments, %u in "
          "all", most, total);
}

/* in/out vectors of every machine of a big cube: reference cube (engine 0), compiled on one thread
   (1) or on 4 threads (2) */
enum { BIG_MAX = 1 << 12 };
static uint64_t big_in[2][BIG_MAX], big_out[2][BIG_MAX], big_want[2][BIG_MAX];

static void big_collective(const char *what, const Program *p, const char **ins, int nin,
                           const char **outs, int nout, int engines)
{
    Vec in[2], out[2];
    int ok = 1;
    unsigned n = ref_cube_machines(p);
    for (int i = 0; i < nin; i++)
        ok &= vec(p, &in[i], ins[i], 0);
    for (int i = 0; i < nout; i++)
        ok &= vec(p, &out[i], outs[i], 1);
    CHECK(ok && n <= BIG_MAX, "%s: the inputs and outputs exist", what);
    for (int engine = 0; ok && engine < 3; engine++) {
        RefMachine *cube = NULL;
        LaneCube c;
        JitCode jc;
        char err[128];
        if (engine == 0) {
            cube = calloc(n, sizeof *cube);
            ref_cube_init(cube, p);
        } else {
            lane_cube_init(&c, p);
            jit_compile(p, 0, &jc, NULL, err, sizeof err);
        }
        for (int i = 0; i < nin; i++)
            for (unsigned k = 0; k < n; k++)
                for (int b = 0; b < in[i].width; b++) {
                    uint8_t v = (uint8_t)((big_in[i][k] >> b) & 1);
                    if (engine == 0)
                        ref_set(&cube[k], vec_cell(p, &in[i], b), v);
                    else
                        lane_cube_set(&c, vec_cell(p, &in[i], b), k, v);
                }
        if (engine == 0)
            ref_cube_tick(cube);
        else if (engine == 1)
            jit_cube_tick(&jc, &c);
        else
            par_cube_run(&jc, &c, 4, NULL, 0, 1, NULL);
        int bad = 0;
        unsigned first = 0;
        for (int i = 0; i < nout; i++)
            for (unsigned k = 0; k < n; k++) {
                uint64_t v = 0;
                for (int b = 0; b < out[i].width; b++) {
                    uint32_t cell = vec_cell(p, &out[i], b);
                    v |= (uint64_t)(engine == 0 ? ref_get(&cube[k], cell)
                                                : lane_cube_get(&c, cell, k)) << b;
                }
                if (v != big_want[i][k] && !bad++)
                    first = k;
            }
        CHECK(bad == 0, "%s on %s: %d values wrong, first in machine %u", what,
              engine == 0 ? "the reference cube" : engine == 1 ? "one thread" : "4 threads", bad,
              first);
        if (engine == 0) {
            ref_cube_free(cube);
            free(cube);
        } else {
            lane_cube_free(&c);
            jit_free(&jc);
        }
    }
    if (engines)
        expect_match(what, p, 990);
}

static int cmp64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void test_big_cube(void)
{
    uint64_t s = 0xB17;
    char src[8192], what[64];
    size_t len;
    Program p;
    const char *x[] = {"x"}, *r[] = {"r"}, *pt[] = {"p", "t"}, *v[] = {"v"};

    /* lane-id cells and links up to dimension 15 */
    if (assemble(".link a = x @ 15\n.link b = a @ 9\nNAND a\nNAND b\nNAND ID12\nSTO y\n", &p)) {
        CHECK(program_dims(&p) == 16 && ref_cube_machines(&p) == 65536,
              "a link across dimension 15 makes a cube of 65536 machines");
        unsigned mask;
        program_resolve(&p, p.code[1].addr, &mask);
        CHECK(mask == (1u << 15 | 1u << 9), "a chain of links across 15 and 9: mask %x", mask);
        program_free(&p);
    }
    if (assemble("NAND ID9\nSTO y\n", &p)) {
        CHECK(program_dims(&p) == 10, "ID9 alone makes a cube of 1024 machines");
        program_free(&p);
    }

    /* OR over 2^dims machines */
    for (unsigned dims = 10; dims <= 12; dims++) {
        unsigned n = 1u << dims;
        len = (size_t)snprintf(src, sizeof src,
                               ".include \"lib/std.inc\"\n.include \"lib/cube.inc\"\n.in x\n.out r\n"
                               "MOV r x\n");
        for (unsigned d = 0; d < dims; d++)
            len += (size_t)snprintf(src + len, sizeof src - len, "OR_DIM r %u\n", d);
        if (!assemble(src, &p))
            continue;
        for (int trial = 0; trial < 3; trial++) {
            unsigned one = trial == 0 ? n - 1 : trial == 1 ? n : (unsigned)(rnd(&s) % n);
            for (unsigned k = 0; k < n; k++) {
                big_in[0][k] = k == one;
                big_want[0][k] = one < n;
            }
            snprintf(what, sizeof what, "OR over %u machines, trial %d", n, trial);
            big_collective(what, &p, x, 1, r, 1, trial == 0 && dims == 11);
        }
        program_free(&p);
    }

    /* running count */
    for (unsigned dims = 10; dims <= 12; dims += 2) {
        unsigned n = 1u << dims;
        gen_rank(&p, (int)dims);
        uint64_t count = 0;
        for (unsigned k = 0; k < n; k++) {
            big_in[0][k] = rnd(&s) % 3 == 0;
            big_want[0][k] = count;
            count += big_in[0][k];
        }
        for (unsigned k = 0; k < n; k++)
            big_want[1][k] = count;
        snprintf(what, sizeof what, "gen_rank on %u machines", n);
        big_collective(what, &p, x, 1, pt, 2, dims == 10);
        program_free(&p);
    }

    /* bitonic sort, CMPX8_FAR across the groups */
    for (unsigned dims = 10; dims <= 11; dims++) {
        unsigned n = 1u << dims;
        len = (size_t)snprintf(src, sizeof src,
                               ".include \"lib/std.inc\"\n.include \"lib/cube.inc\"\n"
                               ".in v0 v1 v2 v3 v4 v5 v6 v7\n.out v0 v1 v2 v3 v4 v5 v6 v7\n");
        for (unsigned st = 1; st <= dims; st++)
            for (int j = (int)st - 1; j >= 0; j--) {
                char dir[8];  /* up or down by the block's next bit; the last merge all up */
                snprintf(dir, sizeof dir, st < dims ? "ID%u" : "ZERO", st);
                len += (size_t)snprintf(src + len, sizeof src - len, "%s v %d %s\n",
                                        j < CUBE_DIMS ? "CMPX8" : "CMPX8_FAR", j, dir);
            }
        if (!assemble(src, &p))
            continue;
        for (int kind = 0; kind < 2; kind++) {
            for (unsigned k = 0; k < n; k++)
                big_in[0][k] = big_want[0][k] = kind ? 255 - k % 256 : rnd(&s) & 255;
            qsort(big_want[0], n, sizeof big_want[0][0], cmp64);
            snprintf(what, sizeof what, "sort of %u bytes, kind %d", n, kind);
            big_collective(what, &p, v, 1, v, 1, 0);
        }
        same_on_threads("sort", &p, 6, 3);
        program_free(&p);
    }

    /* Life on one board across groups, and the threads on it */
    static const int shape[][5] = {
        /* w, h, rbits, cbits, generations */
        {1, 1, 5, 5, 4}, {2, 3, 2, 9, 3}, {4, 4, 6, 5, 3}, {3, 2, 10, 0, 3},
    };
    for (size_t i = 0; i < sizeof shape / sizeof shape[0]; i++) {
        int w = shape[i][0], h = shape[i][1], rb = shape[i][2], cb = shape[i][3];
        uint32_t board[16];
        gen_life_cube(&p, w, h, rb, cb, board);
        int gw = w << cb, gh = h << rb, alive = 0;
        uint8_t *big = malloc((size_t)(gw * gh));
        LaneCube c;
        JitCode jc;
        char err[128];
        lane_cube_init(&c, &p);
        jit_compile(&p, 0, &jc, NULL, err, sizeof err);
        for (int R = 0; R < gh; R++)
            for (int C = 0; C < gw; C++) {
                big[R * gw + C] = (uint8_t)(rnd(&s) % 3 == 0);
                lane_cube_set(&c, board[(R % h) * w + C % w],
                              grid_machine((unsigned)(R / h), (unsigned)(C / w), cb), big[R * gw + C]);
            }
        par_cube_run(&jc, &c, 5, NULL, 0, (uint64_t)shape[i][4], NULL);
        for (int g = 0; g < shape[i][4]; g++)
            life_scalar(big, gw, gh);
        int bad = 0;
        for (int R = 0; R < gh; R++)
            for (int C = 0; C < gw; C++) {
                alive += big[R * gw + C];
                bad += lane_cube_get(&c, board[(R % h) * w + C % w],
                                     grid_machine((unsigned)(R / h), (unsigned)(C / w), cb)) !=
                       big[R * gw + C];
            }
        CHECK(bad == 0, "life on one %d x %d board over %u machines, 5 threads, %d generations: "
              "%d cells wrong (%d alive, %u segments)", gh, gw, c.groups * LANES, shape[i][4],
              bad, alive, jc.nseg);
        if (i == 1)
            expect_match("life over 2048 machines", &p, 991);
        if (i == 2)
            same_on_threads("life", &p, 7, 3);
        lane_cube_free(&c);
        jit_free(&jc);
        free(big);
        program_free(&p);
    }
}

int main(void)
{
    test_assembler();
    test_reference();
    test_benes();
    if (!cpu_has_avx2()) {
        printf("%d checks, %d failures (engine tests skipped: no AVX2)\n", checks, failures);
        return failures != 0;
    }
    test_permute();
    test_random();
    test_any();
    test_collectives();
    test_movement();
    test_transforms();
    test_polymul();
    test_grids();
    test_big_random();
    test_big_cube();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
