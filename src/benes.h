/* Switch settings for PERM8 (lib/cube.inc): a Beneš network on the cube, 17 rounds across the
   dimensions 0 1 .. 8 .. 1 0. In round s, machine k takes the value of machine k XOR 2^dim(s) when
   ctrl[s][k] = 1; the two machines of a pair always get the same setting, so they swap. Any
   permutation of the 512 machines can be routed this way. */
#ifndef ALPHA_BENES_H
#define ALPHA_BENES_H

#include "isa.h"

#define BENES_ROUNDS (2 * CUBE_DIMS - 1)

int  benes_dim(int round);

/* dest[k] = the machine that machine k's value must end up in; dest must be a permutation of
   0..511. */
void benes_route(const uint16_t *dest, uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE]);

/* Plays the network in plain C: v[dest[k]] gets the old v[k] if ctrl is right. */
void benes_apply(const uint8_t ctrl[BENES_ROUNDS][CUBE_SIZE], uint64_t *v);

#endif
