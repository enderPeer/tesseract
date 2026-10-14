/* Every input of a program, 512 at a time, on every thread: alpha verify, equiv and search.

   Pass p runs the assignments x = 512 p + lane (see src/spec.h for how x maps to the inputs): the
   9 lowest bits of x differ between lanes, the others are the same for the whole pass. Before each
   pass the machine is back in its power-on state, so every assignment runs on its own. */
#ifndef ALPHA_ENUM_H
#define ALPHA_ENUM_H

#include "spec.h"

enum { EN_VERIFY, EN_EQUIV, EN_SEARCH };
#define EN_SHOW    16  /* hits kept: the smallest assignments */
#define EN_MAX_IN  46  /* inputs */

typedef struct {
    int                  mode;
    const Program       *a, *b;   /* b: equiv only; its inputs and outputs are matched by name */
    const Spec          *spec;    /* verify: outputs = expression; search: outputs = constant */
    unsigned             ticks;   /* passes per assignment */
    int                  threads; /* 0 = every free hardware thread */
    int                  idle;
    const unsigned char *excluded;
    int                  part, part_end, parts;  /* run shares part..part_end of `parts` */
} Enum;

typedef struct {
    uint64_t space;  /* assignments in total: 2^inputs */
    uint64_t tried;  /* assignments run (this share) */
    uint64_t hits;   /* verify, equiv: mismatches; search: matches */
    uint64_t hit[EN_SHOW];
    int      nhit;
    double   seconds;
    uint64_t ops;    /* 1-bit operations */
    int      threads;
} EnumResult;

int  enum_run(const Enum *e, EnumResult *r, char *err, size_t errlen);
void enum_add_hit(EnumResult *r, uint64_t x);  /* keeps the EN_SHOW smallest */

/* For equiv: map[k] = index in b's inputs (or outputs) of a's input (output) k, matched by name.
   Returns 0 and writes err if the names differ. */
int  enum_match(const Program *a, const Program *b, int *in_map, int *out_map, char *err,
                size_t errlen);

#endif
