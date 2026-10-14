/* alpha gamma (step 11): the world of docs/GAMMA.md on the cube. The fields settle, a soup of cells
   with random genomes goes in, the compiled cube is checked against plain C after every one of the
   first ticks, and then runs on all threads, with a census every so often. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamma.h"
#include "jit.h"
#include "parallel.h"
#include "platform.h"
#include "work.h"

/* "3,11" or "8-15": processors to leave alone */
static int cpu_list(const char *s, unsigned char *flag)
{
    while (*s) {
        char *end;
        long a = strtol(s, &end, 10), b = a;
        if (end == s)
            return 0;
        s = end;
        if (*s == '-') {
            b = strtol(s + 1, &end, 10);
            if (end == s + 1)
                return 0;
            s = end;
        }
        if (a < 0 || b >= 1024 || a > b)
            return 0;
        for (long i = a; i <= b; i++)
            flag[i] = 1;
        if (*s == ',')
            s++;
        else if (*s)
            return 0;
    }
    return 1;
}

/* "X,Y,Z" or "N" (N x N x N), each a power of two from 2 to 512 */
static int parse_size(const char *s, int *bits)
{
    for (int a = 0; a < 3; a++) {
        char *end;
        unsigned long v = strtoul(s, &end, 10);
        if (end == s)
            return 0;
        bits[a] = -1;
        for (int k = 1; k <= CUBE_DIMS; k++)
            if (v == 1ul << k)
                bits[a] = k;
        if (bits[a] < 0)
            return 0;
        if (a == 0 && !*end) {  /* one number: a cube */
            bits[1] = bits[2] = bits[0];
            return 1;
        }
        if (*end != (a < 2 ? ',' : '\0'))
            return 0;
        s = end + 1;
    }
    return 1;
}

static void usage(void)
{
    fputs("usage: alpha gamma [--size X,Y,Z] [--soup P] [--energy E] [--settle N] [--check C]\n"
          "                   [--ticks T] [--every K] [--seconds S] [--seed S] [--show]\n"
          "                   [--threads N] [--exclude LIST] [--idle]\n"
          "  --size X,Y,Z   the world, voxels per axis (powers of two, 2 to 512 each, 512 to\n"
          "                 65536 in all; one number for a cube), default 32\n"
          "  --soup P       percent of voxels that start with a cell, random genome (default 10)\n"
          "  --energy E     the energy each of them starts with (default 64)\n"
          "  --settle N     ticks of the empty world first, for light and minerals (default 2000)\n"
          "  --check C      ticks checked against plain C, every voxel (default 100)\n"
          "  --ticks T      ticks in all (default 10000), a census every K (default T/10)\n"
          "  --seconds S    or stop at the first census after S seconds of running\n"
          "  --show         at the end, a slice through the world\n",
          stderr);
}

/* ---- the census --------------------------------------------------------------------------- */

static uint64_t genome_hash(const uint16_t *g)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < GAMMA_GATES; i++) {
        h = (h ^ g[i]) * 0x100000001B3ull;
    }
    return h;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    uint64_t hash;
    size_t   count;
} Kind;

static int cmp_kind(const void *a, const void *b)
{
    const Kind *x = a, *y = b;
    return x->count != y->count ? (x->count < y->count ? 1 : -1) : cmp_u64(&x->hash, &y->hash);
}

/* the kinds of genome among the cells that are not being born, most common first; returns how
   many (*kinds: malloc'ed) */
static size_t kinds_of(const GammaWorld *w, Kind **kinds)
{
    uint64_t *h = malloc((w->n + 1) * sizeof *h);
    size_t n = 0, k = 0;
    for (size_t v = 0; v < w->n; v++)
        if (w->alive[v] && !(w->busy[v] && w->embryo[v]))
            h[n++] = genome_hash(w->genome + GAMMA_GATES * v);
    qsort(h, n, sizeof *h, cmp_u64);
    *kinds = malloc((n + 1) * sizeof **kinds);
    for (size_t i = 0; i < n; i++) {
        if (i == 0 || h[i] != h[i - 1])
            (*kinds)[k++] = (Kind){h[i], 0};
        (*kinds)[k - 1].count++;
    }
    qsort(*kinds, k, sizeof **kinds, cmp_kind);
    free(h);
    return k;
}

static void census_head(unsigned Z)
{
    printf("\n%9s %7s %8s %8s %6s %7s %6s %6s  cells by depth: z 0-%u | %u-%u | %u-%u | %u-%u\n",
           "tick", "cells", "dividing", "genomes", "top", "energy", "ask", "aim", Z / 4 - 1, Z / 4,
           Z / 2 - 1, Z / 2, 3 * Z / 4 - 1, 3 * Z / 4, Z - 1);
}

/* ask: of the cells free to divide, how many ask to; aim: of those, how many ask for an empty
   voxel (blind asking hits empty voxels about as often as there are empty neighbours: free) */
static void census(const GammaWorld *w, unsigned long long tick)
{
    unsigned Z = 1u << w->zbits;
    size_t cells = 0, dividing = 0, band[4] = {0, 0, 0, 0}, free_ = 0, ask = 0, aim = 0;
    double energy = 0, empty = 0;
    for (size_t v = 0; v < w->n; v++) {
        if (!w->alive[v])
            continue;
        unsigned z = (unsigned)(v >> (w->xbits + w->ybits)), dir = w->gates[v] >> 29 & 7;
        cells++;
        dividing += w->busy[v];
        energy += w->energy[v];
        band[z * 4 / Z]++;
        if (w->busy[v] || w->energy[v] < GAMMA_DIVIDE_MIN)
            continue;
        free_++;
        for (int d = 0; d < 6; d++)
            empty += !w->alive[gamma_neighbour(w, v, d)];
        if (w->request[v]) {
            ask++;
            aim += !w->alive[gamma_neighbour(w, v, (int)dir)];
        }
    }
    Kind *k;
    size_t nk = kinds_of(w, &k), settled = 0;
    for (size_t i = 0; i < nk; i++)
        settled += k[i].count;
    printf("%9llu %7zu %8zu %8zu %5.1f%% %7.1f %5.1f%% %5.1f%%  %7zu | %5zu | %5zu | %5zu"
           "   (empty neighbours %.1f%%)\n", tick, cells, dividing, nk,
           settled ? 100.0 * (double)k[0].count / (double)settled : 0.0,
           cells ? energy / (double)cells : 0.0, free_ ? 100.0 * (double)ask / (double)free_ : 0.0,
           ask ? 100.0 * (double)aim / (double)ask : 0.0, band[0], band[1], band[2], band[3],
           free_ ? 100.0 * empty / 6 / (double)free_ : 0.0);
    fflush(stdout);
    free(k);
}

/* the plane y = Y/2: '.' empty, '+' being born, 'A' .. 'Z' the most common genomes, 'o' others */
static void print_slice(const GammaWorld *w)
{
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits, Z = 1u << w->zbits, cols = X < 96 ? X : 96;
    Kind *k;
    size_t nk = kinds_of(w, &k);
    printf("\nthe plane y = %u, x = 0 .. %u across, z = 0 (sun) at the top: '.' empty, '+' being "
           "born,\n'A' the most common genome, 'B' the next, .. 'Z', 'o' any other\n", Y / 2, cols - 1);
    for (unsigned z = 0; z < Z; z++) {
        for (unsigned x = 0; x < cols; x++) {
            size_t v = gamma_voxel(w, x, Y / 2, z);
            int ch = '.';
            if (w->alive[v] && w->busy[v] && w->embryo[v]) {
                ch = '+';
            } else if (w->alive[v]) {
                uint64_t h = genome_hash(w->genome + GAMMA_GATES * v);
                ch = 'o';
                for (size_t i = 0; i < nk && i < 26; i++)
                    if (k[i].hash == h)
                        ch = 'A' + (int)i;
            }
            putchar(ch);
        }
        putchar('\n');
    }
    free(k);
}

/* ---- the command -------------------------------------------------------------------------- */

int cmd_gamma(int argc, char **argv)
{
    int bits[3] = {5, 5, 5}, threads = 0, idle = 0, show = 0;
    unsigned long long settle = 2000, check = 100, ticks = 10000, every = 0, seed = 1;
    double soup = 10, seconds = 0;
    unsigned energy = 64;
    static unsigned char excluded[1024];
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--size") && v) {
            if (!parse_size(v, bits)) {
                fputs("--size expects X,Y,Z or N, powers of two from 2 to 512\n", stderr);
                return 2;
            }
        } else if (!strcmp(a, "--soup") && v) {
            soup = atof(v);
        } else if (!strcmp(a, "--energy") && v) {
            energy = (unsigned)strtoul(v, NULL, 10);
        } else if (!strcmp(a, "--settle") && v) {
            settle = strtoull(v, NULL, 10);
        } else if (!strcmp(a, "--check") && v) {
            check = strtoull(v, NULL, 10);
        } else if (!strcmp(a, "--ticks") && v) {
            ticks = strtoull(v, NULL, 10);
        } else if (!strcmp(a, "--every") && v) {
            every = strtoull(v, NULL, 10);
        } else if (!strcmp(a, "--seconds") && v) {
            seconds = atof(v);
        } else if (!strcmp(a, "--seed") && v) {
            seed = strtoull(v, NULL, 10);
        } else if (!strcmp(a, "--threads") && v) {
            threads = atoi(v);
        } else if (!strcmp(a, "--exclude") && v) {
            if (!cpu_list(v, excluded)) {
                fputs("--exclude expects processors like 3,11 or 8-15\n", stderr);
                return 2;
            }
        } else if (!strcmp(a, "--idle")) {
            idle = 1;
            continue;
        } else if (!strcmp(a, "--show")) {
            show = 1;
            continue;
        } else {
            usage();
            return 2;
        }
        i++;
    }
    int dims = bits[0] + bits[1] + bits[2];
    if (dims < CUBE_DIMS || dims > CUBE_MAX_DIMS) {
        fprintf(stderr, "the world must have 512 to 65536 voxels (it has 2^%d)\n", dims);
        return 2;
    }
    if (energy > GAMMA_ENERGY_MAX || soup < 0 || soup > 100) {
        fputs("--energy must be 0 to 255, --soup 0 to 100\n", stderr);
        return 2;
    }
    if (check > ticks)
        ticks = check;
    if (!every)
        every = ticks / 10 ? ticks / 10 : 1;
    if (!cpu_has_avx2()) {
        fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
        return 1;
    }

    Program p;
    GammaCells cells;
    JitCode jc;
    LaneCube lc;
    GammaWorld want, got;
    char err[256];
    gen_gamma(&p, bits[0], bits[1], bits[2], &cells);
    if (!jit_compile(&p, 0, &jc, NULL, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (!lane_cube_init(&lc, &p) || !gamma_world_init(&want, bits[0], bits[1], bits[2]) ||
        !gamma_world_init(&got, bits[0], bits[1], bits[2])) {
        fputs("out of memory\n", stderr);
        return 1;
    }
    unsigned X = 1u << bits[0], Y = 1u << bits[1], Z = 1u << bits[2];
    printf("Gamma: a %u x %u x %u world of cells, one voxel per machine of a %d-dimensional cube "
           "(%zu machines, %u groups of 512)\n", X, Y, Z, dims, want.n, lc.groups);
    printf("one tick: %zu NAND/STO instructions in %u segment%s:\n ", p.len, jc.nseg ? jc.nseg : 1,
           jc.nseg > 1 ? "s" : "");
    for (int i = 0; i < GAMMA_PARTS; i++)
        printf(" %s %zu%s", gamma_part_names[i], cells.cost[i], i + 1 < GAMMA_PARTS ? "," : "\n");

    if (lc.groups > 1 && !threads)
        threads = cpu_cores(excluded);  /* lockstep: leave the second hardware threads free */
    int *ids = malloc(1024 * sizeof *ids), k = cpu_pick(ids, 1024, excluded, threads);
    if (!k || lc.groups == 1)  /* 512 machines run on one thread */
        k = 1;

    /* the empty world first, until light and minerals have settled */
    lane_cube_reset(&lc);
    if (settle)
        par_cube_run(&jc, &lc, k, ids, idle, settle, NULL);
    gamma_from_cube(&lc, &cells, &want);
    size_t seeded = gamma_soup(&want, soup / 100, energy, seed);
    gamma_to_cube(&lc, &cells, &want);
    printf("the fields settled for %llu ticks; %zu cells went in with random genomes (%.1f %%), "
           "energy %u each\n", settle, seeded, 100.0 * (double)seeded / (double)want.n, energy);

    /* every voxel against plain C, tick by tick */
    long bad = -1;
    const char *what = NULL;
    unsigned long long t = 0;
    while (t < check && bad < 0) {
        jit_cube_tick(&jc, &lc);
        gamma_step(&want);
        gamma_from_cube(&lc, &cells, &got);
        t++;
        bad = gamma_compare(&want, &got, &what);
    }
    if (bad >= 0) {
        size_t v = (size_t)bad;
        printf("MISMATCH after tick %llu at x %zu, y %zu, z %zu: %s\n", t, v & (X - 1),
               (v >> bits[0]) & (Y - 1), v >> (bits[0] + bits[1]), what);
    } else if (check) {
        printf("OK: every voxel equals plain C after each of %llu ticks\n", check);
    }

    /* then compiled, on all threads, with a census */
    double busy_s = 0;
    unsigned long long run = 0;
    if (bad < 0) {
        census_head(Z);
        gamma_from_cube(&lc, &cells, &got);
        census(&got, t);
        while (t < ticks && !(seconds > 0 && busy_s >= seconds)) {
            unsigned long long n = every - t % every;
            if (n > ticks - t)
                n = ticks - t;
            busy_s += par_cube_run(&jc, &lc, k, ids, idle, n, NULL);
            run += n;
            t += n;
            gamma_from_cube(&lc, &cells, &got);
            census(&got, t);
        }
        if (show)
            print_slice(&got);
        if (run)
            printf("\n%d thread%s: %.3g ticks/s, %.1f us per tick, %.3g voxel updates/s, %.1f T "
                   "1-bit ops/s\n", k, k > 1 ? "s" : "", (double)run / busy_s,
                   1e6 * busy_s / (double)run, (double)run * (double)want.n / busy_s,
                   (double)run * (double)p.len * (double)want.n / busy_s / 1e12);
    }
    free(ids);
    gamma_world_free(&want);
    gamma_world_free(&got);
    lane_cube_free(&lc);
    jit_free(&jc);
    program_free(&p);
    return bad >= 0 ? 3 : 0;
}
