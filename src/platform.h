/* The few things that depend on the operating system and the CPU. */
#ifndef ALPHA_PLATFORM_H
#define ALPHA_PLATFORM_H

#include <stddef.h>

double now_seconds(void);  /* monotonic clock */
void   sleep_seconds(double s);
int    cpu_has_avx2(void);  /* CPU and OS both support AVX2 */
void   cpu_brand(char *buf, size_t len);
void   host_name(char *buf, size_t len);

/* One logical processor. */
typedef struct {
    int id;    /* operating system number, for pinning */
    int core;  /* physical core, numbered 0.. in order of their first processor */
    int smt;   /* 0 = first hardware thread of its core, 1 = second */
    int fast;  /* 1 = fastest core type (P-core); every core is fast on non-hybrid CPUs */
    int numa;  /* NUMA node */
} Cpu;

/* Fills out with the logical processors (at most max), sorted by core, then smt. */
int  cpu_topology(Cpu *out, int max);
/* Processors for n worker threads (n = 0: all), best first: one per fast core, then the slow
   cores, then the second hardware threads. excluded[id] = 1 leaves processor id alone (excluded may
   be NULL, else it must cover every id). Returns how many ids it wrote (at most max). */
int  cpu_pick(int *ids, int max, const unsigned char *excluded, int n);
int  pin_to_cpu(int id);   /* pins the calling thread; returns 0 on failure */
void idle_priority(void);  /* the calling thread only gets cycles nothing else wants */

/* Pins the calling thread to a processor of the fastest core type, away from processor 0.
   Returns its id, or -1. *performance is 1 if slower cores were avoided. On Windows the thread's
   priority is also raised, for steadier single-thread timings. */
int  pin_to_fast_core(int *performance);

typedef void (*ThreadFn)(void *arg);
typedef struct Threads Threads;
Threads *threads_start(int n, ThreadFn fn, void *args, size_t arg_size);  /* fn(args + i*size) */
void     threads_join(Threads *t);

long atomic_inc(volatile long *p);  /* returns the new value */
void cpu_pause(void);               /* inside spin loops */
void thread_yield(void);            /* let another thread have this processor for a while */

/* Physical cores, not counting second hardware threads or excluded processors. */
int  cpu_cores(const unsigned char *excluded);

#endif
