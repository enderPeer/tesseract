/* Tests for gamma (step 11): the world in plain C against hand-worked cases (the fields, a
   hand-written cell that divides, grants, death), the voxels' places on the cube, and the world's
   NAND/STO program on every engine against plain C, from random states of everything. Run from the
   project root. */
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

static uint32_t xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* every part of every voxel at random, any combination */
static void random_world(GammaWorld *w, uint64_t *s)
{
    for (size_t v = 0; v < w->n; v++) {
        uint64_t r = rnd(s);
        w->alive[v] = (uint8_t)(r & 1);
        w->busy[v] = (uint8_t)((r >> 1 & 3) == 0);
        w->embryo[v] = (uint8_t)(r >> 3 & 1);
        w->partner[v] = (uint8_t)(r >> 4 & 7);
        w->counter[v] = (uint8_t)(r >> 7 & 31);
        w->energy[v] = (uint8_t)(r >> 12);
        w->request[v] = (uint8_t)(r >> 20 & 1);
        w->born[v] = (uint8_t)((r >> 21 & 7) == 0);
        w->light[v] = (uint8_t)(r >> 24);
        w->mineral[v] = (uint16_t)(r >> 32 & GAMMA_MINERAL_MAX);
        w->gates[v] = (uint32_t)rnd(s);
        w->rng[v] = (uint32_t)rnd(s) | 1;
        for (int i = 0; i < GAMMA_GATES; i++)
            w->genome[GAMMA_GATES * v + i] = (uint16_t)(rnd(s) & ((1u << GAMMA_GATE_BITS) - 1));
    }
}

/* light after passing `n` empty voxels below the sun */
static unsigned fall(unsigned l, int n)
{
    while (n-- > 0)
        l -= l / 8;
    return l;
}

/* a cell at v, gates (a, b) per gate: gate i reads sources a and b */
static void put_cell(GammaWorld *w, size_t v, const uint16_t *genome, unsigned energy, uint32_t rng)
{
    w->alive[v] = 1;
    w->busy[v] = w->embryo[v] = w->partner[v] = w->counter[v] = w->request[v] = w->born[v] = 0;
    w->energy[v] = (uint8_t)energy;
    w->gates[v] = 0;
    w->rng[v] = rng;
    memcpy(w->genome + GAMMA_GATES * v, genome, GAMMA_GATES * sizeof *genome);
}

static uint16_t gate_of(int a, int b)
{
    return (uint16_t)(a | b << GAMMA_ADDR_BITS);
}

/* the hand-written cell of the tests: asks to divide in direction dir whenever it can. Gate 0 is
   NAND(1, 1) = 0; gate 28 and the direction bits that are 1 are NAND(gate 0, gate 0) = 1. */
static void plant(uint16_t *genome, int dir)
{
    for (int i = 0; i < GAMMA_GATES; i++)
        genome[i] = gate_of(0, 0);
    genome[GAMMA_DIVIDE] = gate_of(32, 32);
    for (int j = 0; j < 3; j++)
        if (dir >> j & 1)
            genome[GAMMA_DIVIDE + 1 + j] = gate_of(32, 32);
}

/* ---- the fields --------------------------------------------------------------------------- */

static void test_fields(void)
{
    GammaWorld w;
    gamma_world_init(&w, 3, 2, 4);  /* 8 x 4 x 16, no cells */
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
    w.mineral[gamma_voxel(&w, 0, 0, 8)] = 3200;
    gamma_step(&w);
    unsigned wrap_x = w.mineral[gamma_voxel(&w, X - 1, 0, 8)],
             wrap_y = w.mineral[gamma_voxel(&w, 0, Y - 1, 8)],
             up = w.mineral[gamma_voxel(&w, 0, 0, 7)], self = w.mineral[gamma_voxel(&w, 0, 0, 8)],
             diag = w.mineral[gamma_voxel(&w, 1, 1, 8)];
    CHECK(wrap_x == 400 && wrap_y == 400 && up == 400 && self == 800 && diag == 0,
          "a mineral blob spreads 1/8 to each face neighbour across the edges (%u %u %u), keeps 2/8 "
          "(%u), none to the diagonal (%u)", wrap_x, wrap_y, up, self, diag);

    /* a shadow under a cell that does nothing (every gate NAND(1, 1) = 0) */
    uint16_t inert[GAMMA_GATES];
    plant(inert, 0);
    inert[GAMMA_DIVIDE] = gate_of(0, 0);
    for (unsigned t = 0; t < Z; t++)  /* light as before */
        gamma_step(&w);
    put_cell(&w, gamma_voxel(&w, 5, 1, 6), inert, 200, 1);
    for (unsigned t = 0; t < Z; t++)
        gamma_step(&w);
    unsigned under = w.light[gamma_voxel(&w, 5, 1, 7)], beside = w.light[gamma_voxel(&w, 4, 1, 7)],
             deeper = w.light[gamma_voxel(&w, 5, 1, 9)], at = w.light[gamma_voxel(&w, 5, 1, 6)];
    CHECK(w.alive[gamma_voxel(&w, 5, 1, 6)] && at == fall(255, 6) && under == fall(255, 6) / 2 &&
              beside == fall(255, 7) && deeper == fall(fall(255, 6) / 2, 2),
          "a shadow under a cell: lit %u, under it %u, beside %u, two further down %u", at, under,
          beside, deeper);
    gamma_world_free(&w);
}

/* ---- cells -------------------------------------------------------------------------------- */

/* a plant at (3, 1, 8) of an 8 x 4 x 16 world that divides upwards: the request, the grant, the
   split, 32 ticks of copying with the mutations the new cell's dice ask for, and the end */
static void test_division(void)
{
    GammaWorld w;
    gamma_world_init(&w, 3, 2, 4);
    uint16_t g[GAMMA_GATES];
    plant(g, 5);
    size_t pv = gamma_voxel(&w, 3, 1, 8), cv = gamma_voxel(&w, 3, 1, 7);
    for (size_t v = 0; v < w.n; v++)
        w.rng[v] = (uint32_t)(v * 2654435761u) | 1;
    w.light[pv] = 100;  /* gains 3 a tick */
    put_cell(&w, pv, g, 100, w.rng[pv]);
    uint32_t crng = w.rng[cv];

    gamma_step(&w);  /* tick 1: the gates run and ask */
    CHECK(w.alive[pv] && !w.busy[pv] && w.request[pv] && (w.gates[pv] >> 28) == (1 | 5 << 1),
          "tick 1: the plant asks to divide upwards (gates %08x)", w.gates[pv]);
    unsigned e = w.energy[pv], gain = (w.light[pv] >> 5) + (w.mineral[pv] >> 9);
    gamma_step(&w);  /* tick 2: granted */
    CHECK(w.busy[pv] && !w.embryo[pv] && w.partner[pv] == 5 && w.counter[pv] == 0 &&
              w.alive[cv] && w.busy[cv] && w.embryo[cv] && w.partner[cv] == 4 && w.counter[cv] == 0,
          "tick 2: the voxel above grants it, both are busy, each pointing at the other");
    CHECK(w.energy[cv] == e / 2 && (unsigned)(w.energy[pv] + w.energy[cv]) == e + gain - 1,
          "tick 2: the new cell gets half the parent's %u, the parent keeps the rest (%u + %u, "
          "gain %u, upkeep 1)", e, w.energy[pv], w.energy[cv], gain);
    for (int t = 0; t < GAMMA_GATES; t++)
        gamma_step(&w);  /* ticks 3 .. 34: one gate a tick */
    uint32_t r = crng;
    for (int t = 0; t < 2; t++)
        r = xorshift32(r);
    int bad = 0, flips = 0;
    for (int i = 0; i < GAMMA_GATES; i++) {
        r = xorshift32(r);  /* the new cell's dice in the tick that brought in gate i */
        unsigned pos = r >> 20 & 15, flip = (r >> 14 & 63) == 63 && pos < 12 ? 1u << pos : 0;
        flips += flip != 0;
        bad += w.genome[GAMMA_GATES * cv + i] != (g[i] ^ flip);
        bad += w.genome[GAMMA_GATES * pv + i] != g[i];
    }
    CHECK(bad == 0 && w.alive[pv] && w.alive[cv] && !w.busy[pv] && !w.busy[cv] && w.born[cv] &&
              !w.born[pv] && !w.embryo[cv],
          "after 32 ticks the new cell holds the parent's genome with the %d mutation(s) its dice "
          "asked for, the parent its own, both are free, the new one just born (%d wrong)", flips,
          bad);
    gamma_step(&w);
    CHECK(!w.born[cv], "born lasts one tick");

    /* the new cell starves in the dark: no light, no minerals, 2 energy */
    GammaWorld d;
    gamma_world_init(&d, 3, 3, 3);
    size_t v = gamma_voxel(&d, 2, 2, 4);
    for (size_t u = 0; u < d.n; u++)
        d.rng[u] = 7;
    put_cell(&d, v, g, 2, 7);
    gamma_step(&d);
    gamma_step(&d);
    int was = d.alive[v];
    gamma_step(&d);
    CHECK(was && !d.alive[v] && d.energy[v] == 0 && d.mineral[v] == GAMMA_RELEASE && !d.gates[v],
          "a cell with 2 energy and nothing to gain lives 2 ticks, dies in the third and gives back "
          "%u minerals (alive %d, minerals %u)", GAMMA_RELEASE, d.alive[v], d.mineral[v]);

    /* priority: two plants ask for the same voxel, from -x (direction 1 as the voxel sees it) and
       from below (direction 4): the lower direction wins */
    uint16_t right[GAMMA_GATES];
    plant(right, 0);
    memset(d.alive, 0, d.n);
    size_t t = gamma_voxel(&d, 4, 4, 4), a = gamma_voxel(&d, 3, 4, 4), bl = gamma_voxel(&d, 4, 4, 5);
    put_cell(&d, a, right, 100, 7);
    put_cell(&d, bl, g, 100, 7);
    gamma_step(&d);
    gamma_step(&d);
    CHECK(d.embryo[t] && d.partner[t] == 1 && d.busy[a] && !d.busy[bl] && d.request[bl],
          "two cells ask for one voxel: the one in the lower direction gets it, the other keeps "
          "asking");

    /* the new cell dies when its parent does */
    d.alive[a] = 0;
    gamma_step(&d);
    CHECK(!d.alive[t], "a new cell dies with its parent");
    gamma_world_free(&d);
    gamma_world_free(&w);
}

/* a soup of random genomes gets going by itself: cells divide and some finish being born */
static void test_soup(void)
{
    GammaWorld w;
    gamma_world_init(&w, 4, 4, 4);
    for (int t = 0; t < 300; t++)  /* the fields first */
        gamma_step(&w);
    size_t seeded = gamma_soup(&w, 0.3, 64, 5), born = 0, most = 0;
    for (int t = 0; t < 400; t++) {
        gamma_step(&w);
        size_t alive = 0;
        for (size_t v = 0; v < w.n; v++) {
            born += w.born[v];
            alive += w.alive[v];
        }
        most = alive > most ? alive : most;
    }
    CHECK(born > 0 && most > seeded, "a soup of %zu random cells gets going: %zu new cells born in "
          "400 ticks, at most %zu at once", seeded, born, most);
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

/* the reference cube (the golden model) against plain C, from a random world */
static void on_reference(int xb, int yb, int zb, int ticks, uint64_t *s)
{
    Program p;
    GammaCells c;
    GammaWorld want, got;
    LaneCube lc;  /* only to move worlds in and out */
    gen_gamma(&p, xb, yb, zb, &c);
    gamma_world_init(&want, xb, yb, zb);
    gamma_world_init(&got, xb, yb, zb);
    lane_cube_init(&lc, &p);
    unsigned machines = ref_cube_machines(&p);
    RefMachine *cube = calloc(machines, sizeof *cube);
    ref_cube_init(cube, &p);
    random_world(&want, s);
    gamma_to_cube(&lc, &c, &want);
    for (unsigned k = 0; k < machines; k++)  /* the same cells into the reference machines */
        for (uint32_t a = CELL_FIRST_FREE; a < p.ncells; a++)
            ref_set(&cube[k], a, lane_cube_get(&lc, a, k));
    long bad = -1;
    const char *what = "";
    int t;
    for (t = 0; t < ticks && bad < 0; t++) {
        ref_cube_tick(cube);
        gamma_step(&want);
        for (unsigned k = 0; k < machines; k++)
            for (uint32_t a = CELL_FIRST_FREE; a < p.ncells; a++)
                lane_cube_set(&lc, a, k, ref_get(&cube[k], a));
        gamma_from_cube(&lc, &c, &got);
        bad = gamma_compare(&want, &got, &what);
    }
    size_t sum = 0;
    for (int i = 0; i < GAMMA_PARTS; i++)
        sum += c.cost[i];
    CHECK(bad < 0 && program_dims(&p) == (unsigned)(xb + yb + zb),
          "%u x %u x %u on the reference cube of %u machines, %d ticks from a random world: voxel "
          "%ld differs after tick %d (%s)", 1u << xb, 1u << yb, 1u << zb, machines, ticks, bad, t,
          bad < 0 ? "" : what);
    CHECK(sum == p.len, "the parts of one tick add up to the %zu instructions", p.len);
    ref_cube_free(cube);
    free(cube);
    lane_cube_free(&lc);
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
        check_lanes(&p, &jc, seed + 1, 2, 0, &r);
        CHECK(r.ok, "%d+%d+%d bits, %s compile: tick %llu lane %u cell %ld: reference %u, "
              "compiled %u", xb, yb, zb, literal ? "literal" : "optimized",
              (unsigned long long)r.tick, r.lane, r.cell, r.want, r.got);
        jit_free(&jc);
    }
    program_free(&p);
}

/* compiled code on one thread, or `threads` threads in lockstep, against plain C, tick by tick,
   from a random world or from a soup */
static void compiled(int xb, int yb, int zb, int ticks, int threads, int soup, uint64_t *s)
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
    if (soup) {
        for (int t = 0; t < 1 << zb && t < 64; t++)
            gamma_step(&want);
        gamma_soup(&want, 0.25, 80, rnd(s));
    } else {
        random_world(&want, s);
    }
    gamma_to_cube(&lc, &c, &want);
    long bad = -1;
    const char *what = "";
    int t;
    for (t = 0; t < ticks && bad < 0; t++) {
        if (threads > 1)
            par_cube_run(&jc, &lc, threads, NULL, 0, 1, NULL);
        else
            jit_cube_tick(&jc, &lc);
        gamma_step(&want);
        gamma_from_cube(&lc, &c, &got);
        bad = gamma_compare(&want, &got, &what);
    }
    CHECK(bad < 0, "%u x %u x %u compiled from %s, %d thread%s, %d ticks: voxel %ld differs after "
          "tick %d (%s; %u segments)", 1u << xb, 1u << yb, 1u << zb, soup ? "a soup" : "random",
          threads, threads > 1 ? "s" : "", ticks, bad, t, bad < 0 ? "" : what, jc.nseg);
    gamma_world_free(&want);
    gamma_world_free(&got);
    lane_cube_free(&lc);
    jit_free(&jc);
    program_free(&p);
}

int main(void)
{
    uint64_t s = 0x6A33A;
    test_fields();
    test_division();
    test_soup();
    test_places();
    on_reference(3, 3, 3, 2, &s);
    on_reference(2, 3, 5, 2, &s);
    if (!cpu_has_avx2()) {
        printf("%d checks, %d failures (engine tests skipped: no AVX2)\n", checks, failures);
        return failures != 0;
    }
    on_engines(3, 3, 3, 11);
    on_engines(2, 3, 5, 12);
    on_engines(1, 1, 9, 13);
    static const int shape[][6] = {
        /* x, y, z bits, ticks, threads, soup */
        {3, 3, 3, 40, 1, 0}, {3, 3, 3, 200, 1, 1}, {2, 3, 5, 40, 1, 0}, {2, 3, 5, 200, 1, 1},
        {5, 5, 5, 12, 1, 0}, {5, 5, 5, 60, 1, 1},  {6, 5, 5, 6, 1, 0},  {1, 1, 9, 30, 1, 0},
        {9, 6, 1, 6, 1, 0},  {4, 8, 2, 10, 1, 0},  {5, 5, 5, 40, 5, 1}, {2, 3, 5, 60, 3, 0},
    };
    for (size_t i = 0; i < sizeof shape / sizeof shape[0]; i++)
        compiled(shape[i][0], shape[i][1], shape[i][2], shape[i][3], shape[i][4], shape[i][5], &s);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
