/* Beta (step 8): a small stored-program byte computer, built from NAND/STO as one fixed program.
   The NAND/STO machine does not change; beta is a circuit in its cells. One pass of the program is
   one beta instruction, in each of the 512 lanes, so a lane group is 512 separate beta computers,
   each with its own memory, program counter and programs. docs/BETA.md describes the machine.

   Memory: 256 bytes, RAM below BETA_ROM, the monitor in ROM from BETA_ROM up. Registers A, X
   (8 bits), PC (8 bits, always even), C (carry). Instructions are two bytes, [mode:2 op:4] [arg].
   The host talks to beta through ports: it puts a byte in IN and sets IN_VALID, beta's IN takes it
   and clears IN_VALID; beta's OUT puts A in OUT and sets OUT_STROBE for that pass. */
#ifndef ALPHA_BETA_H
#define ALPHA_BETA_H

#include <stddef.h>
#include <stdint.h>

#include "isa.h"
#include "lanes.h"

enum { BETA_ROM = 0xA0, BETA_MON = BETA_ROM };  /* the monitor's entry: where beta powers on */

enum {  /* operations, the low 4 bits of the first byte */
    BOP_LDA, BOP_LDX, BOP_ADD, BOP_ADC, BOP_SUB, BOP_AND, BOP_OR, BOP_XOR,
    BOP_STA, BOP_STX, BOP_JMP, BOP_JZ, BOP_JC, BOP_JSR, BOP_IO, BOP_REG
};
enum { BMODE_IMM, BMODE_ABS, BMODE_IDX, BMODE_A };    /* bits 4-5 */
enum { BIO_IN, BIO_OUT, BIO_HLT, BIO_NOP };           /* modes of BOP_IO */
enum { BREG_INX, BREG_TAX, BREG_TXA, BREG_SHR };      /* modes of BOP_REG */

typedef struct {
    uint8_t mem[256];
    uint8_t a, x, pc, c;
    uint8_t in, in_valid;       /* set by the host, taken by IN */
    uint8_t out, out_strobe;    /* set by OUT; the strobe only for that step */
    uint8_t halted, waiting;    /* this step was HLT, or IN with nothing there */
} Beta;

/* The reference emulator: the definition every other beta is checked against. */
void beta_reset(Beta *m);  /* power on: RAM 0, the monitor in ROM, registers 0, PC = BETA_MON */
void beta_step(Beta *m);   /* one instruction */

/* The monitor (its source is in beta.c): the ROM image, BETA_ROM .. 255. */
const uint8_t *beta_rom(void);
const char    *beta_monitor_source(void);

/* The assembler. Returns 1, or 0 with a message in err. */
typedef struct {
    uint8_t  bytes[256];
    uint8_t  used[256];  /* 1 where the source put a byte */
    uint8_t  insn[256];  /* 1 where an instruction starts */
    unsigned lo, hi;     /* lowest address used, highest + 1 (lo = hi = 0 if none) */
} BetaImage;
int beta_assemble(const char *src, const char *name, BetaImage *out, char *err, size_t errlen);

/* One instruction as text, for listings. */
void beta_disasm(uint8_t b0, uint8_t b1, char *buf, size_t len);

/* The beta CPU as a NAND/STO program, and where its state lives: mem[a][b] is bit b of RAM byte
   a (a < BETA_ROM), pc holds PC XOR BETA_MON, so that all cells 0 mean PC = BETA_MON. */
typedef struct {
    uint32_t mem[BETA_ROM][8];
    uint32_t a[8], x[8], pc[8], c;
    uint32_t in[8], in_valid, out[8], out_strobe, halted, waiting;
} BetaCells;

void gen_beta(Program *p, BetaCells *cells);

/* One lane's beta state to and from a lane group (the ROM part of mem is the monitor). */
void beta_to_lane(LaneMachine *m, const BetaCells *c, unsigned lane, const Beta *s);
void beta_from_lane(const LaneMachine *m, const BetaCells *c, unsigned lane, Beta *s);

/* The host side of the ports, for the 512 betas of a lane group and for the emulator alike.
   Before a pass every beta that has taken its last byte gets the next one of its input; after
   the pass what it sent through OUT is collected. A beta is done when it waits for input and
   has none left (normally: back in the monitor), or has halted. */
typedef struct {
    const uint8_t *in;
    size_t         nin, at;     /* its input, and how much it has taken */
    uint8_t       *out;         /* what it printed (realloc'ed; free it) */
    size_t         nout, cap;
    int            done;
} BetaLane;

void beta_feed(LaneMachine *m, const BetaCells *c, BetaLane *lanes);        /* before a pass */
int  beta_collect(const LaneMachine *m, const BetaCells *c, BetaLane *lanes); /* after; # done */
void beta_emu_pass(Beta *s, BetaLane *l);  /* feed, beta_step, collect: the same for one emulator */

/* The monitor commands that load img and start it at its lowest address, then `extra` (input for
   the program). Returns the length, or 0 if it does not fit in cap. */
size_t beta_boot_input(const BetaImage *img, const uint8_t *extra, size_t nextra, uint8_t *buf,
                       size_t cap);

#endif
