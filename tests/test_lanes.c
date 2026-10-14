/* Tests for the 512-lane engine: every lane must match the reference machine bit for bit.
   Run from the project root (build test does). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "check.h"
#include "lanes.h"
#include "platform.h"
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

static uint32_t cell(const Program *p, const char *name)
{
    uint32_t a = 0;
    if (!program_find_cell(p, name, &a)) {
        printf("FAIL no cell named %s\n", name);
        failures++;
    }
    return a;
}

static void expect_match(const char *what, const Program *p, unsigned ticks, int stepwise,
                         uint64_t seed)
{
    CheckResult r;
    check_lanes(p, NULL, seed, ticks, stepwise, &r);
    CHECK(r.ok, "%s: tick %llu pc %ld lane %u cell %ld: reference %u, lanes %u", what,
          (unsigned long long)r.tick, r.pc, r.lane, r.cell, r.want, r.got);
}

/* ---- memory layout ------------------------------------------------------------------------ */

static void test_layout(void)
{
    Program p;
    if (!assemble(".in x\nNAND x\nSTO y\n", &p))
        return;
    LaneMachine m;
    lane_init(&m, &p);
    uint32_t x = cell(&p, "x");

    CHECK(((uintptr_t)m.mem & 63) == 0, "lines are 64-byte aligned");
    int consts = 1;
    for (int w = 0; w < 8; w++)
        consts &= m.mem[CELL_ZERO].w[w] == 0 && m.mem[CELL_ONE].w[w] == ~0ull;
    CHECK(consts, "ZERO line is all 0, ONE line is all 1");

    static const unsigned probe[] = {0, 1, 63, 64, 255, 256, 447, 511};
    for (size_t i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        unsigned l = probe[i];
        lane_set(&m, x, l, 1);
        int only = 1;
        for (int w = 0; w < 8; w++)
            only &= m.mem[x].w[w] == (w == (int)(l / 64) ? 1ull << (l % 64) : 0);
        CHECK(only && lane_get(&m, x, l) == 1, "lane %u is bit %u of word %u, and only that bit", l,
              l % 64, l / 64);
        lane_set(&m, x, l, 0);
        CHECK(lane_get(&m, x, l) == 0, "lane %u cleared", l);
    }

    lane_set(&m, CELL_ZERO, 7, 1);
    lane_set(&m, CELL_ONE, 7, 0);
    CHECK(lane_get(&m, CELL_ZERO, 7) == 0 && lane_get(&m, CELL_ONE, 7) == 1,
          "host writes to constants are ignored");

    lane_free(&m);
    program_free(&p);
}

/* ---- both instructions, all four (RR, x) combinations spread over the lanes ---------------- */

static void test_semantics(void)
{
    Program p;
    if (!assemble(".in x\nNAND x\nSTO y\n", &p))
        return;
    LaneMachine m;
    lane_init(&m, &p);
    uint32_t x = cell(&p, "x"), y = cell(&p, "y");
    for (unsigned l = 0; l < LANES; l++) {
        lane_set_rr(&m, l, (uint8_t)(l & 1));
        lane_set(&m, x, l, (uint8_t)((l >> 1) & 1));
    }
    lane_step(&m);
    int nand_ok = 1, sto_ok = 1;
    for (unsigned l = 0; l < LANES; l++)
        nand_ok &= lane_rr(&m, l) == !((l & 1) & ((l >> 1) & 1));
    lane_step(&m);
    for (unsigned l = 0; l < LANES; l++)
        sto_ok &= lane_get(&m, y, l) == lane_rr(&m, l);
    CHECK(nand_ok, "NAND in every lane");
    CHECK(sto_ok, "STO in every lane");
    CHECK(m.pc == 0 && m.ticks == 1 && m.steps == 2, "PC wraps after the last instruction");
    lane_free(&m);
    program_free(&p);
}

/* ---- every example and macro, from random state, every lane ------------------------------- */

static void test_examples(void)
{
    static const char *files[] = {"examples/xor.asm", "examples/mux.asm", "examples/full_adder.asm",
                                  "examples/adder8.asm", "examples/counter4.asm"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        Program p;
        char err[512];
        if (!asm_file(files[i], &p, err, sizeof err)) {
            CHECK(0, "%s", err);
            continue;
        }
        expect_match(files[i], &p, 20, 0, 1 + i);
        expect_match(files[i], &p, 2, 1, 100 + i);
        program_free(&p);
    }
}

static void test_std(void)
{
    static const char *ops[] = {"SET",    "CLR",    "NOT",    "LD x",   "LDC x",
                                "AND x",  "OR x",   "NOR x",  "ANDC x", "ORC x",
                                "XOR x",  "XNOR x", "STOC x", "MOV x y", "FA a b c s"};
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        char src[256];
        snprintf(src, sizeof src, ".include \"lib/std.inc\"\n%s\n", ops[i]);
        Program p;
        if (!assemble(src, &p))
            continue;
        expect_match(ops[i], &p, 4, 1, 200 + i);
        program_free(&p);
    }
}

/* ---- random programs, including STO to constant cells (which the engine must ignore) ------ */

static uint64_t next(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void test_random_programs(void)
{
    uint64_t s = 0x1234567;
    for (int k = 0; k < 100; k++) {
        uint32_t ncells = 2 + (uint32_t)(next(&s) % 31);
        size_t len = 1 + (size_t)(next(&s) % 200);
        Instr *code = malloc(len * sizeof *code);
        for (size_t i = 0; i < len; i++) {
            code[i].op = (uint8_t)(next(&s) & 1);
            code[i].addr = (uint32_t)(next(&s) % ncells);
        }
        Program p = {code, len, ncells, NULL, NULL, 0, NULL, 0};
        char what[64];
        snprintf(what, sizeof what, "random program %d (%zu instructions, %u cells)", k, len, ncells);
        expect_match(what, &p, 2, 1, 300 + (uint64_t)k);
        free(code);
    }
}

/* ---- lane_tick after some lane_steps finishes the same pass ------------------------------- */

static void test_partial_tick(void)
{
    Program p;
    char err[512];
    if (!asm_file("examples/adder8.asm", &p, err, sizeof err)) {
        CHECK(0, "%s", err);
        return;
    }
    LaneMachine a, b;
    lane_init(&a, &p);
    lane_init(&b, &p);
    for (unsigned l = 0; l < LANES; l++) {
        for (size_t k = 0; k < p.ninputs; k++) {
            uint8_t v = (uint8_t)((l * 2654435761u >> k) & 1);
            lane_set(&a, p.inputs[k], l, v);
            lane_set(&b, p.inputs[k], l, v);
        }
    }
    for (int i = 0; i < 37; i++)
        lane_step(&a);
    lane_tick(&a);
    lane_tick(&b);
    CHECK(a.pc == 0 && a.ticks == 1 && a.steps == p.len, "partial pass counted as one tick");
    CHECK(memcmp(a.mem, b.mem, p.ncells * sizeof(Line)) == 0 && memcmp(&a.rr, &b.rr, sizeof a.rr) == 0,
          "steps + tick give the same state as one tick");
    lane_free(&a);
    lane_free(&b);
    program_free(&p);
}

int main(void)
{
    if (!cpu_has_avx2()) {
        puts("SKIP: this CPU has no AVX2");
        return 0;
    }
    test_layout();
    test_semantics();
    test_examples();
    test_std();
    test_random_programs();
    test_partial_tick();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
