#include "tester.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jit.h"
#include "platform.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int lowest_bit(uint64_t x)
{
#ifdef _MSC_VER
    unsigned long i;
    _BitScanForward64(&i, x);
    return (int)i;
#else
    return __builtin_ctzll(x);
#endif
}

static int bucket(uint64_t lam)
{
    int b = 0;
    while (b < T1_BUCKETS - 2 && ((uint64_t)1 << b) < lam)
        b++;
    return b;
}

uint64_t t1_program_seed(uint64_t seed, uint32_t cells, uint64_t k)
{
    uint64_t s = seed ^ ((uint64_t)cells << 40) ^ (k * 0xD1B54A32D192ED03ull);
    return splitmix64(&s);
}

void random_program(Program *p, uint32_t cells, size_t len, uint64_t seed)
{
    memset(p, 0, sizeof *p);
    p->ncells = cells + CELL_FIRST_FREE;
    p->len = len;
    p->code = xmalloc(len * sizeof *p->code);
    p->cell_names = xmalloc(p->ncells * sizeof *p->cell_names);
    for (uint32_t a = 0; a < p->ncells; a++) {
        p->cell_names[a] = xmalloc(16);
        if (a == CELL_ZERO)
            snprintf(p->cell_names[a], 16, "ZERO");
        else if (a == CELL_ONE)
            snprintf(p->cell_names[a], 16, "ONE");
        else
            snprintf(p->cell_names[a], 16, "c%u", a - CELL_FIRST_FREE);
    }
    uint64_t s = seed;
    for (size_t i = 0; i < len; i++) {
        uint64_t r = splitmix64(&s);
        p->code[i].op = (uint8_t)(r & 1);
        p->code[i].addr = p->code[i].op == OP_NAND
                              ? (uint32_t)((r >> 1) % p->ncells)
                              : CELL_FIRST_FREE + (uint32_t)((r >> 1) % cells);
    }
}

void t1_start(LaneMachine *m, uint64_t seed)
{
    uint64_t s = seed ^ 0x5DEECE66Dull;
    for (uint32_t a = CELL_FIRST_FREE; a < m->prog->ncells; a++)
        for (int w = 0; w < 8; w++)
            m->mem[a].w[w] = splitmix64(&s);
    for (int w = 0; w < 8; w++)
        m->rr.w[w] = splitmix64(&s);
}

/* Brent's algorithm, all 512 lanes in step: the hare runs one pass at a time, the tortoise jumps to
   the hare at powers of two, and a lane's cycle length is the distance between them the first time
   they are equal. The schedule is the same for every lane, so no lane needs anything of its own. */
uint64_t t1_cycles(const Program *p, uint64_t seed, uint64_t max_passes, uint64_t lam_out[LANES],
                   const volatile int *stop, int *aborted)
{
    *aborted = 0;
    JitCode jc;
    char err[128];
    if (!jit_compile(p, 0, &jc, NULL, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        exit(1);
    }
    LaneMachine hare, tort;
    if (!lane_init(&hare, p) || !lane_init(&tort, p)) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    t1_start(&hare, seed);
    memset(lam_out, 0, LANES * sizeof *lam_out);

    /* Big states are compared on a sample of cells first: a lane that equals the tortoise also
       equals it on the sample, so only lanes that pass the sample need the full comparison. */
    enum { SAMPLE = 64 };
    uint32_t sample[SAMPLE], writable = p->ncells - CELL_FIRST_FREE;
    int nsample = writable < SAMPLE ? (int)writable : SAMPLE;
    for (int i = 0; i < nsample; i++)
        sample[i] = CELL_FIRST_FREE + (uint32_t)((uint64_t)i * writable / (uint64_t)nsample);

    lane_copy(&tort, &hare);
    jit_run(&jc, &hare, 1);
    uint64_t lam = 1, power = 1, steps = 1;
    Line active;
    memset(active.w, 0xFF, sizeof active.w);
    for (;;) {
        Line d;
        lane_diff_some(&tort, &hare, sample, nsample, &d);
        if ((uint32_t)nsample < writable) {
            uint64_t maybe = 0;
            for (int w = 0; w < 8; w++)
                maybe |= active.w[w] & ~d.w[w];
            if (maybe)
                lane_diff(&tort, &hare, &d);
        }
        uint64_t left = 0;
        for (int w = 0; w < 8; w++) {
            uint64_t found = active.w[w] & ~d.w[w];
            for (; found; found &= found - 1)
                lam_out[64 * w + lowest_bit(found)] = lam;
            active.w[w] &= d.w[w];
            left |= active.w[w];
        }
        if (!left || steps >= max_passes)
            break;
        if (stop && (steps & 63) == 0 && *stop) {
            *aborted = 1;
            break;
        }
        if (power == lam) {
            lane_copy(&tort, &hare);
            power *= 2;
            lam = 0;
        }
        jit_run(&jc, &hare, 1);
        lam++;
        steps++;
    }
    lane_free(&hare);
    lane_free(&tort);
    jit_free(&jc);
    return steps;
}

/* ---- many programs on many threads -------------------------------------------------------- */

typedef struct {
    const T1Config *cfg;
    volatile long  *next;
    volatile int   *stop;
    int             cpu;
    T1Size         *sizes;
} T1Worker;

static size_t program_len(const T1Config *cfg, uint32_t cells)
{
    size_t len = (size_t)(cells * cfg->ratio + 0.5);
    return len ? len : 1;
}

static void t1_work(void *arg)
{
    T1Worker *w = arg;
    const T1Config *cfg = w->cfg;
    if (w->cpu >= 0)
        pin_to_cpu(w->cpu);
    if (cfg->idle)
        idle_priority();
    uint64_t *lam = xmalloc(LANES * sizeof *lam);
    while (!*w->stop) {
        uint64_t j = (uint64_t)(atomic_inc(w->next) - 1);
        if (cfg->programs && j >= cfg->programs * (uint64_t)cfg->nsizes)
            break;
        int si = (int)(j % (uint64_t)cfg->nsizes);  /* sizes take turns */
        uint64_t k = j / (uint64_t)cfg->nsizes;
        uint32_t cells = cfg->cells[si];
        uint64_t ps = t1_program_seed(cfg->seed, cells, k);
        Program p;
        int aborted;
        random_program(&p, cells, program_len(cfg, cells), ps);
        uint64_t passes = t1_cycles(&p, ps, cfg->max_passes, lam, w->stop, &aborted);
        program_free(&p);

        T1Size *st = &w->sizes[si];
        st->passes += passes;  /* work done counts even when time ran out... */
        if (aborted) {         /* ...but an unfinished search adds no cycle lengths */
            st->unfinished++;
            break;
        }
        st->programs++;
        st->starts += LANES;
        for (int l = 0; l < LANES; l++) {
            if (!lam[l]) {
                st->hist[T1_BUCKETS - 1]++;
                continue;
            }
            st->hist[bucket(lam[l])]++;
            if (lam[l] > st->longest) {
                st->longest = lam[l];
                st->longest_seed = ps;
            }
        }
    }
    free(lam);
}

static void add_size(T1Size *to, const T1Size *from)
{
    to->programs += from->programs;
    to->starts += from->starts;
    to->passes += from->passes;
    to->unfinished += from->unfinished;
    for (int b = 0; b < T1_BUCKETS; b++)
        to->hist[b] += from->hist[b];
    if (from->longest > to->longest) {
        to->longest = from->longest;
        to->longest_seed = from->longest_seed;
    }
}

double t1_run(const T1Config *cfg, T1Size *out, int *nthreads)
{
    int *ids = xmalloc(1024 * sizeof *ids), k = cpu_pick(ids, 1024, cfg->excluded, cfg->threads);
    int threads = k ? k : cfg->threads > 0 ? cfg->threads : 1;  /* no topology: unpinned */

    volatile long next = 0;
    volatile int stop = 0;
    T1Worker *w = xmalloc((size_t)threads * sizeof *w);
    for (int i = 0; i < threads; i++) {
        w[i].cfg = cfg;
        w[i].next = &next;
        w[i].stop = &stop;
        w[i].cpu = i < k ? ids[i] : -1;
        w[i].sizes = calloc((size_t)cfg->nsizes, sizeof *w[i].sizes);
    }
    double t0 = now_seconds();
    Threads *t = threads_start(threads, t1_work, w, sizeof *w);
    if (cfg->seconds > 0) {
        sleep_seconds(cfg->seconds);
        stop = 1;
    }
    threads_join(t);
    double wall = now_seconds() - t0;

    memset(out, 0, (size_t)cfg->nsizes * sizeof *out);
    for (int si = 0; si < cfg->nsizes; si++) {
        out[si].cells = cfg->cells[si];
        out[si].len = program_len(cfg, cfg->cells[si]);
        for (int i = 0; i < threads; i++)
            add_size(&out[si], &w[i].sizes[si]);
    }
    for (int i = 0; i < threads; i++)
        free(w[i].sizes);
    free(w);
    free(ids);
    *nthreads = threads;
    return wall;
}

/* ---- output ------------------------------------------------------------------------------- */

static double share(const T1Size *s, int from, int to)
{
    uint64_t n = 0;
    for (int b = from; b <= to; b++)
        n += s->hist[b];
    return s->starts ? 100.0 * (double)n / (double)s->starts : 0;
}

static void range(char *buf, size_t len, int b)
{
    if (b == T1_BUCKETS - 1)
        snprintf(buf, len, "not found");
    else if (b <= 1)
        snprintf(buf, len, "%d", 1 << b);
    else
        snprintf(buf, len, "%llu-%llu", (1ull << (b - 1)) + 1, 1ull << b);
}

void t1_print(const T1Size *s, int n, double seconds, int threads, uint64_t max_passes)
{
    uint64_t ops = 0, starts = 0, programs = 0;
    for (int i = 0; i < n; i++) {
        ops += s[i].passes * s[i].len * LANES;
        starts += s[i].starts;
        programs += s[i].programs;
    }
    printf("tester 1: %llu random programs, %llu runs from random states, cycles searched up to "
           "%llu passes\n",
           (unsigned long long)programs, (unsigned long long)starts,
           (unsigned long long)max_passes);
    printf("%d threads, %.1f s, %.3g 1-bit operations = %.1f T/s\n\n", threads, seconds,
           (double)ops, (double)ops / seconds / 1e12);
    printf("cycle length reached, share of runs:\n");
    printf("%6s %6s %9s %7s %7s %7s %7s %7s %7s %7s %7s  %-13s %s\n", "cells", "instr", "programs",
           "1", "2", "3-16", "17-256", "-4096", "-65536", "longer", "none", "median",
           "longest (program seed)");
    for (int i = 0; i < n; i++) {
        const T1Size *x = &s[i];
        if (!x->starts)
            continue;
        uint64_t cum = 0;
        int med = T1_BUCKETS - 1;
        for (int b = 0; b < T1_BUCKETS; b++) {
            cum += x->hist[b];
            if (2 * cum >= x->starts) {
                med = b;
                break;
            }
        }
        char m[48];
        range(m, sizeof m, med);
        printf("%6u %6zu %9llu %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%%  "
               "%-13s %llu (0x%llx)\n",
               x->cells, x->len, (unsigned long long)x->programs, share(x, 0, 0), share(x, 1, 1),
               share(x, 2, 4), share(x, 5, 8), share(x, 9, 12), share(x, 13, 16),
               share(x, 17, T1_BUCKETS - 2), share(x, T1_BUCKETS - 1, T1_BUCKETS - 1), m,
               (unsigned long long)x->longest, (unsigned long long)x->longest_seed);
    }
    printf("\nnone = no cycle within %llu passes. Look at a program with\n"
           "  alpha random --cells N --len L --seed 0x... > p.asm\n",
           (unsigned long long)max_passes);
    for (int i = 0; i < n; i++)
        if (s[i].unfinished)
            printf("%u cells: %llu more programs were cut off by the time limit and are not "
                   "counted (they lean toward long cycles)\n",
                   s[i].cells, (unsigned long long)s[i].unfinished);
}

void t1_print_raw(const T1Size *s, int n, double seconds, int threads, uint64_t max_passes)
{
    char host[128];
    host_name(host, sizeof host);
    printf("T1RUN %s %d %.6f %llu\n", host, threads, seconds, (unsigned long long)max_passes);
    for (int i = 0; i < n; i++) {
        const T1Size *x = &s[i];
        printf("T1 %u %zu %llu %llu %llu %llu %llu %llx", x->cells, x->len,
               (unsigned long long)x->programs, (unsigned long long)x->starts,
               (unsigned long long)x->passes, (unsigned long long)x->unfinished,
               (unsigned long long)x->longest, (unsigned long long)x->longest_seed);
        for (int b = 0; b < T1_BUCKETS; b++)
            printf(" %llu", (unsigned long long)x->hist[b]);
        printf("\n");
    }
}

int t1_merge(char **files, int nfiles, T1Size *s, double *seconds, int *threads,
             uint64_t *max_passes)
{
    int n = 0;
    *seconds = 0;
    *threads = 0;
    *max_passes = 0;
    for (int f = 0; f < nfiles; f++) {
        FILE *in = fopen(files[f], "r");
        if (!in)
            return -1;
        char line[4096];
        while (fgets(line, sizeof line, in)) {
            char host[128];
            int th;
            double sec;
            unsigned long long mp;
            if (sscanf(line, "T1RUN %127s %d %lf %llu", host, &th, &sec, &mp) == 4) {
                *threads += th;
                if (sec > *seconds)
                    *seconds = sec;
                *max_passes = mp;
                continue;
            }
            T1Size x;
            unsigned cells;
            size_t len;
            unsigned long long prog, starts, passes, unfinished, longest, seed;
            int used;
            if (sscanf(line, "T1 %u %zu %llu %llu %llu %llu %llu %llx%n", &cells, &len, &prog,
                       &starts, &passes, &unfinished, &longest, &seed, &used) != 8)
                continue;
            memset(&x, 0, sizeof x);
            x.cells = cells;
            x.len = len;
            x.programs = prog;
            x.starts = starts;
            x.passes = passes;
            x.unfinished = unfinished;
            x.longest = longest;
            x.longest_seed = seed;
            const char *p = line + used;
            for (int b = 0; b < T1_BUCKETS; b++) {
                char *end;
                x.hist[b] = strtoull(p, &end, 10);
                p = end;
            }
            int i = 0;
            while (i < n && !(s[i].cells == x.cells && s[i].len == x.len))
                i++;
            if (i == n) {
                if (n == T1_SIZES)
                    continue;
                memset(&s[n], 0, sizeof s[n]);
                s[n].cells = x.cells;
                s[n].len = x.len;
                n++;
            }
            add_size(&s[i], &x);
        }
        fclose(in);
    }
    return n;
}
