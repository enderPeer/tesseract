/* The compiler: turns an ISA v1 program (NAND and STO only) into x86-64 AVX2 machine code that
   runs one pass for all 512 lanes, on the same memory layout as the lane engine (src/lanes.h).
   The machine does not change; only the translation to x86 does. x86 has no NAND instruction, so
   every engine builds it from AND / AND-NOT / XOR.

   Literal mode translates one instruction at a time: NAND = AND + invert, STO = store.

   Optimized mode keeps track at compile time of what every register holds during the pass and
   emits only the work that is left:
   - RR is kept with an "inverted" flag, so NAND x is one AND (or AND-NOT) and NAND ONE is free;
   - constants fold: NAND ZERO, and constants stored and read back, cost nothing;
   - cells stored during the pass stay in registers for later reads (7 register pairs, least
     recently used goes first). Every store still goes to memory.

   In both modes memory and RR after a pass are bit-identical to the interpreter's. Inside a pass
   there is no per-instruction state to look at. */
#ifndef ALPHA_JIT_H
#define ALPHA_JIT_H

#include "lanes.h"

typedef struct {
    size_t x86;     /* x86 instructions in the compiled pass */
    size_t alu;     /* vector logic operations */
    size_t loads;   /* 256-bit loads, memory operands included */
    size_t stores;  /* 256-bit stores */
    size_t free;    /* 1-bit instructions that needed no x86 code at all */
    size_t bytes;   /* machine code size */
} JitStats;

typedef struct {
    const Program *prog;
    void          *code;       /* executable memory */
    size_t         size;
    JitStats       stats;
    /* More than 512 machines (step 7f): the pass cut into nseg segments, segment k being
       instructions seg_first[k] .. seg_first[k + 1] - 1 and its own function at code + seg_off[k].
       nseg = 0 for 512 machines: one function for the whole pass. */
    unsigned       nseg;
    size_t        *seg_first;
    size_t        *seg_off;
} JitCode;

/* Compiles p. If listing is not NULL it receives the annotated assembly (free() it).
   Returns 0 and writes err on failure. */
int  jit_compile(const Program *p, int literal, JitCode *out, char **listing, char *err,
                 size_t errlen);
void jit_free(JitCode *j);

/* One pass of all 512 lanes; m must belong to the same program. Needs AVX2. */
void jit_tick(const JitCode *j, LaneMachine *m);

/* `passes` passes in one call: the loop is part of the compiled code. */
void jit_run(const JitCode *j, LaneMachine *m, uint64_t passes);

/* More than 512 machines (step 7f): passes of the whole cube, on this thread. A segment runs in
   every group before the next one starts; jit_segment runs segment k for one group, for callers
   that spread the groups over threads (src/parallel.c). Programs for 512 machines work too. */
void jit_cube_run(const JitCode *j, LaneCube *c, uint64_t passes);
void jit_cube_tick(const JitCode *j, LaneCube *c);
void jit_segment(const JitCode *j, unsigned k, LaneMachine *m);

#endif
