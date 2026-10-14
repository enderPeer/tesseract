/* A minimal x86-64 encoder for the few AVX2 instructions the compiler needs. Vector registers are
   numbered 0-15; memory operands are [base + disp] with a general-purpose base register. */
#ifndef ALPHA_X86_H
#define ALPHA_X86_H

#include <stddef.h>
#include <stdint.h>

enum {
    X86_RAX, X86_RCX, X86_RDX, X86_RBX, X86_RSP, X86_RBP, X86_RSI, X86_RDI,
    X86_R8,  X86_R9,  X86_R10, X86_R11, X86_R12, X86_R13, X86_R14, X86_R15
};

/* Three-operand logic instructions, VEX.256.66.0F <op> /r. */
enum { X86_VPAND = 0xDB, X86_VPANDN = 0xDF, X86_VPXOR = 0xEF, X86_VPOR = 0xEB, X86_VPCMPEQD = 0x76 };

typedef struct {
    uint8_t *b;
    size_t   len, cap;
    char     text[96];  /* assembly text of the last instruction emitted */
} X86;

void x86_rrr(X86 *x, int op, int dst, int src1, int src2);                /* op ymm, ymm, ymm */
void x86_rrm(X86 *x, int op, int dst, int src1, int base, int32_t disp);  /* op ymm, ymm, [m] */
/* vmovdqa (aligned) or vmovdqu; ymm = 1 for 256 bits, 0 for 128 bits (xmm) */
void x86_load(X86 *x, int dst, int base, int32_t disp, int aligned, int ymm);
void x86_store(X86 *x, int src, int base, int32_t disp, int aligned, int ymm);
/* Moving data between lanes (step 7a) */
void x86_mov(X86 *x, int dst, int src);                      /* vmovdqa ymm, ymm */
void x86_pshufd(X86 *x, int dst, int src, int imm);          /* vpshufd ymm, ymm, imm8 */
void x86_permq(X86 *x, int dst, int src, int imm);           /* vpermq ymm, ymm, imm8 */
void x86_pshufb(X86 *x, int dst, int src, int base, int32_t disp);  /* vpshufb ymm, ymm, [m] */
void x86_shiftq(X86 *x, int left, int dst, int src, int imm);       /* vpsllq / vpsrlq ymm, ymm, imm8 */
void x86_vzeroupper(X86 *x);
void x86_load64(X86 *x, int dst, int base, int32_t disp);  /* mov r64, [m] (step 7f) */
void x86_dec(X86 *x, int reg);        /* dec r64 */
void x86_jnz(X86 *x, size_t target);  /* jnz rel32 to byte offset `target` of this code */
void x86_ret(X86 *x);
void x86_free(X86 *x);

#endif
