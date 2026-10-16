/* Gamma (step 11): a world of cells on the cube, built from NAND/STO like beta. docs/GAMMA.md has
   the plan.

   The world is a 3-D torus of 2^xbits x 2^ybits x 2^zbits voxels, one voxel per machine. Each axis
   sits in its own dimensions in Gray code, x lowest, so all 6 face neighbours are one link away.
   z = 0 is the sun plane at the top, z = 2^zbits - 1 the floor. Directions d = 0 .. 5 are +x, -x,
   +y, -y, +z (down), -z (up); d ^ 1 is the opposite one; 6 and 7 mean none.

   The fields (step 11a). Light, 0 .. 255, falls one voxel per tick: the sun plane gets the sun
   (below), every other voxel the light of the voxel above, of which an empty voxel passes on
   l - l/8 and one with a cell in it l/2, rounded down. Minerals, 0 .. 4095 (8 bits a cell sees and
   4 more below the point, so diffusion does not stop where amounts get small), diffuse up from the
   floor: the floor gets 4095, the sun plane 0, every other voxel (2 m + its 6 neighbours) / 8,
   rounded down, less what a cell eats there, plus what a dying one gives back, at most 4095.

   The cells (step 11b). A voxel is empty or holds a cell: alive, its energy (0 .. 255), its genome,
   the outputs of its gates, and a random generator (xorshift32) that every voxel has. The genome is
   32 NAND gates; gate i has two 6-bit addresses (bits 0-5 and 6-11). An address picks one of 64
   sources: 0 .. 31 the senses below, 32 + j gate j. The gates run in order every tick, each writing
   its output in place, so a gate that reads itself or a later gate gets last tick's value.

   Senses: 0 a constant 1; 1-3 three random bits; 4 + d the neighbour in direction d holds a cell;
   10 + 2d, 11 + 2d that neighbour's gates 26 and 27 (its signals, as they were at the end of the
   last tick); 22-24 the top 3 bits of the cell's energy, 25-27 of the light and 28-30 of the
   minerals where it is; 31 the cell finished being born last tick. Gate 25 gives energy, gate 28
   asks to divide, gates 29-31 give the direction for both (29 lowest).

   Division is physics (step 11b). A cell asks when it is not dividing already and has energy 32
   or more; an empty voxel grants one of the cells asking for it, by lot (below), and the cell
   learns it in the same tick (step 11c: the gates run first, then the asking, then the grants, all
   in one tick). Then both are busy for 32 ticks: every tick the parent's genome turns by one gate
   and the new cell takes that gate in at its end, so after 32 ticks it holds the parent's genome,
   each gate flipped in one bit with probability 1/64 on the way. A busy cell does not run its
   gates. The new cell gets half the parent's energy and dies if its parent does.

   Energy: every tick a cell gains light/32 + minerals/512 (whole units, 0 .. 7 each; it eats
   minerals/512 * 64 of the minerals there) and what it is given, at most 255, and pays 1. It dies
   with nothing left (starves), or when its term is up (below), and gives back 512 minerals.

   Sharing (step 11c): a cell that is not dividing and has gate 25 on offers an eighth of its energy
   (what it had at the start of the tick) to the cell in its direction, if there is one; the
   receiver gets it this tick, as a gain, the giver loses it. What does not fit under 255 is lost.

   Weather (step 11d), from the three books:
   - Day and night. Every voxel holds a clock, 8 bits. It is incremented at the start of every
     tick, before anything else, and wraps after 256 ticks: a day. It is the same in every voxel
     because it starts the same (the soup sets it to 64, sunrise; a new world has 0). The sun
     plane's light is a triangle wave of the clock after the increment: 2 * (clock < 128 ? clock :
     255 - clock), 0 .. 254; bit 0 is 0, bit i (1 .. 7) is clock bit i-1 XOR clock bit 7. Below
     the sun plane light falls as before. Minerals know no day.
   - Manna. At midnight, in the tick in which the clock reads 0 after the increment, whatever
     energy a cell holds above 127 after the tick's accounting is lost: it rots. Upkeep stays 1.
   - An appointed term. Every cell carries a term, 9 bits. A cell dies in the tick in which its
     term reads 0 at the start of the tick (the term is up), the same way as a starving one.
     Otherwise the term goes down by one every tick. In the tick a cell is granted its term is set
     from its own voxel's dice of that tick: 256 + (bits 3 .. 10 of the dice), 256 .. 511 ticks. A
     soup cell draws its term, 1 .. 511, from the soup's generator. An empty voxel's term is 0, and
     so is a dying cell's after the tick. There is no death by chance any more.
   - The lot. When cells ask for the same empty voxel, the voxel draws k, bits 11 .. 13 of its own
     dice of that tick, and grants the asker in direction d with the smallest d XOR k. For two
     askers that is a fair coin; 11b and 11c gave the voxel to the lowest d.
   - The measure (plain C only). gamma_step fills a ledger of the tick (GammaLedger): what the
     cells gained, paid, moved and lost, and the births and deaths by cause. Two identities must
     hold in every tick, and the tests check them on running worlds:
       energy after - energy before = light + minerals - cap - upkeep - rot - death
       cells after - cells before = granted - term - starved - aborted
     with energy and cells summed over the voxels that hold a cell (before: at the start, after:
     at the end), light, minerals and received the gains before the cap, cap what did not fit under
     255, upkeep 1 per cell that pays (a starving cell pays nothing: it had nothing), rot what the
     midnight took, death what a dying cell would otherwise have kept (after its upkeep, split and
     gift), split what granted parents gave their new cells (and the new cells got), given what the
     givers offered (and the receivers got: received = given). A death has one cause: term if the
     term was up, else starved, else aborted (a new cell whose parent died).

   gamma_step() is all of this in plain C: the definition. gen_gamma() is the same tick as one
   NAND/STO program for the whole cube; the two are checked against each other bit for bit. The
   order of a tick, in both: the clock, the dice, the senses and the gates of every cell, what it
   asks and offers; the grants; then for every voxel the energy, life and death, the genome, the
   fields; and last the gates of dead and new cells are cleared. */
#ifndef ALPHA_GAMMA_H
#define ALPHA_GAMMA_H

#include <stddef.h>
#include <stdint.h>

#include "isa.h"
#include "lanes.h"

enum {
    GAMMA_LIGHT_BITS = 8, GAMMA_LIGHT_MAX = (1 << GAMMA_LIGHT_BITS) - 1,
    GAMMA_MINERAL_FRAC = 4, GAMMA_MINERAL_BITS = 8 + GAMMA_MINERAL_FRAC,
    GAMMA_MINERAL_MAX = (1 << GAMMA_MINERAL_BITS) - 1,
    GAMMA_GATES = 32, GAMMA_SENSES = 32, GAMMA_ADDR_BITS = 6, GAMMA_GATE_BITS = 2 * GAMMA_ADDR_BITS,
    GAMMA_ENERGY_BITS = 8, GAMMA_ENERGY_MAX = (1 << GAMMA_ENERGY_BITS) - 1,
    GAMMA_DIVIDE_MIN = 32,         /* energy needed to ask */
    GAMMA_GIVE = 25,               /* gate 25: give an eighth of the energy to the cell in the direction */
    GAMMA_SIGNAL = 26,             /* gates 26, 27: what the neighbours see */
    GAMMA_DIVIDE = 28,             /* gate 28: divide; 29, 30, 31: the direction */
    GAMMA_OFFER_BITS = GAMMA_ENERGY_BITS - 3,
    GAMMA_MUTATION_BITS = 6,       /* a mutation per gate copied: 6 random bits all 1, 1/64 */
    GAMMA_RELEASE = 512,           /* minerals a dying cell gives back */
    GAMMA_CLOCK_BITS = 8,          /* a day is 256 ticks */
    GAMMA_SUNRISE = 64,            /* the clock when the soup goes in */
    GAMMA_OMER = 127,              /* what a cell may keep past midnight */
    GAMMA_TERM_BITS = 9,           /* a new cell's term: 256 + 8 dice bits */
    GAMMA_TERM_MIN = 1 << (GAMMA_TERM_BITS - 1),
    GAMMA_TERM_DICE = 3,           /* dice bits 3 .. 10 of the new cell's voxel */
    GAMMA_LOT_DICE = 11            /* dice bits 11 .. 13 of the empty voxel: the lot */
};

/* ---- the world in plain C ----------------------------------------------------------------- */

/* The ledger of one tick: energy in whole units, cells and deaths in cells, all summed over the
   world (see the header comment for the two identities). grants_by_dir[d]: grants won by the
   asker in direction d of the empty voxel. */
typedef struct {
    uint64_t cells_before, cells_after, energy_before, energy_after;
    uint64_t light, minerals, received, given, cap, upkeep, rot, death, split;
    uint64_t granted, term, starved, aborted;
    uint64_t grants_by_dir[6];
} GammaLedger;

typedef struct GammaWorld {
    int       xbits, ybits, zbits;
    size_t    n;  /* voxels; voxel v = (z << (xbits + ybits)) | (y << xbits) | x */
    uint8_t  *alive, *busy, *embryo;  /* busy: dividing, as the parent or (embryo) the new cell */
    uint8_t  *partner;                /* while busy: the direction of the other one */
    uint8_t  *counter;                /* while busy: gates copied, 0 .. 31 */
    uint8_t  *energy, *born;
    uint8_t  *request, *offer;        /* what the cell asked and offered in the last tick */
    uint8_t  *clock;                  /* the day's clock, the same in every voxel */
    uint16_t *term;                   /* ticks a cell still has, 0 for an empty voxel */
    uint16_t *genome;                 /* GAMMA_GATES per voxel: gate i of v at genome[32 v + i] */
    uint32_t *gates, *rng;
    uint8_t  *light;
    uint16_t *mineral;
    struct GammaWorld *spare;         /* the next state, for gamma_step */
    uint8_t  *grant;
    GammaLedger ledger;               /* of the last gamma_step */
} GammaWorld;

int    gamma_world_init(GammaWorld *w, int xbits, int ybits, int zbits);  /* all 0; 0 if no memory */
void   gamma_world_free(GammaWorld *w);
void   gamma_world_copy(GammaWorld *dst, const GammaWorld *src);  /* same size; the state only */
size_t gamma_voxel(const GammaWorld *w, unsigned x, unsigned y, unsigned z);
size_t gamma_neighbour(const GammaWorld *w, size_t v, int d);
void   gamma_step(GammaWorld *w);  /* one tick; fills w->ledger */
unsigned gamma_sun(unsigned clock);  /* the sun plane's light for a clock value */

/* Every voxel's generator from seed (never 0); with probability `fraction` a cell with a random
   genome and `energy`, then one draw per cell for its term, in voxel order, after all the genomes
   (so a seed gives the 11c soup, with terms); the clock at sunrise in every voxel. The fields are
   left as they are. Returns the cells made. */
size_t gamma_soup(GammaWorld *w, double fraction, unsigned energy, uint64_t seed);

/* Adds the ledger of one tick to a sum; 1 if the two identities hold for `l`. */
void   gamma_ledger_add(GammaLedger *sum, const GammaLedger *l);
int    gamma_ledger_closes(const GammaLedger *l);

/* ---- the world as a NAND/STO program ------------------------------------------------------ */

enum {  /* what one tick spends its instructions on, in this order */
    GAMMA_PART_WHERE, GAMMA_PART_CLOCK, GAMMA_PART_DICE, GAMMA_PART_READS, GAMMA_PART_BRAIN,
    GAMMA_PART_ASK, GAMMA_PART_GRANTS, GAMMA_PART_PARTNER, GAMMA_PART_STATE, GAMMA_PART_COPY,
    GAMMA_PART_LIGHT, GAMMA_PART_MINERALS, GAMMA_PART_REGISTERS, GAMMA_PARTS
};
extern const char *const gamma_part_names[GAMMA_PARTS];

/* Where a voxel's state lives in each machine. request and offer are written during the tick;
   signal holds gates 26 and 27 as they were at the end of the last tick, for the neighbours;
   clock and term are counted in place. */
typedef struct {
    int      xbits, ybits, zbits;
    uint32_t alive, busy, embryo, request, born, signal[2];
    uint32_t partner[3], counter[5], energy[GAMMA_ENERGY_BITS], offer[GAMMA_OFFER_BITS];
    uint32_t clock[GAMMA_CLOCK_BITS], term[GAMMA_TERM_BITS];
    uint32_t genome[GAMMA_GATES][GAMMA_GATE_BITS], gates[GAMMA_GATES], rng[32];
    uint32_t light[GAMMA_LIGHT_BITS], mineral[GAMMA_MINERAL_BITS];
    size_t   cost[GAMMA_PARTS];  /* instructions */
} GammaCells;

/* Each axis 1 .. 9 bits, 9 .. 16 in all (a cube of 512 .. 65,536 machines). Returns 0 otherwise. */
int      gen_gamma(Program *p, int xbits, int ybits, int zbits, GammaCells *cells);
unsigned gamma_machine(const GammaCells *c, unsigned x, unsigned y, unsigned z);

/* The whole world into and out of a cube running gen_gamma's program. */
void gamma_to_cube(LaneCube *lc, const GammaCells *c, const GammaWorld *w);
void gamma_from_cube(const LaneCube *lc, const GammaCells *c, GammaWorld *w);

/* The first voxel where two worlds differ, or -1; *what names the part. */
long gamma_compare(const GammaWorld *a, const GammaWorld *b, const char **what);

#endif
