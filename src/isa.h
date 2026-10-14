/* ISA v1: the NAND/STO machine. The full specification is in docs/ISA.md. */
#ifndef ALPHA_ISA_H
#define ALPHA_ISA_H

#include <stddef.h>
#include <stdint.h>

/* The opcode is one bit. */
enum { OP_NAND = 0, OP_STO = 1 };

/* Cells 0 and 1 are hardwired: reads return 0 / 1, writes are ignored. */
enum { CELL_ZERO = 0, CELL_ONE = 1, CELL_FIRST_FREE = 2 };

typedef struct {
    uint8_t  op;    /* OP_NAND or OP_STO */
    uint32_t addr;  /* cell address */
} Instr;

/* The cube (docs/ISA.md): 512 machines, machine k wired to k XOR 2^d. Dimensions 9 and up (step
   7f) join several of these 512-machine groups into one cube of up to 2^CUBE_MAX_DIMS machines. */
enum { CUBE_DIMS = 9, CUBE_SIZE = 1 << CUBE_DIMS, CUBE_MAX_DIMS = 16,
       CUBE_MAX_GROUPS = 1 << (CUBE_MAX_DIMS - CUBE_DIMS) };

/* What kind of cell: plain, a lane-id cell (bit `dim` of the machine's number), or a link cell
   (cell `src` of the machine across dimension `dim`). */
enum { CK_PLAIN, CK_ID, CK_LINK };

typedef struct {
    uint8_t  kind;
    uint8_t  dim;
    uint32_t src;
} CellInfo;

typedef struct {
    Instr    *code;
    size_t    len;
    uint32_t  ncells;      /* cells used, including ZERO and ONE */
    char    **cell_names;  /* cell_names[addr] */
    uint32_t *inputs;      /* .in cells, in declaration order */
    size_t    ninputs;
    uint32_t *outputs;     /* .out cells, in declaration order */
    size_t    noutputs;
    CellInfo *special;     /* per cell; NULL when the program has no id or link cells */
} Program;

void     program_free(Program *p);
int      program_find_cell(const Program *p, const char *name, uint32_t *addr);
unsigned addr_bits(uint32_t ncells);  /* width of the address field */

/* ZERO, ONE, id cells and link cells: writes are ignored. */
int      program_readonly(const Program *p, uint32_t cell);

/* The cell a read of `cell` really reaches: follows link cells and XORs the dimensions crossed
   into *mask (the machine read is k XOR *mask). Returns a plain, constant or id cell. */
uint32_t program_resolve(const Program *p, uint32_t cell, unsigned *mask);

/* The dimensions of the cube p runs on: 9, or more if its id or link cells reach dimension 9 or up
   (then 2^dims machines, 2^(dims - 9) groups of 512). */
unsigned program_dims(const Program *p);

/* ID0 .. ID15 name lane-id cells: returns the bit, or -1 for any other name. */
int      program_id_name(const char *name);

#endif
