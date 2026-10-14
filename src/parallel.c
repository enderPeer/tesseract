#include "parallel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

typedef struct {
    const JitCode *jc;
    int            cpu, groups, idle;
    volatile long *ready;
    volatile int  *go, *stop;
    uint64_t       passes;
    int            pinned;
} Worker;

static void work(void *arg)
{
    Worker *w = arg;
    w->pinned = w->cpu >= 0 && pin_to_cpu(w->cpu);
    if (w->idle)
        idle_priority();
    LaneMachine *ms = malloc((size_t)w->groups * sizeof *ms);
    for (int g = 0; ms && g < w->groups; g++) {
        if (!lane_init(&ms[g], w->jc->prog)) {
            fputs("out of memory\n", stderr);
            exit(1);
        }
    }
    atomic_inc(w->ready);
    while (!*w->go)
        cpu_pause();

    uint64_t n = 0;
    while (!*w->stop) {
        if (w->groups == 1) {
            jit_run(w->jc, &ms[0], 64);
            n += 64;
        } else {
            for (int g = 0; g < w->groups; g++)
                jit_run(w->jc, &ms[g], 1);
            n += (uint64_t)w->groups;
        }
    }
    w->passes = n;
    for (int g = 0; g < w->groups; g++)
        lane_free(&ms[g]);
    free(ms);
}

void par_measure(const JitCode *jc, int n, const int *cpus, int groups, int idle, double seconds,
                 ParResult *r)
{
    volatile long ready = 0;
    volatile int go = 0, stop = 0;
    Worker *w = calloc((size_t)n, sizeof *w);
    for (int i = 0; i < n; i++) {
        w[i].jc = jc;
        w[i].cpu = cpus ? cpus[i] : -1;
        w[i].groups = groups;
        w[i].idle = idle;
        w[i].ready = &ready;
        w[i].go = &go;
        w[i].stop = &stop;
    }
    Threads *t = threads_start(n, work, w, sizeof *w);
    while (ready < n)
        sleep_seconds(0.001);
    double t0 = now_seconds();
    go = 1;
    sleep_seconds(seconds);
    stop = 1;
    double t1 = now_seconds();
    threads_join(t);

    memset(r, 0, sizeof *r);
    r->seconds = t1 - t0;
    for (int i = 0; i < n; i++) {
        r->passes += w[i].passes;
        r->pinned += w[i].pinned;
    }
    free(w);
}

/* ---- one cube on many threads (step 7f) --------------------------------------------------- */

/* Dissemination barrier (Hensgen, Finkel and Manber, as in Mellor-Crummey and Scott): in round r
   thread i raises a flag of thread (i + 2^r) mod n and waits for its own flag of that round; after
   ceil(log2 n) rounds every thread has heard from every other, with no counter all of them fight
   over. Flags hold a sense that flips every second use, so they never need resetting. x86 keeps
   stores in order, so whatever a thread wrote before the barrier is seen by all after it. */
enum { MAX_ROUNDS = 11 };

typedef struct {
    volatile long flag[2][MAX_ROUNDS];
    char          pad[128 - 2 * MAX_ROUNDS * sizeof(long) % 128];  /* own cache lines */
} Flags;

typedef struct {
    Flags *f;
    int    n, rounds;
} Barrier;

static void barrier_wait(const Barrier *b, int i, int *parity, long *sense)
{
    for (int r = 0; r < b->rounds; r++) {
        b->f[(i + (1 << r)) % b->n].flag[*parity][r] = *sense;
        for (unsigned spins = 0; b->f[i].flag[*parity][r] != *sense; spins++) {
            if (spins < 4096)
                cpu_pause();
            else
                thread_yield();  /* let other work have this processor rather than preempt us */
        }
    }
    if (*parity)
        *sense = !*sense;
    *parity ^= 1;
}

/* Who runs which group in a segment: each thread has a fixed share of three quarters of the
   groups, so their memory stays in its caches; the last quarter goes to whoever is free first, so
   faster cores do more. Short segments are not worth the handing out: fixed shares only. pool[]
   counts the groups taken, a different counter for three segments in a row: thread 0 clears the
   one for the next segment, which nobody has used since two barriers ago. */
enum { SHORT_SEGMENT = 4096 };  /* instructions */

typedef struct {
    volatile long v;
    char          pad[64 - sizeof(long)];
} Counter;

typedef struct {
    const JitCode *jc;
    LaneCube      *c;
    int            cpu, idle, pinned, i, n;
    uint64_t       passes;
    const Barrier *bar;
    Counter       *pool;  /* [3] */
    volatile long *ready;
    volatile int  *go;
} CubeWorker;

static void run_share(const CubeWorker *w, unsigned k, unsigned first, unsigned count)
{
    unsigned a = (unsigned)((uint64_t)count * (unsigned)w->i / (unsigned)w->n),
             b = (unsigned)((uint64_t)count * (unsigned)(w->i + 1) / (unsigned)w->n);
    for (unsigned g = first + a; g < first + b; g++)
        jit_segment(w->jc, k, &w->c->g[g]);
}

static void cube_work(void *arg)
{
    CubeWorker *w = arg;
    w->pinned = w->cpu >= 0 && pin_to_cpu(w->cpu);
    if (w->idle)
        idle_priority();
    atomic_inc(w->ready);
    while (!*w->go)
        cpu_pause();
    int parity = 0;
    long sense = 1;
    unsigned groups = w->c->groups, pooled = groups / 4;
    uint64_t seg = 0;  /* segments done, over all passes */
    for (uint64_t p = 0; p < w->passes; p++)
        for (unsigned k = 0; k < w->jc->nseg; k++, seg++) {
            if (w->i == 0)
                w->pool[(seg + 1) % 3].v = 0;
            if (w->jc->seg_first[k + 1] - w->jc->seg_first[k] < SHORT_SEGMENT) {
                run_share(w, k, 0, groups);
            } else {
                run_share(w, k, 0, groups - pooled);
                for (;;) {
                    long x = atomic_inc(&w->pool[seg % 3].v) - 1;
                    if (x >= (long)pooled)
                        break;
                    jit_segment(w->jc, k, &w->c->g[groups - pooled + (unsigned)x]);
                }
            }
            barrier_wait(w->bar, w->i, &parity, &sense);
        }
}

double par_cube_run(const JitCode *jc, LaneCube *c, int n, const int *cpus, int idle,
                    uint64_t passes, int *pinned)
{
    if (pinned)
        *pinned = 0;
    if (!jc->nseg || n <= 1) {  /* 512 machines, or one thread: no barriers needed */
        double t0 = now_seconds();
        jit_cube_run(jc, c, passes);
        return now_seconds() - t0;
    }
    if (c->g[0].pc != 0 && passes) {  /* a pass started with the interpreter: finish it there */
        lane_cube_tick(c);
        passes--;
    }
    if ((unsigned)n > c->groups)
        n = (int)c->groups;
    if (n > 1 << MAX_ROUNDS)
        n = 1 << MAX_ROUNDS;
    volatile long ready = 0;
    volatile int go = 0;
    Barrier bar = {calloc((size_t)n, sizeof(Flags)), n, 0};
    while ((1 << bar.rounds) < n)
        bar.rounds++;
    Counter *pool = calloc(3, sizeof *pool);
    CubeWorker *w = calloc((size_t)n, sizeof *w);
    if (!w || !bar.f || !pool) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    for (int i = 0; i < n; i++) {
        w[i].jc = jc;
        w[i].c = c;
        w[i].cpu = cpus ? cpus[i] : -1;
        w[i].idle = idle;
        w[i].i = i;
        w[i].n = n;
        w[i].passes = passes;
        w[i].bar = &bar;
        w[i].pool = pool;
        w[i].ready = &ready;
        w[i].go = &go;
    }
    Threads *t = threads_start(n, cube_work, w, sizeof *w);
    while (ready < n)
        cpu_pause();
    double t0 = now_seconds();
    go = 1;
    threads_join(t);
    double t1 = now_seconds();
    for (int i = 0; pinned && i < n; i++)
        *pinned += w[i].pinned;
    for (unsigned g = 0; g < c->groups; g++) {
        c->g[g].steps += passes * c->prog->len;
        c->g[g].ticks += passes;
    }
    free(w);
    free(pool);
    free(bar.f);
    return t1 - t0;
}
