/* Compiled with /arch:AVX2. Callers check cpu_has_avx2() first. */
#include "lanes.h"

#include <assert.h>
#include <immintrin.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE inline __attribute__((always_inline))
#endif

_Static_assert(sizeof(Line) == 64, "a line is one cache line");
_Static_assert(LANES == 8 * 64, "512 lanes per line");

static Line *alloc_lines(size_t n)
{
#ifdef _MSC_VER
    return _aligned_malloc(n * sizeof(Line), 64);
#else
    return aligned_alloc(64, n * sizeof(Line));
#endif
}

static void free_lines(Line *p)
{
#ifdef _MSC_VER
    _aligned_free(p);
#else
    free(p);
#endif
}

/* ---- moving lanes across links ------------------------------------------------------------ */

/* Lane k is bit k & 7 of byte k >> 3, so crossing dimension d swaps: bits inside each byte for
   d = 0..2, bytes for d = 3..5, 64-bit words for d = 6..8. The compiler emits the same steps. */
static ALWAYS_INLINE __m256i swap_bits(__m256i x, unsigned d)
{
    static const unsigned char low[3] = {0x55, 0x33, 0x0F};
    const __m256i m = _mm256_set1_epi8((char)low[d]);
    const int s = 1 << d;
    return _mm256_or_si256(_mm256_slli_epi64(_mm256_and_si256(x, m), s),
                           _mm256_and_si256(_mm256_srli_epi64(x, s), m));
}

static ALWAYS_INLINE void permute(__m256i *lo, __m256i *hi, unsigned mask)
{
    const __m256i bytes3 = _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
                                            1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    const __m256i bytes4 = _mm256_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13,
                                            2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
    for (unsigned d = 0; d < 3; d++) {
        if (mask & (1u << d)) {
            *lo = swap_bits(*lo, d);
            *hi = swap_bits(*hi, d);
        }
    }
    if (mask & 8) {
        *lo = _mm256_shuffle_epi8(*lo, bytes3);
        *hi = _mm256_shuffle_epi8(*hi, bytes3);
    }
    if (mask & 16) {
        *lo = _mm256_shuffle_epi8(*lo, bytes4);
        *hi = _mm256_shuffle_epi8(*hi, bytes4);
    }
    if (mask & 32) {
        *lo = _mm256_shuffle_epi32(*lo, 0xB1);
        *hi = _mm256_shuffle_epi32(*hi, 0xB1);
    }
    if (mask & 64) {
        *lo = _mm256_shuffle_epi32(*lo, 0x4E);
        *hi = _mm256_shuffle_epi32(*hi, 0x4E);
    }
    if (mask & 128) {
        *lo = _mm256_permute4x64_epi64(*lo, 0x4E);
        *hi = _mm256_permute4x64_epi64(*hi, 0x4E);
    }
    if (mask & 256) {
        __m256i t = *lo;
        *lo = *hi;
        *hi = t;
    }
}

void lane_permute(Line *out, const Line *line, unsigned mask)
{
    __m256i lo = _mm256_loadu_si256((const __m256i *)&line->w[0]);
    __m256i hi = _mm256_loadu_si256((const __m256i *)&line->w[4]);
    permute(&lo, &hi, mask);
    _mm256_storeu_si256((__m256i *)&out->w[0], lo);
    _mm256_storeu_si256((__m256i *)&out->w[4], hi);
}

/* ---- the machine -------------------------------------------------------------------------- */

static void fill_extra(Line *x)
{
    static const unsigned char low[3] = {0x55, 0x33, 0x0F};
    for (int d = 0; d < 3; d++)
        memset(&x[LINE_BITS0 + d], low[d], sizeof(Line));
    for (int i = 0; i < 64; i++) {  /* byte i of each 16-byte block comes from byte i ^ 1, i ^ 2 */
        ((unsigned char *)&x[LINE_BYTES3])[i] = (unsigned char)((i & 15) ^ 1);
        ((unsigned char *)&x[LINE_BYTES4])[i] = (unsigned char)((i & 15) ^ 2);
    }
}

unsigned lane_peers(const Program *p, uint32_t *gm)
{
    unsigned n = 1;
    gm[0] = 0;
    for (size_t i = 0; p->special && i < p->len; i++) {
        unsigned mask, s = 0;
        if (p->code[i].op != OP_NAND)
            continue;
        program_resolve(p, p->code[i].addr, &mask);
        while (s < n && gm[s] != mask >> CUBE_DIMS)
            s++;
        if (s == n)
            gm[n++] = mask >> CUBE_DIMS;
    }
    return n;
}

int lane_init(LaneMachine *m, const Program *p)
{
    uint32_t gm[CUBE_MAX_GROUPS];
    memset(m, 0, sizeof *m);
    m->prog = p;
    m->npeers = lane_peers(p, gm);
    m->mem = alloc_lines(p->ncells + LINE_EXTRA);
    m->ops = malloc((p->len ? p->len : 1) * sizeof *m->ops);
    m->peer = calloc(m->npeers, sizeof *m->peer);
    if (!m->mem || !m->ops || !m->peer) {
        free_lines(m->mem);
        free(m->ops);
        free(m->peer);
        return 0;
    }
    m->peer[0] = m->mem;  /* the others are set by lane_cube_init */
    for (size_t i = 0; i < p->len; i++) {
        LaneOp *op = &m->ops[i];
        unsigned mask = 0, s = 0;
        uint32_t a = p->code[i].addr;
        op->mask = 0;
        op->peer = 0;
        if (p->code[i].op == OP_STO) {
            op->op = program_readonly(p, a) ? LOP_NONE : LOP_STO;
            op->addr = a;
        } else {
            op->addr = program_resolve(p, a, &mask);
            while (gm[s] != mask >> CUBE_DIMS)
                s++;
            op->mask = (uint16_t)(mask & (LANES - 1));
            op->peer = (uint8_t)s;
            op->op = mask ? LOP_NAND_LINK : LOP_NAND;
        }
    }
    fill_extra(&m->mem[p->ncells]);
    lane_reset(m);
    return 1;
}

void lane_free(LaneMachine *m)
{
    free_lines(m->mem);
    free(m->ops);
    free(m->peer);
    m->mem = NULL;
    m->ops = NULL;
    m->peer = NULL;
}

void lane_reset(LaneMachine *m)
{
    const Program *p = m->prog;
    memset(m->mem, 0, p->ncells * sizeof(Line));
    memset(&m->mem[CELL_ONE], 0xFF, sizeof(Line));
    for (uint32_t c = CELL_FIRST_FREE; p->special && c < p->ncells; c++)
        if (p->special[c].kind == CK_ID) {  /* lane k holds bit d of its machine, 512 group + k */
            unsigned d = p->special[c].dim;
            for (unsigned k = 0; k < LANES; k++)
                if (d < CUBE_DIMS ? (k >> d) & 1 : (m->group >> (d - CUBE_DIMS)) & 1)
                    m->mem[c].w[k >> 6] |= 1ull << (k & 63);
        }
    memset(&m->rr, 0, sizeof m->rr);
    m->pc = 0;
    m->steps = 0;
    m->ticks = 0;
}

/* One instruction for all 512 lanes; r0 holds lanes 0-255, r1 lanes 256-511. This is the only
   definition of the instruction semantics in the engine. A read across links may be in another
   group's memory: peer[op->peer]. */
static ALWAYS_INLINE void exec(Line *mem, Line *const *peer, const LaneOp *op, __m256i *r0,
                               __m256i *r1)
{
    const __m256i ones = _mm256_set1_epi32(-1);
    __m256i *line = (__m256i *)&(op->op == LOP_NAND_LINK ? peer[op->peer] : mem)[op->addr];
    if (op->op == LOP_NAND) {
        *r0 = _mm256_xor_si256(_mm256_and_si256(*r0, _mm256_load_si256(line)), ones);
        *r1 = _mm256_xor_si256(_mm256_and_si256(*r1, _mm256_load_si256(line + 1)), ones);
    } else if (op->op == LOP_STO) {
        _mm256_store_si256(line, *r0);
        _mm256_store_si256(line + 1, *r1);
    } else if (op->op == LOP_NAND_LINK) {
        __m256i lo = _mm256_load_si256(line), hi = _mm256_load_si256(line + 1);
        permute(&lo, &hi, op->mask);
        *r0 = _mm256_xor_si256(_mm256_and_si256(*r0, lo), ones);
        *r1 = _mm256_xor_si256(_mm256_and_si256(*r1, hi), ones);
    }
}

void lane_step(LaneMachine *m)
{
    const Program *p = m->prog;
    if (p->len == 0) {
        m->ticks++;
        return;
    }
    __m256i r0 = _mm256_loadu_si256((const __m256i *)&m->rr.w[0]);
    __m256i r1 = _mm256_loadu_si256((const __m256i *)&m->rr.w[4]);
    assert(m->peer[m->ops[m->pc].peer]);  /* a peer group: only inside a LaneCube */
    exec(m->mem, m->peer, &m->ops[m->pc], &r0, &r1);
    _mm256_storeu_si256((__m256i *)&m->rr.w[0], r0);
    _mm256_storeu_si256((__m256i *)&m->rr.w[4], r1);
    m->steps++;
    if (++m->pc == p->len) {
        m->pc = 0;
        m->ticks++;
    }
}

void lane_tick(LaneMachine *m)
{
    const Program *p = m->prog;
    const LaneOp *ops = m->ops;
    Line *mem = m->mem;
    size_t start = m->pc;
    __m256i r0 = _mm256_loadu_si256((const __m256i *)&m->rr.w[0]);
    __m256i r1 = _mm256_loadu_si256((const __m256i *)&m->rr.w[4]);
    for (unsigned s = 0; s < m->npeers; s++)
        assert(m->peer[s]);  /* other groups: only inside a LaneCube, which steps them together */
    for (size_t i = start; i < p->len; i++)
        exec(mem, m->peer, &ops[i], &r0, &r1);
    _mm256_storeu_si256((__m256i *)&m->rr.w[0], r0);
    _mm256_storeu_si256((__m256i *)&m->rr.w[4], r1);
    m->steps += p->len - start;
    m->pc = 0;
    m->ticks++;
}

uint8_t lane_get(const LaneMachine *m, uint32_t addr, unsigned lane)
{
    assert(addr < m->prog->ncells && lane < LANES);
    unsigned mask;
    addr = program_resolve(m->prog, addr, &mask);  /* id cells hold their pattern */
    const LaneMachine *src = m;
    if (mask >> CUBE_DIMS) {
        assert(m->groups);
        src = &m->groups[m->group ^ (mask >> CUBE_DIMS)];
    }
    lane ^= mask & (LANES - 1);
    return (uint8_t)((src->mem[addr].w[lane >> 6] >> (lane & 63)) & 1);
}

static void put_bit(Line *line, unsigned lane, uint8_t v)
{
    uint64_t bit = 1ull << (lane & 63);
    uint64_t *w = &line->w[lane >> 6];
    *w = v ? *w | bit : *w & ~bit;
}

void lane_set(LaneMachine *m, uint32_t addr, unsigned lane, uint8_t v)
{
    assert(addr < m->prog->ncells && lane < LANES);
    if (!program_readonly(m->prog, addr))
        put_bit(&m->mem[addr], lane, v);
}

uint8_t lane_rr(const LaneMachine *m, unsigned lane)
{
    assert(lane < LANES);
    return (uint8_t)((m->rr.w[lane >> 6] >> (lane & 63)) & 1);
}

void lane_set_rr(LaneMachine *m, unsigned lane, uint8_t v)
{
    assert(lane < LANES);
    put_bit(&m->rr, lane, v);
}

void lane_copy(LaneMachine *dst, const LaneMachine *src)
{
    assert(dst->prog->ncells == src->prog->ncells);
    memcpy(dst->mem, src->mem, src->prog->ncells * sizeof(Line));
    dst->rr = src->rr;
    dst->pc = src->pc;
    dst->steps = src->steps;
    dst->ticks = src->ticks;
}

void lane_diff(const LaneMachine *a, const LaneMachine *b, Line *out)
{
    __m256i d0 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)&a->rr.w[0]),
                                  _mm256_loadu_si256((const __m256i *)&b->rr.w[0]));
    __m256i d1 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)&a->rr.w[4]),
                                  _mm256_loadu_si256((const __m256i *)&b->rr.w[4]));
    for (uint32_t c = CELL_FIRST_FREE; c < a->prog->ncells; c++) {
        const __m256i *x = (const __m256i *)&a->mem[c], *y = (const __m256i *)&b->mem[c];
        d0 = _mm256_or_si256(d0, _mm256_xor_si256(_mm256_load_si256(x), _mm256_load_si256(y)));
        d1 = _mm256_or_si256(d1, _mm256_xor_si256(_mm256_load_si256(x + 1),
                                                  _mm256_load_si256(y + 1)));
    }
    _mm256_storeu_si256((__m256i *)&out->w[0], d0);
    _mm256_storeu_si256((__m256i *)&out->w[4], d1);
}

void lane_diff_some(const LaneMachine *a, const LaneMachine *b, const uint32_t *cells, int n,
                    Line *out)
{
    __m256i d0 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)&a->rr.w[0]),
                                  _mm256_loadu_si256((const __m256i *)&b->rr.w[0]));
    __m256i d1 = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)&a->rr.w[4]),
                                  _mm256_loadu_si256((const __m256i *)&b->rr.w[4]));
    for (int i = 0; i < n; i++) {
        assert(cells[i] < a->prog->ncells);
        const __m256i *x = (const __m256i *)&a->mem[cells[i]], *y = (const __m256i *)&b->mem[cells[i]];
        d0 = _mm256_or_si256(d0, _mm256_xor_si256(_mm256_load_si256(x), _mm256_load_si256(y)));
        d1 = _mm256_or_si256(d1, _mm256_xor_si256(_mm256_load_si256(x + 1),
                                                  _mm256_load_si256(y + 1)));
    }
    _mm256_storeu_si256((__m256i *)&out->w[0], d0);
    _mm256_storeu_si256((__m256i *)&out->w[4], d1);
}

/* ---- more than 512 machines (step 7f) ----------------------------------------------------- */

int lane_cube_init(LaneCube *c, const Program *p)
{
    uint32_t gm[CUBE_MAX_GROUPS];
    unsigned n = lane_peers(p, gm);
    memset(c, 0, sizeof *c);
    c->prog = p;
    c->dims = program_dims(p);
    c->groups = 1u << (c->dims - CUBE_DIMS);
    c->g = calloc(c->groups, sizeof *c->g);
    if (!c->g)
        return 0;
    for (unsigned g = 0; g < c->groups; g++) {
        if (!lane_init(&c->g[g], p)) {
            while (g--)
                lane_free(&c->g[g]);
            free(c->g);
            c->g = NULL;
            return 0;
        }
    }
    for (unsigned g = 0; g < c->groups; g++) {
        LaneMachine *m = &c->g[g];
        m->group = g;
        m->groups = c->g;
        for (unsigned s = 0; s < n; s++)
            m->peer[s] = c->g[g ^ gm[s]].mem;
        lane_reset(m);  /* the id cells of dimensions 9 and up depend on the group */
    }
    return 1;
}

void lane_cube_free(LaneCube *c)
{
    for (unsigned g = 0; c->g && g < c->groups; g++)
        lane_free(&c->g[g]);
    free(c->g);
    c->g = NULL;
}

void lane_cube_reset(LaneCube *c)
{
    for (unsigned g = 0; g < c->groups; g++)
        lane_reset(&c->g[g]);
}

/* Group by group is the same as all at once, as in the reference cube: a NAND changes only RR,
   and during a STO nobody reads. */
void lane_cube_step(LaneCube *c)
{
    for (unsigned g = 0; g < c->groups; g++)
        lane_step(&c->g[g]);
}

void lane_cube_tick(LaneCube *c)
{
    if (c->groups == 1) {
        lane_tick(&c->g[0]);
        return;
    }
    uint64_t t = c->g[0].ticks;
    while (c->g[0].ticks == t)
        lane_cube_step(c);
}

uint8_t lane_cube_get(const LaneCube *c, uint32_t addr, unsigned machine)
{
    assert(machine >> CUBE_DIMS < c->groups);
    return lane_get(&c->g[machine >> CUBE_DIMS], addr, machine & (LANES - 1));
}

void lane_cube_set(LaneCube *c, uint32_t addr, unsigned machine, uint8_t v)
{
    assert(machine >> CUBE_DIMS < c->groups);
    lane_set(&c->g[machine >> CUBE_DIMS], addr, machine & (LANES - 1), v);
}
