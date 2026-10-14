/* Workloads built from gates (src/build.h), each with a plain scalar version to check against. */
#ifndef ALPHA_GEN_H
#define ALPHA_GEN_H

#include "build.h"
#include "isa.h"

/* Array multiplier: inputs a0.., b0.. (bits each), outputs p0..p(2*bits-1) = a * b. */
void gen_mul(Program *p, int bits);

/* Toy hash: inputs x0.., outputs h0.. (bits each). Each round: x_i ^= !x_(i+1) & x_(i+2), then
   the bits are permuted and a round constant is XORed in. toyhash() is the scalar version. */
int      gen_hash(Program *p, int bits, int rounds);  /* 0 if no permutation exists for bits */
uint64_t toyhash(uint64_t x, int bits, int rounds);

/* Game of Life on a w x h torus, one generation per pass. board[r * w + c] gets the cell of
   row r, column c. life_scalar() advances a board of 0/1 bytes by one generation. */
void gen_life(Program *p, int w, int h, uint32_t *board);
void life_scalar(uint8_t *board, int w, int h);

/* Grids on the cube (step 7e): 2^rbits rows x 2^cbits columns of machines, rbits + cbits = 9 or
   (step 7f) up to 16, in Gray code order, so every grid neighbour is one link away. grid_machine
   gives the machine at a row and column: the row's Gray code above the column's. gen_life_cube:
   one Game of Life board, (2^rbits * h) x (2^cbits * w) on a torus, made of w x h tiles, the tile
   at grid row R and column C in machine grid_machine(R, C, cbits). Each pass fetches the edges of
   the 8 surrounding tiles over the links, then does one generation.

   gen_rank (step 7f): input x, outputs p = how many machines below this one have x = 1 and t = how
   many have it in all, dims + 1 bits each, over a cube of 2^dims machines. */
unsigned grid_machine(unsigned row, unsigned col, int cbits);
void     gen_life_cube(Program *p, int w, int h, int rbits, int cbits, uint32_t *board);
void     gen_rank(Program *p, int dims);

/* The two pieces grids are made of, for other generators (src/gamma.c). grid_masks: for the axis
   of dimensions lo .. lo+k-1 (k at most 9) in Gray code, next[j] (prev[j]) becomes 1 in the
   machines whose next (previous) position on the axis is across dimension lo + j; tag names the
   cells. grid_gather: the new cell `name` = src of the machine the mask points to (one link). */
void     grid_masks(Builder *b, int lo, int k, uint32_t *next, uint32_t *prev, const char *tag);
uint32_t grid_gather(Builder *b, const char *name, uint32_t src, int lo, int k,
                     const uint32_t *mask);

/* The cube (step 7d), one table entry per machine. gen_wht: input f, outputs w0.. = the Walsh-
   Hadamard spectrum of the Boolean function f, sum over x of (-1)^(f(x) + popcount(x & k)), as a
   `bits`-bit two's complement number (12 bits hold -512..512). gen_xorconv: inputs f, g, outputs
   h0..h9 = the number of x with f(x) = g(x XOR k) = 1, via spectrum, product and inverse spectrum. */
void gen_wht(Program *p, int bits);
void gen_xorconv(Program *p);

/* gen_polymul: c = a * b in Z_q[x]/(x^512 + 1), q = 12289 (the ring of Falcon-512), machine k
   holding coefficient k of each, 0 .. q-1 in 14 bits (inputs a, b; output c). Negacyclic NTT both
   ways, 9 butterfly rounds each, with Montgomery products. The twiddle factors are inputs too,
   zf<d>_ and zi<d>_ for round d, from ntt_twiddles(); nothing writes them, so they stay set across
   passes. polymul_scalar: the same product, schoolbook, in plain C. */
#define NTT_Q 12289
void gen_polymul(Program *p);
void ntt_twiddles(uint16_t zf[CUBE_DIMS][CUBE_SIZE], uint16_t zi[CUBE_DIMS][CUBE_SIZE]);
void polymul_scalar(const uint16_t *a, const uint16_t *b, uint16_t *c);

#endif
