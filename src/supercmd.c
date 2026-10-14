/* alpha super: the superoptimizer (src/super.c) on one specification or on the whole library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "super.h"
#include "work.h"

/* Every building block of the library, as a specification, with what it cost before step 10
   (the library now uses the shortest program for each). PXOR_DIM's proof takes about an hour on
   24 threads, so a plain --library run leaves it out. */
static const struct {
    const char *name, *where, *spec;
    int         now;
} library[] = {
    {"SET", "std.inc", "RR = 1", 1},
    {"NOT", "std.inc", "RR = !RR", 1},
    {"CLR", "std.inc", "RR = 0", 2},
    {"LD", "std.inc", "RR = x", 3},
    {"LDC", "std.inc", "RR = !x", 2},
    {"AND", "std.inc", "RR = RR & x", 2},
    {"OR", "std.inc", "RR = RR | x", 5},
    {"NOR", "std.inc", "RR = !(RR | x)", 6},
    {"ANDC", "std.inc", "RR = RR & !x", 5},
    {"ORC", "std.inc", "RR = RR | !x", 2},
    {"XOR", "std.inc", "RR = RR ^ x", 10},
    {"XNOR", "std.inc", "RR = !(RR ^ x)", 11},
    {"STOC", "std.inc", "x = !RR; RR = RR", 3},
    {"MOV", "std.inc", "dst = src; RR = src", 4},
    {"FA", "std.inc", "s{a,b} = a ^ b ^ c; c = a&b | a&c | b&c", 28},
    {"MUX", "cube.inc", "r{a,b} = s ? a : b", 16},
    {"OR_DIM", "cube.inc", "r = r | n", 9},
    {"AND_DIM", "cube.inc", "r = r & n", 6},
    {"XOR_DIM", "cube.inc", "r = r ^ n", 14},
    {"BCAST0_DIM", "cube.inc", "r = id ? n : r", 16},
    {"BCAST_DIM", "cube.inc", "r = (id ^ s) ? n : r", 30},
    {"PXOR_DIM", "cube.inc", "p = p ^ (id & n); t = t ^ n", 30},
    {"MOBIUS_DIM", "cube.inc", "f = f ^ (id & n)", 16},
    {"GE8 (a bit)", "cube.inc", "g = a & !b | a & g | !b & g", 31},
    {"PCOUNT_DIM (a bit)", "cube.inc", "m = id & n", 6},
    {"RING_SEL (a term)", "cube.inc", "t = t & !(m & n)", 7},
    {"XOR gate", "build.c", "o{x,y} = x ^ y", 13},
    {"AND gate", "build.c", "o{x,y} = x & y", 6},
    {"OR gate", "build.c", "o{x,y} = x | y", 7},
    {"b_mux", "build.c", "o{a0,a1} = s ? a1 : a0; ns := !s", 11},
    {"b_select (a term)", "build.c", "acc = acc & !(sel & val)", 7},
    {"half adder", "gen.c", "s{x} = x ^ y; k = x & y", 19},
    {"full adder", "gen.c", "s{x} = x ^ y ^ c; k{c} = x&y | x&c | y&c", 45},
};

static void usage(void)
{
    fputs("usage:\n"
          "  alpha super \"SPEC\" [--max COST] [--threads N] [--part I/K] [--macro]\n"
          "      the shortest NAND/STO program for SPEC, e.g. \"RR = RR ^ x\" or\n"
          "      \"s{a,b} = a ^ b ^ c; c = a&b | a&c | b&c\" (see src/super.h); scratch cells\n"
          "      print as t1, t2, ... (with --macro as @t1, for a macro body)\n"
          "  alpha super --library [NAME...] [--all] [--max COST] [--threads N]\n"
          "      every building block of lib/std.inc, lib/cube.inc and the gate builder: what it\n"
          "      was before step 10, and its shortest (now in the library); PXOR_DIM, an hour's\n"
          "      search, only with --all or by name\n",
          stderr);
}

static int macro_names;  /* --macro: scratch cells as @t1 (a macro body) instead of t1 */

/* the prefix for scratch cells: none, unless the specification has a cell called t1, t2, ... */
static const char *scratch_prefix(const SuperSpec *sp)
{
    if (macro_names)
        return "@";
    for (int c = 0; c < sp->ncells; c++) {
        const char *n = sp->cell[c].name;
        if (n[0] == 't' && n[1] >= '0' && n[1] <= '9')
            return "_";
    }
    return "";
}

static int one(const char *name, const char *spec, int now, const SuperOpts *o, int table)
{
    SuperSpec sp;
    SuperResult r;
    char err[256];
    if (!super_parse(spec, &sp, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", spec, err);
        return 1;
    }
    super_search(&sp, o, &r);
    int ok = !r.found || super_check(&sp, &r.prog, err, sizeof err);
    if (r.contradiction) {
        fprintf(stderr, "SEARCH ERROR: a program of %d instructions turned up after cost %d was "
                        "searched in full\n", r.cost, r.cost);
        ok = 0;
    }
    if (table) {
        char best[32];
        if (r.found)
            snprintf(best, sizeof best, "%d", r.cost);
        else
            snprintf(best, sizeof best, "> %d", r.below - 1);
        printf("%-20s %-9s %-44s %4d %8s %8s %8.1f s%s\n", name, "", spec, now, best,
               r.found ? "proven" : "-", r.seconds, ok ? "" : "  CHECK FAILED");
        fflush(stdout);
        return ok ? 0 : 3;
    }
    if (!r.found) {
        printf("%s: no program of up to %d instructions (%.3g programs' prefixes tried, %.1f s)\n",
               spec, o->max_cost, (double)r.nodes, r.seconds);
        return 1;
    }
    printf("%s\n  shortest: %d instructions; every shorter program was tried (%.3g nodes, %.1f s)\n",
           spec, r.cost, (double)r.nodes, r.seconds);
    super_print(stdout, &r.prog, scratch_prefix(&sp));
    if (ok)
        printf("  checked: right for every input, any garbage in RR, scratch and output cells, and "
               "every allowed sharing of cells\n");
    else
        printf("  CHECK FAILED: %s\n", err);
    return ok ? 0 : 3;
}

int cmd_super(int argc, char **argv)
{
    SuperOpts o = {0, 0, 1, 40, 0};
    const char *spec = NULL;
    int lib = 0, nnames = 0, all = 0;
    const char *names[64];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--max") && i + 1 < argc)
            o.max_cost = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            o.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--part") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d/%d", &o.part, &o.parts) != 2 || o.parts < 1 || o.part < 0 ||
                o.part >= o.parts) {
                fputs("--part expects I/K with 0 <= I < K\n", stderr);
                return 2;
            }
        } else if (!strcmp(argv[i], "--all"))
            all = 1;
        else if (!strcmp(argv[i], "--library"))
            lib = 1;
        else if (!strcmp(argv[i], "--macro"))
            macro_names = 1;
        else if (!strcmp(argv[i], "--quiet"))
            o.quiet = 1;
        else if (argv[i][0] != '-' && lib && nnames < 64)
            names[nnames++] = argv[i];
        else if (argv[i][0] != '-' && !spec)
            spec = argv[i];
        else {
            usage();
            return 2;
        }
    }
    if (!lib) {
        if (!spec) {
            usage();
            return 2;
        }
        return one("", spec, 0, &o, 0);
    }
    o.quiet = 1;
    printf("%-20s %-9s %-44s %4s %8s %8s %10s\n", "building block", "", "specification", "was",
           "shortest", "", "time");
    int rc = 0;
    for (size_t i = 0; i < sizeof library / sizeof library[0]; i++) {
        int want = !nnames && (all || strcmp(library[i].name, "PXOR_DIM"));
        for (int k = 0; k < nnames; k++)
            want |= !strcmp(names[k], library[i].name);
        if (want && one(library[i].name, library[i].spec, library[i].now, &o, 1))
            rc = 3;
    }
    if (!nnames && !all)
        printf("%-20s %-9s %-44s %4d %8d %8s   (about an hour on 24 threads: --all)\n",
               "PXOR_DIM", "", "p = p ^ (id & n); t = t ^ n", 30, 20, "proven");
    return rc;
}
