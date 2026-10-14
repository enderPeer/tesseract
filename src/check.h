/* Cross-checks the bitsliced engine against the reference machine, lane by lane. */
#ifndef ALPHA_CHECK_H
#define ALPHA_CHECK_H

#include "isa.h"
#include "jit.h"

typedef struct {
    int      ok;
    uint64_t compared;  /* lane states compared */
    /* first difference, if !ok */
    uint64_t tick;      /* 1-based tick in which it showed up */
    long     pc;        /* instruction just executed, or -1 = end of the tick */
    unsigned lane;
    long     cell;      /* cell address, or -1 = RR */
    unsigned want;      /* reference machine */
    unsigned got;       /* lane engine */
} CheckResult;

/* Gives each machine (512 lanes, or 2^program_dims of them in several groups) its own random state
   (every cell and RR, from seed), runs `ticks` passes on the lane engine and on as many reference
   machines, and compares the full state of every machine after each tick, or after each
   instruction if stepwise. r->lane is the machine number. With jit, the passes run as
   compiled code (stepwise is not possible then). Returns r->ok. */
int check_lanes(const Program *p, const JitCode *jit, uint64_t seed, unsigned ticks, int stepwise,
                CheckResult *r);

#endif
