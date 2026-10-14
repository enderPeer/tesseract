/* Tests for the workloads: gates, registers, netlists, specs, the enumerator (verify, equiv,
   search), the toy hash and the Game of Life, each against plain C. Run from the project root. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "enum.h"
#include "gen.h"
#include "jit.h"
#include "lanes.h"
#include "net.h"
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

static int gate_value(int type, const int *v, int n)
{
    int and_ = 1, or_ = 0, x = 0;
    for (int i = 0; i < n; i++) {
        and_ &= v[i];
        or_ |= v[i];
        x ^= v[i];
    }
    switch (type) {
    case G_BUF:  return v[0];
    case G_NOT:  return !v[0];
    case G_AND:  return and_;
    case G_NAND: return !and_;
    case G_OR:   return or_;
    case G_NOR:  return !or_;
    case G_XOR:  return x;
    default:     return !x;
    }
}

/* Every gate type with 1 to 5 inputs, every input combination, on the reference machine; also
   with the output written over the first input. */
static void test_gates(void)
{
    for (int type = 0; type < G_COUNT; type++) {
        for (int n = 1; n <= 5; n++) {
            if ((type == G_BUF || type == G_NOT) && n > 1)
                continue;
            for (int self = 0; self < 2; self++) {
                Builder *b = b_new();
                uint32_t in[5], out;
                for (int i = 0; i < n; i++) {
                    char name[8];
                    snprintf(name, sizeof name, "i%d", i);
                    b_input(b, in[i] = b_cell(b, name));
                }
                out = self ? in[0] : b_cell(b, "o");
                b_gate(b, type, out, in, n);
                Program p;
                b_finish(b, &p);
                RefMachine m;
                ref_init(&m, &p);
                int bad = 0;
                for (int x = 0; x < (1 << n); x++) {
                    int v[5];
                    ref_reset(&m);
                    for (int i = 0; i < n; i++)
                        ref_set(&m, in[i], (uint8_t)(v[i] = (x >> i) & 1));
                    ref_tick(&m);
                    bad += ref_get(&m, out) != gate_value(type, v, n);
                }
                CHECK(bad == 0, "%s with %d inputs%s: %d wrong", gate_name(type), n,
                      self ? ", output over input 0" : "", bad);
                ref_free(&m);
                program_free(&p);
            }
        }
    }
}

/* A shift register moves one place per pass; two registers that read each other swap. */
static void test_registers(void)
{
    Program p;
    char err[256];
    int ok = net_parse("INPUT(d)\nOUTPUT(q3)\nq1 = DFF(d)\nq2 = DFF(q1)\nq3 = DFF(q2)\n"
                       "s = DFF(t)\nt = DFF(s)\n",
                       "<shift>", &p, err, sizeof err);
    CHECK(ok, "shift register: %s", err);
    if (!ok)
        return;
    RefMachine m;
    ref_init(&m, &p);
    uint32_t d, q1, q2, q3, s, t;
    program_find_cell(&p, "d", &d);
    program_find_cell(&p, "q1", &q1);
    program_find_cell(&p, "q2", &q2);
    program_find_cell(&p, "q3", &q3);
    program_find_cell(&p, "s", &s);
    program_find_cell(&p, "t", &t);
    ref_set(&m, s, 1);
    int seq[] = {1, 0, 1, 1, 0, 0, 1, 0}, bad = 0;
    for (int k = 0; k < 8; k++) {
        ref_set(&m, d, (uint8_t)seq[k]);
        ref_tick(&m);
        bad += ref_get(&m, q1) != seq[k];
        bad += k >= 1 && ref_get(&m, q2) != seq[k - 1];
        bad += k >= 2 && ref_get(&m, q3) != seq[k - 2];
        bad += ref_get(&m, s) != (k % 2) || ref_get(&m, t) != !(k % 2);
    }
    CHECK(bad == 0, "registers update all at once (%d wrong)", bad);
    ref_free(&m);
    program_free(&p);
}

static int c17(int x, int *o22)
{
    int n1 = (x >> 4) & 1, n2 = (x >> 3) & 1, n3 = (x >> 2) & 1, n6 = (x >> 1) & 1, n7 = x & 1;
    int g10 = !(n1 && n3), g11 = !(n3 && n6), g16 = !(n2 && g11), g19 = !(g11 && n7);
    *o22 = !(g10 && g16);
    return !(g16 && g19);
}

static void test_netlists(void)
{
    /* c17 with its gates in reverse order: the importer sorts them */
    const char *c17r = "INPUT(1)\nINPUT(2)\nINPUT(3)\nINPUT(6)\nINPUT(7)\nOUTPUT(22)\nOUTPUT(23)\n"
                       "23 = NAND(16, 19)\n22 = NAND(10, 16)\n19 = NAND(11, 7)\n"
                       "16 = NAND(2, 11)\n11 = NAND(3, 6)\n10 = NAND(1, 3)  # last\n";
    Program p;
    char err[256];
    int ok = net_parse(c17r, "<c17>", &p, err, sizeof err);
    CHECK(ok, "c17: %s", err);
    if (ok) {
        RefMachine m;
        ref_init(&m, &p);
        int bad = 0;
        for (int x = 0; x < 32; x++) {
            ref_reset(&m);
            for (int k = 0; k < 5; k++)
                ref_set(&m, p.inputs[k], (uint8_t)((x >> (4 - k)) & 1));
            ref_tick(&m);
            int o22, o23 = c17(x, &o22);
            bad += ref_get(&m, p.outputs[0]) != o22 || ref_get(&m, p.outputs[1]) != o23;
        }
        CHECK(bad == 0, "c17 against plain C: %d of 32 wrong", bad);
        ref_free(&m);
        program_free(&p);
    }

    static const struct {
        const char *src, *expect;
    } errors[] = {
        {"INPUT(a)\nOUTPUT(z)\nz = AND(a, b)\n", "b is used but never driven"},
        {"INPUT(a)\nz = NOT(a)\nz = BUF(a)\n", "driven twice"},
        {"INPUT(a)\nx = AND(a, y)\ny = OR(a, x)\n", "loop without a DFF"},
        {"INPUT(a)\nz = MUX(a, a)\n", "unknown gate MUX"},
        {"INPUT(a)\nz = NOT(a, a)\n", "takes one input"},
        {"INPUT a\n", "expected INPUT(name)"},
        {"INPUT(a)\nINPUT(A)\n", "both become cell"},
    };
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
        ok = net_parse(errors[i].src, "<net>", &p, err, sizeof err);
        CHECK(!ok && strstr(err, errors[i].expect), "netlist error %zu: got \"%s\"", i,
              ok ? "(no error)" : err);
        if (ok)
            program_free(&p);
    }
    ok = net_parse("INPUT(a)\nx = AND(a, y)\ny = DFF(x)\nOUTPUT(y)\n", "<net>", &p, err, sizeof err);
    CHECK(ok, "a loop through a DFF is fine: %s", err);
    if (ok)
        program_free(&p);
}

static void test_spec(void)
{
    Program p;
    char err[256];
    if (!load_program("examples/adder8.asm", &p, err, sizeof err)) {
        CHECK(0, "%s", err);
        return;
    }
    static const struct {
        const char *text;
        uint64_t    x, want;  /* x: a7..a0 b7..b0 c, a in the high bits */
    } cases[] = {
        {"c:s = a + b + c", (200ull << 9) | (100ull << 1) | 1, 301},
        {"s = a * 2 + b", (3ull << 9) | (4ull << 1), 10},
        {"s = (a + b) * 2", (3ull << 9) | (4ull << 1), 14},
        {"s = a - b", (3ull << 9) | (4ull << 1), 255},
        {"s = ~a & 0xf0 | b ^ 1", (0x0full << 9) | (0x02ull << 1), 0xf3},
        {"s = 1 << 3 >> 1", 0, 4},
        {"c = -c", 1, 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Spec *s = spec_parse(cases[i].text, &p, 1, err, sizeof err);
        CHECK(s && spec_value(s, cases[i].x) == cases[i].want, "spec \"%s\": %s %llu",
              cases[i].text, s ? "got" : err, s ? (unsigned long long)spec_value(s, cases[i].x) : 0);
        spec_free(s);
    }
    static const struct {
        const char *text, *expect;
        int inputs;
    } bad[] = {
        {"q = a", "no output called q", 1},
        {"s = z", "no input called z", 1},
        {"s = a +", "ends too early", 1},
        {"s = (a", "expected )", 1},
        {"s a", "expected =", 1},
        {"s = a", "must be a number", 0},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Spec *s = spec_parse(bad[i].text, &p, bad[i].inputs, err, sizeof err);
        CHECK(!s && strstr(err, bad[i].expect), "spec \"%s\" should fail with %s, got %s",
              bad[i].text, bad[i].expect, s ? "(no error)" : err);
        spec_free(s);
    }
    program_free(&p);
}

static Enum en(int mode, const Program *a, const Program *b, const Spec *s)
{
    Enum e;
    memset(&e, 0, sizeof e);
    e.mode = mode;
    e.a = a;
    e.b = b;
    e.spec = s;
    e.ticks = 1;
    e.parts = 1;
    return e;
}

static void test_enum(void)
{
    Program add, mul, x1, x2;
    char err[256];
    EnumResult r;
    if (!load_program("examples/adder8.asm", &add, err, sizeof err)) {
        CHECK(0, "%s", err);
        return;
    }

    Spec *s = spec_parse("c:s = a + b + c", &add, 1, err, sizeof err);
    Enum e = en(EN_VERIFY, &add, NULL, s);
    CHECK(enum_run(&e, &r, err, sizeof err) && r.tried == 131072 && r.hits == 0,
          "adder8 = a + b + c: %llu tried, %llu wrong", (unsigned long long)r.tried,
          (unsigned long long)r.hits);
    spec_free(s);

    s = spec_parse("c:s = a + b", &add, 1, err, sizeof err);
    e = en(EN_VERIFY, &add, NULL, s);
    int ok = enum_run(&e, &r, err, sizeof err), sorted = r.nhit == EN_SHOW;
    for (int h = 0; h < r.nhit; h++)
        sorted &= r.hit[h] == 2 * (uint64_t)h + 1;  /* every odd x has c = 1 */
    CHECK(ok && r.hits == 65536 && sorted, "a + b misses the carry exactly when c = 1 (%llu)",
          (unsigned long long)r.hits);

    /* the same in three shares, as on the cluster */
    EnumResult sum;
    memset(&sum, 0, sizeof sum);
    for (int part = 0; part < 3; part++) {
        e.part = e.part_end = part;
        e.parts = 3;
        enum_run(&e, &r, err, sizeof err);
        sum.tried += r.tried;
        sum.hits += r.hits;
        for (int h = 0; h < r.nhit; h++)
            enum_add_hit(&sum, r.hit[h]);
    }
    CHECK(sum.tried == 131072 && sum.hits == 65536 && sum.nhit == EN_SHOW && sum.hit[0] == 1 &&
              sum.hit[EN_SHOW - 1] == 2 * EN_SHOW - 1,
          "three shares add up to the whole");
    e.part = 0;  /* shares 0-1 of 3, then share 2: uneven splits for machines of different speed */
    e.part_end = 1;
    enum_run(&e, &r, err, sizeof err);
    uint64_t tried = r.tried, hits = r.hits;
    e.part = e.part_end = 2;
    enum_run(&e, &r, err, sizeof err);
    CHECK(tried + r.tried == 131072 && hits + r.hits == 65536, "shares 0-1 and 2 of 3 add up");
    spec_free(s);

    for (int bits = 1; bits <= 7; bits++) {
        gen_mul(&mul, bits);
        s = spec_parse("p = a * b", &mul, 1, err, sizeof err);
        e = en(EN_VERIFY, &mul, NULL, s);
        CHECK(enum_run(&e, &r, err, sizeof err) && r.hits == 0 && r.tried == 1ull << (2 * bits),
              "%d-bit multiplier = a * b (%llu wrong)", bits, (unsigned long long)r.hits);
        spec_free(s);
        program_free(&mul);
    }

    /* equiv: the XOR example against one XOR gate, and against XNOR */
    load_program("examples/xor.asm", &x1, err, sizeof err);
    net_parse("INPUT(b)\nINPUT(a)\nOUTPUT(out)\nout = XOR(a, b)\n", "<xor>", &x2, err, sizeof err);
    e = en(EN_EQUIV, &x1, &x2, NULL);
    CHECK(enum_run(&e, &r, err, sizeof err) && r.hits == 0 && r.tried == 4, "xor.asm = XOR gate");
    program_free(&x2);
    net_parse("INPUT(a)\nINPUT(b)\nOUTPUT(out)\nout = XNOR(a, b)\n", "<xnor>", &x2, err, sizeof err);
    e = en(EN_EQUIV, &x1, &x2, NULL);
    CHECK(enum_run(&e, &r, err, sizeof err) && r.hits == 4, "xor.asm differs from XNOR everywhere");
    program_free(&x2);
    net_parse("INPUT(a)\nINPUT(b)\nOUTPUT(o)\no = XOR(a, b)\n", "<x>", &x2, err, sizeof err);
    e = en(EN_EQUIV, &x1, &x2, NULL);
    CHECK(!enum_run(&e, &r, err, sizeof err) && strstr(err, "no output called out"),
          "equiv needs the same names: %s", err);
    program_free(&x2);
    program_free(&x1);
    program_free(&add);
}

static void test_hash(void)
{
    char err[256];
    for (int bits = 3; bits <= 16; bits += 13) {
        Program p;
        CHECK(gen_hash(&p, bits, 5), "gen_hash %d bits", bits);
        RefMachine m;
        ref_init(&m, &p);
        int bad = 0;
        for (uint64_t x = 0; x < (1ull << bits); x++) {
            ref_reset(&m);
            for (int i = 0; i < bits; i++)
                ref_set(&m, p.inputs[i], (uint8_t)((x >> i) & 1));
            ref_tick(&m);
            uint64_t h = 0;
            for (int i = 0; i < bits; i++)
                h |= (uint64_t)ref_get(&m, p.outputs[i]) << i;
            bad += h != toyhash(x, bits, 5);
        }
        CHECK(bad == 0, "%d-bit hash program against plain C: %d wrong", bits, bad);
        ref_free(&m);

        /* search: every preimage of some value, against brute force in plain C */
        uint64_t target = toyhash(12345 % (1ull << bits), bits, 5), want = 0, first = ~0ull;
        for (uint64_t x = 0; x < (1ull << bits); x++)
            if (toyhash(x, bits, 5) == target) {
                want++;
                if (first == ~0ull)
                    first = x;
            }
        char text[64];
        snprintf(text, sizeof text, "h = %llu", (unsigned long long)target);
        Spec *s = spec_parse(text, &p, 0, err, sizeof err);
        Enum e = en(EN_SEARCH, &p, NULL, s);
        EnumResult r;
        enum_run(&e, &r, err, sizeof err);
        /* x's bits are reversed (x0 is input 0, the highest bit of the assignment) */
        uint64_t got_first = 0;
        for (int i = 0; i < bits; i++)
            got_first |= ((r.hit[0] >> (bits - 1 - i)) & 1) << i;
        int got_first_ok = 1;
        for (int h = 0; h < r.nhit; h++) {
            uint64_t x = 0;
            for (int i = 0; i < bits; i++)
                x |= ((r.hit[h] >> (bits - 1 - i)) & 1) << i;
            got_first_ok &= toyhash(x, bits, 5) == target;
        }
        CHECK(r.hits == want && r.nhit > 0 && got_first_ok,
              "%d bits: search found %llu preimages, plain C %llu", bits,
              (unsigned long long)r.hits, (unsigned long long)want);
        (void)got_first;
        spec_free(s);
        program_free(&p);
    }
}

static void test_life(void)
{
    static const int sizes[][2] = {{8, 8}, {10, 7}, {3, 3}};
    for (size_t k = 0; k < 3; k++) {
        int w = sizes[k][0], h = sizes[k][1], cells = w * h;
        uint32_t *board = malloc((size_t)cells * sizeof *board);
        Program p;
        gen_life(&p, w, h, board);
        JitCode jc;
        char err[128];
        jit_compile(&p, 0, &jc, NULL, err, sizeof err);
        LaneMachine m;
        lane_init(&m, &p);
        uint64_t s = 7 + k;
        for (int i = 0; i < cells; i++)
            for (int j = 0; j < 8; j++) {
                s = s * 6364136223846793005ull + 1442695040888963407ull;
                m.mem[board[i]].w[j] = s ^ (s >> 29);
            }
        uint8_t *b = malloc((size_t)LANES * (size_t)cells);
        for (int l = 0; l < LANES; l++)
            for (int i = 0; i < cells; i++)
                b[l * cells + i] = lane_get(&m, board[i], (unsigned)l);
        jit_run(&jc, &m, 20);
        int bad = 0;
        for (int l = 0; l < LANES; l++) {
            for (int g = 0; g < 20; g++)
                life_scalar(b + l * cells, w, h);
            for (int i = 0; i < cells; i++)
                bad += b[l * cells + i] != lane_get(&m, board[i], (unsigned)l);
        }
        CHECK(bad == 0, "Life %dx%d, 512 boards, 20 generations: %d cells wrong", w, h, bad);
        free(b);
        lane_free(&m);
        jit_free(&jc);
        program_free(&p);
        free(board);
    }
}

static void test_print(void)
{
    /* a generated program printed as assembly reads back into the same instructions */
    Program p, q;
    char err[256];
    gen_mul(&p, 3);
    FILE *f = fopen("build/print_test.asm", "w");
    program_print(f, &p);
    fclose(f);
    int ok = load_program("build/print_test.asm", &q, err, sizeof err);
    int same = ok && p.len == q.len && p.ninputs == q.ninputs && p.noutputs == q.noutputs;
    for (size_t i = 0; same && i < p.len; i++)
        same = p.code[i].op == q.code[i].op &&
               !strcmp(p.cell_names[p.code[i].addr], q.cell_names[q.code[i].addr]);
    CHECK(same, "printed program reads back the same: %s", ok ? "" : err);
    program_free(&p);
    if (ok)
        program_free(&q);
    remove("build/print_test.asm");
}

int main(void)
{
    test_gates();
    test_registers();
    test_netlists();
    test_spec();
    test_print();
    if (!cpu_has_avx2()) {
        printf("%d checks, %d failures (512-lane tests skipped: no AVX2)\n", checks, failures);
        return failures != 0;
    }
    test_enum();
    test_hash();
    test_life();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
