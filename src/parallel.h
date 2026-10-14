/* Compiled code on many threads at once. The lanes are independent machines, so the threads
   share nothing: each one owns its 512-lane groups and runs them as fast as it can. */
#ifndef ALPHA_PARALLEL_H
#define ALPHA_PARALLEL_H

#include "jit.h"

typedef struct {
    double   seconds;  /* wall time from the common start to the common stop */
    uint64_t passes;   /* passes of one 512-lane group, summed over all groups and threads */
    int      pinned;   /* threads that got their processor */
} ParResult;

/* Runs n threads for about `seconds`. Thread i is pinned to processor cpus[i] (cpus may be NULL)
   and owns `groups` 512-lane groups, allocated by itself so they live in its NUMA node; with more
   than one group it cycles through them, one pass each. idle = lowest priority. */
void par_measure(const JitCode *jc, int n, const int *cpus, int groups, int idle, double seconds,
                 ParResult *r);

/* One cube of more than 512 machines (step 7f) on n threads at once: thread i runs an equal share
   of the groups, and all threads wait for each other after every segment of the pass, so the cube
   stays in lockstep. Runs `passes` passes and returns the wall time; *pinned (may be NULL) gets how
   many threads got their processor. Programs for 512 machines run on one thread. */
double par_cube_run(const JitCode *jc, LaneCube *c, int n, const int *cpus, int idle,
                    uint64_t passes, int *pinned);

#endif
