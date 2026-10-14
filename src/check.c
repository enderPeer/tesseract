#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lanes.h"
#include "ref.h"

static void out_of_memory(void)
{
    fputs("out of memory\n", stderr);
    exit(1);
}

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int differ(CheckResult *r, unsigned lane, long cell, unsigned want, unsigned got)
{
    r->ok = 0;
    r->lane = lane;
    r->cell = cell;
    r->want = want;
    r->got = got;
    return 0;
}

/* Compares RR and every cell, constants included, of every machine. */
static int same(const LaneCube *lc, const RefMachine *refs, CheckResult *r)
{
    const Program *p = lc->prog;
    for (unsigned k = 0; k < lc->groups * LANES; k++) {
        const LaneMachine *lm = &lc->g[k / LANES];
        const RefMachine *rm = &refs[k];
        unsigned l = k % LANES;
        if (lane_rr(lm, l) != rm->rr)
            return differ(r, k, -1, rm->rr, lane_rr(lm, l));
        for (uint32_t a = 0; a < p->ncells; a++)
            if (lane_get(lm, a, l) != ref_get(rm, a))
                return differ(r, k, (long)a, ref_get(rm, a), lane_get(lm, a, l));
        r->compared++;
    }
    return 1;
}

int check_lanes(const Program *p, const JitCode *jit, uint64_t seed, unsigned ticks, int stepwise,
                CheckResult *r)
{
    if (jit)
        stepwise = 0;
    memset(r, 0, sizeof *r);
    r->ok = 1;

    LaneCube lc;
    unsigned n = ref_cube_machines(p);
    RefMachine *refs = calloc(n, sizeof *refs);
    if (!refs || !lane_cube_init(&lc, p))
        out_of_memory();

    uint64_t s = seed;
    for (unsigned g = 0; g < lc.groups; g++) {
        LaneMachine *lm = &lc.g[g];
        for (uint32_t a = CELL_FIRST_FREE; a < p->ncells; a++)
            for (int w = 0; w < 8; w++) {
                uint64_t v = splitmix64(&s);
                if (!program_readonly(p, a))  /* id cells keep their pattern */
                    lm->mem[a].w[w] = v;
            }
        for (int w = 0; w < 8; w++)
            lm->rr.w[w] = splitmix64(&s);
    }

    /* the reference machines form a cube, so links read the right neighbour */
    if (!ref_cube_init(refs, p))
        out_of_memory();
    for (unsigned k = 0; k < n; k++) {
        for (uint32_t a = CELL_FIRST_FREE; a < p->ncells; a++)
            ref_set(&refs[k], a, lane_cube_get(&lc, a, k));
        refs[k].rr = lane_rr(&lc.g[k / LANES], k % LANES);
    }

    same(&lc, refs, r);
    for (unsigned t = 1; r->ok && t <= ticks; t++) {
        r->tick = t;
        if (stepwise) {
            for (size_t i = 0; r->ok && i < p->len; i++) {
                r->pc = (long)i;
                lane_cube_step(&lc);
                ref_cube_step(refs);
                same(&lc, refs, r);
            }
        } else {
            r->pc = -1;
            if (jit)
                jit_cube_tick(jit, &lc);
            else
                lane_cube_tick(&lc);
            ref_cube_tick(refs);
            same(&lc, refs, r);
        }
    }

    ref_cube_free(refs);
    free(refs);
    lane_cube_free(&lc);
    return r->ok;
}
