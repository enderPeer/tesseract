/* alpha gamma (step 11): the world of docs/GAMMA.md on the cube. Step 11a is the world without
   cells: light and minerals on a 3-D torus, one voxel per machine. The compiled cube is checked
   against plain C after every tick, then timed on one thread and on all of them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamma.h"
#include "jit.h"
#include "parallel.h"
#include "platform.h"
#include "work.h"

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

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
    fputs("usage: alpha gamma [--size X,Y,Z] [--ticks T] [--occupied P] [--seed S] [--show]\n"
          "                   [--seconds S] [--threads N] [--exclude LIST] [--idle]\n"
          "  --size X,Y,Z   the world, voxels per axis (powers of two, 2 to 512 each, 512 to\n"
          "                 65536 in all; one number for a cube), default 32\n"
          "  --ticks T      ticks, each checked against plain C (default 200)\n"
          "  --occupied P   percent of voxels with something in them, at random (default 5)\n"
          "  --show         a slice through the world: light and minerals\n"
          "  --seconds S    time for the speed rows (default 1; 0 skips them)\n",
          stderr);
}

/* averages per layer, at most 16 rows */
static void print_profile(const GammaWorld *w, unsigned long long ticks)
{
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits, Z = 1u << w->zbits,
             step = Z > 16 ? Z / 16 : 1;
    printf("\nafter %llu ticks, the average of each layer (z = 0 the sun plane, z = %u the floor):\n"
           "%6s %8s %9s %9s\n", ticks, Z - 1, "z", "light", "minerals", "occupied");
    for (unsigned z = 0; z < Z; z++) {
        if (z % step && z != Z - 1)
            continue;
        double l = 0, m = 0, o = 0;
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++) {
                size_t i = gamma_voxel(w, x, y, z);
                l += w->light[i];
                m += w->mineral[i];
                o += w->occ[i];
            }
        double n = (double)X * Y;
        printf("%6u %8.1f %9.1f %7.1f %%\n", z, l / n, m / n / (1 << GAMMA_MINERAL_FRAC),
               100 * o / n);
    }
}

/* the plane y = Y/2: one character per voxel, darker for more; 'o' where something is */
static void print_slice(const GammaWorld *w)
{
    static const char ramp[] = " .:-=+*#%@";
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits, Z = 1u << w->zbits, cols = X < 64 ? X : 64;
    printf("\nthe plane y = %u, x = 0 .. %u across, z = 0 (sun) at the top; 'o': occupied\n%-*s  %s\n",
           Y / 2, cols - 1, (int)cols, "light", "minerals");
    for (unsigned z = 0; z < Z; z++) {
        for (int f = 0; f < 2; f++) {
            for (unsigned x = 0; x < cols; x++) {
                size_t i = gamma_voxel(w, x, Y / 2, z);
                unsigned v = f ? w->mineral[i] >> GAMMA_MINERAL_FRAC : w->light[i];
                putchar(w->occ[i] ? 'o' : ramp[v * 10 / 256]);
            }
            fputs(f ? "\n" : "  ", stdout);
        }
    }
}

int cmd_gamma(int argc, char **argv)
{
    int bits[3] = {5, 5, 5}, threads = 0, idle = 0, show = 0;
    unsigned long long ticks = 200, seed = 1;
    double occupied = 5, seconds = 1;
    static unsigned char excluded[1024];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (!parse_size(argv[++i], bits)) {
                fprintf(stderr, "--size expects X,Y,Z or N, powers of two from 2 to 512\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) {
            ticks = strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--occupied") && i + 1 < argc) {
            occupied = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) {
            if (!cpu_list(argv[++i], excluded)) {
                fputs("--exclude expects processors like 3,11 or 8-15\n", stderr);
                return 2;
            }
        } else if (!strcmp(argv[i], "--idle")) {
            idle = 1;
        } else if (!strcmp(argv[i], "--show")) {
            show = 1;
        } else {
            usage();
            return 2;
        }
    }
    int dims = bits[0] + bits[1] + bits[2];
    if (dims < CUBE_DIMS || dims > CUBE_MAX_DIMS) {
        fprintf(stderr, "the world must have 512 to 65536 voxels (it has 2^%d)\n", dims);
        return 2;
    }
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
    printf("Gamma, step 11a: a %u x %u x %u world, one voxel per machine of a %d-dimensional cube "
           "(%zu machines, %u groups of 512)\n", X, Y, Z, dims, want.n, lc.groups);
    printf("one tick: %zu NAND/STO instructions in %u segment%s: where am I %zu, neighbour reads "
           "%zu, light %zu, minerals %zu, registers %zu\n", p.len, jc.nseg ? jc.nseg : 1,
           jc.nseg > 1 ? "s" : "", cells.cost_masks, cells.cost_reads, cells.cost_light,
           cells.cost_minerals, cells.cost_registers);

    /* something in some voxels, no light or minerals yet; then tick by tick against plain C */
    uint64_t s = seed;
    size_t nocc = 0;
    for (size_t i = 0; i < want.n; i++)
        nocc += want.occ[i] = (uint8_t)((double)(splitmix64(&s) % 1000000) < occupied * 1e4);
    gamma_to_cube(&lc, &cells, &want);
    long long bad = -1;
    unsigned long long t;
    for (t = 1; t <= ticks && bad < 0; t++) {
        jit_cube_tick(&jc, &lc);
        gamma_step(&want);
        gamma_from_cube(&lc, &cells, &got);
        for (size_t i = 0; i < want.n && bad < 0; i++)
            if (got.light[i] != want.light[i] || got.mineral[i] != want.mineral[i])
                bad = (long long)i;
    }
    if (bad >= 0) {
        size_t i = (size_t)bad;
        printf("MISMATCH after tick %llu at x %zu, y %zu, z %zu: light %u, minerals %u/16; plain "
               "C %u, %u/16\n", t - 1, i & (X - 1), (i >> bits[0]) & (Y - 1), i >> (bits[0] + bits[1]),
               got.light[i], got.mineral[i], want.light[i], want.mineral[i]);
    } else {
        printf("OK: light and minerals equal plain C in all %zu voxels after each of %llu ticks "
               "(%.1f %% occupied)\n", want.n, ticks, 100.0 * (double)nocc / (double)want.n);
    }
    print_profile(&want, ticks);
    if (show)
        print_slice(&want);

    if (seconds > 0 && bad < 0) {
        uint64_t passes = 1;
        double t1;
        for (;;) {  /* one thread: the whole world, segment by segment */
            double t0 = now_seconds();
            jit_cube_run(&jc, &lc, passes);
            t1 = now_seconds() - t0;
            if (t1 > seconds / 4 || passes > (1ull << 40))
                break;
            passes *= 2;
        }
        double rate = (double)passes / t1;
        printf("\none thread: %.1f us per tick, %.3g ticks/s, %.3g voxel updates/s, %.1f T 1-bit "
               "ops/s\n", 1e6 / rate, rate, rate * (double)want.n,
               rate * (double)p.len * (double)want.n / 1e12);
        if (lc.groups > 1) {  /* one world on all threads, in lockstep */
            if (!threads)
                threads = cpu_cores(excluded);  /* leave the second hardware threads free */
            int *ids = malloc(1024 * sizeof *ids), k = cpu_pick(ids, 1024, excluded, threads);
            uint64_t n = 1;
            double tm;
            for (;;) {
                tm = par_cube_run(&jc, &lc, k ? k : 1, k ? ids : NULL, idle, n, NULL);
                if (tm > seconds / 2 || n > (1ull << 40))
                    break;
                n *= 2;
            }
            rate = (double)n / tm;
            printf("%d threads, one world: %.1f us per tick, %.3g ticks/s, %.3g voxel updates/s, "
                   "%.1f T 1-bit ops/s\n", k ? k : 1, 1e6 / rate, rate, rate * (double)want.n,
                   rate * (double)p.len * (double)want.n / 1e12);
            free(ids);
        }
    }
    gamma_world_free(&want);
    gamma_world_free(&got);
    lane_cube_free(&lc);
    jit_free(&jc);
    program_free(&p);
    return bad >= 0 ? 3 : 0;
}
