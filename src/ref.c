#include "ref.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int ref_init(RefMachine *m, const Program *p)
{
    memset(m, 0, sizeof *m);
    m->prog = p;
    m->mem = calloc(p->ncells, 1);
    if (!m->mem)
        return 0;
    ref_reset(m);
    return 1;
}

void ref_free(RefMachine *m)
{
    free(m->mem);
    m->mem = NULL;
}

void ref_reset(RefMachine *m)
{
    memset(m->mem, 0, m->prog->ncells);
    m->mem[CELL_ONE] = 1;
    m->rr = 0;
    m->pc = 0;
    m->steps = 0;
    m->ticks = 0;
}

uint8_t ref_get(const RefMachine *m, uint32_t addr)
{
    const Program *p = m->prog;
    assert(addr < p->ncells);
    if (!p->special)
        return m->mem[addr];
    unsigned mask;
    uint32_t cell = program_resolve(p, addr, &mask);
    unsigned lane = m->lane ^ mask;
    if (p->special[cell].kind == CK_ID)
        return (uint8_t)((lane >> p->special[cell].dim) & 1);
    assert(mask == 0 || m->cube);  /* a link needs the rest of the cube */
    return mask ? m->cube[lane].mem[cell] : m->mem[cell];
}

void ref_step(RefMachine *m)
{
    const Program *p = m->prog;
    if (p->len == 0) {
        m->ticks++;
        return;
    }
    const Instr in = p->code[m->pc];
    assert(in.addr < p->ncells);
    if (in.op == OP_NAND)
        m->rr = (uint8_t)!(m->rr & ref_get(m, in.addr));
    else if (!program_readonly(p, in.addr))
        m->mem[in.addr] = m->rr;
    m->steps++;
    if (++m->pc == p->len) {
        m->pc = 0;
        m->ticks++;
    }
}

void ref_tick(RefMachine *m)
{
    uint64_t t = m->ticks;
    while (m->ticks == t)
        ref_step(m);
}

void ref_set(RefMachine *m, uint32_t addr, uint8_t v)
{
    assert(addr < m->prog->ncells);
    if (!program_readonly(m->prog, addr))
        m->mem[addr] = v ? 1 : 0;
}

/* ---- the cube ----------------------------------------------------------------------------- */

unsigned ref_cube_machines(const Program *p)
{
    return 1u << program_dims(p);
}

int ref_cube_init(RefMachine *cube, const Program *p)
{
    unsigned n = ref_cube_machines(p);
    for (unsigned k = 0; k < n; k++) {
        if (!ref_init(&cube[k], p)) {
            while (k--)
                ref_free(&cube[k]);
            return 0;
        }
        cube[k].lane = k;
        cube[k].cube = cube;
    }
    return 1;
}

void ref_cube_free(RefMachine *cube)
{
    for (unsigned k = 0, n = ref_cube_machines(cube[0].prog); k < n; k++)
        ref_free(&cube[k]);
}

void ref_cube_reset(RefMachine *cube)
{
    for (unsigned k = 0, n = ref_cube_machines(cube[0].prog); k < n; k++)
        ref_reset(&cube[k]);
}

/* Machine by machine is the same as all at once: a NAND changes only RR, which no other machine
   reads, and during a STO nobody reads at all. */
void ref_cube_step(RefMachine *cube)
{
    for (unsigned k = 0, n = ref_cube_machines(cube[0].prog); k < n; k++)
        ref_step(&cube[k]);
}

void ref_cube_tick(RefMachine *cube)
{
    uint64_t t = cube[0].ticks;
    while (cube[0].ticks == t)
        ref_cube_step(cube);
}
