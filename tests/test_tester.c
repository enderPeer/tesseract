/* Tests for tester 1: random programs, and Brent's cycle lengths against an exhaustive count on
   the reference machine. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lanes.h"
#include "platform.h"
#include "ref.h"
#include "tester.h"

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

static int same_code(const Program *a, const Program *b)
{
    if (a->len != b->len || a->ncells != b->ncells)
        return 0;
    for (size_t i = 0; i < a->len; i++)
        if (a->code[i].op != b->code[i].op || a->code[i].addr != b->code[i].addr)
            return 0;
    return 1;
}

static void test_programs(void)
{
    Program a, b, c;
    random_program(&a, 20, 500, 42);
    random_program(&b, 20, 500, 42);
    random_program(&c, 20, 500, 43);
    int sto_ok = 1, nand = 0, reads_const = 0;
    for (size_t i = 0; i < a.len; i++) {
        if (a.code[i].op == OP_STO)
            sto_ok &= a.code[i].addr >= CELL_FIRST_FREE && a.code[i].addr < a.ncells;
        else {
            nand++;
            reads_const |= a.code[i].addr < CELL_FIRST_FREE;
        }
    }
    CHECK(sto_ok, "STO only writes writable cells");
    CHECK(nand > 200 && nand < 300, "opcode bits are coin flips (%d NANDs of 500)", nand);
    CHECK(reads_const, "NAND can read ZERO and ONE");
    CHECK(a.ncells == 22 && same_code(&a, &b), "same seed, same program");
    CHECK(!same_code(&a, &c), "other seed, other program");
    program_free(&a);
    program_free(&b);
    program_free(&c);
}

/* Exact cycle length of one lane: run the reference machine and remember when each state
   (RR and every writable cell, at most 11 bits) was first seen. */
static uint64_t exact_cycle(const Program *p, const LaneMachine *start, unsigned lane)
{
    RefMachine m;
    ref_init(&m, p);
    for (uint32_t a = CELL_FIRST_FREE; a < p->ncells; a++)
        ref_set(&m, a, lane_get(start, a, lane));
    m.rr = lane_rr(start, lane);
    size_t states = (size_t)1 << (p->ncells - CELL_FIRST_FREE + 1);
    int64_t *seen = malloc(states * sizeof *seen);
    for (size_t i = 0; i < states; i++)
        seen[i] = -1;
    uint64_t lam = 0;
    for (int64_t t = 0;; t++) {
        size_t s = m.rr;
        for (uint32_t a = CELL_FIRST_FREE; a < p->ncells; a++)
            s |= (size_t)ref_get(&m, a) << (a - CELL_FIRST_FREE + 1);
        if (seen[s] >= 0) {
            lam = (uint64_t)(t - seen[s]);
            break;
        }
        seen[s] = t;
        ref_tick(&m);
    }
    free(seen);
    ref_free(&m);
    return lam;
}

static void test_cycles(void)
{
    uint64_t *lam = malloc(LANES * sizeof *lam);
    for (int k = 0; k < 60; k++) {
        uint32_t cells = 1 + (uint32_t)(k % 10);
        size_t len = 1 + (size_t)((k * 7919) % 60);
        uint64_t seed = 1000 + (uint64_t)k;
        Program p;
        random_program(&p, cells, len, seed);
        int aborted;
        t1_cycles(&p, seed, 1 << 16, lam, NULL, &aborted);

        LaneMachine start;
        lane_init(&start, &p);
        t1_start(&start, seed);
        int bad = 0;
        uint64_t want = 0, got = 0;
        unsigned at = 0;
        for (unsigned l = 0; l < LANES; l++) {
            uint64_t e = exact_cycle(&p, &start, l);
            if (e != lam[l] && !bad++) {
                want = e;
                got = lam[l];
                at = l;
            }
        }
        CHECK(bad == 0, "program %d (%u cells, %zu instructions): %d lanes wrong, first lane %u: "
              "exact %llu, Brent %llu", k, cells, len, bad, at, (unsigned long long)want,
              (unsigned long long)got);
        lane_free(&start);
        program_free(&p);
    }
    free(lam);
}

static int ref_same(const RefMachine *a, const RefMachine *b)
{
    return a->rr == b->rr && memcmp(a->mem, b->mem, a->prog->ncells) == 0;
}

static void ref_copy(RefMachine *dst, const RefMachine *src)
{
    memcpy(dst->mem, src->mem, src->prog->ncells);
    dst->rr = src->rr;
}

/* Brent's algorithm on one lane, on the reference machine: same schedule, so the same answer
   (including "not found") as the 512-lane version. */
static uint64_t ref_brent(const Program *p, const LaneMachine *start, unsigned lane, uint64_t max)
{
    RefMachine tort, hare;
    ref_init(&tort, p);
    ref_init(&hare, p);
    for (uint32_t a = CELL_FIRST_FREE; a < p->ncells; a++)
        ref_set(&hare, a, lane_get(start, a, lane));
    hare.rr = lane_rr(start, lane);
    ref_copy(&tort, &hare);
    ref_tick(&hare);
    uint64_t lam = 1, power = 1, steps = 1, found = 0;
    for (;;) {
        if (ref_same(&tort, &hare)) {
            found = lam;
            break;
        }
        if (steps >= max)
            break;
        if (power == lam) {
            ref_copy(&tort, &hare);
            power *= 2;
            lam = 0;
        }
        ref_tick(&hare);
        lam++;
        steps++;
    }
    ref_free(&tort);
    ref_free(&hare);
    return found;
}

/* Machines bigger than the 64-cell sample, where most comparisons stop at the sample. */
static void test_sampled(void)
{
    uint64_t *lam = malloc(LANES * sizeof *lam);
    for (int k = 0; k < 8; k++) {
        uint32_t cells = 70 + 33 * (uint32_t)k;
        uint64_t seed = 5000 + (uint64_t)k;
        Program p;
        random_program(&p, cells, 4 * cells, seed);
        int aborted;
        t1_cycles(&p, seed, 2048, lam, NULL, &aborted);
        LaneMachine start;
        lane_init(&start, &p);
        t1_start(&start, seed);
        int bad = 0;
        for (unsigned l = 0; l < LANES; l++)
            bad += ref_brent(&p, &start, l, 2048) != lam[l];
        CHECK(bad == 0, "%u cells: %d of 512 lanes differ from the reference", cells, bad);
        lane_free(&start);
        program_free(&p);
    }
    free(lam);
}

int main(void)
{
    test_programs();
    if (!cpu_has_avx2()) {
        printf("%d checks, %d failures (cycle tests skipped: no AVX2)\n", checks, failures);
        return failures != 0;
    }
    test_cycles();
    test_sampled();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
