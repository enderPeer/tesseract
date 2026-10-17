/* alpha gamma (step 11): the world of docs/GAMMA.md on the cube. The fields settle, a soup of cells
   with random genomes goes in, the compiled cube is checked against plain C after every one of the
   first ticks, and then runs on all threads, with a census every so often, and under each census
   the ledger of 16 ticks of plain C from there. With --serve the census gives way to the live
   view (step 11e): the world runs batch by batch and a page in the browser looks at it. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamma.h"
#include "gammaserve.h"
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
          "                   [--threads N] [--exclude LIST] [--idle] [--serve PORT]\n"
          "  --size X,Y,Z   the world, voxels per axis (powers of two, 2 to 512 each, 512 to\n"
          "                 65536 in all; one number for a cube), default 32\n"
          "  --soup P       percent of voxels that start with a cell, random genome (default 10)\n"
          "  --energy E     the energy each of them starts with (default 64)\n"
          "  --settle N     ticks of the empty world first, with the sun cycling, for light and\n"
          "                 minerals (default 2000)\n"
          "  --check C      ticks checked against plain C, every voxel (default 100)\n"
          "  --ticks T      ticks in all (default 10000), a census every K (default T/10)\n"
          "  --seconds S    or stop at the first census after S seconds of running\n"
          "  --show         at the end, a slice through the world\n"
          "  --genes N      at the end, the N most common genomes as circuits\n"
          "  --serve PORT   a live view of the world in the browser at http://127.0.0.1:PORT/\n"
          "                 (instead of the census; --every ticks per batch, default 500)\n",
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

enum { LEDGER_TICKS = 16 };  /* ticks of plain C under each census line */

static void census_head(unsigned Z)
{
    printf("\n%9s %7s %6s %7s %6s %7s %6s %6s %6s %7s %6s %7s %4s %7s %7s cells by depth: z 0-%u "
           "| %u-%u | %u-%u | %u-%u\n", "tick", "cells", "busy", "genomes", "top", "energy",
           "ask", "got", "give", "flow", "kin", "hoard", "day", "bodies", "largest", Z / 4 - 1,
           Z / 4, Z / 2 - 1, Z / 2, 3 * Z / 4 - 1, 3 * Z / 4, Z - 1);
    printf("%9s %8s %8s %7s %7s %7s %7s %6s %7s | deaths %5s %7s %7s | grants %5s %5s %5s "
           "%5s %5s %5s\n", "per tick", "light", "minerals", "cap", "upkeep", "rot", "death",
           "split", "given", "term", "starved", "aborted", "+x", "-x", "+y", "-y", "down", "up");
}

/* The bodies: connected groups of 2 or more cells with the same genome (face neighbours, the
   cells being born left out). Returns how many; *largest gets the biggest, *in_bodies the cells
   in them. */
static size_t bodies(const GammaWorld *w, size_t *largest, size_t *in_bodies)
{
    uint64_t *h = malloc(w->n * sizeof *h);
    unsigned char *seen = calloc(w->n, 1);
    size_t *stack = malloc(w->n * sizeof *stack), n = 0;
    *largest = *in_bodies = 0;
    for (size_t v = 0; v < w->n; v++)
        h[v] = w->alive[v] && !(w->busy[v] && w->embryo[v]) ? genome_hash(w->genome + GAMMA_GATES * v)
                                                              : 0;
    for (size_t v = 0; v < w->n; v++) {
        if (!h[v] || seen[v])
            continue;
        size_t size = 0, sp = 0;
        stack[sp++] = v;
        seen[v] = 1;
        while (sp) {
            size_t u = stack[--sp];
            size++;
            for (int d = 0; d < 6; d++) {
                size_t t = gamma_neighbour(w, u, d);
                if (h[t] == h[v] && !seen[t]) {
                    seen[t] = 1;
                    stack[sp++] = t;
                }
            }
        }
        if (size >= 2) {
            n++;
            *in_bodies += size;
            if (size > *largest)
                *largest = size;
        }
    }
    free(h);
    free(seen);
    free(stack);
    return n;
}

/* ask: of the cells that were free at the start of the tick, how many asked to divide; got: of
   those, how many were granted a voxel (they are busy now, with the counter at 0); give: of the
   free cells, how many gave energy away; flow: the energy given, per tick; kin: of it, the share
   offered to a cell that holds the giver's genome, unrotated, at the end of the tick (the one in
   its direction, gates 29-31, which a living giver still holds; a dividing receiver or one that
   died this tick does not count: a lower bound); hoard: what midnight would take now, the energy
   above 127 summed; day: the clock. */
static void census(const GammaWorld *w, unsigned long long tick)
{
    unsigned Z = 1u << w->zbits;
    size_t cells = 0, dividing = 0, band[4] = {0, 0, 0, 0}, free_ = 0, ask = 0, got = 0, give = 0;
    size_t nbodies, largest, in_bodies, hoard = 0;
    double energy = 0, flow = 0, kin = 0;
    for (size_t v = 0; v < w->n; v++) {
        if (!w->alive[v])
            continue;
        unsigned z = (unsigned)(v >> (w->xbits + w->ybits));
        cells++;
        dividing += w->busy[v];
        energy += w->energy[v];
        band[z * 4 / Z]++;
        flow += w->offer[v];
        if (w->energy[v] > GAMMA_OMER)
            hoard += w->energy[v] - GAMMA_OMER;
        if (w->offer[v]) {
            size_t to = gamma_neighbour(w, v, (int)(w->gates[v] >> (GAMMA_DIVIDE + 1) & 7));
            if (w->alive[to] && genome_hash(w->genome + GAMMA_GATES * to) ==
                                    genome_hash(w->genome + GAMMA_GATES * v))
                kin += w->offer[v];
        }
        if (w->busy[v] && (w->embryo[v] || w->counter[v] > 0))  /* was busy before the tick */
            continue;
        free_++;
        give += w->offer[v] > 0;
        if (w->request[v]) {
            ask++;
            got += w->busy[v];
        }
    }
    Kind *k;
    size_t nk = kinds_of(w, &k), settled = 0;
    for (size_t i = 0; i < nk; i++)
        settled += k[i].count;
    nbodies = bodies(w, &largest, &in_bodies);
    printf("%9llu %7zu %6zu %7zu %5.1f%% %7.1f %5.1f%% %5.1f%% %5.1f%% %7.0f %5.1f%% %7zu %4u "
           "%7zu %7zu %7zu | %5zu | %5zu | %5zu\n", tick, cells, dividing, nk,
           settled ? 100.0 * (double)k[0].count / (double)settled : 0.0,
           cells ? energy / (double)cells : 0.0, free_ ? 100.0 * (double)ask / (double)free_ : 0.0,
           ask ? 100.0 * (double)got / (double)ask : 0.0,
           free_ ? 100.0 * (double)give / (double)free_ : 0.0, flow,
           flow > 0 ? 100.0 * kin / flow : 0.0, hoard, (unsigned)w->clock[0], nbodies, largest,
           band[0], band[1], band[2], band[3]);
    fflush(stdout);
    free(k);
}

/* The ledger: a copy of the world runs LEDGER_TICKS ticks of plain C (in `scratch`, made with
   gamma_world_init once), the ledgers summed and printed per tick, and whether every one of the
   single ticks closes (the two identities of gamma.h). The cube is not touched. */
static void ledger_line(const GammaWorld *w, GammaWorld *scratch)
{
    GammaLedger sum = {0};
    int closes = 1;
    gamma_world_copy(scratch, w);
    for (int i = 0; i < LEDGER_TICKS; i++) {
        gamma_step(scratch);
        gamma_ledger_add(&sum, &scratch->ledger);
        closes &= gamma_ledger_closes(&scratch->ledger);
    }
    const double per = 1.0 / LEDGER_TICKS;
    printf("%9s %8.1f %8.1f %7.1f %7.1f %7.1f %7.1f %6.1f %7.1f | deaths %5.1f %7.1f %7.1f | grants "
           "%5.1f %5.1f %5.1f %5.1f %5.1f %5.1f | %s\n", "16 ticks", (double)sum.light * per,
           (double)sum.minerals * per, (double)sum.cap * per, (double)sum.upkeep * per,
           (double)sum.rot * per, (double)sum.death * per, (double)sum.split * per,
           (double)sum.given * per, (double)sum.term * per, (double)sum.starved * per,
           (double)sum.aborted * per, (double)sum.grants_by_dir[0] * per,
           (double)sum.grants_by_dir[1] * per, (double)sum.grants_by_dir[2] * per,
           (double)sum.grants_by_dir[3] * per, (double)sum.grants_by_dir[4] * per,
           (double)sum.grants_by_dir[5] * per, closes ? "closes" : "DOES NOT CLOSE");
    fflush(stdout);
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

/* ---- a genome as a circuit ---------------------------------------------------------------- */

static const char *const dir_name[6] = {"+x", "-x", "+y", "-y", "down", "up"};

/* the name of source a (0 .. 63) of a gate: a sense, or a gate */
static void source_name(unsigned a, char *buf, size_t len)
{
    if (a == 0)
        snprintf(buf, len, "1");
    else if (a < 4)
        snprintf(buf, len, "rnd%u", a - 1);
    else if (a < 10)
        snprintf(buf, len, "occ:%s", dir_name[a - 4]);
    else if (a < 22)
        snprintf(buf, len, "sig:%s.%u", dir_name[(a - 10) / 2], (a - 10) % 2);
    else if (a < 25)
        snprintf(buf, len, "E%u", 29 - a);  /* the top bits: E7, E6, E5 */
    else if (a < 28)
        snprintf(buf, len, "L%u", 32 - a);
    else if (a < 31)
        snprintf(buf, len, "M%u", 39 - a);
    else if (a == 31)
        snprintf(buf, len, "born");
    else
        snprintf(buf, len, "g%u", a - 32);
}

/* appends to buf (len bytes, at characters used so far) like snprintf; returns the new length */
static size_t cat(char *buf, size_t len, size_t at, const char *fmt, ...)
{
    va_list ap;
    if (at >= len)
        return at;
    va_start(ap, fmt);
    int n = vsnprintf(buf + at, len - at, fmt, ap);
    va_end(ap);
    return n < 0 ? at : at + (size_t)n < len ? at + (size_t)n : len - 1;
}

enum { GENOME_TEXT = 4096 };  /* room for print_genome: 33 lines */

/* One genome as a circuit, into buf: every gate with its two sources, which gates feed the
   actions (gates 26 .. 31, backwards along the wires), and which are constant whatever the senses
   say (a NAND with a 0 input is 1, with two 1 inputs 0; repeated until nothing changes, since
   later gates feed earlier ones a tick later). Returns the length. */
static size_t print_genome(const uint16_t *g, char *buf, size_t len)
{
    static const char *const action[7] = {"  <- give", "  <- signal 0", "  <- signal 1",
                                          "  <- divide", "  <- dir bit 0", "  <- dir bit 1",
                                          "  <- dir bit 2"};
    int live[GAMMA_GATES] = {0}, val[64], stack[GAMMA_GATES], n = 0;
    for (int i = GAMMA_GIVE; i < GAMMA_GATES; i++)
        live[stack[n++] = i] = 1;
    while (n) {
        int i = stack[--n];
        for (int h = 0; h < 2; h++) {
            unsigned a = g[i] >> (GAMMA_ADDR_BITS * h) & 63;
            if (a >= 32 && !live[a - 32])
                live[stack[n++] = (int)a - 32] = 1;
        }
    }
    for (int a = 0; a < 64; a++)
        val[a] = a == 0 ? 1 : -1;  /* -1: depends on the senses */
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < GAMMA_GATES; i++) {
            int a = val[g[i] & 63], b = val[g[i] >> GAMMA_ADDR_BITS & 63];
            int v = a == 0 || b == 0 ? 1 : a == 1 && b == 1 ? 0 : -1;
            if (v != val[32 + i]) {
                val[32 + i] = v;
                changed = 1;
            }
        }
    }
    int nlive = 0, dir = 0, dir_known = 1;
    for (int i = 0; i < GAMMA_GATES; i++)
        nlive += live[i];
    for (int j = 0; j < 3; j++) {
        if (val[32 + GAMMA_DIVIDE + 1 + j] < 0)
            dir_known = 0;
        dir |= val[32 + GAMMA_DIVIDE + 1 + j] == 1 ? 1 << j : 0;
    }
    static const char *const fixed[3] = {"always off", "always on", "decided by the senses"};
    size_t at = cat(buf, len, 0,
                    "  %d of 32 gates feed the actions; divide is %s, give is %s, the direction %s\n",
                    nlive, fixed[val[32 + GAMMA_DIVIDE] < 0 ? 2 : val[32 + GAMMA_DIVIDE]],
                    fixed[val[32 + GAMMA_GIVE] < 0 ? 2 : val[32 + GAMMA_GIVE]],
                    !dir_known ? "decided by the senses" : dir < 6 ? dir_name[dir] : "none (6 or 7)");
    for (int i = 0; i < GAMMA_GATES; i++) {
        char a[24], b[24];
        source_name(g[i] & 63, a, sizeof a);
        source_name(g[i] >> GAMMA_ADDR_BITS & 63, b, sizeof b);
        at = cat(buf, len, at, "  %c g%-2d = NAND(%-10s %-10s)%s%s\n", live[i] ? '*' : ' ', i, a, b,
                 val[32 + i] < 0 ? "" : val[32 + i] ? "  = 1 always" : "  = 0 always",
                 i >= GAMMA_GIVE ? action[i - GAMMA_GIVE] : "");
    }
    return at;
}

/* the n most common genomes, 'A', 'B', .. as in the slice */
static void print_genomes(const GammaWorld *w, int n)
{
    Kind *k;
    size_t nk = kinds_of(w, &k), settled = 0;
    char text[GENOME_TEXT];
    for (size_t i = 0; i < nk; i++)
        settled += k[i].count;
    printf("\nthe %d most common genomes of %zu, '*' marks the gates that feed the actions:\n",
           n < (int)nk ? n : (int)nk, nk);
    for (int i = 0; i < n && (size_t)i < nk; i++) {
        size_t v = 0;
        while (v < w->n && !(w->alive[v] && !(w->busy[v] && w->embryo[v]) &&
                             genome_hash(w->genome + GAMMA_GATES * v) == k[i].hash))
            v++;
        printf("\ngenome %c: %zu cells (%.1f %%)\n", 'A' + i, k[i].count,
               100.0 * (double)k[i].count / (double)settled);
        print_genome(w->genome + GAMMA_GATES * v, text, sizeof text);
        fputs(text, stdout);
    }
    free(k);
}

/* ---- the live view (step 11e) ------------------------------------------------------------- */

/* The frame the page polls, little-endian: "GAMA"; u32 version 1; u64 tick; u8 xbits, ybits,
   zbits, clock; u32 cells, busy (dividing), genomes (distinct, among the cells not being born),
   ticks per second (0 while paused); u8 paused, 3 bytes 0; 26 x u32 the cells of the genomes
   'A' .. 'Z' (0 beyond the count); then 2 bytes per voxel in voxel order, the class (0 empty,
   1 .. 26 a cell of genome 'A' .. 'Z', 27 of another, 28 being born) and the energy; then for
   every layer z the average light and the average minerals >> 4, a byte each. */
enum { FRAME_LETTERS = 26, FRAME_HEAD = 40 + 4 * FRAME_LETTERS };  /* the voxels start at 144 */

static void put32(uint8_t *p, uint32_t x)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(x >> 8 * i);
}

static void put64(uint8_t *p, uint64_t x)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(x >> 8 * i);
}

static size_t frame_size(const GammaWorld *w)
{
    return FRAME_HEAD + 2 * w->n + 2 * ((size_t)1 << w->zbits);
}

static void build_frame(const GammaWorld *w, unsigned long long tick, unsigned tps, int paused,
                        uint8_t *f)
{
    size_t layer = (size_t)1 << (w->xbits + w->ybits), Z = (size_t)1 << w->zbits;
    Kind *k;
    size_t nk = kinds_of(w, &k), cells = 0, busy = 0;
    for (size_t v = 0; v < w->n; v++) {
        cells += w->alive[v];
        busy += w->alive[v] && w->busy[v];
    }
    memcpy(f, "GAMA", 4);
    put32(f + 4, 1);
    put64(f + 8, tick);
    f[16] = (uint8_t)w->xbits;
    f[17] = (uint8_t)w->ybits;
    f[18] = (uint8_t)w->zbits;
    f[19] = w->clock[0];
    put32(f + 20, (uint32_t)cells);
    put32(f + 24, (uint32_t)busy);
    put32(f + 28, (uint32_t)nk);
    put32(f + 32, tps);
    f[36] = (uint8_t)paused;
    f[37] = f[38] = f[39] = 0;
    for (size_t i = 0; i < FRAME_LETTERS; i++)
        put32(f + 40 + 4 * i, i < nk ? (uint32_t)k[i].count : 0);
    uint8_t *p = f + FRAME_HEAD;
    for (size_t v = 0; v < w->n; v++, p += 2) {
        unsigned cls = 0;
        if (w->alive[v] && w->busy[v] && w->embryo[v]) {
            cls = FRAME_LETTERS + 2;
        } else if (w->alive[v]) {
            uint64_t h = genome_hash(w->genome + GAMMA_GATES * v);
            cls = FRAME_LETTERS + 1;
            for (size_t i = 0; i < nk && i < FRAME_LETTERS; i++)
                if (k[i].hash == h)
                    cls = (unsigned)i + 1;
        }
        p[0] = (uint8_t)cls;
        p[1] = w->alive[v] ? w->energy[v] : 0;
    }
    for (size_t z = 0; z < Z; z++, p += 2) {
        uint64_t light = 0, mineral = 0;
        for (size_t v = z * layer; v < (z + 1) * layer; v++) {
            light += w->light[v];
            mineral += w->mineral[v];
        }
        p[0] = (uint8_t)(light / layer);
        p[1] = (uint8_t)(mineral / layer >> GAMMA_MINERAL_FRAC);
    }
    free(k);
}

/* /cell?v=N: the voxel and, if a cell is there, its state, the outputs of its 32 gates (gate 0
   first) and its genome as --genes lists it */
static size_t cell_text(const GammaWorld *w, size_t v, char *buf, size_t len)
{
    unsigned X = 1u << w->xbits, Y = 1u << w->ybits;
    size_t at = cat(buf, len, 0, "voxel %zu: x %u, y %u, z %u; light %u, minerals %u\n", v,
                    (unsigned)(v & (X - 1)), (unsigned)(v >> w->xbits & (Y - 1)),
                    (unsigned)(v >> (w->xbits + w->ybits)), w->light[v],
                    w->mineral[v] >> GAMMA_MINERAL_FRAC);
    if (!w->alive[v])
        return cat(buf, len, at, "empty\n");
    at = cat(buf, len, at, "cell: energy %u, term %u, %s\n", w->energy[v], w->term[v],
             !w->busy[v] ? "free" : w->embryo[v] ? "being born" : "dividing");
    at = cat(buf, len, at, "gates: ");
    for (int i = 0; i < GAMMA_GATES; i++)
        at = cat(buf, len, at, "%u", w->gates[v] >> i & 1);
    at = cat(buf, len, at, "\ngenome:\n");
    return at + print_genome(w->genome + GAMMA_GATES * v, buf + at, len > at ? len - at : 0);
}

/* The serve loop, forever: a batch of `every` ticks, the world read back, the frame built, every
   request waiting answered. /pause stops the ticks (the loop then polls 20 times a second),
   /resume starts them again, /step?n=K runs K ticks once while paused. */
static void serve_loop(GammaServer *s, const char *argv0, const JitCode *jc, LaneCube *lc,
                       const GammaCells *cells, GammaWorld *w, int k, const int *ids, int idle,
                       unsigned long long every, unsigned long long t)
{
    static const char text[] = "text/plain; charset=utf-8";
    size_t fsize = frame_size(w);
    uint8_t *frame = malloc(fsize);
    char *cell = malloc(GENOME_TEXT + 512);
    unsigned long long step = 0;
    unsigned tps = 0;
    int paused = 0, changed = 1;
    if (!frame || !cell) {
        fputs("out of memory\n", stderr);
        return;
    }
    for (;;) {
        unsigned long long n = paused ? step : every;
        step = 0;
        if (n) {
            double sec = par_cube_run(jc, lc, k, ids, idle, n, NULL);
            t += n;
            gamma_from_cube(lc, cells, w);
            tps = sec > 0 ? (unsigned)((double)n / sec) : 0;
            changed = 1;
        }
        if (changed)
            build_frame(w, t, paused ? 0 : tps, paused, frame);
        changed = 0;
        GammaRequest r;
        while (serve_poll(s, &r)) {
            long long v;
            if (strcmp(r.method, "GET")) {
                serve_reply(s, 404, text, "GET only\n", 9);
            } else if (!strcmp(r.path, "/")) {
                serve_page(s, argv0);
            } else if (!strcmp(r.path, "/frame")) {
                serve_reply(s, 200, "application/octet-stream", frame, fsize);
            } else if (!strcmp(r.path, "/cell")) {
                if (serve_query_num(&r, "v", &v) && v >= 0 && (unsigned long long)v < w->n)
                    serve_reply(s, 200, text, cell, cell_text(w, (size_t)v, cell, GENOME_TEXT + 512));
                else
                    serve_reply(s, 404, text, "no such voxel\n", 14);
            } else if (!strcmp(r.path, "/pause")) {
                changed |= !paused;
                paused = 1;
                serve_reply(s, 200, text, "paused\n", 7);
            } else if (!strcmp(r.path, "/resume")) {
                changed |= paused;
                paused = 0;
                serve_reply(s, 200, text, "running\n", 8);
            } else if (!strcmp(r.path, "/step")) {
                if (!paused)
                    serve_reply(s, 200, text, "running: pause first\n", 21);
                else if (serve_query_num(&r, "n", &v) && v >= 1 && v <= 100000) {
                    serve_reply(s, 200, text, "ok\n", 3);
                    step = (unsigned long long)v;
                } else
                    serve_reply(s, 404, text, "n must be 1 to 100000\n", 22);
            } else {
                serve_reply(s, 404, text, "not found\n", 10);
            }
        }
        if (paused && !step)
            sleep_seconds(0.05);
    }
}

/* ---- the command -------------------------------------------------------------------------- */

int cmd_gamma(const char *argv0, int argc, char **argv)
{
    int bits[3] = {5, 5, 5}, threads = 0, idle = 0, show = 0, genes = 0;
    unsigned long long settle = 2000, check = 100, ticks = 10000, every = 0, seed = 1, port = 0;
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
        } else if (!strcmp(a, "--genes") && v) {
            genes = atoi(v);
        } else if (!strcmp(a, "--serve") && v) {
            port = strtoull(v, NULL, 10);
            if (port < 1 || port > 65535) {
                fputs("--serve expects a port, 1 to 65535\n", stderr);
                return 2;
            }
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
        every = port ? 500 : ticks / 10 ? ticks / 10 : 1;
    if (!cpu_has_avx2()) {
        fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
        return 1;
    }
    GammaServer server;
    char err[256];
    if (port && !serve_start(&server, (unsigned)port, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }

    Program p;
    GammaCells cells;
    JitCode jc;
    LaneCube lc;
    GammaWorld want, got, scratch;  /* scratch: for the ledger under each census */
    gen_gamma(&p, bits[0], bits[1], bits[2], &cells);
    if (!jit_compile(&p, 0, &jc, NULL, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (!lane_cube_init(&lc, &p) || !gamma_world_init(&want, bits[0], bits[1], bits[2]) ||
        !gamma_world_init(&got, bits[0], bits[1], bits[2]) ||
        !gamma_world_init(&scratch, bits[0], bits[1], bits[2])) {
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

    /* the empty world first, until light and minerals have settled; its clock set so that it reads
       sunrise when the soup goes in, and the column of light below matches */
    memset(want.clock, (int)((GAMMA_SUNRISE - settle) & 255), want.n);
    gamma_to_cube(&lc, &cells, &want);
    if (settle)
        par_cube_run(&jc, &lc, k, ids, idle, settle, NULL);
    gamma_from_cube(&lc, &cells, &want);
    size_t seeded = gamma_soup(&want, soup / 100, energy, seed);
    gamma_to_cube(&lc, &cells, &want);
    printf("the fields settled for %llu ticks; %zu cells went in with random genomes (%.1f %%), "
           "energy %u each,\nat sunrise: the clock at %d of a %d-tick day\n", settle, seeded,
           100.0 * (double)seeded / (double)want.n, energy, GAMMA_SUNRISE, 1 << GAMMA_CLOCK_BITS);

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

    /* then compiled, on all threads: watched from the browser, or with a census */
    double busy_s = 0;
    unsigned long long run = 0;
    if (bad < 0 && port) {
        printf("serving the world at http://127.0.0.1:%llu/ (Ctrl+C stops it)\n", port);
        fflush(stdout);
        serve_loop(&server, argv0, &jc, &lc, &cells, &got, k, ids, idle, every, t);
    } else if (bad < 0) {
        census_head(Z);
        gamma_from_cube(&lc, &cells, &got);
        census(&got, t);
        ledger_line(&got, &scratch);
        while (t < ticks && !(seconds > 0 && busy_s >= seconds)) {
            unsigned long long n = every - t % every;
            if (n > ticks - t)
                n = ticks - t;
            busy_s += par_cube_run(&jc, &lc, k, ids, idle, n, NULL);
            run += n;
            t += n;
            gamma_from_cube(&lc, &cells, &got);
            census(&got, t);
            ledger_line(&got, &scratch);
        }
        if (show)
            print_slice(&got);
        if (genes > 0)
            print_genomes(&got, genes);
        if (run)
            printf("\n%d thread%s: %.3g ticks/s, %.1f us per tick, %.3g voxel updates/s, %.1f T "
                   "1-bit ops/s\n", k, k > 1 ? "s" : "", (double)run / busy_s,
                   1e6 * busy_s / (double)run, (double)run * (double)want.n / busy_s,
                   (double)run * (double)p.len * (double)want.n / busy_s / 1e12);
    }
    if (port)
        serve_stop(&server);
    free(ids);
    gamma_world_free(&want);
    gamma_world_free(&got);
    gamma_world_free(&scratch);
    lane_cube_free(&lc);
    jit_free(&jc);
    program_free(&p);
    return bad >= 0 ? 3 : 0;
}
