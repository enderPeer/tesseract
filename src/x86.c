#include "x86.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const char *const gpr[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

static void put(X86 *x, uint8_t v)
{
    if (x->len == x->cap) {
        x->cap = x->cap ? 2 * x->cap : 4096;
        x->b = realloc(x->b, x->cap);
        if (!x->b) {
            fputs("out of memory\n", stderr);
            exit(1);
        }
    }
    x->b[x->len++] = v;
}

/* VEX prefix. r = ModRM.reg, v = VEX.vvvv (-1 if unused), b = ModRM.rm register or base register,
   l = 1 for 256 bits, pp = 1 for 66, 2 for F3, map = 1 for 0F, 2 for 0F38, 3 for 0F3A, w = VEX.W.
   The short C5 form works for map 0F with W = 0 unless b is r8-r15; the bits that name registers
   8-15 are stored inverted. */
static void vex_map(X86 *x, int r, int v, int b, int l, int pp, int map, int w)
{
    int nr = !(r & 8), nb = !(b & 8), nv = ~(v < 0 ? 0 : v) & 15;
    if (nb && map == 1 && !w) {
        put(x, 0xC5);
        put(x, (uint8_t)(nr << 7 | nv << 3 | l << 2 | pp));
    } else {
        put(x, 0xC4);
        put(x, (uint8_t)(nr << 7 | 1 << 6 | nb << 5 | map));
        put(x, (uint8_t)(w << 7 | nv << 3 | l << 2 | pp));
    }
}

static void vex(X86 *x, int r, int v, int b, int l, int pp)
{
    vex_map(x, r, v, b, l, pp, 1, 0);
}

static void modrm_reg(X86 *x, int r, int rm)
{
    put(x, (uint8_t)(0xC0 | (r & 7) << 3 | (rm & 7)));
}

static void modrm_mem(X86 *x, int r, int base, int32_t disp)
{
    assert((base & 7) != 4);  /* rsp and r12 would need a SIB byte */
    if (disp == 0 && (base & 7) != 5) {
        put(x, (uint8_t)((r & 7) << 3 | (base & 7)));
    } else if (disp >= -128 && disp <= 127) {
        put(x, (uint8_t)(0x40 | (r & 7) << 3 | (base & 7)));
        put(x, (uint8_t)disp);
    } else {
        put(x, (uint8_t)(0x80 | (r & 7) << 3 | (base & 7)));
        for (int i = 0; i < 4; i++)
            put(x, (uint8_t)((uint32_t)disp >> (8 * i)));
    }
}

static void mem_text(char *buf, size_t n, int base, int32_t disp)
{
    if (disp == 0)
        snprintf(buf, n, "[%s]", gpr[base]);
    else if (disp > 0)
        snprintf(buf, n, "[%s+0x%x]", gpr[base], (unsigned)disp);
    else
        snprintf(buf, n, "[%s-0x%x]", gpr[base], (unsigned)-(int64_t)disp);
}

static const char *op_name(int op)
{
    switch (op) {
    case X86_VPAND:    return "vpand";
    case X86_VPANDN:   return "vpandn";
    case X86_VPXOR:    return "vpxor";
    case X86_VPOR:     return "vpor";
    case X86_VPCMPEQD: return "vpcmpeqd";
    default:           return "?";
    }
}

void x86_rrr(X86 *x, int op, int dst, int src1, int src2)
{
    vex(x, dst, src1, src2, 1, 1);
    put(x, (uint8_t)op);
    modrm_reg(x, dst, src2);
    snprintf(x->text, sizeof x->text, "%-9s ymm%d, ymm%d, ymm%d", op_name(op), dst, src1, src2);
}

void x86_rrm(X86 *x, int op, int dst, int src1, int base, int32_t disp)
{
    char m[40];
    vex(x, dst, src1, base, 1, 1);
    put(x, (uint8_t)op);
    modrm_mem(x, dst, base, disp);
    mem_text(m, sizeof m, base, disp);
    snprintf(x->text, sizeof x->text, "%-9s ymm%d, ymm%d, %s", op_name(op), dst, src1, m);
}

void x86_load(X86 *x, int dst, int base, int32_t disp, int aligned, int ymm)
{
    char m[40];
    vex(x, dst, -1, base, ymm, aligned ? 1 : 2);
    put(x, 0x6F);
    modrm_mem(x, dst, base, disp);
    mem_text(m, sizeof m, base, disp);
    snprintf(x->text, sizeof x->text, "%-9s %s%d, %s", aligned ? "vmovdqa" : "vmovdqu",
             ymm ? "ymm" : "xmm", dst, m);
}

void x86_store(X86 *x, int src, int base, int32_t disp, int aligned, int ymm)
{
    char m[40];
    vex(x, src, -1, base, ymm, aligned ? 1 : 2);
    put(x, 0x7F);
    modrm_mem(x, src, base, disp);
    mem_text(m, sizeof m, base, disp);
    snprintf(x->text, sizeof x->text, "%-9s %s, %s%d", aligned ? "vmovdqa" : "vmovdqu", m,
             ymm ? "ymm" : "xmm", src);
}

void x86_mov(X86 *x, int dst, int src)
{
    vex(x, dst, -1, src, 1, 1);
    put(x, 0x6F);
    modrm_reg(x, dst, src);
    snprintf(x->text, sizeof x->text, "%-9s ymm%d, ymm%d", "vmovdqa", dst, src);
}

void x86_pshufd(X86 *x, int dst, int src, int imm)
{
    vex(x, dst, -1, src, 1, 1);
    put(x, 0x70);
    modrm_reg(x, dst, src);
    put(x, (uint8_t)imm);
    snprintf(x->text, sizeof x->text, "%-9s ymm%d, ymm%d, 0x%02x", "vpshufd", dst, src, imm);
}

void x86_permq(X86 *x, int dst, int src, int imm)
{
    vex_map(x, dst, -1, src, 1, 1, 3, 1);
    put(x, 0x00);
    modrm_reg(x, dst, src);
    put(x, (uint8_t)imm);
    snprintf(x->text, sizeof x->text, "%-9s ymm%d, ymm%d, 0x%02x", "vpermq", dst, src, imm);
}

void x86_pshufb(X86 *x, int dst, int src, int base, int32_t disp)
{
    char m[40];
    vex_map(x, dst, src, base, 1, 1, 2, 0);
    put(x, 0x00);
    modrm_mem(x, dst, base, disp);
    mem_text(m, sizeof m, base, disp);
    snprintf(x->text, sizeof x->text, "%-9s ymm%d, ymm%d, %s", "vpshufb", dst, src, m);
}

void x86_shiftq(X86 *x, int left, int dst, int src, int imm)
{
    vex(x, left ? 6 : 2, dst, src, 1, 1);  /* the destination is in VEX.vvvv, ModRM.reg is /6 or /2 */
    put(x, 0x73);
    modrm_reg(x, left ? 6 : 2, src);
    put(x, (uint8_t)imm);
    snprintf(x->text, sizeof x->text, "%-9s ymm%d, ymm%d, %d", left ? "vpsllq" : "vpsrlq", dst, src,
             imm);
}

void x86_vzeroupper(X86 *x)
{
    put(x, 0xC5);
    put(x, 0xF8);
    put(x, 0x77);
    snprintf(x->text, sizeof x->text, "vzeroupper");
}

void x86_load64(X86 *x, int dst, int base, int32_t disp)
{
    char m[40];
    put(x, (uint8_t)(0x48 | (dst >> 3) << 2 | (base >> 3)));  /* REX.W, .R and .B for r8-r15 */
    put(x, 0x8B);
    modrm_mem(x, dst, base, disp);
    mem_text(m, sizeof m, base, disp);
    snprintf(x->text, sizeof x->text, "%-9s %s, %s", "mov", gpr[dst], m);
}

void x86_dec(X86 *x, int reg)
{
    put(x, (uint8_t)(0x48 | (reg >> 3)));  /* REX.W, plus REX.B for r8-r15 */
    put(x, 0xFF);
    put(x, (uint8_t)(0xC8 | (reg & 7)));   /* /1 = dec */
    snprintf(x->text, sizeof x->text, "%-9s %s", "dec", gpr[reg]);
}

void x86_jnz(X86 *x, size_t target)
{
    int32_t rel = (int32_t)((int64_t)target - (int64_t)(x->len + 6));
    put(x, 0x0F);
    put(x, 0x85);
    for (int i = 0; i < 4; i++)
        put(x, (uint8_t)((uint32_t)rel >> (8 * i)));
    snprintf(x->text, sizeof x->text, "%-9s 0x%zx", "jnz", target);
}

void x86_ret(X86 *x)
{
    put(x, 0xC3);
    snprintf(x->text, sizeof x->text, "ret");
}

void x86_free(X86 *x)
{
    free(x->b);
    x->b = NULL;
    x->len = x->cap = 0;
}
