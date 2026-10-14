/* Tests for the assembler and the reference machine. Run from the project root (build test does). */
#include <stdio.h>
#include <string.h>

#include "asm.h"
#include "ref.h"

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

static int assemble(const char *src, Program *p)
{
    char err[512];
    if (asm_string(src, "<test>", ".", p, err, sizeof err))
        return 1;
    printf("FAIL assembling test source: %s\n", err);
    failures++;
    return 0;
}

static int load(const char *path, Program *p)
{
    char err[512];
    if (asm_file(path, p, err, sizeof err))
        return 1;
    printf("FAIL %s\n", err);
    failures++;
    return 0;
}

static uint32_t cell(const Program *p, const char *name)
{
    uint32_t a = 0;
    if (!program_find_cell(p, name, &a)) {
        printf("FAIL no cell named %s\n", name);
        failures++;
    }
    return a;
}

static void set(RefMachine *m, uint32_t addr, unsigned v)
{
    ref_set(m, addr, (uint8_t)(v & 1));
}

/* ---- the two instructions and the memory model -------------------------------------------- */

static void test_core(void)
{
    Program p;
    if (!assemble(".in x\nNAND x\nSTO y\n", &p))
        return;
    RefMachine m;
    ref_init(&m, &p);
    uint32_t x = cell(&p, "x"), y = cell(&p, "y");

    for (unsigned rr = 0; rr < 2; rr++) {
        for (unsigned v = 0; v < 2; v++) {
            ref_reset(&m);
            m.rr = (uint8_t)rr;
            set(&m, x, v);
            ref_step(&m);
            CHECK(m.rr == !(rr & v), "NAND with RR=%u x=%u gave %u", rr, v, m.rr);
            ref_step(&m);
            CHECK(ref_get(&m, y) == m.rr, "STO did not copy RR");
            CHECK(m.pc == 0 && m.ticks == 1 && m.steps == 2, "PC wraps after the last instruction");
        }
    }

    ref_reset(&m);
    CHECK(ref_get(&m, CELL_ZERO) == 0 && ref_get(&m, CELL_ONE) == 1, "constant cells after reset");
    CHECK(ref_get(&m, x) == 0 && ref_get(&m, y) == 0 && m.rr == 0, "power-on state is all zero");
    ref_set(&m, CELL_ZERO, 1);
    ref_set(&m, CELL_ONE, 0);
    CHECK(ref_get(&m, CELL_ZERO) == 0 && ref_get(&m, CELL_ONE) == 1, "host writes to constants are ignored");

    /* The assembler refuses STO to a constant; the machine must ignore it anyway. */
    Instr code[] = {{OP_NAND, CELL_ZERO}, {OP_STO, CELL_ZERO}, {OP_NAND, CELL_ONE}, {OP_STO, CELL_ONE}};
    Program raw = p;
    raw.code = code;
    raw.len = 4;
    RefMachine r;
    ref_init(&r, &raw);
    ref_tick(&r);
    CHECK(ref_get(&r, CELL_ZERO) == 0 && ref_get(&r, CELL_ONE) == 1, "STO to constant cells is ignored");
    ref_free(&r);

    ref_free(&m);
    program_free(&p);
}

/* ---- lib/std.inc: every macro against its truth table ------------------------------------- */

static void test_std(void)
{
    /* Expected RR after the macro: bit (2*RR + x) of tt. */
    static const struct {
        const char *src;
        unsigned    tt;
    } cases[] = {
        {"NAND x", 0x7}, {"SET", 0xF},    {"CLR", 0x0},     {"NOT", 0x3},    {"LD x", 0xA},
        {"LDC x", 0x5},  {"AND x", 0x8},  {"OR x", 0xE},    {"XOR x", 0x6},  {"XNOR x", 0x9},
        {"NOR x", 0x1},  {"ANDC x", 0x4}, {"ORC x", 0xD},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char src[256];
        snprintf(src, sizeof src, ".include \"lib/std.inc\"\n.in x\n%s\n", cases[i].src);
        Program p;
        if (!assemble(src, &p))
            continue;
        RefMachine m;
        ref_init(&m, &p);
        uint32_t x = cell(&p, "x");
        for (unsigned rr = 0; rr < 2; rr++) {
            for (unsigned v = 0; v < 2; v++) {
                ref_reset(&m);
                m.rr = (uint8_t)rr;
                set(&m, x, v);
                ref_tick(&m);
                unsigned want = (cases[i].tt >> (2 * rr + v)) & 1;
                CHECK(m.rr == want, "%s with RR=%u x=%u gave %u", cases[i].src, rr, v, m.rr);
                CHECK(ref_get(&m, x) == v, "%s changed its operand", cases[i].src);
            }
        }
        ref_free(&m);
        program_free(&p);
    }

    Program p;
    if (assemble(".include \"lib/std.inc\"\nSTOC y\n", &p)) {
        RefMachine m;
        ref_init(&m, &p);
        for (unsigned rr = 0; rr < 2; rr++) {
            ref_reset(&m);
            m.rr = (uint8_t)rr;
            ref_tick(&m);
            CHECK(ref_get(&m, cell(&p, "y")) == !rr && m.rr == rr, "STOC with RR=%u", rr);
        }
        ref_free(&m);
        program_free(&p);
    }
    if (assemble(".include \"lib/std.inc\"\n.in s\nMOV d s\n", &p)) {
        RefMachine m;
        ref_init(&m, &p);
        for (unsigned v = 0; v < 2; v++) {
            ref_reset(&m);
            set(&m, cell(&p, "s"), v);
            ref_tick(&m);
            CHECK(ref_get(&m, cell(&p, "d")) == v && m.rr == v, "MOV with s=%u", v);
        }
        ref_free(&m);
        program_free(&p);
    }
}

/* ---- examples, checked over every input --------------------------------------------------- */

static void test_examples(void)
{
    Program p;
    RefMachine m;

    if (load("examples/xor.asm", &p)) {
        ref_init(&m, &p);
        uint32_t a = cell(&p, "a"), b = cell(&p, "b"), out = cell(&p, "out");
        for (unsigned v = 0; v < 4; v++) {
            ref_reset(&m);
            set(&m, a, v >> 1);
            set(&m, b, v);
            ref_tick(&m);
            CHECK(ref_get(&m, out) == ((v >> 1) ^ (v & 1)), "xor.asm a=%u b=%u", v >> 1, v & 1);
        }
        CHECK(p.len == 8, "xor.asm should be 8 instructions, is %zu", p.len);
        ref_free(&m);
        program_free(&p);
    }

    if (load("examples/mux.asm", &p)) {
        ref_init(&m, &p);
        uint32_t sel = cell(&p, "sel"), x = cell(&p, "x"), y = cell(&p, "y"), out = cell(&p, "out");
        for (unsigned v = 0; v < 8; v++) {
            unsigned s = v >> 2, xv = (v >> 1) & 1, yv = v & 1;
            ref_reset(&m);
            set(&m, sel, s);
            set(&m, x, xv);
            set(&m, y, yv);
            ref_tick(&m);
            CHECK(ref_get(&m, out) == (s ? xv : yv), "mux.asm sel=%u x=%u y=%u", s, xv, yv);
        }
        ref_free(&m);
        program_free(&p);
    }

    if (load("examples/full_adder.asm", &p)) {
        ref_init(&m, &p);
        uint32_t a = cell(&p, "a"), b = cell(&p, "b"), c = cell(&p, "c"), s = cell(&p, "s");
        for (unsigned v = 0; v < 8; v++) {
            unsigned sum = (v >> 2) + ((v >> 1) & 1) + (v & 1);
            ref_reset(&m);
            set(&m, a, v >> 2);
            set(&m, b, v >> 1);
            set(&m, c, v);
            ref_tick(&m);
            CHECK(ref_get(&m, s) == (sum & 1) && ref_get(&m, c) == (sum >> 1), "full_adder.asm %u", v);
        }
        CHECK(p.len == 19, "full_adder.asm should be 19 instructions (the shortest), is %zu", p.len);
        ref_free(&m);
        program_free(&p);
    }

    if (load("examples/adder8.asm", &p)) {
        ref_init(&m, &p);
        uint32_t a[8], b[8], s[8], c = cell(&p, "c");
        for (int i = 0; i < 8; i++) {
            char name[8];
            snprintf(name, sizeof name, "a%d", i);
            a[i] = cell(&p, name);
            snprintf(name, sizeof name, "b%d", i);
            b[i] = cell(&p, name);
            snprintf(name, sizeof name, "s%d", i);
            s[i] = cell(&p, name);
        }
        int bad = 0;
        for (unsigned v = 0; v < (1u << 17); v++) {
            unsigned x = v & 0xFF, y = (v >> 8) & 0xFF, cin = v >> 16, got = 0;
            ref_reset(&m);
            for (int i = 0; i < 8; i++) {
                set(&m, a[i], x >> i);
                set(&m, b[i], y >> i);
            }
            set(&m, c, cin);
            ref_tick(&m);
            for (int i = 0; i < 8; i++)
                got |= (unsigned)ref_get(&m, s[i]) << i;
            got |= (unsigned)ref_get(&m, c) << 8;
            if (got != x + y + cin && bad++ < 5)
                CHECK(0, "adder8.asm %u + %u + %u gave %u", x, y, cin, got);
        }
        CHECK(bad == 0, "adder8.asm wrong for %d of 131072 inputs", bad);
        CHECK(p.len == 152, "adder8.asm should be 152 instructions, is %zu", p.len);
        ref_free(&m);
        program_free(&p);
    }

    if (load("examples/counter4.asm", &p)) {
        ref_init(&m, &p);
        uint32_t q[4] = {cell(&p, "q0"), cell(&p, "q1"), cell(&p, "q2"), cell(&p, "q3")};
        for (unsigned t = 1; t <= 40; t++) {
            ref_tick(&m);
            unsigned got = 0;
            for (int i = 0; i < 4; i++)
                got |= (unsigned)ref_get(&m, q[i]) << i;
            CHECK(got == t % 16, "counter4.asm after %u ticks shows %u", t, got);
        }
        ref_free(&m);
        program_free(&p);
    }
}

/* ---- assembler ---------------------------------------------------------------------------- */

static void test_errors(void)
{
    static const struct {
        const char *src, *expect;
    } cases[] = {
        {"FOO x\n", "unknown instruction or macro 'FOO'"},
        {"STO ONE\n", "cannot store to constant cell"},
        {"NAND\n", "expects exactly one cell"},
        {"NAND a b\n", "expects exactly one cell"},
        {"NAND 1x\n", "not a valid cell name"},
        {"NAND @t\n", "only allowed inside macros"},
        {".in ONE\n", "constant cell"},
        {".in a a\n", "declared twice"},
        {".bogus\n", "unknown directive"},
        {".endm\n", ".endm without .macro"},
        {".macro M\nNAND a\n", "missing .endm"},
        {".macro M x\nNAND x\n.endm\nM\n", "expects 1 argument, got 0"},
        {".macro R\nR\n.endm\nR\n", "nested too deeply"},
        {".macro NAND\n.endm\n", "cannot redefine instruction"},
        {".macro M\n.endm\n.macro M\n.endm\n", "already defined"},
        {".macro M\n.macro N\n.endm\n.endm\n", "inside macros"},
        {".macro M x\nNAND x\n.endm\nM @y\n", "only allowed inside macros"},
        {".include \"no_such_file.inc\"\n", "cannot open include file"},
        {"NAND a\nNAND b\nFOO\n", "<test>:3:"},
        {".macro M\nFOO\n.endm\nNAND a\nM\n", "<test>:2: unknown instruction or macro 'FOO' (in macro called at <test>:5)"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char err[512];
        Program p;
        int ok = asm_string(cases[i].src, "<test>", ".", &p, err, sizeof err);
        CHECK(!ok && strstr(err, cases[i].expect), "case %zu: expected an error containing \"%s\", got \"%s\"",
              i, cases[i].expect, ok ? "(no error)" : err);
        if (ok)
            program_free(&p);
    }
}

static void test_misc(void)
{
    Program p;

    if (assemble(".macro T\nSTO @t\n.endm\nT\nT\n", &p)) {
        CHECK(p.ncells == 4 && p.code[0].addr != p.code[1].addr, "@locals are fresh for every call");
        program_free(&p);
    }
    if (assemble("nand A\nsto a ; comment\n", &p)) {
        CHECK(p.len == 2 && p.ncells == 3 && p.code[0].addr == p.code[1].addr, "names are case-insensitive");
        program_free(&p);
    }
    if (assemble(".in a, b\n.out b\n", &p)) {
        CHECK(p.ninputs == 2 && p.noutputs == 1 && p.inputs[1] == p.outputs[0], "commas and in+out cells");
        program_free(&p);
    }
    if (assemble("NAND ONE\n", &p)) {
        RefMachine m;
        ref_init(&m, &p);
        ref_tick(&m);
        CHECK(m.rr == 1, "NOT of power-on RR");
        ref_tick(&m);
        CHECK(m.rr == 0, "RR keeps its value between ticks");
        ref_free(&m);
        program_free(&p);
    }

    CHECK(addr_bits(2) == 1 && addr_bits(3) == 2 && addr_bits(4) == 2 && addr_bits(5) == 3 &&
              addr_bits(8) == 3 && addr_bits(9) == 4 && addr_bits(70) == 7,
          "addr_bits");
}

int main(void)
{
    test_core();
    test_std();
    test_examples();
    test_errors();
    test_misc();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
