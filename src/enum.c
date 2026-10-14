#include "enum.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jit.h"
#include "platform.h"

#define CHUNK 64  /* passes per hand-out */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

void enum_add_hit(EnumResult *r, uint64_t x)
{
    int i;
    if (r->nhit < EN_SHOW) {
        i = r->nhit++;
    } else {
        if (x >= r->hit[EN_SHOW - 1])
            return;
        i = EN_SHOW - 1;
    }
    while (i > 0 && r->hit[i - 1] > x) {
        r->hit[i] = r->hit[i - 1];
        i--;
    }
    r->hit[i] = x;
}

static int match_list(const Program *a, const uint32_t *la, size_t na, const Program *b,
                      const uint32_t *lb, size_t nb, int *map, const char *what, char *err,
                      size_t errlen)
{
    if (na != nb) {
        snprintf(err, errlen, "the programs have %zu and %zu %s", na, nb, what);
        return 0;
    }
    for (size_t i = 0; i < na; i++) {
        map[i] = -1;
        for (size_t j = 0; j < nb; j++)
            if (same_name(a->cell_names[la[i]], b->cell_names[lb[j]]))
                map[i] = (int)j;
        if (map[i] < 0) {
            snprintf(err, errlen, "the second program has no %s called %s",
                     what[0] == 'i' ? "input" : "output", a->cell_names[la[i]]);
            return 0;
        }
    }
    return 1;
}

int enum_match(const Program *a, const Program *b, int *in_map, int *out_map, char *err,
               size_t errlen)
{
    return match_list(a, a->inputs, a->ninputs, b, b->inputs, b->ninputs, in_map, "inputs", err,
                      errlen) &&
           match_list(a, a->outputs, a->noutputs, b, b->outputs, b->noutputs, out_map, "outputs",
                      err, errlen);
}

/* What one program needs before every pass: its input cells in assignment order, and the cells it
   reads before writing (only those must be set back to their power-on 0). */
typedef struct {
    const Program *p;
    JitCode        jc;
    uint32_t      *in;   /* in[k] = cell for input k of program a's order */
    uint32_t      *live;
    size_t         nlive;
    int            rr_live, full;
} Prep;

static void prepare(Prep *pr, const Program *p, const uint32_t *in, unsigned ticks)
{
    pr->p = p;
    pr->in = xmalloc((p->ninputs + 1) * sizeof *pr->in);
    memcpy(pr->in, in, p->ninputs * sizeof *in);
    pr->full = ticks > 1;  /* state carries from pass to pass: reset everything */
    unsigned char *seen = calloc(p->ncells + 1, 1);
    pr->live = xmalloc((p->ncells + 1) * sizeof *pr->live);
    pr->nlive = 0;
    for (size_t k = 0; k < p->ninputs; k++)
        seen[p->inputs[k]] = 1;
    for (size_t i = 0; i < p->len; i++) {
        uint32_t c = p->code[i].addr;
        if (c < CELL_FIRST_FREE || seen[c])
            continue;
        seen[c] = 1;  /* either written first (no reset needed) or read first (reset) */
        if (p->code[i].op == OP_NAND)
            pr->live[pr->nlive++] = c;
    }
    free(seen);
    pr->rr_live = !(p->len > 0 && p->code[0].op == OP_NAND && p->code[0].addr == CELL_ZERO);
}

static Line lane_bit_pattern[9];  /* lane l has bit j of l */

static void set_inputs(const Prep *pr, LaneMachine *m, uint64_t x0, int n)
{
    if (pr->full) {
        lane_reset(m);
    } else {
        for (size_t i = 0; i < pr->nlive; i++)
            memset(&m->mem[pr->live[i]], 0, sizeof(Line));
        if (pr->rr_live)
            memset(&m->rr, 0, sizeof m->rr);
        m->pc = 0;
    }
    for (int k = 0; k < n; k++) {
        int j = n - 1 - k;
        if (j < 9)
            m->mem[pr->in[k]] = lane_bit_pattern[j];
        else
            memset(&m->mem[pr->in[k]], (x0 >> j) & 1 ? 0xFF : 0, sizeof(Line));
    }
}

static int bit(const Line *l, int lane)
{
    return (int)((l->w[lane >> 6] >> (lane & 63)) & 1);
}

typedef struct {
    const Enum    *e;
    const Prep    *pa, *pb;
    const int     *out_map;
    volatile long *next;
    uint64_t       nchunks, passes;
    int            cpu;
    EnumResult     r;
} Worker;

static void run_pass(Worker *w, LaneMachine *ma, LaneMachine *mb, uint64_t pass)
{
    const Enum *e = w->e;
    int n = (int)e->a->ninputs;
    uint64_t x0 = pass * LANES, space = w->r.space, valid_lanes = space - x0;
    Line valid, hit;
    for (int k = 0; k < 8; k++) {
        uint64_t from = 64 * (uint64_t)k;
        valid.w[k] = valid_lanes >= from + 64 ? ~0ull
                   : valid_lanes <= from      ? 0
                                              : (1ull << (valid_lanes - from)) - 1;
    }
    set_inputs(w->pa, ma, x0, n);
    jit_run(&w->pa->jc, ma, e->ticks);
    w->r.ops += e->ticks * w->pa->p->len * LANES;

    if (e->mode == EN_SEARCH) {
        uint64_t target = spec_value(e->spec, 0);
        hit = valid;
        for (int b = 0; b < spec_width(e->spec); b++) {
            const Line *o = &ma->mem[spec_out_cell(e->spec, b)];
            for (int k = 0; k < 8; k++)
                hit.w[k] &= (target >> b) & 1 ? o->w[k] : ~o->w[k];
        }
    } else if (e->mode == EN_EQUIV) {
        set_inputs(w->pb, mb, x0, n);
        jit_run(&w->pb->jc, mb, e->ticks);
        w->r.ops += e->ticks * w->pb->p->len * LANES;
        memset(&hit, 0, sizeof hit);
        for (size_t o = 0; o < e->a->noutputs; o++) {
            const Line *la = &ma->mem[e->a->outputs[o]], *lb = &mb->mem[e->b->outputs[w->out_map[o]]];
            for (int k = 0; k < 8; k++)
                hit.w[k] |= la->w[k] ^ lb->w[k];
        }
        for (int k = 0; k < 8; k++)
            hit.w[k] &= valid.w[k];
    } else {
        int width = spec_width(e->spec);
        const Line *out[64];
        for (int b = 0; b < width; b++)
            out[b] = &ma->mem[spec_out_cell(e->spec, b)];
        memset(&hit, 0, sizeof hit);
        for (int l = 0; l < LANES; l++) {
            if (!bit(&valid, l))
                continue;
            uint64_t got = 0;
            for (int b = 0; b < width; b++)
                got |= (uint64_t)bit(out[b], l) << b;
            if (got != spec_value(e->spec, x0 + (uint64_t)l))
                hit.w[l >> 6] |= 1ull << (l & 63);
        }
    }

    w->r.tried += valid_lanes < LANES ? valid_lanes : LANES;
    for (int k = 0; k < 8; k++) {
        for (uint64_t h = hit.w[k]; h; h &= h - 1) {
            int lane = 64 * k;
            for (uint64_t t = h & (0 - h); t > 1; t >>= 1)
                lane++;
            w->r.hits++;
            enum_add_hit(&w->r, x0 + (uint64_t)lane);
        }
    }
}

static void work(void *arg)
{
    Worker *w = arg;
    const Enum *e = w->e;
    if (w->cpu >= 0)
        pin_to_cpu(w->cpu);
    if (e->idle)
        idle_priority();
    LaneMachine ma, mb;
    if (!lane_init(&ma, e->a) || (e->mode == EN_EQUIV && !lane_init(&mb, e->b))) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    for (;;) {
        uint64_t c = (uint64_t)(atomic_inc(w->next) - 1);
        if (c >= w->nchunks)
            break;
        int share = (int)(c % (uint64_t)e->parts);
        if (share < e->part || share > e->part_end)
            continue;
        uint64_t end = (c + 1) * CHUNK < w->passes ? (c + 1) * CHUNK : w->passes;
        for (uint64_t pass = c * CHUNK; pass < end; pass++)
            run_pass(w, &ma, &mb, pass);
    }
    lane_free(&ma);
    if (e->mode == EN_EQUIV)
        lane_free(&mb);
}

int enum_run(const Enum *e, EnumResult *r, char *err, size_t errlen)
{
    memset(r, 0, sizeof *r);
    if (e->a->special || (e->mode == EN_EQUIV && e->b->special)) {
        snprintf(err, errlen, "id and link cells connect the lanes, but every input needs a lane "
                              "of its own here");
        return 0;
    }
    size_t n = e->a->ninputs;
    if (n > EN_MAX_IN) {
        snprintf(err, errlen, "%zu inputs is too many to try them all (at most %d)", n, EN_MAX_IN);
        return 0;
    }
    if (e->parts < 1 || e->part < 0 || e->part > e->part_end || e->part_end >= e->parts ||
        e->ticks < 1) {
        snprintf(err, errlen, "bad --part or --ticks");
        return 0;
    }
    int *in_map = xmalloc((n + 1) * sizeof *in_map);
    int *out_map = xmalloc((e->a->noutputs + 1) * sizeof *out_map);
    if (e->mode == EN_EQUIV && !enum_match(e->a, e->b, in_map, out_map, err, errlen)) {
        free(in_map);
        free(out_map);
        return 0;
    }

    for (int j = 0; j < 9; j++)
        for (int l = 0; l < LANES; l++)
            if ((l >> j) & 1)
                lane_bit_pattern[j].w[l >> 6] |= 1ull << (l & 63);

    Prep pa, pb;
    memset(&pb, 0, sizeof pb);
    prepare(&pa, e->a, e->a->inputs, e->ticks);
    int ok = jit_compile(e->a, 0, &pa.jc, NULL, err, errlen), compiled_b = 0;
    if (e->mode == EN_EQUIV) {
        uint32_t *in = xmalloc((n + 1) * sizeof *in);
        for (size_t k = 0; k < n; k++)
            in[k] = e->b->inputs[in_map[k]];
        prepare(&pb, e->b, in, e->ticks);
        free(in);
        if (ok)
            ok = compiled_b = jit_compile(e->b, 0, &pb.jc, NULL, err, errlen);
    }

    if (ok) {
        int *ids = xmalloc(1024 * sizeof *ids), k = cpu_pick(ids, 1024, e->excluded, e->threads);
        int threads = k ? k : e->threads > 0 ? e->threads : 1;
        uint64_t space = 1ull << n, passes = (space + LANES - 1) / LANES;
        volatile long next = 0;
        Worker *w = calloc((size_t)threads, sizeof *w);
        for (int i = 0; i < threads; i++) {
            w[i].e = e;
            w[i].pa = &pa;
            w[i].pb = &pb;
            w[i].out_map = out_map;
            w[i].next = &next;
            w[i].passes = passes;
            w[i].nchunks = (passes + CHUNK - 1) / CHUNK;
            w[i].cpu = i < k ? ids[i] : -1;
            w[i].r.space = space;
        }
        double t0 = now_seconds();
        threads_join(threads_start(threads, work, w, sizeof *w));
        r->seconds = now_seconds() - t0;
        r->space = space;
        r->threads = threads;
        for (int i = 0; i < threads; i++) {
            r->tried += w[i].r.tried;
            r->hits += w[i].r.hits;
            r->ops += w[i].r.ops;
            for (int h = 0; h < w[i].r.nhit; h++)
                enum_add_hit(r, w[i].r.hit[h]);
        }
        free(w);
        free(ids);
    }
    if (pa.jc.code)
        jit_free(&pa.jc);
    if (compiled_b)
        jit_free(&pb.jc);
    free(pa.in);
    free(pa.live);
    free(pb.in);
    free(pb.live);
    free(in_map);
    free(out_map);
    return ok;
}
