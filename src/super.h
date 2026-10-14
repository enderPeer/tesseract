/* The superoptimizer: provably shortest NAND/STO programs for small specifications.

   A specification lists outputs as expressions of inputs, for example
       "RR = RR ^ x"                            XOR: RR in and out, the cell x read-only
       "s{a,b} = a ^ b ^ c; c = a&b | a&c | b&c"  a full adder: c in and out; s may be the
                                                same cell as a or b
       "r = s ? a : b; ns := !s"                a multiplexer, given a cell ns holding !s
   Names on the left are written (RR, or cells); every other name is an input cell. A cell on both
   sides starts with its input value and ends with its output value. "{a,b}" lets an output share
   its cell with those inputs, so it must be written after their last read. "name := expr" is an
   input cell holding a function of the others. Expressions: names, 0, 1, ! ~ & ^ | ?: ( ).

   The rules for programs: inputs not written keep their values; RR and outputs not given a value
   are never assumed to hold anything (garbage); any number of scratch cells, also garbage until
   written. The cost is the number of instructions; the search finds the smallest. */
#ifndef ALPHA_SUPER_H
#define ALPHA_SUPER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { SUPER_VARS = 6, SUPER_CELLS = 16, SUPER_LEN = 64, SUPER_NAME = 24 };

typedef struct {
    int      nvars;      /* input variables; the truth tables have 2^nvars rows */
    uint64_t mask;       /* the 2^nvars rows */
    int      rr_in;      /* RR's first value is variable 0 */
    int      rr_out;     /* RR must end as rr_target */
    uint64_t rr_target;
    int      ncells;
    struct {
        char     name[SUPER_NAME];
        int      in, out;  /* holds init at the start / must hold target at the end */
        uint64_t init, target;
        uint32_t alias;    /* bit i: this output may be the same cell as input cell i */
        int      sym_prev; /* interchangeable inputs (a symmetry of the specification): this
                              cell is only read after cell sym_prev was; -1 if none */
    } cell[SUPER_CELLS];
} SuperSpec;

int super_parse(const char *text, SuperSpec *sp, char *err, size_t errlen);

typedef struct {
    int len;
    struct {
        uint8_t op;  /* OP_NAND or OP_STO */
        char    cell[SUPER_NAME];
    } code[SUPER_LEN];
} SuperProg;

typedef struct {
    int threads;      /* 0: all */
    int part, parts;  /* only this share of the search (the cluster): 0 <= part < parts */
    int max_cost;
    int quiet;
} SuperOpts;

typedef struct {
    int       found, cost;  /* the shortest program, and its length */
    int       below;        /* every program shorter than this was tried: none exists */
    int       contradiction; /* found one shorter than `below`: the search is wrong somewhere */
    SuperProg prog;
    uint64_t  nodes;
    double    seconds;
} SuperResult;

void super_search(const SuperSpec *sp, const SuperOpts *o, SuperResult *r);

/* Runs p on truth tables with random garbage and every allowed sharing of cells; returns 1 if it
   meets the specification every time, else 0 with a reason. */
int  super_check(const SuperSpec *sp, const SuperProg *p, char *err, size_t errlen);

/* p as assembly lines; scratch cells get the prefix (e.g. "@" for macro locals). */
void super_print(FILE *f, const SuperProg *p, const char *prefix);

#endif
