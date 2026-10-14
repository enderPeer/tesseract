/* Tester 1: what do random 1-bit computers do?

   A random program is a random string of instructions: every opcode bit is a coin flip, a NAND
   reads any cell (ZERO and ONE included), a STO writes any writable cell. As a machine, a program
   maps the state (RR and every cell) to the next state once per pass. There are finitely many
   states, so every run ends in a cycle. Tester 1 starts each program from 512 random states at
   once, one per lane, finds the length of the cycle each run ends in (Brent's algorithm, on the
   compiled code), and collects the lengths per machine size, on every thread. */
#ifndef ALPHA_TESTER_H
#define ALPHA_TESTER_H

#include "lanes.h"

#define T1_BUCKETS 64  /* bucket b: cycle length in (2^(b-1), 2^b]; the last one: not found */
#define T1_SIZES   32

void     random_program(Program *p, uint32_t cells, size_t len, uint64_t seed);  /* program_free */
uint64_t t1_program_seed(uint64_t seed, uint32_t cells, uint64_t k);

/* Gives every lane of m its own random state (all writable cells and RR) from seed. */
void     t1_start(LaneMachine *m, uint64_t seed);

/* Cycle length reached from each lane's t1_start state, or 0 if no cycle showed up within
   max_passes passes. Returns the passes run. If *stop becomes nonzero (stop may be NULL) the
   search ends early and *aborted is set; lam is then incomplete. */
uint64_t t1_cycles(const Program *p, uint64_t seed, uint64_t max_passes, uint64_t lam[LANES],
                   const volatile int *stop, int *aborted);

typedef struct {
    uint32_t cells;
    size_t   len;
    uint64_t programs, starts, passes;
    uint64_t unfinished;             /* programs cut off by the time limit, not in hist */
    uint64_t hist[T1_BUCKETS];
    uint64_t longest, longest_seed;  /* longest cycle found, and the program that has it */
} T1Size;

typedef struct {
    uint32_t             cells[T1_SIZES];  /* machine sizes to test */
    int                  nsizes;
    double               ratio;            /* instructions per cell */
    uint64_t             programs;         /* per size; 0 = until `seconds` are up */
    double               seconds;
    uint64_t             max_passes;
    uint64_t             seed;
    int                  threads;          /* 0 = every free hardware thread */
    int                  idle;
    const unsigned char *excluded;         /* processors to leave alone, or NULL */
} T1Config;

/* Runs the test; out has cfg->nsizes entries. Returns the wall time; *threads gets the count. */
double t1_run(const T1Config *cfg, T1Size *out, int *threads);

void t1_print(const T1Size *s, int n, double seconds, int threads, uint64_t max_passes);
void t1_print_raw(const T1Size *s, int n, double seconds, int threads, uint64_t max_passes);

/* Adds the raw output of several runs (files written by t1_print_raw) into s; returns the number
   of sizes, or -1 if a file cannot be read. */
int  t1_merge(char **files, int nfiles, T1Size *s, double *seconds, int *threads,
              uint64_t *max_passes);

#endif
