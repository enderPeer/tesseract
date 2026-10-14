#include "gamma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "gen.h"

/* ---- the world in plain C ----------------------------------------------------------------- */

int gamma_world_init(GammaWorld *w, int xbits, int ybits, int zbits)
{
    memset(w, 0, sizeof *w);
    w->xbits = xbits;
    w->ybits = ybits;
    w->zbits = zbits;
    w->n = (size_t)1 << (xbits + ybits + zbits);
    w->occ = calloc(w->n, 1);
    w->light = calloc(w->n, 1);
    w->mineral = calloc(w->n, sizeof *w->mineral);
    w->next_light = malloc(w->n);
    w->next_mineral = malloc(w->n * sizeof *w->next_mineral);
    if (w->occ && w->light && w->mineral && w->next_light && w->next_mineral)
        return 1;
    gamma_world_free(w);
    return 0;
}

void gamma_world_free(GammaWorld *w)
{
    free(w->occ);
    free(w->light);
    free(w->mineral);
    free(w->next_light);
    free(w->next_mineral);
    memset(w, 0, sizeof *w);
}

size_t gamma_voxel(const GammaWorld *w, unsigned x, unsigned y, unsigned z)
{
    return (size_t)z << (w->xbits + w->ybits) | (size_t)y << w->xbits | x;
}

void gamma_step(GammaWorld *w)
{
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits, Z = 1u << w->zbits;
    uint8_t *light = w->next_light;
    uint16_t *mineral = w->next_mineral;
    const uint16_t *m = w->mineral;
    for (unsigned z = 0; z < Z; z++)
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++) {
                size_t i = gamma_voxel(w, x, y, z), up = gamma_voxel(w, x, y, (z + Z - 1) % Z);
                unsigned l = w->light[up];
                light[i] = (uint8_t)(z == 0 ? GAMMA_LIGHT_MAX : w->occ[up] ? l / 2 : l - l / 8);
                unsigned s = 2u * m[i] + m[up] + m[gamma_voxel(w, x, y, (z + 1) % Z)] +
                             m[gamma_voxel(w, (x + 1) % X, y, z)] +
                             m[gamma_voxel(w, (x + X - 1) % X, y, z)] +
                             m[gamma_voxel(w, x, (y + 1) % Y, z)] +
                             m[gamma_voxel(w, x, (y + Y - 1) % Y, z)];
                mineral[i] = (uint16_t)(z == Z - 1 ? GAMMA_MINERAL_MAX : z == 0 ? 0 : s / 8);
            }
    memcpy(w->light, light, w->n);
    memcpy(w->mineral, mineral, w->n * sizeof *mineral);
}

/* ---- the world as a NAND/STO program ------------------------------------------------------ */

static uint32_t cellf(Builder *b, const char *fmt, int i)
{
    char name[32];
    snprintf(name, sizeof name, fmt, i);
    return b_cell(b, name);
}

/* acc[0 .. *w-1] (at most *max) += x[0 .. n-1] << shift, for shift <= *w. The sum gets as many
   bits as its largest possible value needs, so the last carry is always 0. New bits of acc are
   cells named tag0, tag1, ...; x is only read (it may be a register its neighbours still read). */
static void add_into(Builder *b, uint32_t *acc, int *w, unsigned *max, const uint32_t *x, int n,
                     int shift, const char *tag)
{
    char name[32];
    uint32_t carry[2] = {b_cell(b, "ad_c"), b_cell(b, "ad_d")}, zero = CELL_ZERO;
    unsigned top = *max + (((1u << n) - 1) << shift);
    int nw = 0, c = -1;  /* carry[c] holds the carry into bit i; -1: there is none */
    while (top >> nw)
        nw++;
    for (int i = shift; i < nw; i++) {
        uint32_t in[3];
        int k = 0;
        if (i < *w) {
            in[k++] = acc[i];
        } else {
            snprintf(name, sizeof name, "%s%d", tag, i);
            acc[i] = b_cell(b, name);
        }
        if (i - shift < n)
            in[k++] = x[i - shift];
        if (c >= 0)
            in[k++] = carry[c];
        if (k == 3) {
            b_full_adder(b, acc[i], carry[c], in[0], in[1], in[2]);
        } else if (k == 2) {  /* the carry out must not be an input */
            c = c >= 0 ? 1 - c : 0;
            b_half_adder(b, acc[i], carry[c], in[0], in[1]);
        } else {
            if (k == 0 || in[0] != acc[i])
                b_gate(b, G_BUF, acc[i], k ? &in[0] : &zero, 1);
            c = -1;
        }
    }
    *w = nw;
    *max = top;
}

static unsigned gray(unsigned i)
{
    return i ^ (i >> 1);
}

unsigned gamma_machine(const GammaCells *c, unsigned x, unsigned y, unsigned z)
{
    return gray(z) << (c->xbits + c->ybits) | gray(y) << c->xbits | gray(x);
}

int gen_gamma(Program *p, int xbits, int ybits, int zbits, GammaCells *cells)
{
    int k[3] = {xbits, ybits, zbits}, lo[3] = {0, xbits, xbits + ybits}, dims = xbits + ybits + zbits;
    for (int a = 0; a < 3; a++)
        if (k[a] < 1 || k[a] > CUBE_DIMS)
            return 0;
    if (dims < CUBE_DIMS || dims > CUBE_MAX_DIMS)
        return 0;
    memset(cells, 0, sizeof *cells);
    cells->xbits = xbits;
    cells->ybits = ybits;
    cells->zbits = zbits;

    Builder *b = b_new();
    uint32_t *light = cells->light, *mineral = cells->mineral, one = CELL_ONE;
    b_input(b, cells->occ = b_cell(b, "occ"));
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++)
        b_output(b, light[i] = cellf(b, "L%d", i));
    for (int i = 0; i < GAMMA_MINERAL_BITS; i++)
        b_output(b, mineral[i] = cellf(b, "M%d", i));

    /* where this machine is: which link leads to each neighbour, the sun plane, the floor */
    uint32_t next[3][CUBE_DIMS], prev[3][CUBE_DIMS], idz[CUBE_DIMS];
    static const char *tag[3] = {"gx", "gy", "gz"};
    for (int a = 0; a < 3; a++)
        grid_masks(b, lo[a], k[a], next[a], prev[a], tag[a]);
    uint32_t sun = b_cell(b, "sun"), nsun = b_cell(b, "nsun"), floor = b_cell(b, "floor");
    for (int j = 0; j < zbits; j++)
        idz[j] = cellf(b, "ID%d", lo[2] + j);
    b_gate(b, G_NOR, sun, idz, zbits);  /* z = 0 is Gray code 0 */
    b_gate(b, G_NOT, nsun, &sun, 1);
    if (zbits == 1) {  /* z = 2^zbits - 1 is Gray code 100..0 */
        b_gate(b, G_BUF, floor, &idz[0], 1);
    } else {
        b_gate(b, G_NOR, floor, idz, zbits - 1);
        b_gate2(b, G_AND, floor, floor, idz[zbits - 1]);
    }
    cells->cost_masks = b_len(b);

    /* the neighbours, one link each: light and occ from above, minerals from all 6 */
    uint32_t la[GAMMA_LIGHT_BITS], occa, nb[6][GAMMA_MINERAL_BITS];
    char name[32];
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++) {
        snprintf(name, sizeof name, "la%d", i);
        la[i] = grid_gather(b, name, light[i], lo[2], zbits, prev[2]);
    }
    occa = grid_gather(b, "occa", cells->occ, lo[2], zbits, prev[2]);
    for (int d = 0; d < 6; d++)
        for (int i = 0; i < GAMMA_MINERAL_BITS; i++) {
            snprintf(name, sizeof name, "mn%d_%d", d, i);
            nb[d][i] = grid_gather(b, name, mineral[i], lo[d / 2], k[d / 2],
                                   d % 2 ? prev[d / 2] : next[d / 2]);
        }
    cells->cost_reads = b_len(b) - cells->cost_masks;

    /* light: l - l/8 = l + ~(l >> 3) + 1 through an empty voxel, l/2 through an occupied one */
    uint32_t lt[GAMMA_LIGHT_BITS], c = b_cell(b, "lt_c"), nocc = b_cell(b, "lt_n");
    b_gate(b, G_BUF, c, &one, 1);
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++) {
        uint32_t y = CELL_ONE;
        if (i + 3 < GAMMA_LIGHT_BITS)
            b_gate(b, G_NOT, y = cellf(b, "lt_y%d", i), &la[i + 3], 1);
        b_full_adder(b, lt[i] = cellf(b, "lt%d", i), c, la[i], y, c);
    }
    b_gate(b, G_NOT, nocc, &occa, 1);
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++) {
        b_mux(b, lt[i], occa, nocc, i + 1 < GAMMA_LIGHT_BITS ? la[i + 1] : CELL_ZERO, lt[i]);
        b_gate2(b, G_OR, lt[i], lt[i], sun);  /* the sun plane: all ones */
        b_reg(b, light[i], lt[i]);
    }
    cells->cost_light = b_len(b) - cells->cost_masks - cells->cost_reads;

    /* minerals: (2 m + the 6 neighbours) / 8, 3 bits more before the shift */
    uint32_t acc[GAMMA_MINERAL_BITS + 3];
    int w = GAMMA_MINERAL_BITS;
    unsigned max = GAMMA_MINERAL_MAX;
    memcpy(acc, nb[0], sizeof nb[0]);
    for (int d = 1; d < 6; d++)
        add_into(b, acc, &w, &max, nb[d], GAMMA_MINERAL_BITS, 0, "ms");
    add_into(b, acc, &w, &max, mineral, GAMMA_MINERAL_BITS, 1, "ms");
    for (int i = 0; i < GAMMA_MINERAL_BITS; i++) {
        uint32_t s = acc[i + 3];
        b_gate2(b, G_AND, s, s, nsun);   /* the sun plane: 0 */
        b_gate2(b, G_OR, s, s, floor);   /* the floor: all ones */
        b_reg(b, mineral[i], s);
    }
    size_t before = b_len(b);
    cells->cost_minerals = before - cells->cost_masks - cells->cost_reads - cells->cost_light;
    b_finish(b, p);
    cells->cost_registers = p->len - before;
    return 1;
}

void gamma_to_cube(LaneCube *lc, const GammaCells *c, const GammaWorld *w)
{
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits, Z = 1u << w->zbits;
    for (unsigned z = 0; z < Z; z++)
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++) {
                unsigned k = gamma_machine(c, x, y, z);
                size_t i = gamma_voxel(w, x, y, z);
                lane_cube_set(lc, c->occ, k, w->occ[i]);
                for (int b = 0; b < GAMMA_LIGHT_BITS; b++)
                    lane_cube_set(lc, c->light[b], k, (uint8_t)((w->light[i] >> b) & 1));
                for (int b = 0; b < GAMMA_MINERAL_BITS; b++)
                    lane_cube_set(lc, c->mineral[b], k, (uint8_t)((w->mineral[i] >> b) & 1));
            }
}

void gamma_from_cube(const LaneCube *lc, const GammaCells *c, GammaWorld *w)
{
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits, Z = 1u << w->zbits;
    for (unsigned z = 0; z < Z; z++)
        for (unsigned y = 0; y < Y; y++)
            for (unsigned x = 0; x < X; x++) {
                unsigned k = gamma_machine(c, x, y, z), l = 0, m = 0;
                size_t i = gamma_voxel(w, x, y, z);
                for (int b = 0; b < GAMMA_LIGHT_BITS; b++)
                    l |= (unsigned)lane_cube_get(lc, c->light[b], k) << b;
                for (int b = 0; b < GAMMA_MINERAL_BITS; b++)
                    m |= (unsigned)lane_cube_get(lc, c->mineral[b], k) << b;
                w->occ[i] = lane_cube_get(lc, c->occ, k);
                w->light[i] = (uint8_t)l;
                w->mineral[i] = (uint16_t)m;
            }
}
