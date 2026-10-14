/* Builds NAND/STO programs out of logic gates.

   Each gate becomes a short run of NAND and STO that leaves its result in a cell. A gate reads
   all of its inputs before it writes its output, so a gate may write one of its own inputs. A few
   scratch cells are shared by all gates, since a gate is finished before the next one starts.
   Registers (flip-flops) take their new value at the end of the pass, all at once. */
#ifndef ALPHA_BUILD_H
#define ALPHA_BUILD_H

#include <stdio.h>

#include "isa.h"

enum { G_BUF, G_NOT, G_AND, G_NAND, G_OR, G_NOR, G_XOR, G_XNOR, G_COUNT };

typedef struct Builder Builder;

Builder *b_new(void);
uint32_t b_cell(Builder *b, const char *name);  /* created on first use; ZERO and ONE are constants */
int      b_has(const Builder *b, const char *name);
void     b_input(Builder *b, uint32_t cell);
void     b_output(Builder *b, uint32_t cell);
void     b_gate(Builder *b, int type, uint32_t out, const uint32_t *in, int n);
void     b_gate2(Builder *b, int type, uint32_t out, uint32_t x, uint32_t y);
void     b_reg(Builder *b, uint32_t q, uint32_t d);  /* q <- d at the end of every pass */

/* Wider parts, emitted directly (step 8). b_mux: out = s ? a1 : a0, given ns = !s; out may be a0
   or a1. b_select: out = OR of sel[i] & val[i] (a multiplexer when one sel is 1, as from a
   decoder); val may be ZERO or ONE, which costs less. */
void     b_mux(Builder *b, uint32_t out, uint32_t s, uint32_t ns, uint32_t a1, uint32_t a0);
/* Adders, the shortest NAND/STO programs there are (alpha super, step 10). b_half_adder: s = x ^ y,
   k = x & y; s may be x or y, k is another cell. b_full_adder: s = x ^ y ^ c, k = the carry; s
   may be x or y, k may be c. */
void     b_half_adder(Builder *b, uint32_t s, uint32_t k, uint32_t x, uint32_t y);
void     b_full_adder(Builder *b, uint32_t s, uint32_t k, uint32_t x, uint32_t y, uint32_t c);
void     b_select(Builder *b, uint32_t out, const uint32_t *sel, const uint32_t *val, int n);

/* The cube (docs/ISA.md): b_cell(b, "ID3") is a lane-id cell; b_link makes the new cell `name`
   read `src` of the machine across dimension dim. Both are read-only. */
uint32_t b_link(Builder *b, const char *name, uint32_t src, int dim);
size_t   b_len(const Builder *b);                    /* instructions so far, registers not yet */
void     b_finish(Builder *b, Program *out);         /* also frees the builder */

const char *gate_name(int type);
int         gate_type(const char *name);  /* -1 if unknown; BUF and BUFF both mean G_BUF */

/* Writes p as assembly that the assembler reads back into the same program. */
void program_print(FILE *f, const Program *p);

#endif
