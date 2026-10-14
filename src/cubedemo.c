/* alpha cube: the cube's collectives and data movement (lib/cube.inc) on compiled code. Every one
   gets random inputs, is checked against plain C in all 512 machines, and is timed on one thread
   and on every thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "benes.h"
#include "gen.h"
#include "jit.h"
#include "lanes.h"
#include "parallel.h"
#include "platform.h"
#include "spec.h"
#include "work.h"

static uint64_t rnd(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

typedef struct {
    uint32_t cell[64];
    int      width;
} Vec;

static int vec_of(const Program *p, int out, const char *name, Vec *v)
{
    int idx[64];
    const uint32_t *list = out ? p->outputs : p->inputs;
    v->width = vec_resolve(p, list, out ? p->noutputs : p->ninputs, name, idx);
    for (int b = 0; b < v->width; b++)
        v->cell[b] = list[idx[b]];
    return v->width > 0;
}

static void put(LaneMachine *m, const Vec *v, unsigned lane, uint64_t x)
{
    for (int b = 0; b < v->width; b++)
        lane_set(m, v->cell[b], lane, (uint8_t)((x >> b) & 1));
}

static uint64_t get(const LaneMachine *m, const Vec *v, unsigned lane)
{
    uint64_t x = 0;
    for (int b = 0; b < v->width; b++)
        x |= (uint64_t)lane_get(m, v->cell[b], lane) << b;
    return x;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* ---- the demos: set random inputs, run one pass, check against plain C -------------------- */

static int demo_any(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec x, r;
    vec_of(p, 0, "x", &x);
    vec_of(p, 1, "r", &r);
    int any = 0;
    for (unsigned k = 0; k < LANES; k++) {
        uint64_t v = rnd(s) % 100 == 0;
        any |= (int)v;
        put(m, &x, k, v);
    }
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &r, k) != (uint64_t)any)
            return 0;
    return 1;
}

static int demo_rank(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec x, pv, t;
    vec_of(p, 0, "x", &x);
    vec_of(p, 1, "p", &pv);
    vec_of(p, 1, "t", &t);
    uint64_t want[LANES], count = 0;
    for (unsigned k = 0; k < LANES; k++) {
        uint64_t v = rnd(s) % 10 < 3;
        put(m, &x, k, v);
        want[k] = count;
        count += v;
    }
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &pv, k) != want[k] || get(m, &t, k) != count)
            return 0;
    return 1;
}

static int demo_max(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec v, mx;
    vec_of(p, 0, "v", &v);
    vec_of(p, 1, "m", &mx);
    uint64_t hi = 0;
    for (unsigned k = 0; k < LANES; k++) {
        uint64_t x = rnd(s) & 255;
        hi = x > hi ? x : hi;
        put(m, &v, k, x);
    }
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &mx, k) != hi)
            return 0;
    return 1;
}

static int demo_sort(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec v;
    vec_of(p, 0, "v", &v);
    uint64_t want[LANES];
    for (unsigned k = 0; k < LANES; k++)
        put(m, &v, k, want[k] = rnd(s) & 255);
    qsort(want, LANES, sizeof want[0], cmp_u64);
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &v, k) != want[k])
            return 0;
    return 1;
}

static int demo_pack(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec v, h;
    vec_of(p, 0, "v", &v);
    vec_of(p, 0, "h", &h);
    uint64_t hits[LANES];
    unsigned n = 0;
    for (unsigned k = 0; k < LANES; k++) {
        uint64_t x = rnd(s) & 255, hit = rnd(s) % 10 == 0;
        put(m, &v, k, x);
        put(m, &h, k, hit);
        if (hit)
            hits[n++] = x;
    }
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &h, k) != (k < n) || (k < n && get(m, &v, k) != hits[k]))
            return 0;
    return 1;
}

static int demo_perm(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec v, c;
    vec_of(p, 0, "v", &v);
    vec_of(p, 0, "c", &c);
    uint16_t dest[LANES];
    uint64_t start[LANES];
    static uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE];
    for (unsigned k = 0; k < LANES; k++)
        dest[k] = (uint16_t)k;
    for (unsigned k = LANES - 1; k > 0; k--) {  /* a random permutation */
        unsigned j = (unsigned)(rnd(s) % (k + 1));
        uint16_t t = dest[k];
        dest[k] = dest[j];
        dest[j] = t;
    }
    benes_route(dest, ctrl);
    for (unsigned k = 0; k < LANES; k++) {
        uint64_t bits = 0;
        for (int r = 0; r < BENES_ROUNDS; r++)
            bits |= (uint64_t)ctrl[r][k] << r;
        put(m, &c, k, bits);
        put(m, &v, k, start[k] = rnd(s) & 255);
    }
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &v, dest[k]) != start[k])
            return 0;
    return 1;
}

static int parity(unsigned x)
{
    int p = 0;
    for (; x; x &= x - 1)
        p ^= 1;
    return p;
}

static int demo_anf(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec f;
    vec_of(p, 0, "f", &f);
    uint64_t a[LANES];
    for (unsigned k = 0; k < LANES; k++)
        put(m, &f, k, a[k] = rnd(s) & 1);
    for (unsigned d = 1; d < LANES; d <<= 1)  /* the Möbius transform in plain C */
        for (unsigned k = 0; k < LANES; k++)
            if (k & d)
                a[k] ^= a[k ^ d];
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &f, k) != a[k])
            return 0;
    return 1;
}

static int demo_walsh(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec f, w;
    vec_of(p, 0, "f", &f);
    vec_of(p, 1, "w", &w);
    uint64_t x[LANES];
    for (unsigned k = 0; k < LANES; k++)
        put(m, &f, k, x[k] = rnd(s) & 1);
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++) {
        int sum = 0;
        for (unsigned j = 0; j < LANES; j++)
            sum += ((int)x[j] ^ parity(j & k)) ? -1 : 1;
        if (get(m, &w, k) != ((uint64_t)(int64_t)sum & ((1u << w.width) - 1)))
            return 0;
    }
    return 1;
}

static int demo_xorconv(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec f, g, h;
    vec_of(p, 0, "f", &f);
    vec_of(p, 0, "g", &g);
    vec_of(p, 1, "h", &h);
    uint64_t x[LANES], y[LANES];
    for (unsigned k = 0; k < LANES; k++) {
        put(m, &f, k, x[k] = rnd(s) & 1);
        put(m, &g, k, y[k] = rnd(s) & 1);
    }
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++) {
        uint64_t n = 0;
        for (unsigned j = 0; j < LANES; j++)
            n += x[j] & y[j ^ k];
        if (get(m, &h, k) != n)
            return 0;
    }
    return 1;
}

static int demo_polymul(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    static uint16_t zf[CUBE_DIMS][CUBE_SIZE], zi[CUBE_DIMS][CUBE_SIZE];
    static int have;
    if (!have) {
        ntt_twiddles(zf, zi);
        have = 1;
    }
    Vec a, b, c, z;
    vec_of(p, 0, "a", &a);
    vec_of(p, 0, "b", &b);
    vec_of(p, 1, "c", &c);
    for (int d = 0; d < CUBE_DIMS; d++)
        for (int inv = 0; inv < 2; inv++) {
            char name[16];
            snprintf(name, sizeof name, "%s%d_", inv ? "zi" : "zf", d);
            vec_of(p, 0, name, &z);
            for (unsigned k = 0; k < LANES; k++)
                put(m, &z, k, inv ? zi[d][k] : zf[d][k]);
        }
    uint16_t x[LANES], y[LANES], want[LANES];
    for (unsigned k = 0; k < LANES; k++) {
        put(m, &a, k, x[k] = (uint16_t)(rnd(s) % NTT_Q));
        put(m, &b, k, y[k] = (uint16_t)(rnd(s) % NTT_Q));
    }
    polymul_scalar(x, y, want);
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &c, k) != want[k])
            return 0;
    return 1;
}

static int demo_ring(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    Vec x;
    vec_of(p, 0, "x", &x);
    uint64_t v[LANES], want[LANES];
    for (unsigned k = 0; k < LANES; k++)
        put(m, &x, k, v[k] = rnd(s) & 1);
    for (unsigned i = 0; i < LANES; i++) {  /* Rule 110 on the ring, machine grid_machine(0, i) */
        unsigned l = grid_machine(0, (i + LANES - 1) % LANES, CUBE_DIMS),
                 c = grid_machine(0, i, CUBE_DIMS),
                 r = grid_machine(0, (i + 1) % LANES, CUBE_DIMS);
        want[c] = (110 >> ((v[l] << 2) | (v[c] << 1) | v[r])) & 1;
    }
    jit_tick(jc, m);
    for (unsigned k = 0; k < LANES; k++)
        if (get(m, &x, k) != want[k])
            return 0;
    return 1;
}

enum { TILE = 32, TILE_RBITS = 4 };  /* 16 x 32 tiles of 32 x 32: one 512 x 1024 board */

static void gen_life_board(Program *p)
{
    static uint32_t board[TILE * TILE];
    gen_life_cube(p, TILE, TILE, TILE_RBITS, CUBE_DIMS - TILE_RBITS, board);
}

static int demo_life(const Program *p, const JitCode *jc, LaneMachine *m, uint64_t *s)
{
    enum { GW = TILE << (CUBE_DIMS - TILE_RBITS), GH = TILE << TILE_RBITS };
    static uint32_t cell[TILE * TILE];
    static uint8_t big[GH * GW];
    for (int i = 0; i < TILE * TILE; i++) {
        char name[16];
        snprintf(name, sizeof name, "x%d_%d", i / TILE, i % TILE);
        if (!program_find_cell(p, name, &cell[i]))
            return 0;
    }
    for (int R = 0; R < GH; R++)
        for (int C = 0; C < GW; C++)
            lane_set(m, cell[(R % TILE) * TILE + C % TILE],
                     grid_machine((unsigned)(R / TILE), (unsigned)(C / TILE), CUBE_DIMS - TILE_RBITS),
                     big[R * GW + C] = (uint8_t)(rnd(s) % 3 == 0));
    life_scalar(big, GW, GH);
    jit_tick(jc, m);
    for (int R = 0; R < GH; R++)
        for (int C = 0; C < GW; C++)
            if (lane_get(m, cell[(R % TILE) * TILE + C % TILE],
                         grid_machine((unsigned)(R / TILE), (unsigned)(C / TILE), CUBE_DIMS - TILE_RBITS)) !=
                big[R * GW + C])
                return 0;
    return 1;
}

static void gen_walsh(Program *p)
{
    gen_wht(p, 12);
}

typedef struct {
    const char *name, *what, *body;  /* the program: body with the libraries, or made by gen */
    int (*run)(const Program *, const JitCode *, LaneMachine *, uint64_t *);
    void (*gen)(Program *);
} Demo;

static const Demo demos[] = {
    {"any", "OR of one bit over all 512 machines", ".in x\n.out r\nALL_OR x r", demo_any},
    {"rank", "running count of 512 bits, 10-bit results",
     ".in x\n.out p0 p1 p2 p3 p4 p5 p6 p7 p8 p9 t0 t1 t2 t3 t4 t5 t6 t7 t8 t9\nPCOUNT x p t",
     demo_rank},
    {"max", "largest of 512 bytes", ".in v0 v1 v2 v3 v4 v5 v6 v7\n.out m0 m1 m2 m3 m4 m5 m6 m7\n"
                                    "MAX8 v m", demo_max},
    {"sort", "sort 512 bytes (bitonic, 45 rounds)",
     ".in v0 v1 v2 v3 v4 v5 v6 v7\n.out v0 v1 v2 v3 v4 v5 v6 v7\nSORT8 v", demo_sort},
    {"pack", "gather the bytes of the machines with a hit",
     ".in v0 v1 v2 v3 v4 v5 v6 v7 h\n.out v0 v1 v2 v3 v4 v5 v6 v7 h\nPCOUNT h p t\nPACK8 v h p",
     demo_pack},
    {"perm", "any permutation of 512 bytes (Beneš, 17 rounds)",
     ".in v0 v1 v2 v3 v4 v5 v6 v7 c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11 c12 c13 c14 c15 c16\n"
     ".out v0 v1 v2 v3 v4 v5 v6 v7\nPERM8 v c",
     demo_perm},
    {"anf", "truth table of 9 inputs -> normal form (Möbius)", ".in f\n.out f\nMOBIUS f", demo_anf},
    {"walsh", "Walsh-Hadamard spectrum of f, 12-bit results", NULL, demo_walsh, gen_walsh},
    {"xorconv", "h(k) = #x: f(x) = g(x^k) = 1, by spectra", NULL, demo_xorconv, gen_xorconv},
    {"polymul", "c = a * b in Z_12289[x]/(x^512 + 1), by NTT", NULL, demo_polymul, gen_polymul},
    {"ring", "Rule 110 on a ring of 512 cells, one step",
     ".in x\n.out x\nRING_MASKS\nRING_PREV x left\nRING_NEXT x right\nLD x\nAND left\n"
     "AND right\nSTO all3\nLD x\nOR right\nANDC all3\nSTO x",
     demo_ring},
    {"life", "Life: one 512 x 1024 board, one generation", NULL, demo_life, gen_life_board},
};

/* lib/ next to the current folder, or next to the program's own folder */
static void lib_dir(const char *argv0, char *buf, size_t len)
{
    FILE *f = fopen("lib/cube.inc", "r");
    if (f) {
        fclose(f);
        snprintf(buf, len, "lib");
        return;
    }
    const char *slash = NULL;
    for (const char *c = argv0; *c; c++)
        if (*c == '/' || *c == '\\')
            slash = c;
    if (slash)
        snprintf(buf, len, "%.*s/../lib", (int)(slash - argv0), argv0);
    else
        snprintf(buf, len, "../lib");
}

/* ---- more than 512 machines (step 7f): alpha cube --dims D -------------------------------- */

static void cput(LaneCube *c, const Vec *v, unsigned k, uint64_t x)
{
    for (int b = 0; b < v->width; b++)
        lane_cube_set(c, v->cell[b], k, (uint8_t)((x >> b) & 1));
}

static uint64_t cget(const LaneCube *c, const Vec *v, unsigned k)
{
    uint64_t x = 0;
    for (int b = 0; b < v->width; b++)
        x |= (uint64_t)lane_cube_get(c, v->cell[b], k) << b;
    return x;
}

static int big_any(const Program *p, const JitCode *jc, LaneCube *c, uint64_t *s)
{
    Vec x, r;
    vec_of(p, 0, "x", &x);
    vec_of(p, 1, "r", &r);
    unsigned n = c->groups * LANES, one = (unsigned)(rnd(s) % (2 * n));  /* sometimes none */
    for (unsigned k = 0; k < n; k++)
        cput(c, &x, k, k == one);
    jit_cube_tick(jc, c);
    for (unsigned k = 0; k < n; k++)
        if (cget(c, &r, k) != (one < n))
            return 0;
    return 1;
}

static int big_rank(const Program *p, const JitCode *jc, LaneCube *c, uint64_t *s)
{
    Vec x, pv, t;
    vec_of(p, 0, "x", &x);
    vec_of(p, 1, "p", &pv);
    vec_of(p, 1, "t", &t);
    unsigned n = c->groups * LANES;
    uint64_t *want = malloc(n * sizeof *want), count = 0;
    for (unsigned k = 0; k < n; k++) {
        uint64_t v = rnd(s) % 10 < 3;
        cput(c, &x, k, v);
        want[k] = count;
        count += v;
    }
    jit_cube_tick(jc, c);
    int ok = 1;
    for (unsigned k = 0; k < n && ok; k++)
        ok = cget(c, &pv, k) == want[k] && cget(c, &t, k) == count;
    free(want);
    return ok;
}

static int big_sort(const Program *p, const JitCode *jc, LaneCube *c, uint64_t *s)
{
    Vec v;
    vec_of(p, 0, "v", &v);
    unsigned n = c->groups * LANES;
    uint64_t *want = malloc(n * sizeof *want);
    for (unsigned k = 0; k < n; k++)
        cput(c, &v, k, want[k] = rnd(s) & 255);
    qsort(want, n, sizeof want[0], cmp_u64);
    jit_cube_tick(jc, c);
    int ok = 1;
    for (unsigned k = 0; k < n && ok; k++)
        ok = cget(c, &v, k) == want[k];
    free(want);
    return ok;
}

enum { BIG_TILE = 32 };

static int big_life(const Program *p, const JitCode *jc, LaneCube *c, uint64_t *s)
{
    int dims = (int)c->dims, rbits = dims / 2, cbits = dims - rbits;
    int gw = BIG_TILE << cbits, gh = BIG_TILE << rbits;
    static uint32_t cell[BIG_TILE * BIG_TILE];
    for (int i = 0; i < BIG_TILE * BIG_TILE; i++) {
        char name[16];
        snprintf(name, sizeof name, "x%d_%d", i / BIG_TILE, i % BIG_TILE);
        if (!program_find_cell(p, name, &cell[i]))
            return 0;
    }
    uint8_t *big = malloc((size_t)gw * (size_t)gh);
    for (int R = 0; R < gh; R++)
        for (int C = 0; C < gw; C++)
            lane_cube_set(c, cell[(R % BIG_TILE) * BIG_TILE + C % BIG_TILE],
                          grid_machine((unsigned)(R / BIG_TILE), (unsigned)(C / BIG_TILE), cbits),
                          big[(size_t)R * gw + C] = (uint8_t)(rnd(s) % 3 == 0));
    life_scalar(big, gw, gh);
    jit_cube_tick(jc, c);
    int ok = 1;
    for (int R = 0; R < gh && ok; R++)
        for (int C = 0; C < gw && ok; C++)
            ok = lane_cube_get(c, cell[(R % BIG_TILE) * BIG_TILE + C % BIG_TILE],
                               grid_machine((unsigned)(R / BIG_TILE), (unsigned)(C / BIG_TILE),
                                            cbits)) == big[(size_t)R * gw + C];
    free(big);
    return ok;
}

/* The program of each demo for 2^dims machines: assembly into src, or built into p. */
static int big_program(const char *name, int dims, const char *lib, Program *p, char *what,
                       size_t wlen)
{
    size_t cap = 1 << 16, len = 0;
    char *src = malloc(cap), err[512];
    unsigned n = 1u << dims;
    src[0] = '\0';
#define ADD(...) (len += (size_t)snprintf(src + len, cap - len, __VA_ARGS__))
    ADD(".include \"%s/std.inc\"\n.include \"%s/cube.inc\"\n", lib, lib);
    if (!strcmp(name, "any")) {
        snprintf(what, wlen, "OR of one bit over all %u machines", n);
        ADD(".in x\n.out r\nMOV r x\n");
        for (int d = 0; d < dims; d++)
            ADD("OR_DIM r %d\n", d);
    } else if (!strcmp(name, "sort")) {
        snprintf(what, wlen, "sort %u bytes (bitonic, %d rounds)", n, dims * (dims + 1) / 2);
        ADD(".in v0 v1 v2 v3 v4 v5 v6 v7\n.out v0 v1 v2 v3 v4 v5 v6 v7\n");
        for (int st = 1; st <= dims; st++)
            for (int j = st - 1; j >= 0; j--) {
                char dir[8];
                snprintf(dir, sizeof dir, st < dims ? "ID%d" : "ZERO", st);
                ADD("%s v %d %s\n", j < CUBE_DIMS ? "CMPX8" : "CMPX8_FAR", j, dir);
            }
    } else if (!strcmp(name, "rank")) {
        snprintf(what, wlen, "running count of %u bits, %d-bit results", n, dims + 1);
        gen_rank(p, dims);
        free(src);
        return 1;
    } else if (!strcmp(name, "life")) {
        snprintf(what, wlen, "Life: one %d x %d board, one generation", BIG_TILE << (dims / 2),
                 BIG_TILE << (dims - dims / 2));
        static uint32_t board[BIG_TILE * BIG_TILE];
        gen_life_cube(p, BIG_TILE, BIG_TILE, dims / 2, dims - dims / 2, board);
        free(src);
        return 1;
    }
#undef ADD
    int ok = asm_string(src, name, ".", p, err, sizeof err);
    if (!ok)
        fprintf(stderr, "%s\n", err);
    free(src);
    return ok;
}

static const struct {
    const char *name;
    int (*run)(const Program *, const JitCode *, LaneCube *, uint64_t *);
} big_demos[] = {{"any", big_any}, {"rank", big_rank}, {"sort", big_sort}, {"life", big_life}};

static int cube_big(const char *lib, int dims, const char *only, int rounds, int threads,
                    int idle, double seconds)
{
    static unsigned char excluded[1024];
    if (!threads)
        threads = cpu_cores(excluded);  /* lockstep: leave the second hardware threads free */
    int *ids = malloc(1024 * sizeof *ids), nids = cpu_pick(ids, 1024, excluded, threads), bad = 0;
    printf("the cube: %u machines in %u groups of 512, wired across %d dimensions, in lockstep\n",
           1u << dims, 1u << (dims - CUBE_DIMS), dims);
    printf("each demo: checked against plain C in all machines, then timed on 1 thread and on %d "
           "threads\nrunning one cube together (a barrier after each segment of the pass)\n\n",
           nids ? nids : 1);
    printf("%-5s %-44s %7s %4s %6s %12s %13s %9s\n", "demo", "what one pass does", "instr", "segs",
           "check", "1 thread", "all threads", "1-bit ops");
    for (size_t d = 0; d < sizeof big_demos / sizeof big_demos[0]; d++) {
        if (only && strcmp(only, big_demos[d].name))
            continue;
        Program p;
        JitCode jc;
        LaneCube c;
        char what[96], err[512];
        if (!big_program(big_demos[d].name, dims, lib, &p, what, sizeof what))
            return 1;
        if (!jit_compile(&p, 0, &jc, NULL, err, sizeof err) || !lane_cube_init(&c, &p)) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        uint64_t s = 42 + d;
        int ok = 1, n = !strcmp(big_demos[d].name, "life") && rounds > 2 ? 2 : rounds;
        for (int r = 0; r < n && ok; r++) {
            lane_cube_reset(&c);
            ok = big_demos[d].run(&p, &jc, &c, &s);
        }
        bad += !ok;

        uint64_t passes = 1;
        double t1;
        for (;;) {  /* one thread: the whole cube, segment by segment */
            double t0 = now_seconds();
            jit_cube_run(&jc, &c, passes);
            t1 = now_seconds() - t0;
            if (t1 > 0.2)
                break;
            passes *= 2;
        }
        double one = t1 / (double)passes;
        uint64_t m = 1;
        double tm;
        for (;;) {
            tm = par_cube_run(&jc, &c, nids ? nids : 1, nids ? ids : NULL, idle, m, NULL);
            if (tm > seconds / 2 || m > (1ull << 40))
                break;
            m *= 2;
        }
        double rate = (double)m / tm;
        char t1s[32], tms[32];
        if (one < 1e-3)
            snprintf(t1s, sizeof t1s, "%.1f us", one * 1e6);
        else
            snprintf(t1s, sizeof t1s, "%.2f ms", one * 1e3);
        snprintf(tms, sizeof tms, "%.3g /s", rate);
        printf("%-5s %-44s %7zu %4u %6s %12s %13s %7.1f T\n", big_demos[d].name, what, p.len,
               jc.nseg, ok ? "OK" : "WRONG", t1s, tms,
               rate * (double)p.len * (double)(1u << dims) / 1e12);
        fflush(stdout);
        lane_cube_free(&c);
        jit_free(&jc);
        program_free(&p);
    }
    printf("\n1 thread: one pass of all %u machines. all threads: passes per second of the same "
           "cube.\nsegs: the pass is cut where machines in different groups would see each "
           "other out of step.\n", 1u << dims);
    free(ids);
    return bad ? 3 : 0;
}

int cmd_cube(const char *argv0, int argc, char **argv)
{
    double seconds = 1;
    int threads = 0, idle = 0, rounds = 20, dims = CUBE_DIMS;
    const char *only = NULL;
    static unsigned char excluded[1024];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle"))
            idle = 1;
        else if (!strcmp(argv[i], "--checks") && i + 1 < argc)
            rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dims") && i + 1 < argc)
            dims = atoi(argv[++i]);
        else if (argv[i][0] != '-' && !only)
            only = argv[i];
        else {
            fprintf(stderr, "usage: alpha cube [DEMO] [--dims D] [--checks N] [--seconds S] "
                            "[--threads N] [--idle]\n"
                            "demos: any, rank, max, sort, pack, perm, anf, walsh, xorconv, polymul, "
                            "ring, life\n"
                            "--dims 10 .. 16: 2^D machines, demos any, rank, sort, life\n");
            return 2;
        }
    }
    if (dims < CUBE_DIMS || dims > CUBE_MAX_DIMS) {
        fprintf(stderr, "--dims must be %d to %d\n", CUBE_DIMS, CUBE_MAX_DIMS);
        return 2;
    }
    if (!cpu_has_avx2()) {
        fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
        return 1;
    }
    char lib[512];
    lib_dir(argv0, lib, sizeof lib);
    if (dims > CUBE_DIMS)
        return cube_big(lib, dims, only, rounds, threads, idle, seconds);
    int *ids = malloc(1024 * sizeof *ids), nids = cpu_pick(ids, 1024, excluded, threads), bad = 0;
    printf("the cube: 512 machines in lockstep, every one a NAND/STO machine (lib/cube.inc)\n");
    printf("each demo: %d random inputs checked against plain C in all 512 machines, then timed\n\n",
           rounds);
    printf("%-7s %-48s %8s %6s %12s %14s %10s\n", "demo", "what one pass does", "instr", "check",
           "1 thread", "all threads", "1-bit ops");
    for (size_t d = 0; d < sizeof demos / sizeof demos[0]; d++) {
        const Demo *dm = &demos[d];
        if (only && strcmp(only, dm->name))
            continue;
        char src[1024], err[512];
        snprintf(src, sizeof src, ".include \"%s/std.inc\"\n.include \"%s/cube.inc\"\n%s\n", lib, lib,
                 dm->body);
        Program p;
        JitCode jc;
        if (dm->gen)
            dm->gen(&p);
        if ((!dm->gen && !asm_string(src, dm->name, ".", &p, err, sizeof err)) ||
            !jit_compile(&p, 0, &jc, NULL, err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        LaneMachine m;
        lane_init(&m, &p);
        uint64_t s = 42 + d;
        int ok = 1;
        for (int r = 0; r < rounds && ok; r++) {
            lane_reset(&m);
            ok = dm->run(&p, &jc, &m, &s);
        }
        bad += !ok;

        uint64_t n = 1;  /* one thread: double the passes until it takes 0.2 s */
        double t;
        for (;;) {
            double t0 = now_seconds();
            jit_run(&jc, &m, n);
            t = now_seconds() - t0;
            if (t > 0.2)
                break;
            n *= 2;
        }
        ParResult pr;
        par_measure(&jc, nids ? nids : 1, nids ? ids : NULL, 1, idle, seconds, &pr);
        double rate = (double)pr.passes / pr.seconds;
        char one[32], all[32];
        double ns = t / (double)n * 1e9;
        if (ns < 1000)
            snprintf(one, sizeof one, "%.1f ns", ns);
        else
            snprintf(one, sizeof one, "%.2f us", ns / 1000);
        snprintf(all, sizeof all, "%.3g /s", rate);
        printf("%-7s %-48s %8zu %6s %12s %14s %8.1f T\n", dm->name, dm->what, p.len,
               ok ? "OK" : "WRONG", one, all, rate * (double)p.len * LANES / 1e12);
        fflush(stdout);
        lane_free(&m);
        jit_free(&jc);
        program_free(&p);
    }
    printf("\n1 thread: one pass, all 512 machines. all threads (%d): passes per second, each its own "
           "512 machines.\n", nids ? nids : 1);
    free(ids);
    return bad ? 3 : 0;
}
