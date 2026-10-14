/* The superoptimizer: specifications, the search against a plain brute force that knows none of
   its rules, and the programs it finds on the reference NAND/STO machine. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "ref.h"
#include "super.h"

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

static int parse(const char *spec, SuperSpec *sp)
{
    char err[256];
    if (super_parse(spec, sp, err, sizeof err))
        return 1;
    printf("FAIL parsing %s: %s\n", spec, err);
    failures++;
    return 0;
}

/* ---- specifications ----------------------------------------------------------------------- */

static void test_parse(void)
{
    SuperSpec sp;
    if (parse("s{a,b} = a ^ b ^ c; c = a&b | a&c | b&c", &sp)) {
        CHECK(sp.nvars == 3 && !sp.rr_in && !sp.rr_out && sp.ncells == 4, "full adder: 3 inputs, 4 cells");
        int a = 0, b = 1, c = 2, s = 3;
        CHECK(sp.cell[c].in && sp.cell[c].out && sp.cell[s].out && !sp.cell[s].in &&
              sp.cell[s].alias == (1u << a | 1u << b), "c in and out, s may share with a or b");
        CHECK(sp.cell[s].target == 0x96 && sp.cell[c].target == 0xE8, "sum 0x96, carry 0xE8");
        CHECK(sp.cell[b].sym_prev == a && sp.cell[a].sym_prev < 0 && sp.cell[c].sym_prev < 0,
              "a and b are interchangeable, c is not");
    }
    if (parse("RR = RR ^ x", &sp))
        CHECK(sp.rr_in && sp.rr_out && sp.nvars == 2 && sp.rr_target == 0x6, "XOR: RR is variable 0");
    if (parse("o{a0,a1} = s ? a1 : a0; ns := !s", &sp)) {
        int ns = -1;
        for (int i = 0; i < sp.ncells; i++)
            if (!strcmp(sp.cell[i].name, "ns"))
                ns = i;
        CHECK(sp.nvars == 3 && ns >= 0 && sp.cell[ns].in && !sp.cell[ns].out,
              "a given cell is an input but no variable");
    }
    static const struct {
        const char *spec, *expect;
    } bad[] = {
        {"RR = ", "expected"},
        {"x = a; x = b", "set twice"},
        {"s{q} = a", "must be another input"},
        {"y = a + b", "unexpected"},
        {"x := a", "nothing to compute"},
        {"o = a ^ b ^ c ^ d ^ e ^ f ^ g", "more than 6"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char err[256];
        int ok = super_parse(bad[i].spec, &sp, err, sizeof err);
        CHECK(!ok && strstr(err, bad[i].expect), "\"%s\": expected \"%s\", got \"%s\"", bad[i].spec,
              bad[i].expect, ok ? "(accepted)" : err);
    }
}

/* ---- a brute force that knows nothing -----------------------------------------------------
   Every sequence of NAND and STO over the constants, the specification's cells (inputs may be
   written too, outputs may be read before they are written) and `temps` scratch cells, shortest
   first. A candidate passing one quick run with garbage is checked in full by super_check. */
typedef struct {
    const SuperSpec *sp;
    int              ncells, temps, maxlen, found;
    SuperProg        prog;
    uint64_t         garbage[SUPER_CELLS + 8], rr0;
} Naive;

static int quick_ok(const Naive *nv, const uint64_t *val, uint64_t rr)
{
    const SuperSpec *sp = nv->sp;
    if (sp->rr_out && rr != sp->rr_target)
        return 0;
    for (int c = 0; c < sp->ncells; c++)
        if (val[c] != (sp->cell[c].out ? sp->cell[c].target : sp->cell[c].init))
            return 0;
    return 1;
}

static void naive_step(Naive *nv, int depth, const uint64_t *val, uint64_t rr)
{
    const SuperSpec *sp = nv->sp;
    if (nv->found)
        return;
    if (depth == nv->prog.len) {
        char err[128];
        if (quick_ok(nv, val, rr) && super_check(sp, &nv->prog, err, sizeof err))
            nv->found = 1;
        return;
    }
    int ncells = sp->ncells + nv->temps;
    uint64_t v2[SUPER_CELLS + 8];
    for (int op = 0; op < 2; op++)
        for (int c = -2; c < ncells && !nv->found; c++) {
            if (op == 1 && c < 0)
                continue;  /* STO to a constant: ignored by the machine, never shorter */
            nv->prog.code[depth].op = (uint8_t)op;
            if (c == -2)
                snprintf(nv->prog.code[depth].cell, SUPER_NAME, "ZERO");
            else if (c == -1)
                snprintf(nv->prog.code[depth].cell, SUPER_NAME, "ONE");
            else if (c < sp->ncells)
                snprintf(nv->prog.code[depth].cell, SUPER_NAME, "%s", sp->cell[c].name);
            else
                snprintf(nv->prog.code[depth].cell, SUPER_NAME, "_t%d", c - sp->ncells + 1);
            memcpy(v2, val, sizeof(uint64_t) * (size_t)ncells);
            uint64_t r = rr;
            if (op == 0)
                r = ~(rr & (c == -2 ? 0 : c == -1 ? sp->mask : val[c])) & sp->mask;
            else
                v2[c] = rr;
            naive_step(nv, depth + 1, v2, r);
        }
}

/* the shortest length the brute force finds, or -1 if none up to maxlen */
static int naive(const SuperSpec *sp, int maxlen, int temps)
{
    Naive nv;
    memset(&nv, 0, sizeof nv);
    nv.sp = sp;
    nv.temps = temps;
    uint64_t s = 99, val[SUPER_CELLS + 8];
    for (int c = 0; c < sp->ncells + temps; c++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        val[c] = (s >> 7) & sp->mask;  /* garbage */
        if (c < sp->ncells && sp->cell[c].in)
            val[c] = sp->cell[c].init;
    }
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t rr = sp->rr_in ? 0xAAAAAAAAAAAAAAAAull & sp->mask : (s >> 9) & sp->mask;
    for (int len = 0; len <= maxlen; len++) {
        nv.prog.len = len;
        naive_step(&nv, 0, val, rr);
        if (nv.found)
            return len;
    }
    return -1;
}

static const struct {
    const char *spec;
    int         best;
} small[] = {
    {"RR = 1", 1},           {"RR = !RR", 1},          {"RR = 0", 2},
    {"RR = x", 3},           {"RR = !x", 2},           {"RR = RR & x", 2},
    {"RR = RR | x", 4},      {"RR = !(RR | x)", 5},    {"RR = RR & !x", 4},
    {"RR = RR | !x", 2},     {"x = !RR; RR = RR", 3},  {"dst = src; RR = src", 4},
    {"r = r | n", 6},        {"r = r & n", 6},         {"m = id & n", 6},
    {"o{x,y} = x & y", 6},   {"o{x,y} = x | y", 6},    {"RR = RR ^ x", 7},
    {"RR = !(RR ^ x)", 8},   {"r = r ^ n", 8},         {"o{x,y} = x ^ y", 8},
    {"r{a,b} = s ? a : b", 9},
};

static void test_against_brute_force(void)
{
    SuperOpts o = {0, 0, 1, 20, 1};
    for (size_t i = 0; i < sizeof small / sizeof small[0]; i++) {
        SuperSpec sp;
        SuperResult r;
        if (!parse(small[i].spec, &sp))
            continue;
        super_search(&sp, &o, &r);
        char err[256] = "";
        CHECK(r.found && r.cost == small[i].best && super_check(&sp, &r.prog, err, sizeof err),
              "%s: shortest %d, expected %d %s", small[i].spec, r.found ? r.cost : -1, small[i].best, err);
        CHECK(r.below == r.cost && !r.contradiction,
              "%s: found at the first cost that was not searched in full (%d, below %d)",
              small[i].spec, r.cost, r.below);
        /* the brute force: the same length where it can get there, and nothing shorter */
        int limit = small[i].best <= 6 ? small[i].best : 6;
        int n = naive(&sp, limit, 3);
        if (small[i].best <= 6)
            CHECK(n == small[i].best, "%s: the brute force finds %d, the search %d", small[i].spec, n,
                  small[i].best);
        else
            CHECK(n < 0, "%s: the brute force finds %d instructions, the search says %d", small[i].spec,
                  n, small[i].best);
    }
}

/* ---- the programs on the reference machine ------------------------------------------------ */

/* Assembles p for a cell layout where output `shared` (if >= 0) lives in input cell `into`, and
   runs every input row with garbage in RR (unless read), outputs and scratch cells. */
static int on_machine(const SuperSpec *sp, const SuperProg *p, int shared, int into)
{
    char src[4096];
    size_t n = 0;
    const char *name[SUPER_CELLS];
    for (int c = 0; c < sp->ncells; c++)
        name[c] = c == shared ? sp->cell[into].name : sp->cell[c].name;
    int inputs = 0;
    for (int c = 0; c < sp->ncells; c++)
        inputs += sp->cell[c].in;
    if (inputs) {
        n += (size_t)snprintf(src + n, sizeof src - n, ".in ");
        for (int c = 0; c < sp->ncells; c++)
            if (sp->cell[c].in)
                n += (size_t)snprintf(src + n, sizeof src - n, " %s", name[c]);
        n += (size_t)snprintf(src + n, sizeof src - n, "\n");
    }
    for (int i = 0; i < p->len; i++) {
        const char *cell = p->code[i].cell;
        for (int c = 0; c < sp->ncells; c++)
            if (!strcmp(cell, sp->cell[c].name))
                cell = name[c];
        n += (size_t)snprintf(src + n, sizeof src - n, "%s %s\n", p->code[i].op ? "STO" : "NAND", cell);
    }
    Program prog;
    char err[256];
    if (!asm_string(src, "<super>", ".", &prog, err, sizeof err)) {
        printf("FAIL assembling: %s\n%s", err, src);
        return 0;
    }
    RefMachine m;
    ref_init(&m, &prog);
    uint64_t seed = 7;
    int ok = 1;
    for (int row = 0; row < 1 << sp->nvars && ok; row++)
        for (int trial = 0; trial < 4 && ok; trial++) {
            ref_reset(&m);
            for (uint32_t a = CELL_FIRST_FREE; a < prog.ncells; a++) {  /* garbage everywhere */
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                ref_set(&m, a, (uint8_t)(seed >> 63));
            }
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            m.rr = (uint8_t)(sp->rr_in ? row & 1 : (int)(seed >> 63));
            for (int c = 0; c < sp->ncells; c++) {
                uint32_t a;
                if (sp->cell[c].in && c != shared && program_find_cell(&prog, name[c], &a))
                    ref_set(&m, a, (uint8_t)((sp->cell[c].init >> row) & 1));
            }
            ref_tick(&m);
            for (int c = 0; c < sp->ncells; c++) {
                uint32_t a;
                uint64_t want = sp->cell[c].out ? sp->cell[c].target : sp->cell[c].init;
                if (c == into && shared >= 0)
                    continue;  /* its cell now holds the output */
                if (program_find_cell(&prog, name[c], &a) && ref_get(&m, a) != ((want >> row) & 1))
                    ok = 0;
            }
            if (sp->rr_out && m.rr != ((sp->rr_target >> row) & 1))
                ok = 0;
        }
    ref_free(&m);
    program_free(&prog);
    return ok;
}

static void machine_check(const char *what, const SuperSpec *sp, const SuperProg *p)
{
    int ok = on_machine(sp, p, -1, -1), variants = 1;
    for (int c = 0; c < sp->ncells; c++)
        for (int a = 0; a < sp->ncells; a++)
            if ((sp->cell[c].alias >> a) & 1) {
                ok &= on_machine(sp, p, c, a);
                variants++;
            }
    CHECK(ok, "%s: right on the reference machine in all %d ways of sharing cells", what, variants);
}

static void test_machine(void)
{
    SuperOpts o = {0, 0, 1, 20, 1};
    for (size_t i = 0; i < sizeof small / sizeof small[0]; i++) {
        SuperSpec sp;
        SuperResult r;
        if (!parse(small[i].spec, &sp))
            continue;
        super_search(&sp, &o, &r);
        if (r.found)
            machine_check(small[i].spec, &sp, &r.prog);
    }
    /* the full adder: the 19-instruction program the search proved shortest (alpha super
       --library FA); here it is checked, and that nothing of 15 or fewer exists */
    static const char *const fa[] = {
        "NAND ZERO", "NAND a",  "STO _t1",  "NAND ONE", "NAND b",  "STO _t2",   "NAND b",
        "NAND _t1",  "STO _t3", "NAND _t2", "NAND c",   "STO _t4", "NAND c",    "NAND _t2",
        "STO c",     "NAND ONE", "NAND _t3", "NAND _t4", "STO s",
    };
    SuperSpec sp;
    if (parse("s{a,b} = a ^ b ^ c; c = a&b | a&c | b&c", &sp)) {
        SuperProg p;
        p.len = (int)(sizeof fa / sizeof fa[0]);
        for (int i = 0; i < p.len; i++) {
            p.code[i].op = fa[i][0] == 'S';
            snprintf(p.code[i].cell, SUPER_NAME, "%s", strchr(fa[i], ' ') + 1);
        }
        char err[256] = "";
        CHECK(super_check(&sp, &p, err, sizeof err), "the 19-instruction full adder: %s", err);
        machine_check("the 19-instruction full adder", &sp, &p);
        SuperResult r;
        SuperOpts q = {0, 0, 1, 15, 1};
        super_search(&sp, &q, &r);
        CHECK(!r.found && r.below == 16, "no full adder of 15 instructions or fewer (below %d)", r.below);
    }
}

/* Searches deep enough for the threads and the transposition table: each must find its program
   exactly at the first cost not ruled out, or some budget was not searched in full. */
static void test_deeper(void)
{
    static const struct {
        const char *spec;
        int         best, threads;  /* one thread: the table sees every state again, in order */
    } deep[] = {
        {"g = a & !b | a & g | !b & g", 9, 1},
        {"f = f ^ (id & n)", 12, 1},
        {"r = (id ^ s) ? n : r", 14, 0},
    };
    for (size_t i = 0; i < sizeof deep / sizeof deep[0]; i++) {
        SuperSpec sp;
        SuperResult r;
        SuperOpts o = {deep[i].threads, 0, 1, 20, 1};
        char err[256] = "";
        if (!parse(deep[i].spec, &sp))
            continue;
        super_search(&sp, &o, &r);
        CHECK(r.found && r.cost == deep[i].best && r.below == r.cost && !r.contradiction &&
              super_check(&sp, &r.prog, err, sizeof err),
              "%s: %d instructions (below %d), expected %d %s", deep[i].spec, r.found ? r.cost : -1,
              r.below, deep[i].best, err);
        if (r.found)
            machine_check(deep[i].spec, &sp, &r.prog);
    }
}

int main(void)
{
    test_parse();
    test_against_brute_force();
    test_machine();
    test_deeper();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
