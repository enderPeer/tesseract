/* Gamma (step 11): a world of cells on the cube, built from NAND/STO like beta. docs/GAMMA.md has
   the plan; this is step 11a, the world without the cells.

   The world is a 3-D torus of 2^xbits x 2^ybits x 2^zbits voxels, one voxel per machine. Each axis
   sits in its own dimensions in Gray code, x lowest, so all 6 face neighbours are one link away.
   z = 0 is the sun plane at the top, z = 2^zbits - 1 the floor. Every voxel has
   - occ: something is there (set by the host in 11a; the cells of 11b will set it),
   - light, 0 .. 255,
   - minerals, 0 .. 4095: 8 bits a cell will see (0 .. 255) and 4 more below the point. With whole
     numbers the rounding down stops the diffusion about 8 layers above the floor; with 4 more
     bits the minerals reach the whole depth.

   One tick, every voxel at once, from the values before the tick:
   - light: the sun plane gets 255. Every other voxel gets the light of the voxel above it, of which
     an empty voxel passes on l - l/8 and an occupied one l/2, rounded down. So light falls one
     voxel per tick, and shade forms under whatever is there.
   - minerals: the floor gets 4095 and the sun plane 0. In between a voxel gets
     (2 m + the sum of its 6 neighbours) / 8, rounded down: they diffuse up from the floor.

   gamma_step() is the world in plain C, the definition. gen_gamma() is the same tick as one NAND/STO
   program for the whole cube; the two are checked against each other bit for bit. */
#ifndef ALPHA_GAMMA_H
#define ALPHA_GAMMA_H

#include <stddef.h>
#include <stdint.h>

#include "isa.h"
#include "lanes.h"

enum {
    GAMMA_LIGHT_BITS = 8, GAMMA_LIGHT_MAX = (1 << GAMMA_LIGHT_BITS) - 1,
    GAMMA_MINERAL_FRAC = 4, GAMMA_MINERAL_BITS = 8 + GAMMA_MINERAL_FRAC,
    GAMMA_MINERAL_MAX = (1 << GAMMA_MINERAL_BITS) - 1
};

/* ---- the world in plain C ----------------------------------------------------------------- */

typedef struct {
    int       xbits, ybits, zbits;
    size_t    n;          /* voxels; voxel i = (z << (xbits + ybits)) | (y << xbits) | x */
    uint8_t  *occ, *light;
    uint16_t *mineral;
    uint8_t  *next_light;  /* scratch for gamma_step */
    uint16_t *next_mineral;
} GammaWorld;

int    gamma_world_init(GammaWorld *w, int xbits, int ybits, int zbits);  /* all 0; 0 if no memory */
void   gamma_world_free(GammaWorld *w);
size_t gamma_voxel(const GammaWorld *w, unsigned x, unsigned y, unsigned z);
void   gamma_step(GammaWorld *w);  /* one tick */

/* ---- the world as a NAND/STO program ------------------------------------------------------ */

/* Where a voxel's state lives in each machine, and what the parts of one tick cost. */
typedef struct {
    int      xbits, ybits, zbits;
    uint32_t occ;  /* an input: nothing writes it */
    uint32_t light[GAMMA_LIGHT_BITS], mineral[GAMMA_MINERAL_BITS];  /* registers */
    size_t   cost_masks, cost_reads, cost_light, cost_minerals, cost_registers;  /* instructions */
} GammaCells;

/* Each axis 1 .. 9 bits, 9 .. 16 in all (a cube of 512 .. 65,536 machines). Returns 0 otherwise. */
int      gen_gamma(Program *p, int xbits, int ybits, int zbits, GammaCells *cells);
unsigned gamma_machine(const GammaCells *c, unsigned x, unsigned y, unsigned z);

/* The whole world into and out of a cube running gen_gamma's program. */
void gamma_to_cube(LaneCube *lc, const GammaCells *c, const GammaWorld *w);
void gamma_from_cube(const LaneCube *lc, const GammaCells *c, GammaWorld *w);

#endif
