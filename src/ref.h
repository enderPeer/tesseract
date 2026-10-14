/* The reference machine: the golden model every faster engine is checked against.
   One byte per cell, one instruction at a time, nothing clever.

   A single machine is machine 0 of a cube on its own. For programs with link cells, a cube of 512
   (or more) machines runs in lockstep (ref_cube_*): every machine finishes an instruction before any starts
   the next one, and a read across a link looks into the neighbour. */
#ifndef ALPHA_REF_H
#define ALPHA_REF_H

#include "isa.h"

typedef struct RefMachine {
    const Program     *prog;
    uint8_t           *mem;    /* one byte per cell, always 0 or 1 */
    uint8_t            rr;
    size_t             pc;
    uint64_t           steps;  /* instructions executed */
    uint64_t           ticks;  /* completed passes through the program */
    unsigned           lane;   /* number in the cube (0 on its own) */
    struct RefMachine *cube;   /* all 512 machines, or NULL on its own */
} RefMachine;

int     ref_init(RefMachine *m, const Program *p);  /* returns 0 if out of memory */
void    ref_free(RefMachine *m);
void    ref_reset(RefMachine *m);                   /* power-on state: all cells 0, ONE = 1, RR = 0 */
void    ref_step(RefMachine *m);                    /* execute one instruction */
void    ref_tick(RefMachine *m);                    /* run until the PC wraps to 0 */
uint8_t ref_get(const RefMachine *m, uint32_t addr);  /* any cell, links and ids included */
void    ref_set(RefMachine *m, uint32_t addr, uint8_t v);  /* host write, ignored if read-only */

/* A cube: cube[ref_cube_machines(p)] machines numbered 0.., wired to each other: 512, or more
   when the program reaches dimension 9 or up (program_dims). */
unsigned ref_cube_machines(const Program *p);
int     ref_cube_init(RefMachine *cube, const Program *p);
void    ref_cube_free(RefMachine *cube);
void    ref_cube_reset(RefMachine *cube);
void    ref_cube_step(RefMachine *cube);  /* one instruction in every machine */
void    ref_cube_tick(RefMachine *cube);  /* finish the pass in every machine */

#endif
