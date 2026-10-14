/* The bitsliced engine: 512 independent copies ("lanes") of the ISA v1 machine.

   Memory is transposed: line a holds cell a of every lane, bit k of the line belonging to lane k.
   A line is 64 bytes, one cache line, so every instruction reads or writes exactly one cache line
   for all 512 lanes. RR of all lanes is one more line. Needs AVX2 (see cpu_has_avx2). */
#ifndef ALPHA_LANES_H
#define ALPHA_LANES_H

#include "isa.h"

#define LANES CUBE_SIZE

typedef struct {
    _Alignas(64) uint64_t w[8];  /* lane k is bit k % 64 of w[k / 64] */
} Line;

/* An instruction decoded for the lane engines: a read across links names the cell really read, the
   XOR of the dimensions 0..8 crossed (lanes moving inside the line) and the peer: which group's
   memory it is in, for dimensions 9 and up (peer 0 is the group itself, see lane_peers); a write to
   a read-only cell does nothing. */
enum { LOP_NAND, LOP_STO, LOP_NAND_LINK, LOP_NONE };

typedef struct {
    uint32_t addr;
    uint16_t mask;
    uint8_t  op;
    uint8_t  peer;
} LaneOp;

/* The groups a program reads across dimensions 9 and up, as group masks: gm[0] = 0 (itself), then
   every other group mask its links reach, in order of first use. Returns how many (at most
   CUBE_MAX_GROUPS). Group g reads peer s from group g ^ gm[s]. */
unsigned lane_peers(const Program *p, uint32_t *gm);

/* After the program's cells, a few constant lines the engines use to move lanes across a link. */
enum { LINE_BITS0 = 0, LINE_BITS1, LINE_BITS2, LINE_BYTES3, LINE_BYTES4, LINE_EXTRA };

typedef struct LaneMachine {
    const Program      *prog;
    Line               *mem;     /* prog->ncells lines + LINE_EXTRA, 64-byte aligned */
    Line                rr;
    size_t              pc;
    uint64_t            steps;   /* instructions executed (each one for all lanes) */
    uint64_t            ticks;
    LaneOp             *ops;     /* the program, decoded */
    unsigned            group;   /* in a bigger cube: machines 512 * group .. 512 * group + 511 */
    struct LaneMachine *groups;  /* all groups of that cube, or NULL on its own */
    Line              **peer;    /* peer[s]: the memory of group group ^ gm[s] (lane_peers) */
    unsigned            npeers;
} LaneMachine;

/* Line `line` as read across the dimensions in mask: lane k gets lane k XOR mask. */
void    lane_permute(Line *out, const Line *line, unsigned mask);

int     lane_init(LaneMachine *m, const Program *p);  /* returns 0 if out of memory */
void    lane_free(LaneMachine *m);
void    lane_reset(LaneMachine *m);                   /* every lane in the power-on state */
void    lane_step(LaneMachine *m);
void    lane_tick(LaneMachine *m);                    /* finish the current pass */
uint8_t lane_get(const LaneMachine *m, uint32_t addr, unsigned lane);
void    lane_set(LaneMachine *m, uint32_t addr, unsigned lane, uint8_t v);  /* ignored for constants */
uint8_t lane_rr(const LaneMachine *m, unsigned lane);
void    lane_set_rr(LaneMachine *m, unsigned lane, uint8_t v);

/* A cube of more than 512 machines (step 7f): 2^(dims - 9) groups of 512 lanes, group g holding
   machines 512 g .. 512 g + 511, wired across dimensions 9 and up by reading each other's memory.
   The groups step in lockstep, one instruction in all of them before the next. With
   program_dims(p) = 9 it is one ordinary group. */
typedef struct {
    const Program *prog;
    unsigned       dims, groups;
    LaneMachine   *g;
} LaneCube;

int     lane_cube_init(LaneCube *c, const Program *p);  /* returns 0 if out of memory */
void    lane_cube_free(LaneCube *c);
void    lane_cube_reset(LaneCube *c);
void    lane_cube_step(LaneCube *c);
void    lane_cube_tick(LaneCube *c);
uint8_t lane_cube_get(const LaneCube *c, uint32_t addr, unsigned machine);
void    lane_cube_set(LaneCube *c, uint32_t addr, unsigned machine, uint8_t v);

/* Whole-state helpers for machines of the same program. The state of a lane is RR and every
   writable cell. */
void    lane_copy(LaneMachine *dst, const LaneMachine *src);
void    lane_diff(const LaneMachine *a, const LaneMachine *b, Line *out);  /* 1 = lane differs */
/* The same over RR and the n given cells only. */
void    lane_diff_some(const LaneMachine *a, const LaneMachine *b, const uint32_t *cells, int n,
                       Line *out);

#endif
