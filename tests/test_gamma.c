/* Tests for gamma (step 11): the world in plain C against hand-worked cases (the fields under the
   sun wave, a hand-written cell that divides, the lot, terms, death, energy given from cell to
   cell, midnight, the riddle and its credit, predators), the ledger on running soups, the
   voxels' places on the cube, and the world's
   NAND/STO program on every engine against plain C, from random states of everything. Run from
   the project root. The expected values were worked out from the rules at the top of gamma.h,
   never from the code. */
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

enum { TERM_MAX = (1 << GAMMA_TERM_BITS) - 1 };  /* 511: a term that outlives every case */

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

/* every part of every voxel at random, any combination: a clock of any value in any voxel, a term
   of 0 .. 511 whether a cell is there or not */
static void random_world(GammaWorld *w, uint64_t *s)
{
    for (size_t v = 0; v < w->n; v++) {
        uint64_t r = rnd(s), q = rnd(s);
        w->alive[v] = (uint8_t)(r & 1);
        w->busy[v] = (uint8_t)((r >> 1 & 3) == 0);
        w->embryo[v] = (uint8_t)(r >> 3 & 1);
        w->partner[v] = (uint8_t)(r >> 4 & 7);
        w->counter[v] = (uint8_t)(r >> 7 & 31);
        w->energy[v] = (uint8_t)(r >> 12);
        w->request[v] = (uint8_t)(r >> 20 & 1);
        w->born[v] = (uint8_t)((r >> 21 & 7) == 0);
        w->offer[v] = (uint8_t)(r >> 40 & 31);
        w->light[v] = (uint8_t)(r >> 24);
        w->mineral[v] = (uint16_t)(r >> 32 & GAMMA_MINERAL_MAX);
        w->clock[v] = (uint8_t)q;
        w->term[v] = (uint16_t)(q >> 8 & TERM_MAX);
        w->gates[v] = (uint32_t)rnd(s);
        w->rng[v] = (uint32_t)rnd(s) | 1;
        for (int i = 0; i < GAMMA_GATES; i++)
            w->genome[GAMMA_GATES * v + i] = (uint16_t)(rnd(s) & ((1u << GAMMA_GATE_BITS) - 1));
        w->token[v] = (uint8_t)(q >> 17 & 1);
        w->credit[v] = (uint8_t)(q >> 18 & GAMMA_CREDIT_MAX);
        w->pred[v] = (uint8_t)((q >> 22 & 3) == 0);
        w->fresh[v] = (uint8_t)(q >> 24 & 1);
    }
    w->need = (unsigned)(rnd(s) & GAMMA_CREDIT_MAX);
}

/* light after passing `n` empty voxels below the sun */
static unsigned fall(unsigned l, int n)
{
    while (n-- > 0)
        l -= l / 8;
    return l;
}

/* the light at depth z of an empty column T ticks after a new world (clock 0): the sun of T - z
   ticks ago, fallen z voxels, and nothing yet further down */
static unsigned wave(unsigned T, unsigned z)
{
    return T >= z ? fall(gamma_sun((T - z) & 255), (int)z) : 0;
}

/* a cell at v, gates (a, b) per gate: gate i reads sources a and b; the clock as the world has it,
   the term as given (0 means it dies next tick) */
static void put_cell(GammaWorld *w, size_t v, const uint16_t *genome, unsigned energy, uint32_t rng,
                     unsigned term)
{
    w->alive[v] = 1;
    w->busy[v] = w->embryo[v] = w->partner[v] = w->counter[v] = w->request[v] = w->born[v] = 0;
    w->offer[v] = 0;
    w->credit[v] = 0;
    w->energy[v] = (uint8_t)energy;
    w->term[v] = (uint16_t)term;
    w->gates[v] = 0;
    w->rng[v] = rng;
    memcpy(w->genome + GAMMA_GATES * v, genome, GAMMA_GATES * sizeof *genome);
}

static uint16_t gate_of(int a, int b)
{
    return (uint16_t)(a | b << GAMMA_ADDR_BITS);
}

/* The hand-written cells of the tests, which exist only here. Gate 0 is NAND(1, 1) = 0, and
   every gate that should be 1 is NAND(gate 0, gate 0). cell(): direction dir, dividing and giving
   as asked; plant(): divides in direction dir whenever it can. */
static void cell(uint16_t *genome, int dir, int divide, int give)
{
    for (int i = 0; i < GAMMA_GATES; i++)
        genome[i] = gate_of(0, 0);
    if (divide)
        genome[GAMMA_DIVIDE] = gate_of(32, 32);
    if (give)
        genome[GAMMA_GIVE] = gate_of(32, 32);
    for (int j = 0; j < 3; j++)
        if (dir >> j & 1)
            genome[GAMMA_DIVIDE + 1 + j] = gate_of(32, 32);
}

static void plant(uint16_t *genome, int dir)
{
    cell(genome, dir, 1, 0);
}

/* The social game's cells (step 11f). talker(): points in direction dir, says its token on signal
   0 and answers its token XOR what the cell it points at says on signal 0 (sense 10 + 2 dir), in
   four NANDs; with say 0 it says nothing (signal 0 is 0). guesser(): answers its own token,
   says nothing. */
static void talker(uint16_t *genome, int dir, int say)
{
    int heard = 10 + 2 * dir;
    cell(genome, dir, 0, 0);
    genome[1] = gate_of(2, 2);                   /* g1 = !token */
    genome[2] = gate_of(2, heard);               /* g2 = NAND(token, heard) */
    genome[3] = gate_of(2, 34);                  /* NAND(token, g2) */
    genome[4] = gate_of(heard, 34);              /* NAND(heard, g2) */
    genome[GAMMA_ANSWER] = gate_of(35, 36);      /* token XOR heard */
    if (say)
        genome[GAMMA_SIGNAL] = gate_of(33, 33);  /* signal 0 = token */
}

static void guesser(uint16_t *genome, int dir)
{
    cell(genome, dir, 0, 0);
    genome[1] = gate_of(2, 2);
    genome[GAMMA_ANSWER] = gate_of(33, 33);  /* the answer is the cell's own token */
}

static unsigned up(unsigned credit, unsigned solved)  /* + 1 for a round solved, at most 15 */
{
    return credit + solved > GAMMA_CREDIT_MAX ? GAMMA_CREDIT_MAX : credit + solved;
}

/* a dice state whose next draws (xorshift32 `ahead` times) have bits 27 and 28 as asked */
static uint32_t dice_for(uint32_t from, int ahead, unsigned strike)
{
    for (uint32_t r = from;; r++) {
        uint32_t x = r ? r : 1;
        for (int i = 0; i < ahead; i++)
            x = xorshift32(x);
        if ((x >> GAMMA_STRIKE_DICE & 3) == strike && r)
            return r;
    }
}

/* ---- the fields --------------------------------------------------------------------------- */

static void test_fields(void)
{
    /* the sun: a triangle wave of the clock, 0 at midnight, 254 at noon (clocks 127 and 128) */
    static const unsigned clocks[] = {0, 1, 100, 127, 128, 200, 255},
                          suns[] = {0, 2, 200, 254, 254, 110, 0};
    int bad = 0;
    for (size_t i = 0; i < sizeof clocks / sizeof clocks[0]; i++)
        bad += gamma_sun(clocks[i]) != suns[i];
    CHECK(bad == 0, "the sun at clock 0, 1, 100, 127, 128, 200, 255 is 0, 2, 200, 254, 254, 110, 0 "
          "(%d wrong)", bad);

    /* a new world has clock 0; the clock counts the ticks, and the light at depth z is the sun of
       z ticks ago fallen z voxels (nothing yet below depth T) */
    GammaWorld w;
    gamma_world_init(&w, 3, 2, 4);  /* 8 x 4 x 16, no cells */
    unsigned X = 8, Y = 4, Z = 16, T = 0;
    bad = 0;
    for (size_t v = 0; v < w.n; v++)
        bad += w.clock[v] != 0 || w.term[v] != 0;
    CHECK(bad == 0, "a new world has clock 0 and term 0 in every voxel (%d wrong)", bad);
    static const unsigned stop[] = {1, 16, 143, 300};
    for (size_t i = 0; i < sizeof stop / sizeof stop[0]; i++) {
        while (T < stop[i]) {
            gamma_step(&w);
            T++;
        }
        bad = 0;
        for (unsigned z = 0; z < Z; z++)
            for (unsigned y = 0; y < Y; y++)
                for (unsigned x = 0; x < X; x++) {
                    size_t v = gamma_voxel(&w, x, y, z);
                    bad += w.light[v] != wave(T, z) || w.clock[v] != (T & 255);
                    if (T == 1)
                        bad += w.mineral[v] != (z == Z - 1 ? GAMMA_MINERAL_MAX : 0);
                }
        CHECK(bad == 0, "after %u ticks the clock reads %u and the light is the sun of z ticks ago "
              "fallen z voxels: %u at the top, %u at the floor%s (%d wrong)", T, T & 255,
              wave(T, 0), wave(T, Z - 1), T == 1 ? "; the floor full of minerals" : "", bad);
    }
    /* the expected values themselves, pinned, so that a wrong fall() and a wrong sun cannot
       cancel: noon fallen 15 voxels is the old constant sun's 38 */
    CHECK(wave(143, 15) == 38 && wave(16, 15) == 2 && wave(300, 0) == 88 && wave(16, 0) == 32,
          "the sun of noon fallen 15 voxels is 38, after 16 ticks the floor sees 2, the top 32, and at "
          "tick 300 the top sees 88 (%u %u %u %u)", wave(143, 15), wave(16, 15), wave(300, 0),
          wave(16, 0));

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

    /* a shadow under a cell that does nothing (every gate NAND(1, 1) = 0), under the sun wave
       from the clock the world has now, c0: after 16 ticks the cell's light is the sun of clock
       c0 + 10 fallen 6, under it the sun of c0 + 9 fallen 6 and halved, two further down the sun
       of c0 + 7 fallen 6, halved and fallen 2 more */
    uint16_t inert[GAMMA_GATES];
    plant(inert, 0);
    inert[GAMMA_DIVIDE] = gate_of(0, 0);
    unsigned c0 = w.clock[0];
    put_cell(&w, gamma_voxel(&w, 5, 1, 6), inert, 200, 1, TERM_MAX);
    for (unsigned t = 0; t < Z; t++)
        gamma_step(&w);
    unsigned s10 = gamma_sun((c0 + 10) & 255), s9 = gamma_sun((c0 + 9) & 255),
             s7 = gamma_sun((c0 + 7) & 255);
    unsigned under = w.light[gamma_voxel(&w, 5, 1, 7)], beside = w.light[gamma_voxel(&w, 4, 1, 7)],
             deeper = w.light[gamma_voxel(&w, 5, 1, 9)], at = w.light[gamma_voxel(&w, 5, 1, 6)];
    CHECK(w.alive[gamma_voxel(&w, 5, 1, 6)] && at == fall(s10, 6) && under == fall(s9, 6) / 2 &&
              beside == fall(s9, 7) && deeper == fall(fall(s7, 6) / 2, 2),
          "a shadow under a cell, from clock %u: lit %u, under it %u, beside %u, two further down "
          "%u", c0, at, under, beside, deeper);
    gamma_world_free(&w);
}

/* ---- cells -------------------------------------------------------------------------------- */

/* a plant at (3, 1, 8) of an 8 x 4 x 16 world that divides upwards: the request, the grant, the
   split, the new cell's term from its dice, 32 ticks of copying with the mutations the new cell's
   dice ask for, and the end; then starving, a term that runs out, the lot, an abort, and a parent
   whose term is up in the tick it is granted */
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
    put_cell(&w, pv, g, 100, w.rng[pv], TERM_MAX);
    uint32_t crng = w.rng[cv];

    unsigned e = w.energy[pv], gain = (w.light[pv] >> 5) + (w.mineral[pv] >> 9);
    gamma_step(&w);  /* tick 1: the gates run, the plant asks, the voxel above grants it */
    CHECK(w.request[pv] && (w.gates[pv] >> 28) == (1 | 5 << 1) && w.busy[pv] && !w.embryo[pv] &&
              w.partner[pv] == 5 && w.counter[pv] == 0 && w.alive[cv] && w.busy[cv] &&
              w.embryo[cv] && w.partner[cv] == 4 && w.counter[cv] == 0,
          "tick 1: the plant asks to divide upwards (gates %08x), the voxel above grants it in the "
          "same tick, both are busy, each pointing at the other", w.gates[pv]);
    CHECK(w.energy[cv] == e / 2 && (unsigned)(w.energy[pv] + w.energy[cv]) == e + gain - 1,
          "tick 1: the new cell gets half the parent's %u, the parent keeps the rest (%u + %u, "
          "gain %u, upkeep 1)", e, w.energy[pv], w.energy[cv], gain);
    /* the new cell's term: 256 + bits 3 .. 10 of its own voxel's dice of tick 1, e2338b1a */
    unsigned cterm = GAMMA_TERM_MIN + (xorshift32(crng) >> GAMMA_TERM_DICE & 255);
    CHECK(cterm == 355 && w.term[cv] == cterm && w.term[pv] == TERM_MAX - 1 &&
              w.ledger.granted == 1 && w.ledger.grants_by_dir[4] == 1 && w.ledger.split == e / 2,
          "tick 1: the new cell's term is 256 + 8 bits of its voxel's dice, %u (%u), the parent's "
          "counts down (%u); the ledger: 1 granted, to the asker below the voxel, split %llu",
          cterm, w.term[cv], w.term[pv], (unsigned long long)w.ledger.split);
    for (int t = 0; t < GAMMA_GATES; t++)
        gamma_step(&w);  /* ticks 2 .. 33: one gate a tick */
    uint32_t r = xorshift32(crng);  /* the new cell's dice rolled in tick 1 too */
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
    CHECK(w.term[cv] == cterm - 32 && w.term[pv] == TERM_MAX - 33,
          "both terms counted down through the division (%u, %u)", w.term[cv], w.term[pv]);
    gamma_step(&w);
    CHECK(!w.born[cv] && w.term[cv] == cterm - 33, "born lasts one tick");

    /* the new cell starves in the dark: no light, no minerals, 2 energy; far from it a cell with
       20 and a term of 2: both live two ticks and die in the third, each for its own reason */
    GammaWorld d;
    gamma_world_init(&d, 3, 3, 3);
    size_t v = gamma_voxel(&d, 2, 2, 4), u = gamma_voxel(&d, 6, 6, 4);
    for (size_t q = 0; q < d.n; q++)
        d.rng[q] = 7;
    put_cell(&d, v, g, 2, 7, TERM_MAX);
    put_cell(&d, u, g, 20, 7, 2);
    gamma_step(&d);
    gamma_step(&d);
    int was = d.alive[v] && d.alive[u] && d.term[u] == 0 && d.energy[u] == 18;
    gamma_step(&d);
    CHECK(was && !d.alive[v] && d.energy[v] == 0 && d.mineral[v] == GAMMA_RELEASE && !d.gates[v] &&
              d.term[v] == 0,
          "a cell with 2 energy and nothing to gain lives 2 ticks, dies in the third and gives back "
          "%u minerals (alive %d, minerals %u, term %u)", GAMMA_RELEASE, d.alive[v], d.mineral[v],
          d.term[v]);
    CHECK(was && !d.alive[u] && d.energy[u] == 0 && d.mineral[u] == GAMMA_RELEASE && !d.gates[u] &&
              d.term[u] == 0,
          "a cell with a term of 2 lives 2 ticks (its term reads 0 after the second) and dies in "
          "the third: minerals +%u, gates 0, term 0 (alive %d, minerals %u, gates %08x, term %u)",
          GAMMA_RELEASE, d.alive[u], d.mineral[u], d.gates[u], d.term[u]);
    CHECK(d.ledger.term == 1 && d.ledger.starved == 1 && d.ledger.aborted == 0 &&
              d.ledger.death == 17 && d.ledger.upkeep == 1 && d.ledger.cells_before == 2 &&
              d.ledger.cells_after == 0,
          "the ledger of that tick: 1 death by term, 1 starved, 17 lost in death (18 less the "
          "upkeep, which the starving cell did not pay), 2 cells before, 0 after (%llu %llu %llu "
          "%llu %llu)", (unsigned long long)d.ledger.term, (unsigned long long)d.ledger.starved,
          (unsigned long long)d.ledger.aborted, (unsigned long long)d.ledger.death,
          (unsigned long long)d.ledger.upkeep);

    /* the lot: two plants ask for the same voxel, from -x (direction 1 as the voxel sees it) and
       from below (direction 4). The voxel's dice of that tick, xorshift32 four times from 7,
       b6fc5245, give k = bits 11 .. 13 = 2, and the smaller d ^ k wins: 1 ^ 2 = 3 beats
       4 ^ 2 = 6, so the cell at -x gets it */
    uint16_t right[GAMMA_GATES];
    plant(right, 0);
    memset(d.alive, 0, d.n);
    size_t t = gamma_voxel(&d, 4, 4, 4), a = gamma_voxel(&d, 3, 4, 4), bl = gamma_voxel(&d, 4, 4, 5);
    put_cell(&d, a, right, 100, 7, TERM_MAX);
    put_cell(&d, bl, g, 100, 7, TERM_MAX);
    uint32_t dice = xorshift32(d.rng[t]);
    unsigned k = dice >> GAMMA_LOT_DICE & 7;
    gamma_step(&d);
    CHECK(dice == 0xb6fc5245 && k == 2 && d.embryo[t] && d.partner[t] == 1 && d.busy[a] &&
              !d.busy[bl] && d.request[bl] && d.ledger.grants_by_dir[1] == 1 && d.ledger.granted == 1,
          "two cells ask for one voxel: its dice %08x draw k = %u, the one at -x (1 ^ k = %u, "
          "against 4 ^ k = %u) gets it, the other keeps asking", dice, k, 1 ^ k, 4 ^ k);

    /* the new cell dies when its parent does: an abort, one in the ledger */
    d.alive[a] = 0;
    gamma_step(&d);
    CHECK(!d.alive[t] && d.term[t] == 0 && d.ledger.aborted == 1 && d.ledger.term == 0 &&
              d.ledger.starved == 0,
          "a new cell dies with its parent: 1 aborted in the ledger (%llu)",
          (unsigned long long)d.ledger.aborted);
    gamma_world_free(&d);

    /* a parent whose term is up in the tick it is granted: it still asks and is granted, gives
       its new cell half and dies that tick; the new cell, whose parent is gone, aborts the next */
    GammaWorld f;
    gamma_world_init(&f, 3, 3, 3);
    for (size_t q = 0; q < f.n; q++)
        f.rng[q] = 7;
    size_t p = gamma_voxel(&f, 5, 5, 4), c = gamma_voxel(&f, 5, 5, 3);
    put_cell(&f, p, g, 100, 7, 0);
    unsigned term0 = GAMMA_TERM_MIN + (xorshift32(7) >> GAMMA_TERM_DICE & 255);  /* 284 */
    gamma_step(&f);
    CHECK(term0 == 284 && !f.alive[p] && f.term[p] == 0 && f.energy[p] == 0 &&
              f.mineral[p] == GAMMA_RELEASE && !f.gates[p] && f.alive[c] && f.busy[c] &&
              f.embryo[c] && f.partner[c] == 4 && f.energy[c] == 50 && f.term[c] == term0,
          "a parent granted in the tick its term is up dies that tick (alive %d, term %u, minerals "
          "%u), its new cell is there with half, 50 (%u), and a term of %u (%u)", f.alive[p],
          f.term[p], f.mineral[p], f.energy[c], term0, f.term[c]);
    CHECK(f.ledger.granted == 1 && f.ledger.term == 1 && f.ledger.split == 50 &&
              f.ledger.death == 49 && f.ledger.cells_before == 1 && f.ledger.cells_after == 1 &&
              f.ledger.energy_before == 100 && f.ledger.energy_after == 50,
          "its ledger: 1 granted, 1 death by term, split 50, death 49 (100 less upkeep and the "
          "half), 1 cell before and after, energy 100 then 50 (%llu %llu %llu %llu)",
          (unsigned long long)f.ledger.granted, (unsigned long long)f.ledger.term,
          (unsigned long long)f.ledger.split, (unsigned long long)f.ledger.death);
    gamma_step(&f);
    CHECK(!f.alive[c] && f.term[c] == 0 && f.energy[c] == 0 && f.mineral[c] == 576 &&
              f.ledger.aborted == 1 && f.ledger.death == 49 && f.ledger.cells_after == 0,
          "the next tick the embryo aborts: 1 aborted, death 49, minerals 576 there (512 and an "
          "eighth of the parent's 512) (alive %d, minerals %u, aborted %llu)", f.alive[c],
          f.mineral[c], (unsigned long long)f.ledger.aborted);
    gamma_world_free(&f);
    gamma_world_free(&w);
}

/* energy given from cell to cell: a giver and a taker side by side, a chain of three, a gift to
   nobody, the cap; then midnight */
static void test_sharing(void)
{
    GammaWorld w;
    gamma_world_init(&w, 3, 3, 3);  /* no light, no minerals: gains are only gifts */
    uint16_t giver[GAMMA_GATES], taker[GAMMA_GATES];
    cell(giver, 0, 0, 1);  /* gives towards +x, never divides */
    cell(taker, 0, 0, 0);
    for (size_t v = 0; v < w.n; v++)
        w.rng[v] = 7;
    size_t a = gamma_voxel(&w, 1, 1, 1), b = gamma_voxel(&w, 2, 1, 1), c = gamma_voxel(&w, 3, 1, 1);
    size_t lone = gamma_voxel(&w, 5, 5, 5), full = gamma_voxel(&w, 6, 6, 6), fg = gamma_voxel(&w, 5, 6, 6);
    put_cell(&w, a, giver, 80, 7, TERM_MAX);
    put_cell(&w, b, giver, 40, 7, TERM_MAX);  /* takes from a, gives to c */
    put_cell(&w, c, taker, 40, 7, TERM_MAX);
    put_cell(&w, lone, giver, 80, 7, TERM_MAX);  /* nothing at its +x */
    gamma_step(&w);
    CHECK(w.offer[a] == 10 && w.energy[a] == 80 - 1 - 10,
          "a cell with 80 energy and the give gate on offers an eighth, 10, and keeps 69 (offer %u, "
          "energy %u)", w.offer[a], w.energy[a]);
    CHECK(w.offer[b] == 5 && w.energy[b] == 40 + 10 - 1 - 5 && w.offer[c] == 0 &&
              w.energy[c] == 40 + 5 - 1,
          "in the chain the middle cell takes 10 and gives 5 of its own 40 (%u), the last takes 5 "
          "(%u)", w.energy[b], w.energy[c]);
    CHECK(w.offer[lone] == 0 && w.energy[lone] == 79, "a gift to an empty voxel is not made (%u)",
          w.energy[lone]);
    CHECK(w.ledger.given == 15 && w.ledger.received == 15,
          "the ledger: 15 given, 15 received (%llu, %llu)", (unsigned long long)w.ledger.given,
          (unsigned long long)w.ledger.received);
    gamma_step(&w);
    CHECK(w.offer[a] == 8 && w.energy[a] == 69 - 1 - 8 && w.energy[c] == 44 + 5 - 1,
          "the next tick: an eighth of 69 is 8 (offer %u, energy %u), the last cell takes 5 again "
          "(%u)", w.offer[a], w.energy[a], w.energy[c]);
    /* the cap: a full cell takes nothing it cannot hold */
    put_cell(&w, full, taker, 255, 7, TERM_MAX);
    put_cell(&w, fg, giver, 200, 7, TERM_MAX);
    gamma_step(&w);
    CHECK(w.offer[fg] == 25 && w.energy[fg] == 200 - 1 - 25 && w.energy[full] == 254 &&
              w.ledger.cap == 25,
          "a cell at 255 is offered 25 and stays at the cap less upkeep (%u, the giver %u); the "
          "ledger: cap %llu", w.energy[full], w.energy[fg], (unsigned long long)w.ledger.cap);
    gamma_world_free(&w);

    /* midnight: the clock reads 255, the tick takes it to 0, and whatever a cell holds above 127
       after its accounting rots; a cell granted in that tick keeps its half */
    GammaWorld m;
    gamma_world_init(&m, 3, 3, 3);
    uint16_t pl[GAMMA_GATES];
    plant(pl, 5);
    for (size_t v = 0; v < m.n; v++) {
        m.rng[v] = 7;
        m.clock[v] = 255;
    }
    size_t rich = gamma_voxel(&m, 1, 1, 1), poor = gamma_voxel(&m, 2, 2, 2),
           pp = gamma_voxel(&m, 4, 4, 4), nc = gamma_voxel(&m, 4, 4, 3);
    put_cell(&m, rich, taker, 200, 7, TERM_MAX);
    put_cell(&m, poor, taker, 100, 7, TERM_MAX);
    put_cell(&m, pp, pl, 254, 7, TERM_MAX);  /* divides upwards: the new cell gets 127 */
    gamma_step(&m);
    int bad = 0;
    for (size_t v = 0; v < m.n; v++)
        bad += m.clock[v] != 0;
    CHECK(bad == 0 && m.energy[rich] == 127 && m.energy[poor] == 99 && m.alive[nc] &&
              m.energy[nc] == 127 && m.energy[pp] == 254 - 1 - 127 && m.ledger.rot == 72 &&
              m.ledger.granted == 1,
          "midnight (clock 0 in every voxel, %d wrong): a cell at 200 with no gains ends at 127, one "
          "at 100 at 99, a cell granted that tick keeps its half, 127, its parent 126 (%u %u %u %u); "
          "the ledger: rot 200 - 1 - 127 = 72 (%llu)", bad, m.energy[rich], m.energy[poor],
          m.energy[nc], m.energy[pp], (unsigned long long)m.ledger.rot);
    gamma_step(&m);
    CHECK(m.energy[rich] == 126 && m.energy[poor] == 98 && m.ledger.rot == 0 && m.clock[0] == 1,
          "the tick after midnight takes only the upkeep (%u, %u), nothing rots (%llu)",
          m.energy[rich], m.energy[poor], (unsigned long long)m.ledger.rot);
    gamma_world_free(&m);
}

/* ---- the social game (step 11f) ---------------------------------------------------------- */

/* the riddle and a day of credit, on the sun plane of an 8 x 8 x 8 world (light, so nobody
   starves): two talkers that point at each other solve every round; two guessers that answer
   their own token solve a round when the other's token is 0; a cell alone never answers. The
   need is 12: at midnight the talkers live, the lonely cell dies, the guessers as their count
   says */
static void test_riddle(void)
{
    GammaWorld w;
    gamma_world_init(&w, 3, 3, 3);
    for (size_t v = 0; v < w.n; v++)
        w.rng[v] = (uint32_t)(v * 2654435761u) | 1;
    w.need = 12;
    uint16_t ta[GAMMA_GATES], tb[GAMMA_GATES], ga[GAMMA_GATES], gb[GAMMA_GATES], lo[GAMMA_GATES];
    talker(ta, 0, 1);
    talker(tb, 1, 1);
    guesser(ga, 0);
    guesser(gb, 1);
    talker(lo, 0, 1);
    size_t a = gamma_voxel(&w, 1, 1, 0), b = gamma_voxel(&w, 2, 1, 0), c = gamma_voxel(&w, 5, 5, 0),
           d = gamma_voxel(&w, 6, 5, 0), l = gamma_voxel(&w, 1, 5, 0);
    put_cell(&w, a, ta, 200, w.rng[a], TERM_MAX);
    put_cell(&w, b, tb, 200, w.rng[b], TERM_MAX);
    put_cell(&w, c, ga, 200, w.rng[c], TERM_MAX);
    put_cell(&w, d, gb, 200, w.rng[d], TERM_MAX);
    put_cell(&w, l, lo, 200, w.rng[l], TERM_MAX);
    int rounds = 0, talk = 0, bad = 0, nc = 0, nd = 0;
    unsigned tokens = 0;
    uint64_t answered = 0, solved = 0;
    for (int t = 0; t < 255; t++) {  /* the clock 1 .. 255: the 16 rounds of the day */
        unsigned judge = ((w.clock[0] + 1u) & 15) == 15, tc = w.token[c], td = w.token[d];
        unsigned cr[5] = {w.credit[a], w.credit[b], w.credit[c], w.credit[d], w.credit[l]};
        unsigned tok = w.token[a];
        tokens |= 1u << tok;
        gamma_step(&w);
        answered += w.ledger.answered;
        solved += w.ledger.solved;
        if (judge) {
            rounds++;
            nc += td == 0;  /* c answers its own token, and must say tc ^ td */
            nd += tc == 0;
            talk += w.credit[a] == up(cr[0], 1) && w.credit[b] == up(cr[1], 1);
            bad += w.credit[c] != up(cr[2], td == 0) || w.credit[d] != up(cr[3], tc == 0) ||
                   w.credit[l] != 0;
            bad += w.token[a] != (w.rng[a] >> GAMMA_TOKEN_DICE & 1);  /* the next round's */
        } else {
            bad += w.credit[a] != cr[0] || w.credit[c] != cr[2] || w.token[a] != tok;
        }
    }
    int all = w.alive[a] && w.alive[b] && w.alive[c] && w.alive[d] && w.alive[l];
    CHECK(rounds == 16 && talk == 16 && bad == 0 && all && w.clock[0] == 255 &&
              w.credit[a] == GAMMA_CREDIT_MAX && tokens == 3,
          "the 16 rounds of a day: the talkers solve every one (%d), at most 15 credit (%u); the "
          "guessers' credit follows the other's token, the lone cell has none, the token changes "
          "only at a round's end, to bit 24 of the dice (%d wrong); tokens both ways (%u)", talk,
          w.credit[a], bad, tokens);
    CHECK(answered == 4 * 16 && solved == 2 * 16 + (uint64_t)(nc + nd),
          "the ledger: 64 answers (4 cells, 16 rounds; the lone cell points at nobody) (%llu), "
          "solved 32 + %d + %d (%llu)", (unsigned long long)answered, nc, nd,
          (unsigned long long)solved);
    gamma_step(&w);  /* midnight */
    CHECK(w.clock[0] == 0 && w.alive[a] && w.alive[b] && w.credit[a] == 0 && w.credit[b] == 0 &&
              !w.alive[l] && w.alive[c] == (nc >= 12) && w.alive[d] == (nd >= 12) &&
              w.ledger.lonely == 1u + (nc < 12) + (nd < 12),
          "midnight with need 12: the talkers live and start again at 0, the lone cell dies of "
          "loneliness, the guessers with %d and %d rounds %s and %s (lonely %llu)", nc, nd,
          nc >= 12 ? "live" : "die", nd >= 12 ? "live" : "die",
          (unsigned long long)w.ledger.lonely);
    gamma_world_free(&w);

    /* a new cell's credit is the rounds of the day past: granted with the clock at 201, 12; a
       round that ends while a cell divides counts, for the parent and the new cell alike (the
       clock at 207) */
    GammaWorld s;
    gamma_world_init(&s, 3, 3, 3);
    for (size_t v = 0; v < s.n; v++) {
        s.rng[v] = 7;
        s.clock[v] = 200;
    }
    uint16_t pl[GAMMA_GATES];
    plant(pl, 5);
    size_t pv = gamma_voxel(&s, 4, 4, 4), nv = gamma_voxel(&s, 4, 4, 3);
    put_cell(&s, pv, pl, 100, 7, TERM_MAX);
    s.credit[pv] = 3;
    gamma_step(&s);
    int born12 = s.alive[nv] && s.busy[nv] && s.credit[nv] == 12 && s.credit[pv] == 3 &&
                 s.credit[gamma_voxel(&s, 0, 0, 0)] == 0;
    for (int t = 0; t < 6; t++)
        gamma_step(&s);  /* the clock to 207: a round ends */
    CHECK(born12 && s.clock[0] == 207 && s.busy[pv] && s.credit[nv] == 13 && s.credit[pv] == 4 &&
              s.ledger.solved == 0,
          "a new cell granted at clock 201 starts with the 12 rounds past (%u); at the round's end "
          "both, dividing, are counted a round (%u, %u) that nobody solved", s.credit[nv] - 1,
          s.credit[nv], s.credit[pv]);
    gamma_world_free(&s);
}

/* predators: they swap within pairs and are never made or lost; they strike after their first
   tick, 1 in 4, unless the cell hides; the cells next to them see them, and an alarm saves a cell
   that listens */
static void test_predators(void)
{
    GammaWorld w;
    gamma_world_init(&w, 3, 2, 3);  /* 8 x 4 x 8, no cells */
    uint64_t s = 99;
    for (size_t v = 0; v < w.n; v++)
        w.rng[v] = (uint32_t)rnd(&s) | 1;
    size_t placed = gamma_predators(&w, 40, 3);
    uint8_t *before = malloc(w.n);
    int lost = 0, still = 0, far = 0, fresh = 0, moved = 0;
    for (int t = 0; t < 300; t++) {
        memcpy(before, w.pred, w.n);
        unsigned phase = (w.clock[0] + 1u) & 7;
        gamma_step(&w);
        size_t n = 0;
        for (size_t v = 0; v < w.n; v++) {
            n += w.pred[v];
            fresh += w.fresh[v] != (w.pred[v] & !before[v]);
            moved += w.pred[v] != before[v];
            if (phase >= 6)
                still += w.pred[v] != before[v];
            else if (w.pred[v] && !before[v]) {  /* it came from the pair's other voxel */
                unsigned ax = phase >> 1, x = (unsigned)v & 7, y = (unsigned)(v >> 3) & 3,
                         z = (unsigned)(v >> 5), c = ax == 0 ? x : ax == 1 ? y : z;
                int lower = (c & 1) == (phase & 1);
                far += !before[gamma_neighbour(&w, v, lower ? 2 * (int)ax : 2 * (int)ax + 1)];
            }
        }
        lost += n != placed;
    }
    CHECK(placed == 40 && lost == 0 && still == 0 && far == 0 && fresh == 0 && moved > 300,
          "40 predators for 300 ticks: never one more or less (%d), none moves when the clock's "
          "low bits are 6 or 7 (%d), each comes from its pair (%d), fresh = came this tick (%d), "
          "%d moves", lost, still, far, fresh, moved);
    free(before);

    /* the swap by hand: the clock's low bits 0 (axis x, pairs 2-3, 4-5, ..), one predator at x 2,
       so the lower voxel's dice decide; at x 3 the voxel at x 2 decides as well */
    memset(w.pred, 0, w.n);
    memset(w.clock, 7, w.n);
    size_t p2 = gamma_voxel(&w, 2, 1, 1), p3 = gamma_voxel(&w, 3, 1, 1), q5 = gamma_voxel(&w, 5, 1, 1),
           q4 = gamma_voxel(&w, 4, 1, 1);
    w.pred[p2] = 1;
    w.pred[q5] = 1;
    unsigned sw2 = xorshift32(w.rng[p2]) >> GAMMA_SWAP_DICE & 1,
             sw4 = xorshift32(w.rng[q4]) >> GAMMA_SWAP_DICE & 1;
    gamma_step(&w);
    CHECK(w.pred[p3] == sw2 && w.pred[p2] == !sw2 && w.fresh[p3] == sw2 && w.pred[q4] == sw4 &&
              w.pred[q5] == !sw4,
          "with the clock at 0 the pair x 2-3 swaps on x 2's dice (%u) and the pair 4-5 on x 4's "
          "(%u): a predator at x 5 goes down to 4 by the lower voxel's dice", sw2, sw4);
    gamma_world_free(&w);

    /* the strike: the clock's low bits 6 and 7 next (nobody moves), a predator in the voxel of
       an idle cell, of a hiding one, and a fresh one with an idle cell, all with dice that strike;
       the idle cell dies, the hiding one lives without light, the fresh predator waits */
    GammaWorld k;
    gamma_world_init(&k, 3, 3, 3);
    uint16_t idle[GAMMA_GATES], hider[GAMMA_GATES];
    cell(idle, 6, 0, 0);
    cell(hider, 6, 0, 0);
    hider[GAMMA_HIDE] = gate_of(32, 32);
    for (size_t v = 0; v < k.n; v++) {
        k.rng[v] = 7;
        k.clock[v] = 5;
        k.light[v] = 160;  /* 5 a tick, for a cell that does not hide */
    }
    size_t vi = gamma_voxel(&k, 1, 1, 4), vh = gamma_voxel(&k, 4, 1, 4), vf = gamma_voxel(&k, 6, 6, 4);
    uint32_t hit = dice_for(1000, 1, 3);
    put_cell(&k, vi, idle, 100, hit, TERM_MAX);
    put_cell(&k, vh, hider, 100, hit, TERM_MAX);
    put_cell(&k, vf, idle, 100, hit, TERM_MAX);
    k.rng[vi] = k.rng[vh] = k.rng[vf] = hit;
    k.pred[vi] = k.pred[vh] = k.pred[vf] = 1;
    k.fresh[vf] = 1;
    unsigned lit = k.light[vh] >> 5;
    gamma_step(&k);
    CHECK(!k.alive[vi] && k.mineral[vi] >= GAMMA_RELEASE && k.alive[vh] && k.energy[vh] == 99 &&
              k.alive[vf] && k.energy[vf] == 100 + lit - 1 && k.pred[vi] && k.pred[vh] &&
              k.ledger.killed == 1 && k.ledger.hidden == 1 && k.ledger.strikes == 2,
          "a predator strikes (dice %08x): the idle cell dies, the hiding one lives on 99 (no light "
          "while hiding), the fresh predator does not strike (%u); killed %llu, hidden %llu, "
          "strikes %llu", xorshift32(hit), k.energy[vf], (unsigned long long)k.ledger.killed,
          (unsigned long long)k.ledger.hidden, (unsigned long long)k.ledger.strikes);
    gamma_world_free(&k);

    /* sight and the alarm: a fresh predator comes into the voxel of a listener, which hides when
       the cell at its -x says so on signal 0; the watcher there says what it sees (sense 27, a
       predator next to it). Tick 1: the predator is fresh, the watcher sees it, the listener,
       which does not see it in its own voxel, hides not; tick 2: the listener hears it and hides,
       and the strike misses. The same without the watcher: the listener dies. */
    for (int with = 0; with < 2; with++) {
        GammaWorld a;
        gamma_world_init(&a, 3, 3, 3);
        uint16_t watcher[GAMMA_GATES], listener[GAMMA_GATES];
        cell(watcher, 6, 0, 0);
        watcher[1] = gate_of(27, 27);                    /* !seen */
        watcher[GAMMA_SIGNAL] = gate_of(33, 33);         /* signal 0 = seen */
        cell(listener, 6, 0, 0);
        listener[1] = gate_of(12, 12);                   /* !(signal 0 of the cell at -x) */
        listener[GAMMA_HIDE] = gate_of(33, 33);
        for (size_t v = 0; v < a.n; v++) {
            a.rng[v] = 7;
            a.clock[v] = 5;
        }
        size_t lv = gamma_voxel(&a, 4, 4, 4), wv = gamma_voxel(&a, 3, 4, 4);
        put_cell(&a, lv, listener, 100, dice_for(2000, 2, 3), TERM_MAX);
        if (with)
            put_cell(&a, wv, watcher, 100, 7, TERM_MAX);
        uint32_t r1 = xorshift32(a.rng[lv]);
        a.pred[lv] = 1;
        a.fresh[lv] = 1;
        gamma_step(&a);
        int t1 = a.alive[lv] && (!with || (a.gates[wv] >> GAMMA_SIGNAL & 1)) &&
                 !(a.gates[lv] >> GAMMA_HIDE & 1) && !a.fresh[lv] && a.pred[lv];
        gamma_step(&a);
        if (with)
            CHECK(t1 && a.alive[lv] && (a.gates[lv] >> GAMMA_HIDE & 1) && a.ledger.strikes == 1 &&
                      a.ledger.killed == 0,
                  "a watcher next to the voxel sees the fresh predator and says so; the next tick "
                  "the cell there hides and the strike (dice %08x) misses", xorshift32(r1));
        else
            CHECK(t1 && !a.alive[lv] && a.ledger.killed == 1,
                  "without the watcher the same cell hears nothing and is killed");
        gamma_world_free(&a);
    }
}

/* a soup of random genomes gets going by itself: cells divide and some finish being born */
static void test_soup(void)
{
    GammaWorld w;
    gamma_world_init(&w, 4, 4, 4);
    for (int t = 0; t < 300; t++)  /* the fields first */
        gamma_step(&w);
    size_t seeded = gamma_soup(&w, 0.3, 64, 5), born = 0, most = 0;
    int bad = 0;
    for (size_t v = 0; v < w.n; v++)
        bad += w.clock[v] != GAMMA_SUNRISE ||
               (w.alive[v] ? w.term[v] < 1 || w.term[v] > TERM_MAX : w.term[v] != 0) ||
               w.credit[v] != (w.alive[v] ? GAMMA_SUNRISE >> GAMMA_ROUND_BITS : 0);
    CHECK(bad == 0, "the soup goes in at sunrise, every cell with a term of 1 .. 511 and a credit "
          "of 4 (the rounds before sunrise), every empty voxel with 0 and 0 (%d wrong)", bad);
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

/* the ledger on a running soup, after every tick: its sums against the arrays, the two
   identities (as gamma_ledger_closes says and as summed here), received = given, no living cell
   past 511 and no empty voxel with a term, one clock in every voxel */
static void test_ledger(int xb, int yb, int zb, int ticks, uint64_t seed, unsigned need,
                        size_t predators)
{
    GammaWorld w;
    gamma_world_init(&w, xb, yb, zb);
    for (int t = 0; t < 300; t++)  /* the fields first */
        gamma_step(&w);
    size_t seeded = gamma_soup(&w, 0.3, 64, seed);
    gamma_predators(&w, predators, seed);
    w.need = need;
    GammaLedger sum = {0};
    int sums = 0, closes = 0, identities = 0, gifts = 0, terms = 0, clocks = 0;
    for (int t = 0; t < ticks; t++) {
        uint64_t cells = 0, energy = 0, cells2 = 0, energy2 = 0, dirs = 0;
        unsigned clock = w.clock[0];
        for (size_t v = 0; v < w.n; v++)
            if (w.alive[v]) {
                cells++;
                energy += w.energy[v];
            }
        gamma_step(&w);
        const GammaLedger *l = &w.ledger;
        for (size_t v = 0; v < w.n; v++) {
            if (w.alive[v]) {
                cells2++;
                energy2 += w.energy[v];
            }
            terms += w.alive[v] ? w.term[v] > TERM_MAX : w.term[v] != 0;
            clocks += w.clock[v] != ((clock + 1) & 255);
        }
        for (int d = 0; d < 6; d++)
            dirs += l->grants_by_dir[d];
        sums += l->cells_before != cells || l->cells_after != cells2 ||
                l->energy_before != energy || l->energy_after != energy2 || dirs != l->granted;
        closes += !gamma_ledger_closes(l);
        identities += (int64_t)(energy2 - energy) !=
                          (int64_t)(l->light + l->minerals) -
                              (int64_t)(l->cap + l->upkeep + l->rot + l->death) ||
                      (int64_t)(cells2 - cells) !=
                          (int64_t)l->granted - (int64_t)(l->term + l->starved + l->killed +
                                                          l->lonely + l->aborted);
        gifts += l->received != l->given;
        gamma_ledger_add(&sum, l);
    }
    CHECK(sums == 0 && closes == 0 && identities == 0 && gifts == 0,
          "%u x %u x %u from a soup of %zu, %d ticks: the ledger's sums against the arrays (%d "
          "ticks wrong), both identities as gamma_ledger_closes says (%d) and as summed here (%d), "
          "received = given (%d)", 1u << xb, 1u << yb, 1u << zb, seeded, ticks, sums, closes,
          identities, gifts);
    CHECK(terms == 0 && clocks == 0, "%u x %u x %u, %d ticks: no living cell past 511 and no empty "
          "voxel with a term (%d), the clock the same everywhere and counting (%d)", 1u << xb,
          1u << yb, 1u << zb, ticks, terms, clocks);
    CHECK(gamma_ledger_closes(&sum) && sum.granted > 0 && sum.term > 0 && sum.rot > 0 &&
              (!need || sum.lonely > 0) && (!predators || sum.killed > 0),
          "%u x %u x %u, %d ticks, need %u, %zu predators: the ledgers added up close too, with "
          "births, deaths by term and midnights that took something: %llu granted, %llu died of "
          "their term, %llu starved, %llu killed, %llu lonely, %llu aborted, %llu rotted at "
          "midnight, %llu given", 1u << xb, 1u << yb, 1u << zb, ticks, need, predators,
          (unsigned long long)sum.granted, (unsigned long long)sum.term,
          (unsigned long long)sum.starved, (unsigned long long)sum.killed,
          (unsigned long long)sum.lonely, (unsigned long long)sum.aborted,
          (unsigned long long)sum.rot, (unsigned long long)sum.given);
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
    {   /* every bit-for-bit check below relies on gamma_compare seeing a clock or a term differ */
        GammaWorld cp;
        const char *w1, *w2;
        gamma_world_init(&cp, xb, yb, zb);
        gamma_world_copy(&cp, &want);
        cp.term[5] ^= 1;
        long t_at = gamma_compare(&want, &cp, &w1);
        cp.term[5] ^= 1;
        cp.clock[6] ^= 1;
        long c_at = gamma_compare(&want, &cp, &w2);
        cp.clock[6] ^= 1;
        static const char *const part[4] = {"token", "credit", "predator", "fresh"};
        int seen = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t *f = i == 0 ? cp.token : i == 1 ? cp.credit : i == 2 ? cp.pred : cp.fresh;
            const char *w3;
            f[7] ^= 1;
            seen += gamma_compare(&want, &cp, &w3) == 7 && !strcmp(w3, part[i]);
            f[7] ^= 1;
        }
        CHECK(t_at == 5 && c_at == 6 && !strcmp(w1, "term") && !strcmp(w2, "clock") && seen == 4,
              "gamma_compare sees a term, a clock, a token, a credit, a predator and a fresh one "
              "that differ (%ld %s, %ld %s, %d of 4)", t_at, w1, c_at, w2, seen);
        gamma_world_free(&cp);
    }
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

enum { FROM_RANDOM, FROM_SOUP, FROM_SHORT_TERMS, FROM_GAME };  /* what compiled() starts from */
static const char *const from_names[] = {"random", "a soup at clock 200", "random with terms 0 .. 3",
                                         "a soup at clock 200 with predators and need 8"};

/* compiled code on one thread, or `threads` threads in lockstep, against plain C, tick by tick,
   from a random world, from a soup with midnight 56 ticks away, or from a random world whose every
   term is 0 .. 3, so that every cell dies of its term in the first ticks */
static void compiled(int xb, int yb, int zb, int ticks, int threads, int from, uint64_t *s)
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
    if (from == FROM_SOUP || from == FROM_GAME) {
        for (int t = 0; t < 1 << zb && t < 64; t++)
            gamma_step(&want);
        gamma_soup(&want, 0.25, 80, rnd(s));
        memset(want.clock, 200, want.n);
        if (from == FROM_GAME) {
            gamma_predators(&want, want.n / 16, rnd(s));
            want.need = 8;
        }
    } else {
        random_world(&want, s);
        if (from == FROM_SHORT_TERMS)
            for (size_t v = 0; v < want.n; v++)
                want.term[v] &= 3;
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
          "tick %d (%s; %u segments)", 1u << xb, 1u << yb, 1u << zb, from_names[from], threads,
          threads > 1 ? "s" : "", ticks, bad, t, bad < 0 ? "" : what, jc.nseg);
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
    test_sharing();
    test_riddle();
    test_predators();
    test_soup();
    test_ledger(3, 3, 3, 600, 5, 0, 0);
    test_ledger(4, 4, 4, 300, 6, 0, 0);
    test_ledger(4, 4, 4, 600, 7, 6, 64);
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
        /* x, y, z bits, ticks, threads, from (FROM_RANDOM, FROM_SOUP, FROM_SHORT_TERMS) */
        {3, 3, 3, 40, 1, 0},  {3, 3, 3, 300, 1, 1}, {3, 3, 3, 40, 1, 2}, {2, 3, 5, 40, 1, 0},
        {2, 3, 5, 300, 1, 1}, {5, 5, 5, 12, 1, 0},  {5, 5, 5, 60, 1, 1}, {6, 5, 5, 6, 1, 0},
        {1, 1, 9, 30, 1, 0},  {9, 6, 1, 6, 1, 0},   {4, 8, 2, 10, 1, 0}, {5, 5, 5, 40, 5, 1},
        {2, 3, 5, 60, 3, 0},  {2, 3, 5, 20, 3, 2},  {3, 3, 3, 300, 1, 3}, {5, 5, 5, 80, 1, 3},
        {2, 3, 5, 300, 3, 3},
    };
    for (size_t i = 0; i < sizeof shape / sizeof shape[0]; i++)
        compiled(shape[i][0], shape[i][1], shape[i][2], shape[i][3], shape[i][4], shape[i][5], &s);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
