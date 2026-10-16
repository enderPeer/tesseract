#include "gamma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "gen.h"

const char *const gamma_part_names[GAMMA_PARTS] = {
    "where am I", "clock", "dice", "neighbour reads", "gates (the brain)", "asking and offering",
    "grants", "partner reads", "energy and state", "genome copy", "light", "minerals", "registers"};

/* ---- the world in plain C ----------------------------------------------------------------- */

static int arrays(GammaWorld *w)
{
    size_t n = w->n;
    w->alive = calloc(n, 1);
    w->busy = calloc(n, 1);
    w->embryo = calloc(n, 1);
    w->partner = calloc(n, 1);
    w->counter = calloc(n, 1);
    w->energy = calloc(n, 1);
    w->request = calloc(n, 1);
    w->offer = calloc(n, 1);
    w->born = calloc(n, 1);
    w->clock = calloc(n, 1);
    w->term = calloc(n, sizeof *w->term);
    w->genome = calloc(n * GAMMA_GATES, sizeof *w->genome);
    w->gates = calloc(n, sizeof *w->gates);
    w->rng = calloc(n, sizeof *w->rng);
    w->light = calloc(n, 1);
    w->mineral = calloc(n, sizeof *w->mineral);
    return w->alive && w->busy && w->embryo && w->partner && w->counter && w->energy &&
           w->request && w->offer && w->born && w->clock && w->term && w->genome && w->gates &&
           w->rng && w->light && w->mineral;
}

static void free_arrays(GammaWorld *w)
{
    free(w->alive);
    free(w->busy);
    free(w->embryo);
    free(w->partner);
    free(w->counter);
    free(w->energy);
    free(w->request);
    free(w->offer);
    free(w->born);
    free(w->clock);
    free(w->term);
    free(w->genome);
    free(w->gates);
    free(w->rng);
    free(w->light);
    free(w->mineral);
}

int gamma_world_init(GammaWorld *w, int xbits, int ybits, int zbits)
{
    memset(w, 0, sizeof *w);
    w->xbits = xbits;
    w->ybits = ybits;
    w->zbits = zbits;
    w->n = (size_t)1 << (xbits + ybits + zbits);
    w->spare = calloc(1, sizeof *w->spare);
    w->grant = calloc(w->n, 1);
    if (w->spare) {
        w->spare->xbits = xbits;
        w->spare->ybits = ybits;
        w->spare->zbits = zbits;
        w->spare->n = w->n;
    }
    if (w->spare && w->grant && arrays(w) && arrays(w->spare))
        return 1;
    gamma_world_free(w);
    return 0;
}

void gamma_world_free(GammaWorld *w)
{
    if (w->spare) {
        free_arrays(w->spare);
        free(w->spare);
    }
    free_arrays(w);
    free(w->grant);
    memset(w, 0, sizeof *w);
}

void gamma_world_copy(GammaWorld *dst, const GammaWorld *src)
{
    size_t n = src->n;
    memcpy(dst->alive, src->alive, n);
    memcpy(dst->busy, src->busy, n);
    memcpy(dst->embryo, src->embryo, n);
    memcpy(dst->partner, src->partner, n);
    memcpy(dst->counter, src->counter, n);
    memcpy(dst->energy, src->energy, n);
    memcpy(dst->request, src->request, n);
    memcpy(dst->offer, src->offer, n);
    memcpy(dst->born, src->born, n);
    memcpy(dst->clock, src->clock, n);
    memcpy(dst->term, src->term, n * sizeof *dst->term);
    memcpy(dst->genome, src->genome, n * GAMMA_GATES * sizeof *dst->genome);
    memcpy(dst->gates, src->gates, n * sizeof *dst->gates);
    memcpy(dst->rng, src->rng, n * sizeof *dst->rng);
    memcpy(dst->light, src->light, n);
    memcpy(dst->mineral, src->mineral, n * sizeof *dst->mineral);
}

static void swap_state(GammaWorld *a, GammaWorld *b)
{
#define SWAP(f)          \
    do {                 \
        void *t_ = a->f; \
        a->f = b->f;     \
        b->f = t_;       \
    } while (0)
    SWAP(alive);
    SWAP(busy);
    SWAP(embryo);
    SWAP(partner);
    SWAP(counter);
    SWAP(energy);
    SWAP(request);
    SWAP(offer);
    SWAP(born);
    SWAP(clock);
    SWAP(term);
    SWAP(genome);
    SWAP(gates);
    SWAP(rng);
    SWAP(light);
    SWAP(mineral);
#undef SWAP
}

size_t gamma_voxel(const GammaWorld *w, unsigned x, unsigned y, unsigned z)
{
    return (size_t)z << (w->xbits + w->ybits) | (size_t)y << w->xbits | x;
}

size_t gamma_neighbour(const GammaWorld *w, size_t v, int d)
{
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits, Z = 1u << w->zbits;
    unsigned x = (unsigned)v & (X - 1), y = (unsigned)(v >> w->xbits) & (Y - 1),
             z = (unsigned)(v >> (w->xbits + w->ybits));
    switch (d) {
    case 0: x = (x + 1) & (X - 1); break;
    case 1: x = (x + X - 1) & (X - 1); break;
    case 2: y = (y + 1) & (Y - 1); break;
    case 3: y = (y + Y - 1) & (Y - 1); break;
    case 4: z = (z + 1) & (Z - 1); break;
    default: z = (z + Z - 1) & (Z - 1); break;
    }
    return gamma_voxel(w, x, y, z);
}

static uint32_t xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static unsigned dir_of(uint32_t gates)  /* the direction a cell's gates ask to divide in */
{
    return gates >> (GAMMA_DIVIDE + 1) & 7;
}

unsigned gamma_sun(unsigned clock)  /* a triangle wave over the day: 0, 2, .. 254, .. 2, 0 */
{
    clock &= (1u << GAMMA_CLOCK_BITS) - 1;
    return 2 * (clock < 128 ? clock : 255 - clock);
}

void gamma_step(GammaWorld *w)
{
    GammaWorld *o = w->spare;
    GammaLedger *L = &w->ledger;
    unsigned Z = 1u << w->zbits, zshift = (unsigned)(w->xbits + w->ybits);
    const uint16_t *m = w->mineral;
    memset(L, 0, sizeof *L);

    /* 1. The clock, then the dice, the senses and the gates of every cell, and what it asks and
       offers this tick (into the next state, for the other voxels to read in passes 2 and 3). */
    for (size_t v = 0; v < w->n; v++) {
        unsigned E = w->energy[v], active = w->alive[v] & !w->busy[v];
        uint32_t r = xorshift32(w->rng[v]), s = 1u | (r & 7u) << 1, g = w->gates[v];
        size_t nb[6];
        o->clock[v] = (uint8_t)(w->clock[v] + 1);
        for (int d = 0; d < 6; d++) {
            nb[d] = gamma_neighbour(w, v, d);
            s |= (uint32_t)w->alive[nb[d]] << (4 + d);
            s |= (w->gates[nb[d]] >> GAMMA_SIGNAL & 3u) << (10 + 2 * d);
        }
        for (int j = 0; j < 3; j++) {
            s |= (uint32_t)(E >> (7 - j) & 1) << (22 + j);
            s |= (uint32_t)(w->light[v] >> (7 - j) & 1) << (25 + j);
            s |= (uint32_t)(m[v] >> (11 - j) & 1) << (28 + j);
        }
        s |= (uint32_t)w->born[v] << 31;
        if (active) {  /* in order, in place: a later gate is read as it was last tick */
            const uint16_t *gen = w->genome + GAMMA_GATES * v;
            for (int i = 0; i < GAMMA_GATES; i++) {
                unsigned a = gen[i] & 63, b = gen[i] >> 6 & 63;
                unsigned va = (a < 32 ? s >> a : g >> (a - 32)) & 1;
                unsigned vb = (b < 32 ? s >> b : g >> (b - 32)) & 1;
                g = (g & ~(1u << i)) | (uint32_t)!(va & vb) << i;
            }
        }
        unsigned D = dir_of(g), dirok = D < 6;
        o->rng[v] = r;
        o->gates[v] = g;
        o->request[v] = (uint8_t)(active & dirok & (g >> GAMMA_DIVIDE & 1) & (E >= GAMMA_DIVIDE_MIN));
        o->offer[v] = (uint8_t)(active & dirok & (g >> GAMMA_GIVE & 1) && w->alive[nb[D]] ? E >> 3 : 0);
    }

    /* 2. The grants: an empty voxel takes, of the cells asking for it, the one in the direction d
       with the smallest d XOR k, k being 3 bits of its own dice this tick (the lot). */
    for (size_t v = 0; v < w->n; v++) {
        unsigned hits = 0, k = o->rng[v] >> GAMMA_LOT_DICE & 7, win = 0;
        for (int d = 0; d < 6; d++) {
            size_t u = gamma_neighbour(w, v, d);
            if (o->request[u] && dir_of(o->gates[u]) == (unsigned)(d ^ 1))
                hits |= 1u << d;
        }
        for (unsigned j = 0; j < 8 && !win; j++)  /* j = d ^ k, the smallest first */
            win = hits & (1u << (j ^ k));
        w->grant[v] = (uint8_t)(w->alive[v] ? 0 : win);
    }

    /* 3. Energy, life and death, the genome, the fields. */
    for (size_t v = 0; v < w->n; v++) {
        size_t nb[6];
        for (int d = 0; d < 6; d++)
            nb[d] = gamma_neighbour(w, v, d);
        unsigned z = (unsigned)(v >> zshift), E = w->energy[v], A = w->alive[v], B = w->busy[v];
        unsigned active = A & !B, busy = A & B, emb = busy & w->embryo[v];
        unsigned clock = o->clock[v], midnight = clock == 0;
        uint32_t r = o->rng[v], g = o->gates[v];

        /* division: did this cell's request go through, did this voxel grant one */
        unsigned D = dir_of(g);
        unsigned granted = o->request[v] && (w->grant[nb[D]] >> (D ^ 1) & 1);
        unsigned grant = w->grant[v], gdir = 0;
        while (grant && !(grant >> gdir & 1))
            gdir++;
        unsigned newborn = !A && grant;

        /* the other one of a division: the cell's partner, or the cell granted */
        unsigned pd = A ? w->partner[v] : gdir, pE = 0, pA = 0, pB = 0, pK = 0, pH = 0;
        if (pd < 6) {
            size_t u = nb[pd];
            pE = w->energy[u];
            pA = w->alive[u];
            pB = w->busy[u];
            pK = w->embryo[u];
            pH = w->genome[GAMMA_GATES * u];
        }

        /* what the neighbours pointing at this cell offer it (nothing if it is empty) */
        unsigned recv = 0;
        for (int d = 0; d < 6; d++)
            if (dir_of(o->gates[nb[d]]) == (unsigned)(d ^ 1))
                recv += o->offer[nb[d]];

        /* energy: gains, upkeep, the split and the gift (never below 0: the gift is an eighth of
           what the cell had, the split half, and nothing is taken before upkeep is paid), death
           by starving, by the term being up, or with the parent; at midnight the manna: a cell
           keeps at most 127 of what it ends the tick with */
        unsigned gain = (w->light[v] >> 5) + (m[v] >> 9u) + recv;
        unsigned e1 = E + gain > GAMMA_ENERGY_MAX ? GAMMA_ENERGY_MAX : E + gain;
        unsigned starve = e1 == 0, e2 = starve ? 0 : e1 - 1;
        unsigned parent = active & granted, e3 = (parent ? e2 - (E >> 1) : e2) - o->offer[v];
        unsigned termup = w->term[v] == 0;
        unsigned abort = emb & !(pA & pB & !pK);
        unsigned die = (A & (starve | termup)) | abort, live = A & !die;
        unsigned finish = busy & (w->counter[v] == GAMMA_GATES - 1);
        unsigned en = newborn ? pE >> 1 : live ? e3 : 0;
        unsigned rot = midnight && en > GAMMA_OMER ? en - GAMMA_OMER : 0;
        unsigned term = GAMMA_TERM_MIN + (r >> GAMMA_TERM_DICE & (GAMMA_TERM_MIN - 1u));

        o->alive[v] = (uint8_t)(live | newborn);
        o->busy[v] = (uint8_t)((((busy & !finish) | parent) & !die) | newborn);
        o->embryo[v] = (uint8_t)((emb & !finish & !die) | newborn);
        o->partner[v] = (uint8_t)(newborn ? gdir : parent ? D : w->partner[v]);
        o->counter[v] = (uint8_t)(busy ? (w->counter[v] + 1) & (GAMMA_GATES - 1) : 0);
        o->energy[v] = (uint8_t)(en - rot);
        o->born[v] = (uint8_t)(emb & finish & !die);
        o->term[v] = (uint16_t)(newborn ? term : live ? w->term[v] - 1u : 0);

        /* the ledger. A cell's new energy is E + gain - cap - upkeep - split - gift - rot, or 0
           with death = what it would have kept, so summed over the world
               energy after - energy before = light + minerals - cap - upkeep - rot - death
           by construction: the splits come back as the new cells' energy and the gifts as what
           the receivers got (received = given, a gift goes only to a cell that is there). And
               cells after - cells before = granted - term - starved - aborted
           since a cell at the end is one from the start that did not die, or a new one. */
        if (A) {
            L->cells_before++;
            L->energy_before += E;
            L->light += w->light[v] >> 5;
            L->minerals += m[v] >> 9u;
            L->received += recv;
            L->cap += E + gain - e1;
            L->upkeep += !starve;
            L->given += o->offer[v];
            L->split += parent ? E >> 1 : 0;
            if (die) {
                L->death += e3;
                if (termup)
                    L->term++;
                else if (starve)
                    L->starved++;
                else
                    L->aborted++;
            }
        }
        if (newborn) {
            L->granted++;
            L->grants_by_dir[gdir]++;
        }
        L->rot += rot;
        L->cells_after += o->alive[v];
        L->energy_after += o->energy[v];

        /* the genome: while dividing it moves up one gate, the parent's first gate going to its
           end, the parent's first gate (maybe mutated) to the end of the new cell's */
        const uint16_t *og = w->genome + GAMMA_GATES * v;
        uint16_t *ng = o->genome + GAMMA_GATES * v;
        if (busy) {
            unsigned tail = og[0], pos = r >> 20 & 15;
            if (emb) {
                unsigned mutate = (r >> 14 & ((1u << GAMMA_MUTATION_BITS) - 1)) ==
                                  (1u << GAMMA_MUTATION_BITS) - 1;
                tail = pH ^ (mutate && pos < GAMMA_GATE_BITS ? 1u << pos : 0);
            }
            memmove(ng, og + 1, (GAMMA_GATES - 1) * sizeof *ng);
            ng[GAMMA_GATES - 1] = (uint16_t)tail;
        } else {
            memcpy(ng, og, GAMMA_GATES * sizeof *ng);
        }

        /* the fields */
        size_t up = nb[5];
        unsigned l = w->light[up];
        o->light[v] = (uint8_t)(z == 0 ? gamma_sun(clock) : w->alive[up] ? l / 2 : l - l / 8);
        unsigned sum = 2u * m[v] + m[nb[0]] + m[nb[1]] + m[nb[2]] + m[nb[3]] + m[nb[4]] + m[nb[5]];
        unsigned eat = A ? (m[v] >> 9u) << 6 : 0;  /* never more than sum / 8 */
        unsigned val = sum / 8 - eat + (die ? GAMMA_RELEASE : 0);
        if (val > GAMMA_MINERAL_MAX)
            val = GAMMA_MINERAL_MAX;
        o->mineral[v] = (uint16_t)(z == Z - 1 ? GAMMA_MINERAL_MAX : z == 0 ? 0 : val);
    }

    /* 4. A dead cell's gates are 0, and so are a new one's. Only now: in pass 3 the neighbours
       still read this tick's direction bits, to take the offer of a cell that dies this tick. */
    for (size_t v = 0; v < w->n; v++)
        if (!(o->alive[v] & w->alive[v]))
            o->gates[v] = 0;
    swap_state(w, o);
}

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

size_t gamma_soup(GammaWorld *w, double fraction, unsigned energy, uint64_t seed)
{
    uint64_t s = seed;
    size_t made = 0;
    for (size_t v = 0; v < w->n; v++) {
        uint32_t r;
        do
            r = (uint32_t)splitmix64(&s);
        while (!r);
        int cell = (double)(splitmix64(&s) >> 11) < fraction * 9007199254740992.0;  /* 2^53 */
        w->rng[v] = r;
        w->alive[v] = (uint8_t)cell;
        w->busy[v] = w->embryo[v] = w->partner[v] = w->counter[v] = 0;
        w->request[v] = w->offer[v] = w->born[v] = 0;
        w->gates[v] = 0;
        w->energy[v] = (uint8_t)(cell ? energy : 0);
        w->clock[v] = GAMMA_SUNRISE;
        for (int i = 0; i < GAMMA_GATES; i++)
            w->genome[GAMMA_GATES * v + i] =
                (uint16_t)(cell ? splitmix64(&s) & ((1u << GAMMA_GATE_BITS) - 1) : 0);
        made += (size_t)cell;
    }
    for (size_t v = 0; v < w->n; v++)  /* the terms after all the genomes: the 11c soup, aged */
        w->term[v] =
            (uint16_t)(w->alive[v] ? 1 + splitmix64(&s) % ((1u << GAMMA_TERM_BITS) - 1) : 0);
    return made;
}

void gamma_ledger_add(GammaLedger *sum, const GammaLedger *l)
{
#define ADD(f) (sum->f += l->f)
    ADD(cells_before);
    ADD(cells_after);
    ADD(energy_before);
    ADD(energy_after);
    ADD(light);
    ADD(minerals);
    ADD(received);
    ADD(given);
    ADD(cap);
    ADD(upkeep);
    ADD(rot);
    ADD(death);
    ADD(split);
    ADD(granted);
    ADD(term);
    ADD(starved);
    ADD(aborted);
    for (int d = 0; d < 6; d++)
        ADD(grants_by_dir[d]);
#undef ADD
}

int gamma_ledger_closes(const GammaLedger *l)  /* each identity with its sums on either side */
{
    uint64_t by_dir = 0;
    for (int d = 0; d < 6; d++)
        by_dir += l->grants_by_dir[d];
    return l->energy_after + l->cap + l->upkeep + l->rot + l->death ==
               l->energy_before + l->light + l->minerals &&
           l->cells_after + l->term + l->starved + l->aborted == l->cells_before + l->granted &&
           l->received == l->given && by_dir == l->granted;
}

long gamma_compare(const GammaWorld *a, const GammaWorld *b, const char **what)
{
    for (size_t v = 0; v < a->n; v++) {
        const char *f = a->alive[v] != b->alive[v]       ? "alive"
                        : a->busy[v] != b->busy[v]       ? "busy"
                        : a->embryo[v] != b->embryo[v]   ? "embryo"
                        : a->partner[v] != b->partner[v] ? "partner"
                        : a->counter[v] != b->counter[v] ? "counter"
                        : a->energy[v] != b->energy[v]   ? "energy"
                        : a->request[v] != b->request[v] ? "request"
                        : a->offer[v] != b->offer[v]     ? "offer"
                        : a->born[v] != b->born[v]       ? "born"
                        : a->clock[v] != b->clock[v]     ? "clock"
                        : a->term[v] != b->term[v]       ? "term"
                        : a->gates[v] != b->gates[v]     ? "gates"
                        : a->rng[v] != b->rng[v]         ? "random generator"
                        : a->light[v] != b->light[v]     ? "light"
                        : a->mineral[v] != b->mineral[v] ? "minerals"
                        : memcmp(a->genome + GAMMA_GATES * v, b->genome + GAMMA_GATES * v,
                                 GAMMA_GATES * sizeof *a->genome)
                            ? "genome"
                            : NULL;
        if (f) {
            *what = f;
            return (long)v;
        }
    }
    *what = NULL;
    return -1;
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

typedef struct {
    Builder *b;
    int      k[3], lo[3], dims;
    uint32_t next[3][CUBE_DIMS], prev[3][CUBE_DIMS];
} Gen;

/* new cells: a gate's output, by name */
static uint32_t gate(Builder *b, int type, const char *name, const uint32_t *in, int n)
{
    uint32_t out = b_cell(b, name);
    b_gate(b, type, out, in, n);
    return out;
}

static uint32_t gate2(Builder *b, int type, const char *name, uint32_t x, uint32_t y)
{
    uint32_t in[2] = {x, y};
    return gate(b, type, name, in, 2);
}

static uint32_t not1(Builder *b, const char *name, uint32_t x)
{
    return gate(b, G_NOT, name, &x, 1);
}

/* src of the neighbour in direction d, one link away */
static uint32_t from_dir(Gen *g, const char *name, uint32_t src, int d)
{
    int a = d / 2;
    return grid_gather(g->b, name, src, g->lo[a], g->k[a], d % 2 ? g->prev[a] : g->next[a]);
}

/* src of the machine sel[0 .. dims-1] points to (one 1 at most), across any dimension */
static uint32_t from_sel(Gen *g, const char *name, uint32_t src, const uint32_t *sel)
{
    Builder *b = g->b;
    uint32_t t[CUBE_MAX_DIMS], out = b_cell(b, name);
    char link[48];
    for (int j = 0; j < g->dims; j++) {
        snprintf(link, sizeof link, "lk%u_%d", (unsigned)src, j);  /* as grid_gather names them */
        uint32_t l = b_has(b, link) ? b_cell(b, link) : b_link(b, link, src, j);
        b_gate2(b, G_NAND, t[j] = cellf(b, "gs%d", j), sel[j], l);
    }
    b_gate(b, G_NAND, out, t, g->dims);
    return out;
}

/* one-hot of the 3-bit number v (v[0] lowest): out[d] = (v == d) for d < 6 */
static void decode6(Builder *b, const uint32_t *v, uint32_t *out, const char *tag)
{
    char name[32];
    uint32_t nv[3];
    for (int j = 0; j < 3; j++) {
        snprintf(name, sizeof name, "%s_n%d", tag, j);
        nv[j] = not1(b, name, v[j]);
    }
    for (int d = 0; d < 6; d++) {
        uint32_t in[3];
        for (int j = 0; j < 3; j++)
            in[j] = d >> j & 1 ? v[j] : nv[j];
        snprintf(name, sizeof name, "%s%d", tag, d);
        out[d] = gate(b, G_AND, name, in, 3);
    }
}

/* out = src[a], a = addr[0 .. 5] (lowest first), from 64 sources; mx: 32 scratch cells. The result
   is left in mx[0]. */
static void select64(Builder *b, const uint32_t *src, const uint32_t *addr, const uint32_t *naddr,
                     const uint32_t *mx)
{
    for (int k = 0; k < 32; k++)
        b_mux(b, mx[k], addr[0], naddr[0], src[2 * k + 1], src[2 * k]);
    for (int level = 1; level < GAMMA_ADDR_BITS; level++)
        for (int k = 0; k < 32 >> level; k++)
            b_mux(b, mx[k], addr[level], naddr[level], mx[2 * k + 1], mx[2 * k]);
}

int gen_gamma(Program *p, int xbits, int ybits, int zbits, GammaCells *c)
{
    int dims = xbits + ybits + zbits;
    if (xbits < 1 || ybits < 1 || zbits < 1 || xbits > CUBE_DIMS || ybits > CUBE_DIMS ||
        zbits > CUBE_DIMS || dims < CUBE_DIMS || dims > CUBE_MAX_DIMS)
        return 0;
    memset(c, 0, sizeof *c);
    c->xbits = xbits;
    c->ybits = ybits;
    c->zbits = zbits;
    Gen g = {.b = b_new(), .k = {xbits, ybits, zbits}, .lo = {0, xbits, xbits + ybits}, .dims = dims};
    Builder *b = g.b;
    char name[48];
    size_t mark = 0;
#define PART(part)                          \
    do {                                    \
        c->cost[part] = b_len(b) - mark;    \
        mark = b_len(b);                    \
    } while (0)

    /* the state of a voxel */
    c->alive = b_cell(b, "alive");
    c->busy = b_cell(b, "busy");
    c->embryo = b_cell(b, "embryo");
    c->request = b_cell(b, "request");
    c->born = b_cell(b, "born");
    for (int j = 0; j < 2; j++)
        c->signal[j] = cellf(b, "so%d", j);
    for (int j = 0; j < 3; j++)
        c->partner[j] = cellf(b, "pt%d", j);
    for (int j = 0; j < 5; j++)
        c->counter[j] = cellf(b, "ct%d", j);
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++)
        c->energy[i] = cellf(b, "E%d", i);
    for (int j = 0; j < GAMMA_OFFER_BITS; j++)
        c->offer[j] = cellf(b, "of%d", j);
    for (int i = 0; i < GAMMA_CLOCK_BITS; i++)
        c->clock[i] = cellf(b, "ck%d", i);
    for (int i = 0; i < GAMMA_TERM_BITS; i++)
        c->term[i] = cellf(b, "tm%d", i);
    for (int i = 0; i < GAMMA_GATES; i++)
        for (int j = 0; j < GAMMA_GATE_BITS; j++) {
            snprintf(name, sizeof name, "G%d_%d", i, j);
            c->genome[i][j] = b_cell(b, name);
        }
    for (int i = 0; i < GAMMA_GATES; i++)
        c->gates[i] = cellf(b, "g%d", i);
    for (int i = 0; i < 32; i++)
        c->rng[i] = cellf(b, "x%d", i);
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++)
        c->light[i] = cellf(b, "L%d", i);
    for (int i = 0; i < GAMMA_MINERAL_BITS; i++)
        c->mineral[i] = cellf(b, "M%d", i);
    uint32_t *x = c->rng, *gt = c->gates, *E = c->energy, *L = c->light, *M = c->mineral;
    uint32_t *ck = c->clock, *tm = c->term, one = CELL_ONE, zero = CELL_ZERO;

    /* where this machine is: which link leads to each neighbour, the sun plane, the floor */
    static const char *tag[3] = {"gx", "gy", "gz"};
    uint32_t idz[CUBE_DIMS];
    for (int a = 0; a < 3; a++)
        grid_masks(b, g.lo[a], g.k[a], g.next[a], g.prev[a], tag[a]);
    for (int j = 0; j < zbits; j++)
        idz[j] = cellf(b, "ID%d", g.lo[2] + j);
    uint32_t sun = gate(b, G_NOR, "sun", idz, zbits), nsun = not1(b, "nsun", sun), floor;
    if (zbits == 1) {  /* z = 2^zbits - 1 is Gray code 100..0 */
        floor = gate(b, G_BUF, "floor", idz, 1);
    } else {
        floor = gate(b, G_NOR, "floor", idz, zbits - 1);
        b_gate2(b, G_AND, floor, floor, idz[zbits - 1]);
    }
    PART(GAMMA_PART_WHERE);

    /* the clock: + 1 in place before anything else, the same in every voxel; a day is 256 ticks.
       The carry out of the top bit is the wrap to 0: midnight, for the manna below. */
    uint32_t ck_d = b_cell(b, "ck_d");
    uint32_t ck_c[2] = {gate(b, G_BUF, "ck_c", &one, 1), ck_d};
    for (int i = 0; i < GAMMA_CLOCK_BITS; i++)
        b_half_adder(b, ck[i], ck_c[(i + 1) & 1], ck[i], ck_c[i & 1]);
    uint32_t midnight = ck_c[GAMMA_CLOCK_BITS & 1];
    PART(GAMMA_PART_CLOCK);

    /* the dice: xorshift32 in place, the new state is this tick's random bits */
    for (int i = 31; i >= 13; i--)
        b_gate2(b, G_XOR, x[i], x[i], x[i - 13]);
    for (int i = 0; i + 17 < 32; i++)
        b_gate2(b, G_XOR, x[i], x[i], x[i + 17]);
    for (int i = 31; i >= 5; i--)
        b_gate2(b, G_XOR, x[i], x[i], x[i - 5]);
    PART(GAMMA_PART_DICE);

    /* the neighbours as they were at the start of the tick, one link each: is there a cell, its
       signals (registers: the gates as they ended last tick), the light above, minerals all round */
    uint32_t occ[6], sig[6][2], la[GAMMA_LIGHT_BITS], mn[6][GAMMA_MINERAL_BITS];
    for (int d = 0; d < 6; d++) {
        snprintf(name, sizeof name, "oc%d", d);
        occ[d] = from_dir(&g, name, c->alive, d);
        for (int j = 0; j < 2; j++) {
            snprintf(name, sizeof name, "sg%d_%d", d, j);
            sig[d][j] = from_dir(&g, name, c->signal[j], d);
        }
    }
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++) {
        snprintf(name, sizeof name, "la%d", i);
        la[i] = from_dir(&g, name, L[i], 5);
    }
    for (int d = 0; d < 6; d++)
        for (int i = 0; i < GAMMA_MINERAL_BITS; i++) {
            snprintf(name, sizeof name, "mn%d_%d", d, i);
            mn[d][i] = from_dir(&g, name, M[i], d);
        }
    PART(GAMMA_PART_READS);

    /* the gates: a cell that is alive and not dividing runs them, in order, in place */
    uint32_t src[64], mx[32], na[GAMMA_ADDR_BITS];
    src[0] = one;
    for (int j = 0; j < 3; j++)
        src[1 + j] = x[j];
    for (int d = 0; d < 6; d++) {
        src[4 + d] = occ[d];
        src[10 + 2 * d] = sig[d][0];
        src[11 + 2 * d] = sig[d][1];
    }
    for (int j = 0; j < 3; j++) {
        src[22 + j] = E[7 - j];
        src[25 + j] = L[7 - j];
        src[28 + j] = M[11 - j];
    }
    src[31] = c->born;
    for (int i = 0; i < GAMMA_GATES; i++)
        src[32 + i] = gt[i];
    for (int k = 0; k < 32; k++)
        mx[k] = cellf(b, "mx%d", k);
    for (int j = 0; j < GAMMA_ADDR_BITS; j++)
        na[j] = cellf(b, "na%d", j);
    uint32_t nbusy = not1(b, "nbusy", c->busy), act = gate2(b, G_AND, "act", c->alive, nbusy),
             nact = not1(b, "nact", act), va = b_cell(b, "va"), nv = b_cell(b, "nv");
    for (int i = 0; i < GAMMA_GATES; i++) {
        for (int h = 0; h < 2; h++) {
            const uint32_t *addr = &c->genome[i][GAMMA_ADDR_BITS * h];
            for (int j = 0; j < GAMMA_ADDR_BITS; j++)
                b_gate(b, G_NOT, na[j], &addr[j], 1);
            select64(b, src, addr, na, mx);
            if (h == 0)
                b_gate(b, G_BUF, va, &mx[0], 1);
        }
        b_gate2(b, G_NAND, nv, va, mx[0]);
        b_mux(b, gt[i], act, nact, nv, gt[i]);
    }
    PART(GAMMA_PART_BRAIN);

    /* asking and offering, from this tick's gates: to ask, a direction below 6 and energy 32 or
       more; the offer is an eighth of the energy, if there is a cell in the direction */
    const uint32_t *D = &gt[GAMMA_DIVIDE + 1];
    uint32_t dsel[6], tsel[6];
    decode6(b, D, dsel, "ds");
    uint32_t dirok = gate2(b, G_NAND, "dirok", D[1], D[2]), hi[3] = {E[5], E[6], E[7]};
    uint32_t rich = gate(b, G_OR, "rich", hi, 3), rin[4] = {act, dirok, gt[GAMMA_DIVIDE], rich};
    b_gate(b, G_AND, c->request, rin, 4);
    for (int d = 0; d < 6; d++) {
        snprintf(name, sizeof name, "ts%d", d);
        tsel[d] = gate2(b, G_AND, name, dsel[d], occ[d]);
    }
    uint32_t target = gate(b, G_OR, "target", tsel, 6), vin[4] = {act, dirok, gt[GAMMA_GIVE], target};
    uint32_t give = gate(b, G_AND, "give", vin, 4);
    for (int j = 0; j < GAMMA_OFFER_BITS; j++)
        b_gate2(b, G_AND, c->offer[j], give, E[j + 3]);
    PART(GAMMA_PART_ASK);

    /* grants. The neighbours' asks, directions and offers, this tick's, one link each; point[d]:
       the neighbour in direction d points at this voxel (towards d ^ 1); an empty voxel grants
       one of the neighbours pointing at it and asking (the hits), by lot. Then each cell reads
       whether the voxel it asked for granted it: that voxel's grant from the opposite direction. */
    uint32_t nalive = not1(b, "nalive", c->alive), req[6], dir[6][3], off[6][GAMMA_OFFER_BITS];
    uint32_t point[6], hit[6], grant[6], gin[6];
    for (int d = 0; d < 6; d++) {
        snprintf(name, sizeof name, "rq%d", d);
        req[d] = from_dir(&g, name, c->request, d);
        for (int j = 0; j < 3; j++) {
            snprintf(name, sizeof name, "dr%d_%d", d, j);
            dir[d][j] = from_dir(&g, name, D[j], d);
        }
        for (int j = 0; j < GAMMA_OFFER_BITS; j++) {
            snprintf(name, sizeof name, "of%d_%d", d, j);
            off[d][j] = from_dir(&g, name, c->offer[j], d);
        }
    }
    for (int d = 0; d < 6; d++) {
        uint32_t in[3];
        for (int j = 0; j < 3; j++) {
            snprintf(name, sizeof name, "pn%d_%d", d, j);
            in[j] = (d ^ 1) >> j & 1 ? dir[d][j] : not1(b, name, dir[d][j]);
        }
        snprintf(name, sizeof name, "point%d", d);
        point[d] = gate(b, G_AND, name, in, 3);
        snprintf(name, sizeof name, "hit%d", d);
        hit[d] = gate2(b, G_AND, name, req[d], point[d]);
    }
    /* the lot: of the hits, the direction d with the smallest d XOR k wins, k being 3 of the
       voxel's dice bits. Bit by bit from the top, the winner agrees with k in bit j unless no hit
       left does. So d wins when at every bit j it agrees with k there, or no hit agrees with d
       above bit j and differs in bit j (d's sibling group there). hits[j][q]: a hit among the
       directions whose bits above j are q (a tree of ORs; a group of one is the hit itself);
       ok[j][q]: those directions pass bit j; the group of 6 and 7 never hits and passes nothing.
       151 instructions, against about 1,100 for a one-hot of k, the hits permuted by k, a scan
       and the permutation back. */
    static const int groups[3] = {6, 3, 2};  /* at level j: the directions d >> j */
    const uint32_t *lot = &x[GAMMA_LOT_DICE];
    uint32_t nlot[3], hits[3][6] = {{0}}, ok[3][6] = {{0}};
    for (int j = 0; j < 3; j++) {
        snprintf(name, sizeof name, "nlot%d", j);
        nlot[j] = not1(b, name, lot[j]);
    }
    for (int d = 0; d < 6; d++)
        hits[0][d] = hit[d];
    for (int j = 1; j < 3; j++)
        for (int q = 0; q < groups[j]; q++) {
            snprintf(name, sizeof name, "hits%d_%d", j, q);
            hits[j][q] = 2 * q + 1 < groups[j - 1]
                             ? gate2(b, G_OR, name, hits[j - 1][2 * q], hits[j - 1][2 * q + 1])
                             : hits[j - 1][2 * q];
        }
    for (int j = 0; j < 3; j++)
        for (int q = 0; q < groups[j]; q++)
            if ((q ^ 1) < groups[j]) {  /* bit j of q agrees with k, or the sibling has no hit */
                snprintf(name, sizeof name, "ok%d_%d", j, q);
                ok[j][q] = gate2(b, G_NAND, name, q & 1 ? nlot[j] : lot[j], hits[j][q ^ 1]);
            }
    for (int d = 0; d < 6; d++) {
        uint32_t in[5] = {hit[d], nalive};
        int n = 2;
        for (int j = 2; j >= 0; j--)
            if (((d >> j) ^ 1) < groups[j])
                in[n++] = ok[j][d >> j];
        snprintf(name, sizeof name, "gr%d", d);
        grant[d] = gate(b, G_AND, name, in, n);
    }
    for (int d = 0; d < 6; d++) {
        snprintf(name, sizeof name, "gi%d", d);
        gin[d] = from_dir(&g, name, grant[d ^ 1], d);
    }
    uint32_t any[6], gd[3];
    for (int d = 0; d < 6; d++) {
        snprintf(name, sizeof name, "ga%d", d);
        any[d] = gate2(b, G_AND, name, dsel[d], gin[d]);
    }
    uint32_t granted = gate(b, G_OR, "granted", any, 6);
    b_gate2(b, G_AND, granted, granted, c->request);
    uint32_t grant_any = gate(b, G_OR, "grany", grant, 6);
    uint32_t g135[3] = {grant[1], grant[3], grant[5]}, g23[2] = {grant[2], grant[3]},
             g45[2] = {grant[4], grant[5]};
    gd[0] = gate(b, G_OR, "gd0", g135, 3);
    gd[1] = gate(b, G_OR, "gd1", g23, 2);
    gd[2] = gate(b, G_OR, "gd2", g45, 2);
    PART(GAMMA_PART_GRANTS);

    /* the other one of a division: the partner, or for an empty voxel the cell it granted. sel
       picks its link among all dimensions. */
    uint32_t pd[3], po[6], sel[CUBE_MAX_DIMS], pE[GAMMA_ENERGY_BITS], pH[GAMMA_GATE_BITS];
    for (int j = 0; j < 3; j++) {
        snprintf(name, sizeof name, "pd%d", j);
        pd[j] = b_cell(b, name);
        b_mux(b, pd[j], c->alive, nalive, c->partner[j], gd[j]);
    }
    decode6(b, pd, po, "po");
    for (int a = 0; a < 3; a++)
        for (int j = 0; j < g.k[a]; j++) {
            snprintf(name, sizeof name, "sn%d", g.lo[a] + j);
            uint32_t t = gate2(b, G_AND, name, po[2 * a], g.next[a][j]);
            snprintf(name, sizeof name, "sp%d", g.lo[a] + j);
            uint32_t u = gate2(b, G_AND, name, po[2 * a + 1], g.prev[a][j]);
            snprintf(name, sizeof name, "se%d", g.lo[a] + j);
            sel[g.lo[a] + j] = gate2(b, G_OR, name, t, u);
        }
    for (int i = 1; i < GAMMA_ENERGY_BITS; i++) {
        snprintf(name, sizeof name, "pe%d", i);
        pE[i] = from_sel(&g, name, E[i], sel);
    }
    uint32_t pA = from_sel(&g, "pa", c->alive, sel), pB = from_sel(&g, "pb", c->busy, sel),
             pK = from_sel(&g, "pk", c->embryo, sel);
    for (int j = 0; j < GAMMA_GATE_BITS; j++) {
        snprintf(name, sizeof name, "ph%d", j);
        pH[j] = from_sel(&g, name, c->genome[0][j], sel);
    }
    PART(GAMMA_PART_PARTNER);

    /* energy: gains (light/32 + minerals/512 + what the neighbours pointing here offer), at most
       255, less 1 */
    uint32_t gn[4], rv[GAMMA_ENERGY_BITS], rt[GAMMA_OFFER_BITS], e1[12], e2[8], e3[8], en[8];
    int w;
    unsigned max;
    for (int j = 0; j < 3; j++)
        gn[j] = gate(b, G_BUF, j == 0 ? "gn0" : j == 1 ? "gn1" : "gn2", &L[5 + j], 1);
    w = 3;
    max = 7;
    add_into(b, gn, &w, &max, &M[9], 3, 0, "gn");
    int wr = GAMMA_OFFER_BITS;
    unsigned maxr = (1u << GAMMA_OFFER_BITS) - 1;
    for (int j = 0; j < GAMMA_OFFER_BITS; j++) {
        snprintf(name, sizeof name, "rv%d", j);
        rv[j] = gate2(b, G_AND, name, point[0], off[0][j]);
    }
    for (int d = 1; d < 6; d++) {
        for (int j = 0; j < GAMMA_OFFER_BITS; j++) {
            snprintf(name, sizeof name, "rt%d", j);
            rt[j] = gate2(b, G_AND, name, point[d], off[d][j]);
        }
        add_into(b, rv, &wr, &maxr, rt, GAMMA_OFFER_BITS, 0, "rv");
    }
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++) {
        snprintf(name, sizeof name, "ea%d", i);
        e1[i] = gate(b, G_BUF, name, &E[i], 1);
    }
    w = GAMMA_ENERGY_BITS;
    max = GAMMA_ENERGY_MAX;
    add_into(b, e1, &w, &max, gn, 4, 0, "ea");
    add_into(b, e1, &w, &max, rv, wr, 0, "ea");  /* at most 255 + 14 + 186; the adder makes a bit
                                                    more than that needs, always 0 */
    uint32_t over = gate(b, G_OR, "over", e1 + GAMMA_ENERGY_BITS, w - GAMMA_ENERGY_BITS);
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++)
        b_gate2(b, G_OR, e1[i], e1[i], over);  /* at most 255 */
    uint32_t starve = gate(b, G_NOR, "starve", e1, GAMMA_ENERGY_BITS), nstarve = not1(b, "nstv", starve);
    uint32_t bw = gate(b, G_BUF, "bw", &one, 1);  /* e1 - 1: the borrow runs up */
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++) {
        snprintf(name, sizeof name, "eb%d", i);
        e2[i] = gate2(b, G_XOR, name, e1[i], bw);
        snprintf(name, sizeof name, "en_%d", i);
        b_gate2(b, G_AND, bw, bw, not1(b, name, e1[i]));
        b_gate2(b, G_AND, e2[i], e2[i], nstarve);
    }
    /* the parent gives half of what it had: e2 - (E >> 1) = e2 + ~(E >> 1) + 1 */
    uint32_t parent = gate2(b, G_AND, "parent", act, granted), nparent = not1(b, "nparent", parent);
    uint32_t sc = gate(b, G_BUF, "sc", &one, 1);
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++) {
        uint32_t y = one;
        if (i + 1 < GAMMA_ENERGY_BITS) {
            snprintf(name, sizeof name, "eh%d", i);
            y = not1(b, name, E[i + 1]);
        }
        snprintf(name, sizeof name, "ec%d", i);
        e3[i] = b_cell(b, name);
        b_full_adder(b, e3[i], sc, e2[i], y, sc);
        b_mux(b, e3[i], parent, nparent, e3[i], e2[i]);
    }
    /* and the offer goes: e3 - offer = e3 + ~offer + 1, the offer being 0 above bit 4 */
    uint32_t sg = gate(b, G_BUF, "sg", &one, 1);
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++) {
        uint32_t y = one;
        if (i < GAMMA_OFFER_BITS) {
            snprintf(name, sizeof name, "eo%d", i);
            y = not1(b, name, c->offer[i]);
        }
        b_full_adder(b, e3[i], sg, e3[i], y, sg);
    }
    /* death: no energy, the term up (0 at the start of the tick), the parent of a new cell gone */
    uint32_t term_up = gate(b, G_NOR, "term_up", tm, GAMMA_TERM_BITS);
    uint32_t busyA = gate2(b, G_AND, "busyA", c->alive, c->busy), nbusyA = not1(b, "nbusyA", busyA);
    uint32_t emb = gate2(b, G_AND, "emb", busyA, c->embryo);
    uint32_t npk = not1(b, "npk", pK), pin[3] = {pA, pB, npk};
    uint32_t pok = gate(b, G_AND, "pok", pin, 3);
    uint32_t abort = gate2(b, G_AND, "abort", emb, not1(b, "npok", pok));
    uint32_t die = gate2(b, G_OR, "die", starve, term_up);
    b_gate2(b, G_AND, die, die, c->alive);
    b_gate2(b, G_OR, die, die, abort);
    uint32_t ndie = not1(b, "ndie", die), live = gate2(b, G_AND, "live", c->alive, ndie);
    uint32_t newborn = gate2(b, G_AND, "newborn", nalive, grant_any);
    uint32_t fin_in[6] = {busyA, c->counter[0], c->counter[1], c->counter[2], c->counter[3],
                          c->counter[4]};
    uint32_t finish = gate(b, G_AND, "finish", fin_in, 6), nfinish = not1(b, "nfinish", finish);
    /* the new state */
    uint32_t alive_n = gate2(b, G_OR, "alive_n", live, newborn);
    uint32_t busy_n = gate2(b, G_AND, "busy_n", busyA, nfinish);
    b_gate2(b, G_OR, busy_n, busy_n, parent);
    b_gate2(b, G_AND, busy_n, busy_n, ndie);
    b_gate2(b, G_OR, busy_n, busy_n, newborn);
    uint32_t kin[3] = {emb, nfinish, ndie};
    uint32_t emb_n = gate(b, G_AND, "embryo_n", kin, 3);
    b_gate2(b, G_OR, emb_n, emb_n, newborn);
    uint32_t nnewborn = not1(b, "nnewborn", newborn), pt_n[3], ct_n[5];
    for (int j = 0; j < 3; j++) {
        snprintf(name, sizeof name, "pt_n%d", j);
        pt_n[j] = b_cell(b, name);
        b_mux(b, pt_n[j], parent, nparent, D[j], c->partner[j]);
        b_mux(b, pt_n[j], newborn, nnewborn, gd[j], pt_n[j]);
    }
    uint32_t cc = gate(b, G_BUF, "cc", &one, 1);  /* counter + 1 while busy, else 0 */
    for (int j = 0; j < 5; j++) {
        snprintf(name, sizeof name, "ct_n%d", j);
        ct_n[j] = gate2(b, G_XOR, name, c->counter[j], cc);
        b_gate2(b, G_AND, cc, cc, c->counter[j]);
        b_gate2(b, G_AND, ct_n[j], ct_n[j], busyA);
    }
    /* the term, in place (nothing reads it after this): a new cell's is 256 + 8 of its voxel's
       dice bits, a living cell's one less, an empty voxel's and a dead cell's 0. The borrow of the
       decrement runs up, kept inverted (a lower bit was 1): no NOT per bit. A new cell is never
       alive, so its load is a multiplexer, not an AND and an OR. */
    uint32_t tb = gate(b, G_BUF, "tm_b", &zero, 1), td[GAMMA_TERM_BITS];
    for (int i = 0; i < GAMMA_TERM_BITS; i++) {
        snprintf(name, sizeof name, "td%d", i);
        td[i] = gate2(b, G_XNOR, name, tm[i], tb);
        if (i + 1 < GAMMA_TERM_BITS)
            b_gate2(b, G_OR, tb, tb, tm[i]);
        b_gate2(b, G_AND, td[i], td[i], live);
        if (i + 1 < GAMMA_TERM_BITS)
            b_mux(b, tm[i], newborn, nnewborn, x[GAMMA_TERM_DICE + i], td[i]);
        else
            b_gate2(b, G_OR, tm[i], td[i], newborn);
    }
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++) {  /* newborn: half the parent's; alive: e3 */
        snprintf(name, sizeof name, "en%d", i);
        en[i] = gate2(b, G_AND, name, live, e3[i]);
        if (i + 1 < GAMMA_ENERGY_BITS) {
            snprintf(name, sizeof name, "ep%d", i);
            b_gate2(b, G_OR, en[i], en[i], gate2(b, G_AND, name, newborn, pE[i + 1]));
        }
    }
    /* manna: at midnight whatever is above 127 rots, to 127 (a new cell holds at most 127) */
    uint32_t rot = gate2(b, G_AND, "rot", midnight, en[GAMMA_ENERGY_BITS - 1]);
    b_gate2(b, G_AND, en[GAMMA_ENERGY_BITS - 1], en[GAMMA_ENERGY_BITS - 1], not1(b, "nrot", rot));
    for (int i = 0; i + 1 < GAMMA_ENERGY_BITS; i++)
        b_gate2(b, G_OR, en[i], en[i], rot);
    uint32_t bin[3] = {emb, finish, ndie};
    uint32_t born_n = gate(b, G_AND, "born_n", bin, 3);
    for (int i = 0; i < GAMMA_GATES; i++)  /* a dead cell's gates are 0 */
        b_gate2(b, G_AND, gt[i], gt[i], live);
    b_reg(b, c->alive, alive_n);
    b_reg(b, c->busy, busy_n);
    b_reg(b, c->embryo, emb_n);
    b_reg(b, c->born, born_n);
    for (int j = 0; j < 2; j++)  /* the signals the neighbours read next tick */
        b_reg(b, c->signal[j], gt[GAMMA_SIGNAL + j]);
    for (int j = 0; j < 3; j++)
        b_reg(b, c->partner[j], pt_n[j]);
    for (int j = 0; j < 5; j++)
        b_reg(b, c->counter[j], ct_n[j]);
    for (int i = 0; i < GAMMA_ENERGY_BITS; i++)
        b_reg(b, E[i], en[i]);
    PART(GAMMA_PART_STATE);

    /* the genome moves up one gate while dividing; the gate coming in at the end is the cell's own
       first gate (the parent) or the parent's first gate, maybe with one bit flipped (the new cell) */
    uint32_t mutate = gate(b, G_AND, "mutate", &x[14], GAMMA_MUTATION_BITS), nx[4], tail[12];
    for (int j = 0; j < 4; j++) {
        snprintf(name, sizeof name, "nx%d", j);
        nx[j] = not1(b, name, x[20 + j]);
    }
    uint32_t nemb = not1(b, "nemb", emb);
    for (int j = 0; j < GAMMA_GATE_BITS; j++) {
        uint32_t in[5] = {mutate};
        for (int k = 0; k < 4; k++)
            in[1 + k] = j >> k & 1 ? x[20 + k] : nx[k];
        snprintf(name, sizeof name, "fl%d", j);
        uint32_t flip = gate(b, G_AND, name, in, 5);
        snprintf(name, sizeof name, "tl%d", j);
        tail[j] = gate2(b, G_XOR, name, pH[j], flip);
        b_mux(b, tail[j], emb, nemb, tail[j], c->genome[0][j]);
    }
    for (int i = 0; i < GAMMA_GATES; i++)
        for (int j = 0; j < GAMMA_GATE_BITS; j++)
            b_mux(b, c->genome[i][j], busyA, nbusyA,
                  i + 1 < GAMMA_GATES ? c->genome[i + 1][j] : tail[j], c->genome[i][j]);
    PART(GAMMA_PART_COPY);

    /* light: l - l/8 = l + ~(l >> 3) + 1 through an empty voxel, l/2 through a cell */
    uint32_t lt[GAMMA_LIGHT_BITS], lc = gate(b, G_BUF, "lt_c", &one, 1), occa = occ[5];
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++) {
        uint32_t y = one;
        if (i + 3 < GAMMA_LIGHT_BITS) {
            snprintf(name, sizeof name, "lt_y%d", i);
            y = not1(b, name, la[i + 3]);
        }
        b_full_adder(b, lt[i] = cellf(b, "lt%d", i), lc, la[i], y, lc);
    }
    uint32_t nocc = not1(b, "lt_n", occa);
    for (int i = 0; i < GAMMA_LIGHT_BITS; i++) {
        b_mux(b, lt[i], occa, nocc, i + 1 < GAMMA_LIGHT_BITS ? la[i + 1] : zero, lt[i]);
        /* the sun plane: the triangle wave of the clock, 2 * (ck < 128 ? ck : 255 - ck): bit 0
           is 0, bit i is bit i - 1 of the clock, inverted in the afternoon */
        if (i == 0) {
            b_gate2(b, G_AND, lt[i], lt[i], nsun);
        } else {
            snprintf(name, sizeof name, "wv%d", i);
            uint32_t wave = gate2(b, G_XOR, name, ck[i - 1], ck[GAMMA_CLOCK_BITS - 1]);
            b_mux(b, lt[i], sun, nsun, wave, lt[i]);
        }
        b_reg(b, L[i], lt[i]);
    }
    PART(GAMMA_PART_LIGHT);

    /* minerals: (2 m + the 6 neighbours) / 8, less what a cell eats (m/512 * 64, never more), plus
       what a dying one gives back, at most 4095 */
    uint32_t acc[GAMMA_MINERAL_BITS + 4], *d = acc + 3, sc2 = gate(b, G_BUF, "ms_c", &one, 1);
    w = GAMMA_MINERAL_BITS;
    max = GAMMA_MINERAL_MAX;
    memcpy(acc, mn[0], sizeof mn[0]);
    for (int k = 1; k < 6; k++)
        add_into(b, acc, &w, &max, mn[k], GAMMA_MINERAL_BITS, 0, "ms");
    add_into(b, acc, &w, &max, M, GAMMA_MINERAL_BITS, 1, "ms");
    for (int i = 6; i < GAMMA_MINERAL_BITS; i++) {
        uint32_t y = one;
        if (i < 9) {
            snprintf(name, sizeof name, "et%d", i);
            y = gate2(b, G_NAND, name, c->alive, M[i + 3]);  /* ~(alive & M[i + 3]) */
        }
        b_full_adder(b, d[i], sc2, d[i], y, sc2);
    }
    w = GAMMA_MINERAL_BITS;
    max = GAMMA_MINERAL_MAX;
    add_into(b, d, &w, &max, &die, 1, 9, "mr");
    for (int i = 0; i < GAMMA_MINERAL_BITS; i++) {
        b_gate2(b, G_OR, d[i], d[i], d[GAMMA_MINERAL_BITS]);  /* at most 4095 */
        b_gate2(b, G_AND, d[i], d[i], nsun);                  /* the sun plane: 0 */
        b_gate2(b, G_OR, d[i], d[i], floor);                  /* the floor: all ones */
        b_reg(b, M[i], d[i]);
    }
    PART(GAMMA_PART_MINERALS);
    b_finish(b, p);
    c->cost[GAMMA_PART_REGISTERS] = p->len - mark;
#undef PART
    return 1;
}

/* ---- moving a world in and out of the cube ------------------------------------------------ */

static unsigned igray(unsigned v)
{
    for (unsigned s = 1; s < 32; s <<= 1)
        v ^= v >> s;
    return v;
}

static size_t voxel_of(const GammaCells *c, const GammaWorld *w, unsigned m)
{
    unsigned X = 1u << c->xbits, Y = 1u << c->ybits;
    return gamma_voxel(w, igray(m & (X - 1)), igray((m >> c->xbits) & (Y - 1)),
                       igray(m >> (c->xbits + c->ybits)));
}

static void put(LaneMachine *lm, const uint32_t *cells, int n, unsigned lane, uint32_t v)
{
    for (int i = 0; i < n; i++)
        lane_set(lm, cells[i], lane, (uint8_t)(v >> i & 1));
}

static uint32_t get(const LaneMachine *lm, const uint32_t *cells, int n, unsigned lane)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)lane_get(lm, cells[i], lane) << i;
    return v;
}

void gamma_to_cube(LaneCube *lc, const GammaCells *c, const GammaWorld *w)
{
    for (unsigned grp = 0; grp < lc->groups; grp++)
        for (unsigned lane = 0; lane < LANES; lane++) {
            LaneMachine *lm = &lc->g[grp];
            size_t v = voxel_of(c, w, grp * LANES + lane);
            put(lm, &c->alive, 1, lane, w->alive[v]);
            put(lm, &c->busy, 1, lane, w->busy[v]);
            put(lm, &c->embryo, 1, lane, w->embryo[v]);
            put(lm, &c->request, 1, lane, w->request[v]);
            put(lm, &c->born, 1, lane, w->born[v]);
            put(lm, c->offer, GAMMA_OFFER_BITS, lane, w->offer[v]);
            put(lm, c->signal, 2, lane, w->gates[v] >> GAMMA_SIGNAL & 3u);
            put(lm, c->partner, 3, lane, w->partner[v]);
            put(lm, c->counter, 5, lane, w->counter[v]);
            put(lm, c->energy, GAMMA_ENERGY_BITS, lane, w->energy[v]);
            put(lm, c->clock, GAMMA_CLOCK_BITS, lane, w->clock[v]);
            put(lm, c->term, GAMMA_TERM_BITS, lane, w->term[v]);
            for (int i = 0; i < GAMMA_GATES; i++)
                put(lm, c->genome[i], GAMMA_GATE_BITS, lane, w->genome[GAMMA_GATES * v + i]);
            put(lm, c->gates, GAMMA_GATES, lane, w->gates[v]);
            put(lm, c->rng, 32, lane, w->rng[v]);
            put(lm, c->light, GAMMA_LIGHT_BITS, lane, w->light[v]);
            put(lm, c->mineral, GAMMA_MINERAL_BITS, lane, w->mineral[v]);
        }
}

void gamma_from_cube(const LaneCube *lc, const GammaCells *c, GammaWorld *w)
{
    for (unsigned grp = 0; grp < lc->groups; grp++)
        for (unsigned lane = 0; lane < LANES; lane++) {
            const LaneMachine *lm = &lc->g[grp];
            size_t v = voxel_of(c, w, grp * LANES + lane);
            w->alive[v] = (uint8_t)get(lm, &c->alive, 1, lane);
            w->busy[v] = (uint8_t)get(lm, &c->busy, 1, lane);
            w->embryo[v] = (uint8_t)get(lm, &c->embryo, 1, lane);
            w->request[v] = (uint8_t)get(lm, &c->request, 1, lane);
            w->born[v] = (uint8_t)get(lm, &c->born, 1, lane);
            w->offer[v] = (uint8_t)get(lm, c->offer, GAMMA_OFFER_BITS, lane);
            w->partner[v] = (uint8_t)get(lm, c->partner, 3, lane);
            w->counter[v] = (uint8_t)get(lm, c->counter, 5, lane);
            w->energy[v] = (uint8_t)get(lm, c->energy, GAMMA_ENERGY_BITS, lane);
            w->clock[v] = (uint8_t)get(lm, c->clock, GAMMA_CLOCK_BITS, lane);
            w->term[v] = (uint16_t)get(lm, c->term, GAMMA_TERM_BITS, lane);
            for (int i = 0; i < GAMMA_GATES; i++)
                w->genome[GAMMA_GATES * v + i] =
                    (uint16_t)get(lm, c->genome[i], GAMMA_GATE_BITS, lane);
            w->gates[v] = get(lm, c->gates, GAMMA_GATES, lane);
            w->rng[v] = get(lm, c->rng, 32, lane);
            w->light[v] = (uint8_t)get(lm, c->light, GAMMA_LIGHT_BITS, lane);
            w->mineral[v] = (uint16_t)get(lm, c->mineral, GAMMA_MINERAL_BITS, lane);
        }
}
