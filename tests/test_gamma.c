/* Tests for gamma (step 11a): the world in plain C against hand-worked cases, the voxels' places on
   the cube, and the world's NAND/STO program on every engine against plain C. Run from the project
   root. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "gamma.h"
#include "jit.h"
#include "lanes.h"
#include "parallel.h"
#include "platform.h"
#include "ref.h"

static int checks, failures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        checks++;                                            \
        if (!(cond)) {                                       \
            failures++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            putchar('\n');                                   \
        }                                                    \
    } while (0)

static uint64_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void random_world(GammaWorld *w, int occ_percent, uint64_t *s)
{
    for (size_t i = 0; i < w->n; i++) {
        w->occ[i] = (uint8_t)((int)(rnd(s) % 100) < occ_percent);
        w->light[i] = (uint8_t)rnd(s);
        w->mineral[i] = (uint16_t)(rnd(s) & GAMMA_MINERAL_MAX);
    }
}

/* light after passing `n` empty voxels below the sun */
static unsigned fall(unsigned l, int n)
{
    while (n-- > 0)
        l -= l / 8;
    return l;
}

/* ---- the world in plain C ----------------------------------------------------------------- */

static void test_plain(void)
{
    GammaWorld w;
    gamma_world_init(&w, 3, 2, 4);  /* 8 x 4 x 16 */
    unsigned X = 8, Y = 4, Z = 16;
    gamma_step(&w);
    int bad = 0;
    for (unsigned z = 0; z < Z; z++)
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++) {
                size_t i = gamma_voxel(&w, x, y, z);
                bad += w.light[i] != (z == 0 ? 255 : 0) ||
                       w.mineral[i] != (z == Z - 1 ? GAMMA_MINERAL_MAX : 0);
            }
    CHECK(bad == 0, "one tick from nothing: the sun plane lit, the floor full of minerals (%d wrong)",
          bad);
    for (unsigned t = 1; t < Z; t++)
        gamma_step(&w);
    bad = 0;
    for (unsigned z = 0; z < Z; z++)
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++)
                bad += w.light[gamma_voxel(&w, x, y, z)] != fall(255, (int)z);
    CHECK(bad == 0 && fall(255, 1) == 224 && fall(255, 15) == 38, "after 16 ticks light falls off "
          "by 1/8 per empty voxel all the way down (%d wrong)", bad);

    /* one occupied voxel: the voxel right under it gets half, the rest of its column follows */
    w.occ[gamma_voxel(&w, 5, 1, 6)] = 1;
    for (unsigned t = 0; t < Z; t++)
        gamma_step(&w);
    unsigned under = w.light[gamma_voxel(&w, 5, 1, 7)], beside = w.light[gamma_voxel(&w, 4, 1, 7)],
             deeper = w.light[gamma_voxel(&w, 5, 1, 9)], at = w.light[gamma_voxel(&w, 5, 1, 6)];
    CHECK(at == fall(255, 6) && under == fall(255, 6) / 2 && beside == fall(255, 7) &&
              deeper == fall(fall(255, 6) / 2, 2),
          "a shadow under an occupied voxel: lit %u, under it %u, beside %u, two further down %u",
          at, under, beside, deeper);

    /* minerals: started from nothing, so they only grow (the tick is monotone) until a fixed
       point, a gradient up from the floor, the same everywhere in a layer, in every layer below
       the sun plane (with whole numbers they would stop about 8 layers above the floor) */
    uint16_t *before = malloc(w.n * sizeof *before);
    int grew = 1, ticks = 0;
    bad = 0;
    while (grew && ticks < 20000) {
        memcpy(before, w.mineral, w.n * sizeof *before);
        gamma_step(&w);
        ticks++;
        grew = 0;
        for (size_t i = 0; i < w.n; i++) {
            bad += w.mineral[i] < before[i];
            grew |= w.mineral[i] != before[i];
        }
    }
    for (unsigned z = 0; z < Z; z++)
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++) {
                unsigned m = w.mineral[gamma_voxel(&w, x, y, z)];
                bad += m != w.mineral[gamma_voxel(&w, 0, 0, z)];
                if (z > 0)
                    bad += m < w.mineral[gamma_voxel(&w, x, y, z - 1)] || m == 0;
            }
    unsigned mid = w.mineral[gamma_voxel(&w, 0, 0, Z / 2)];
    CHECK(bad == 0 && !grew && w.mineral[0] == 0 &&
              w.mineral[gamma_voxel(&w, 0, 0, Z - 1)] == GAMMA_MINERAL_MAX && mid < GAMMA_MINERAL_MAX,
          "minerals only grow, to a fixed point after %d more ticks: layers flat, deeper is richer, "
          "none empty below the sun plane, %u/16 in the middle (%d wrong)", ticks, mid, bad);
    free(before);

    /* the torus: minerals spread across the edges, and only to the 6 face neighbours */
    memset(w.mineral, 0, w.n * sizeof *w.mineral);
    memset(w.occ, 0, w.n);
    w.mineral[gamma_voxel(&w, 0, 0, 8)] = 3200;
    gamma_step(&w);
    unsigned wrap_x = w.mineral[gamma_voxel(&w, X - 1, 0, 8)],
             wrap_y = w.mineral[gamma_voxel(&w, 0, Y - 1, 8)],
             up = w.mineral[gamma_voxel(&w, 0, 0, 7)], self = w.mineral[gamma_voxel(&w, 0, 0, 8)],
             diag = w.mineral[gamma_voxel(&w, 1, 1, 8)];
    CHECK(wrap_x == 400 && wrap_y == 400 && up == 400 && self == 800 && diag == 0,
          "a mineral blob spreads 1/8 to each face neighbour across the edges (%u %u %u), keeps 2/8 "
          "(%u), none to the diagonal (%u)", wrap_x, wrap_y, up, self, diag);
    gamma_world_free(&w);
}

/* ---- the voxels on the cube --------------------------------------------------------------- */

static void test_places(void)
{
    static const int shape[][3] = {{3, 3, 3}, {5, 5, 5}, {6, 5, 5}, {1, 1, 9}, {9, 6, 1}};
    for (size_t s = 0; s < sizeof shape / sizeof shape[0]; s++) {
        GammaCells c = {.xbits = shape[s][0], .ybits = shape[s][1], .zbits = shape[s][2]};
        unsigned X = 1u << c.xbits, Y = 1u << c.ybits, Z = 1u << c.zbits, n = X * Y * Z;
        unsigned char *seen = calloc(n, 1);
        int bad = 0;
        for (unsigned z = 0; z < Z; z++)
            for (unsigned y = 0; y < Y; y++)
                for (unsigned x = 0; x < X; x++) {
                    unsigned k = gamma_machine(&c, x, y, z), lo = 0;
                    bad += k >= n || seen[k]++;
                    unsigned nb[3] = {gamma_machine(&c, (x + 1) % X, y, z),
                                      gamma_machine(&c, x, (y + 1) % Y, z),
                                      gamma_machine(&c, x, y, (z + 1) % Z)};
                    int kb[3] = {c.xbits, c.ybits, c.zbits};
                    for (int a = 0; a < 3; a++) {  /* one link, across a dimension of the axis */
                        unsigned d = k ^ nb[a];
                        bad += d == 0 || (d & (d - 1)) || d >> lo >= 1u << kb[a] || d >> lo == 0;
                        lo += (unsigned)kb[a];
                    }
                }
        CHECK(bad == 0, "%u x %u x %u: every voxel its own machine, every face neighbour one link "
              "away along its own axis (%d wrong)", X, Y, Z, bad);
        free(seen);
    }

    Program p;
    GammaCells c;
    static const int no[][3] = {{0, 4, 5}, {10, 3, 3}, {2, 3, 3}, {6, 6, 5}};
    int refused = 0;
    for (size_t s = 0; s < sizeof no / sizeof no[0]; s++)
        refused += !gen_gamma(&p, no[s][0], no[s][1], no[s][2], &c);
    CHECK(refused == 4, "an axis of 0 or 10 bits, a world of 2^8 or 2^17 voxels: refused (%d of 4)",
          refused);
}

/* ---- the program on every engine --------------------------------------------------------- */

static int compare(const GammaWorld *want, const GammaWorld *got)
{
    int bad = 0;
    for (size_t i = 0; i < want->n; i++)
        bad += got->light[i] != want->light[i] || got->mineral[i] != want->mineral[i];
    return bad;
}

/* the reference cube (the golden model) against plain C, from a random world */
static void on_reference(int xb, int yb, int zb, int ticks, uint64_t *s)
{
    Program p;
    GammaCells c;
    GammaWorld want, got;
    gen_gamma(&p, xb, yb, zb, &c);
    gamma_world_init(&want, xb, yb, zb);
    gamma_world_init(&got, xb, yb, zb);
    unsigned machines = ref_cube_machines(&p);
    RefMachine *cube = calloc(machines, sizeof *cube);
    ref_cube_init(cube, &p);
    random_world(&want, 30, s);
    memcpy(got.occ, want.occ, want.n);
    unsigned X = 1u << xb, Y = 1u << yb, Z = 1u << zb;
    for (unsigned z = 0; z < Z; z++)
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++) {
                RefMachine *m = &cube[gamma_machine(&c, x, y, z)];
                size_t i = gamma_voxel(&want, x, y, z);
                ref_set(m, c.occ, want.occ[i]);
                for (int b = 0; b < GAMMA_LIGHT_BITS; b++)
                    ref_set(m, c.light[b], (want.light[i] >> b) & 1);
                for (int b = 0; b < GAMMA_MINERAL_BITS; b++)
                    ref_set(m, c.mineral[b], (want.mineral[i] >> b) & 1);
            }
    int bad = 0, t;
    for (t = 0; t < ticks && !bad; t++) {
        ref_cube_tick(cube);
        gamma_step(&want);
        for (unsigned z = 0; z < Z; z++)
            for (unsigned y = 0; y < Y; y++)
                for (unsigned x = 0; x < X; x++) {
                    RefMachine *m = &cube[gamma_machine(&c, x, y, z)];
                    size_t i = gamma_voxel(&got, x, y, z);
                    unsigned l = 0, mn = 0;
                    for (int b = 0; b < GAMMA_LIGHT_BITS; b++)
                        l |= (unsigned)ref_get(m, c.light[b]) << b;
                    for (int b = 0; b < GAMMA_MINERAL_BITS; b++)
                        mn |= (unsigned)ref_get(m, c.mineral[b]) << b;
                    got.light[i] = (uint8_t)l;
                    got.mineral[i] = (uint16_t)mn;
                }
        bad = compare(&want, &got);
    }
    CHECK(bad == 0 && program_dims(&p) == (unsigned)(xb + yb + zb),
          "%u x %u x %u on the reference cube of %u machines, %d ticks from a random world: %d "
          "voxels wrong after tick %d", X, Y, Z, machines, ticks, bad, t);
    CHECK(c.cost_masks + c.cost_reads + c.cost_light + c.cost_minerals + c.cost_registers == p.len,
          "the parts of one tick add up to the %zu instructions", p.len);
    ref_cube_free(cube);
    free(cube);
    gamma_world_free(&want);
    gamma_world_free(&got);
    program_free(&p);
}

/* the interpreter and both compilers against the reference cube, from random states of every cell */
static void on_engines(int xb, int yb, int zb, uint64_t seed)
{
    Program p;
    GammaCells c;
    CheckResult r;
    gen_gamma(&p, xb, yb, zb, &c);
    check_lanes(&p, NULL, seed, 2, 0, &r);
    CHECK(r.ok, "%d+%d+%d bits, interpreter: tick %llu lane %u cell %ld", xb, yb, zb,
          (unsigned long long)r.tick, r.lane, r.cell);
    for (int literal = 0; literal < 2; literal++) {
        JitCode jc;
        char err[128];
        if (!jit_compile(&p, literal, &jc, NULL, err, sizeof err)) {
            CHECK(0, "%s", err);
            continue;
        }
        check_lanes(&p, &jc, seed + 1, 3, 0, &r);
        CHECK(r.ok, "%d+%d+%d bits, %s compile: tick %llu lane %u cell %ld: reference %u, "
              "compiled %u", xb, yb, zb, literal ? "literal" : "optimized",
              (unsigned long long)r.tick, r.lane, r.cell, r.want, r.got);
        jit_free(&jc);
    }
    program_free(&p);
}

/* compiled code on one thread, or `threads` threads in lockstep, against plain C, tick by tick */
static void compiled(int xb, int yb, int zb, int ticks, int threads, uint64_t *s)
{
    Program p;
    GammaCells c;
    JitCode jc;
    LaneCube lc;
    GammaWorld want, got;
    char err[128];
    gen_gamma(&p, xb, yb, zb, &c);
    if (!jit_compile(&p, 0, &jc, NULL, err, sizeof err) || !lane_cube_init(&lc, &p)) {
        CHECK(0, "%s", err);
        return;
    }
    gamma_world_init(&want, xb, yb, zb);
    gamma_world_init(&got, xb, yb, zb);
    random_world(&want, 20, s);
    gamma_to_cube(&lc, &c, &want);
    int bad = 0, t;
    for (t = 0; t < ticks && !bad; t++) {
        if (threads > 1)
            par_cube_run(&jc, &lc, threads, NULL, 0, 1, NULL);
        else
            jit_cube_tick(&jc, &lc);
        gamma_step(&want);
        gamma_from_cube(&lc, &c, &got);
        bad = compare(&want, &got);
    }
    CHECK(bad == 0 && (lc.groups == 1 ? jc.nseg == 0 : jc.nseg == 2),
          "%u x %u x %u compiled, %d thread%s, %d ticks: %d voxels wrong after tick %d (%u "
          "segments)", 1u << xb, 1u << yb, 1u << zb, threads, threads > 1 ? "s" : "", ticks, bad, t,
          jc.nseg);
    gamma_world_free(&want);
    gamma_world_free(&got);
    lane_cube_free(&lc);
    jit_free(&jc);
    program_free(&p);
}

int main(void)
{
    uint64_t s = 0x6A33A;
    test_plain();
    test_places();
    on_reference(3, 3, 3, 3, &s);
    on_reference(2, 3, 5, 3, &s);
    if (!cpu_has_avx2()) {
        printf("%d checks, %d failures (engine tests skipped: no AVX2)\n", checks, failures);
        return failures != 0;
    }
    on_engines(3, 3, 3, 11);
    on_engines(2, 3, 5, 12);
    on_engines(1, 1, 9, 13);
    static const int shape[][5] = {
        /* x, y, z bits, ticks, threads */
        {3, 3, 3, 40, 1}, {2, 3, 5, 40, 1}, {5, 5, 5, 30, 1}, {6, 5, 5, 8, 1}, {1, 1, 9, 20, 1},
        {9, 6, 1, 6, 1},  {4, 8, 2, 10, 1}, {5, 5, 5, 20, 5}, {2, 3, 5, 30, 3},
    };
    for (size_t i = 0; i < sizeof shape / sizeof shape[0]; i++)
        compiled(shape[i][0], shape[i][1], shape[i][2], shape[i][3], shape[i][4], &s);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
