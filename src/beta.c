#include "beta.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"

/* ---- the reference emulator --------------------------------------------------------------- */

void beta_reset(Beta *m)
{
    memset(m, 0, sizeof *m);
    memcpy(m->mem + BETA_ROM, beta_rom() + BETA_ROM, 256 - BETA_ROM);
    m->pc = BETA_MON;
}

void beta_step(Beta *m)
{
    uint8_t b0 = m->mem[m->pc], arg = m->mem[(uint8_t)(m->pc + 1)];
    unsigned op = b0 & 15, mode = (b0 >> 4) & 3, t;
    uint8_t ea = (uint8_t)(arg + (mode == BMODE_IDX ? m->x : 0)), d = m->mem[ea];
    uint8_t v = mode == BMODE_IMM ? arg : mode == BMODE_A ? m->a : d;
    uint8_t next = (uint8_t)(m->pc + 2);
    m->out_strobe = m->halted = m->waiting = 0;
    switch (op) {
    case BOP_LDA: m->a = v; break;
    case BOP_LDX: m->x = v; break;
    case BOP_ADD: t = m->a + v;                        m->c = (uint8_t)(t >> 8); m->a = (uint8_t)t; break;
    case BOP_ADC: t = m->a + v + m->c;                 m->c = (uint8_t)(t >> 8); m->a = (uint8_t)t; break;
    case BOP_SUB: t = m->a + (uint8_t)~v + 1u;         m->c = (uint8_t)(t >> 8); m->a = (uint8_t)t; break;
    case BOP_AND: m->a &= v; break;
    case BOP_OR:  m->a |= v; break;
    case BOP_XOR: m->a ^= v; break;
    case BOP_STA: if (ea < BETA_ROM) m->mem[ea] = m->a; break;
    case BOP_STX: if (ea < BETA_ROM) m->mem[ea] = m->x; break;
    case BOP_JMP: next = v & 0xFE; break;
    case BOP_JZ:  if (m->a == 0) next = v & 0xFE; break;
    case BOP_JC:  if (m->c) next = v & 0xFE; break;
    case BOP_JSR:
        if (ea < BETA_ROM)
            m->mem[ea] = next;
        next = (uint8_t)(ea + 2) & 0xFE;
        break;
    case BOP_IO:
        if (mode == BIO_IN) {
            if (m->in_valid) {
                m->a = m->in;
                m->in_valid = 0;
            } else {
                next = m->pc;
                m->waiting = 1;
            }
        } else if (mode == BIO_OUT) {
            m->out = m->a;
            m->out_strobe = 1;
        } else if (mode == BIO_HLT) {
            next = m->pc;
            m->halted = 1;
        }
        break;
    default:  /* BOP_REG */
        if (mode == BREG_INX)
            m->x++;
        else if (mode == BREG_TAX)
            m->x = m->a;
        else if (mode == BREG_TXA)
            m->a = m->x;
        else {
            m->c = m->a & 1;
            m->a >>= 1;
        }
        break;
    }
    m->pc = next;
}

/* ---- names of things ---------------------------------------------------------------------- */

static const char *const op_names[16] = {"LDA", "LDX", "ADD", "ADC", "SUB", "AND", "OR",  "XOR",
                                         "STA", "STX", "JMP", "JZ",  "JC",  "JSR", NULL,  NULL};
static const char *const io_names[4] = {"IN", "OUT", "HLT", "NOP"};
static const char *const reg_names[4] = {"INX", "TAX", "TXA", "SHR"};

void beta_disasm(uint8_t b0, uint8_t b1, char *buf, size_t len)
{
    unsigned op = b0 & 15, mode = (b0 >> 4) & 3;
    int jump = op == BOP_JMP || op == BOP_JZ || op == BOP_JC;
    if (op == BOP_IO || op == BOP_REG)
        snprintf(buf, len, "%s", (op == BOP_IO ? io_names : reg_names)[mode]);
    else if (mode == BMODE_A)
        snprintf(buf, len, "%-3s A", op_names[op]);
    else if (jump)
        snprintf(buf, len, mode == BMODE_IMM ? "%-3s 0x%02X" : mode == BMODE_ABS ? "%-3s (0x%02X)"
                                                                                  : "%-3s (0x%02X,X)",
                 op_names[op], b1);
    else if (op >= BOP_STA && mode == BMODE_IMM)  /* a store or JSR treats # as the address */
        snprintf(buf, len, "%-3s 0x%02X", op_names[op], b1);
    else
        snprintf(buf, len, mode == BMODE_IMM ? "%-3s #0x%02X" : mode == BMODE_ABS ? "%-3s 0x%02X"
                                                                                   : "%-3s 0x%02X,X",
                 op_names[op], b1);
}

/* ---- the assembler ------------------------------------------------------------------------ */

typedef struct {
    char name[40];
    int  value;
} Sym;

typedef struct {
    const char *name;
    int         line, pass, failed;
    char       *err;
    size_t      errlen;
    Sym        *syms;
    size_t      nsyms, capsyms;
    BetaImage  *img;
    int         pc;
} Asm;

static void fail(Asm *a, const char *fmt, ...)
{
    if (a->failed)
        return;
    a->failed = 1;
    int n = snprintf(a->err, a->errlen, "%s:%d: ", a->name, a->line);
    va_list ap;
    va_start(ap, fmt);
    if (n >= 0 && (size_t)n < a->errlen)
        vsnprintf(a->err + n, a->errlen - (size_t)n, fmt, ap);
    va_end(ap);
}

static Sym *find_sym(Asm *a, const char *name)
{
    for (size_t i = 0; i < a->nsyms; i++)
        if (!strcmp(a->syms[i].name, name))
            return &a->syms[i];
    return NULL;
}

static void define(Asm *a, const char *name, int value)
{
    Sym *s = find_sym(a, name);
    if (s) {
        if (a->pass == 1 && s->value != value) {
            fail(a, "%s is defined twice", name);
            return;
        }
        s->value = value;
        return;
    }
    if (strlen(name) >= sizeof s->name) {
        fail(a, "name too long: %s", name);
        return;
    }
    if (a->nsyms == a->capsyms) {
        a->capsyms = a->capsyms ? 2 * a->capsyms : 64;
        a->syms = realloc(a->syms, a->capsyms * sizeof *a->syms);
        if (!a->syms) {
            fputs("out of memory\n", stderr);
            exit(1);
        }
    }
    s = &a->syms[a->nsyms++];
    snprintf(s->name, sizeof s->name, "%s", name);
    s->value = value;
}

static const char *skip(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int is_name_start(int c)
{
    return isalpha(c) || c == '_' || c == '.';
}

static int is_name_char(int c)
{
    return isalnum(c) || c == '_' || c == '.';
}

/* A character in quotes, at s just after the opening quote: returns its value and moves s on. */
static int char_value(Asm *a, const char **s)
{
    int c = (unsigned char)*(*s)++;
    if (c == '\\') {
        c = (unsigned char)*(*s)++;
        c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == '0' ? 0 : c;
    }
    if (!c)
        fail(a, "unfinished character");
    return c;
}

/* expr := term (('+' | '-') term)*, term := number | 'c' | name */
static int expr(Asm *a, const char **sp)
{
    const char *s = skip(*sp);
    int total = 0, sign = 1;
    for (;;) {
        int v = 0;
        if (*s == '-') {
            sign = -sign;
            s = skip(s + 1);
            continue;
        }
        if (isdigit((unsigned char)*s)) {
            char *end;
            v = (int)strtol(s, &end, 0);
            s = end;
        } else if (*s == '\'') {
            s++;
            v = char_value(a, &s);
            if (*s++ != '\'')
                fail(a, "expected ' after the character");
        } else if (is_name_start((unsigned char)*s)) {
            char name[64];
            size_t n = 0;
            while (is_name_char((unsigned char)*s) && n + 1 < sizeof name)
                name[n++] = *s++;
            name[n] = '\0';
            Sym *sym = find_sym(a, name);
            if (sym)
                v = sym->value;
            else if (a->pass == 2)
                fail(a, "unknown name %s", name);
        } else {
            fail(a, "expected a number, a character or a name");
            return 0;
        }
        total += sign * v;
        s = skip(s);
        if (*s == '+') {
            sign = 1;
            s = skip(s + 1);
        } else if (*s == '-') {
            sign = -1;
            s = skip(s + 1);
        } else {
            break;
        }
    }
    *sp = s;
    return total;
}

static int byte_value(Asm *a, int v)
{
    if (v < -128 || v > 255)
        fail(a, "%d does not fit in a byte", v);
    return v & 0xFF;
}

static void emit(Asm *a, int v)
{
    if (a->pc > 255) {
        fail(a, "past the end of memory (address 255)");
        return;
    }
    if (a->pass == 2) {
        if (a->img->used[a->pc])
            fail(a, "address 0x%02X is used twice", a->pc);
        a->img->bytes[a->pc] = (uint8_t)byte_value(a, v);
        a->img->used[a->pc] = 1;
    }
    a->pc++;
}

static void string(Asm *a, const char *s, int zero)
{
    s = skip(s);
    if (*s++ != '"') {
        fail(a, "expected a string in double quotes");
        return;
    }
    while (*s && *s != '"' && !a->failed)
        emit(a, char_value(a, &s));
    if (*s != '"')
        fail(a, "unfinished string");
    else if (*skip(s + 1))
        fail(a, "unexpected text after the string");
    if (zero)
        emit(a, 0);
}

static int reg_word(const char *s, int r)
{
    s = skip(s);
    return toupper((unsigned char)s[0]) == r && !is_name_char((unsigned char)s[1]) && !*skip(s + 1);
}

/* One instruction: mnemonic m, operand text s. */
static void instruction(Asm *a, const char *m, const char *s)
{
    char up[8];
    size_t n = 0;
    for (; m[n] && n + 1 < sizeof up; n++)
        up[n] = (char)toupper((unsigned char)m[n]);
    up[n] = '\0';
    int op = -1, mode = -1, arg = 0;
    for (int i = 0; i < 14; i++)
        if (!strcmp(up, op_names[i]))
            op = i;
    for (int i = 0; i < 4 && op < 0; i++) {
        if (!strcmp(up, io_names[i]))
            op = BOP_IO, mode = i;
        else if (!strcmp(up, reg_names[i]))
            op = BOP_REG, mode = i;
    }
    if (op < 0) {
        fail(a, "unknown instruction %s", m);
        return;
    }
    if (a->pc & 1)
        fail(a, "instruction at the odd address 0x%02X: put .align before it", a->pc);
    s = skip(s);
    if (op == BOP_IO || op == BOP_REG) {
        if (*s)
            fail(a, "%s takes no operand", up);
    } else if (!*s) {
        fail(a, "%s needs an operand", up);
    } else if (op <= BOP_XOR || op == BOP_JMP || op == BOP_JZ || op == BOP_JC) {
        int jump = op >= BOP_JMP;
        if (reg_word(s, 'A')) {
            mode = BMODE_A;
            s = skip(s) + 1;
        } else if (!jump && *s == '#') {
            s++;
            mode = BMODE_IMM;
            arg = expr(a, &s);
        } else if (jump && *s == '(') {
            s++;
            arg = expr(a, &s);
            mode = BMODE_ABS;
            if (*s == ',') {
                s = skip(s + 1);
                if (toupper((unsigned char)*s) != 'X')
                    fail(a, "expected (address,X)");
                s = skip(s + 1);
                mode = BMODE_IDX;
            }
            if (*s++ != ')')
                fail(a, "expected )");
        } else {
            arg = expr(a, &s);
            mode = jump ? BMODE_IMM : BMODE_ABS;
            if (!jump && *s == ',') {
                s = skip(s + 1);
                if (toupper((unsigned char)*s) != 'X')
                    fail(a, "expected address,X");
                s = skip(s + 1);
                mode = BMODE_IDX;
            }
        }
    } else {  /* STA, STX, JSR: an address, maybe ,X */
        arg = expr(a, &s);
        mode = BMODE_ABS;
        if (*s == ',') {
            s = skip(s + 1);
            if (toupper((unsigned char)*s) != 'X')
                fail(a, "expected address,X");
            s = skip(s + 1);
            mode = BMODE_IDX;
        }
    }
    if (*skip(s))
        fail(a, "unexpected text: %s", skip(s));
    if (a->pass == 2 && a->pc >= 0 && a->pc < 256)
        a->img->insn[a->pc] = 1;
    emit(a, op | mode << 4);
    emit(a, a->pass == 2 ? byte_value(a, arg) : 0);
}

static void line(Asm *a, char *text)
{
    /* the comment goes, except inside quotes */
    for (char *p = text, q = 0; *p; p++) {
        if ((*p == '"' || *p == '\'') && (!q || q == *p) && (p == text || p[-1] != '\\'))
            q = q ? 0 : *p;
        else if (*p == ';' && !q) {
            *p = '\0';
            break;
        }
    }
    const char *s = skip(text);
    char word[64];
    for (;;) {  /* labels, then a directive or an instruction */
        size_t n = 0;
        if (!*s)
            return;
        while (is_name_char((unsigned char)*s) && n + 1 < sizeof word)
            word[n++] = *s++;
        word[n] = '\0';
        if (!n) {
            fail(a, "expected a label, a directive or an instruction");
            return;
        }
        s = skip(s);
        if (*s == ':') {
            define(a, word, a->pc);
            s = skip(s + 1);
            continue;
        }
        if (*s == '=') {
            s++;
            int v = expr(a, &s);
            define(a, word, v);
            if (*skip(s))
                fail(a, "unexpected text: %s", skip(s));
            return;
        }
        break;
    }
    if (!strcmp(word, ".org")) {
        a->pc = expr(a, &s);
        if (a->pc < 0 || a->pc > 255)
            fail(a, ".org 0x%X is outside memory", a->pc);
    } else if (!strcmp(word, ".byte")) {
        for (;;) {
            emit(a, expr(a, &s));
            s = skip(s);
            if (*s != ',')
                break;
            s++;
        }
        if (*skip(s))
            fail(a, "unexpected text: %s", skip(s));
    } else if (!strcmp(word, ".ascii") || !strcmp(word, ".asciz")) {
        string(a, s, word[5] == 'z');
    } else if (!strcmp(word, ".align")) {
        if (a->pc & 1)
            emit(a, 0);
    } else if (word[0] == '.') {
        fail(a, "unknown directive %s", word);
    } else {
        instruction(a, word, s);
    }
}

static int assemble(const char *src, const char *name, BetaImage *out, char *err, size_t errlen,
                    const Sym *pre, int npre)
{
    Asm a;
    memset(&a, 0, sizeof a);
    memset(out, 0, sizeof *out);
    a.name = name;
    a.err = err;
    a.errlen = errlen;
    a.img = out;
    for (int i = 0; i < npre; i++)
        define(&a, pre[i].name, pre[i].value);
    size_t len = strlen(src);
    char *buf = malloc(len + 1);
    for (a.pass = 1; a.pass <= 2 && !a.failed; a.pass++) {
        a.pc = 0;
        a.line = 0;
        for (const char *p = src; *p && !a.failed;) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            memcpy(buf, p, n);
            buf[n] = '\0';
            if (n && buf[n - 1] == '\r')
                buf[n - 1] = '\0';
            a.line++;
            line(&a, buf);
            p += n + (e != NULL);
        }
    }
    free(buf);
    free(a.syms);
    if (a.failed)
        return 0;
    out->lo = out->hi = 0;
    for (unsigned i = 0, first = 1; i < 256; i++)
        if (out->used[i]) {
            if (first)
                out->lo = i;
            first = 0;
            out->hi = i + 1;
        }
    return 1;
}

/* ---- the monitor -------------------------------------------------------------------------- */

static const char monitor_src[] =
    "; The beta monitor, in ROM from 0xA0, where beta powers on. It reads commands through IN,\n"
    "; one byte at a time:\n"
    ";   'L' addr n byte*n   load n bytes at addr\n"
    ";   'G' addr            run the program at addr; it comes back with JMP MON\n"
    ";   'D' addr n          send the n bytes from addr out through OUT\n"
    "; and skips any other byte. A service for programs, entered with the return address in A:\n"
    ";   PUTS                print the zero-terminated string at X\n"
    "; It keeps three bytes at the top of RAM, 0x9D .. 0x9F.\n"
    "RET = 0x9D\n"
    "TGT = 0x9E\n"
    "CNT = 0x9F\n"
    "        .org 0xA0\n"
    "MON:    IN              ; a command\n"
    "        SUB #'L'\n"
    "        JZ  load\n"
    "        SUB #'G'-'L'\n"
    "        JZ  go\n"
    "        SUB #'D'-'G'\n"
    "        JZ  dump\n"
    "        JMP MON\n"
    "load:   IN              ; address\n"
    "        TAX\n"
    "        IN              ; count\n"
    "lnext:  JZ  MON\n"
    "        STA CNT\n"
    "        IN\n"
    "        STA 0,X\n"
    "        INX\n"
    "        LDA CNT\n"
    "        SUB #1\n"
    "        JMP lnext\n"
    "go:     IN              ; address\n"
    "        STA TGT\n"
    "        JMP (TGT)\n"
    "dump:   IN              ; address\n"
    "        TAX\n"
    "        IN              ; count\n"
    "dnext:  JZ  MON\n"
    "        STA CNT\n"
    "        LDA 0,X\n"
    "        OUT\n"
    "        INX\n"
    "        LDA CNT\n"
    "        SUB #1\n"
    "        JMP dnext\n"
    "PUTS:   STA RET\n"
    "pnext:  LDA 0,X\n"
    "        JZ  pdone\n"
    "        OUT\n"
    "        INX\n"
    "        JMP pnext\n"
    "pdone:  JMP (RET)\n";

static uint8_t rom[256];
static Sym exported[2] = {{"MON", -1}, {"PUTS", -1}};

static void load_monitor(void)
{
    if (exported[0].value >= 0)
        return;
    BetaImage img;
    char err[256];
    Asm probe;
    if (!assemble(monitor_src, "monitor", &img, err, sizeof err, NULL, 0)) {
        fprintf(stderr, "the beta monitor does not assemble: %s\n", err);
        exit(1);
    }
    memcpy(rom, img.bytes, sizeof rom);
    /* the two names programs may use: assemble once more, reading the symbol table */
    memset(&probe, 0, sizeof probe);
    probe.name = "monitor";
    probe.err = err;
    probe.errlen = sizeof err;
    probe.img = &img;
    probe.pass = 1;
    char buf[256];
    for (const char *p = monitor_src; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        memcpy(buf, p, n);
        buf[n] = '\0';
        line(&probe, buf);
        p += n + (e != NULL);
    }
    for (int i = 0; i < 2; i++)
        exported[i].value = find_sym(&probe, exported[i].name)->value;
    free(probe.syms);
}

const uint8_t *beta_rom(void)
{
    load_monitor();
    return rom;
}

const char *beta_monitor_source(void)
{
    return monitor_src;
}

int beta_assemble(const char *src, const char *name, BetaImage *out, char *err, size_t errlen)
{
    load_monitor();
    return assemble(src, name, out, err, errlen, exported, 2);
}

/* ---- the CPU as a NAND/STO program -------------------------------------------------------- */

static uint32_t cellf(Builder *b, const char *fmt, int i, int j)
{
    char name[48];
    snprintf(name, sizeof name, fmt, i, j);
    return b_cell(b, name);
}

static uint32_t gate(Builder *b, int type, const char *name, uint32_t x, uint32_t y)
{
    uint32_t c = b_cell(b, name);
    b_gate2(b, type, c, x, y);
    return c;
}

static uint32_t not1(Builder *b, const char *name, uint32_t x)
{
    uint32_t c = b_cell(b, name);
    b_gate(b, G_NOT, c, &x, 1);
    return c;
}

/* s = x ^ y ^ c, k = carry; s may be x or y and k may be c */
static void full_adder(Builder *b, uint32_t s, uint32_t k, uint32_t x, uint32_t y, uint32_t c)
{
    b_full_adder(b, s, k, x, y, c);
}

/* out[i] = in[i] + 1 for bits lo .. 7 (out may be in); bits below lo are copied */
static void increment(Builder *b, const uint32_t *out, const uint32_t *in, int lo)
{
    uint32_t c = b_cell(b, "inc_c"), one = CELL_ONE;
    b_gate(b, G_BUF, c, &one, 1);
    for (int i = 0; i < 8; i++) {
        if (i < lo) {
            if (out[i] != in[i])
                b_gate(b, G_BUF, out[i], &in[i], 1);
            continue;
        }
        uint32_t t = b_cell(b, "inc_t");
        b_gate2(b, G_AND, t, in[i], c);
        b_gate2(b, G_XOR, out[i], in[i], c);
        b_gate(b, G_BUF, c, &t, 1);
    }
}

/* line[v] = 1 exactly when the k bits (bit 0 first, with their complements) spell v */
static void decode(Builder *b, const uint32_t *bit, const uint32_t *nbit, int k, uint32_t *line,
                   const char *tag)
{
    char name[48];
    if (k <= 4) {
        for (int v = 0; v < 1 << k; v++) {
            uint32_t in[4];
            for (int i = 0; i < k; i++)
                in[i] = (v >> i) & 1 ? bit[i] : nbit[i];
            snprintf(name, sizeof name, "%s%d", tag, v);
            line[v] = b_cell(b, name);
            b_gate(b, k == 1 ? G_BUF : G_AND, line[v], in, k);
        }
        return;
    }
    int kl = k / 2, kh = k - kl;
    uint32_t lo[16], hi[16];
    snprintf(name, sizeof name, "%sl", tag);
    decode(b, bit, nbit, kl, lo, name);
    snprintf(name, sizeof name, "%sh", tag);
    decode(b, bit + kl, nbit + kl, kh, hi, name);
    for (int h = 0; h < 1 << kh; h++)
        for (int l = 0; l < 1 << kl; l++) {
            snprintf(name, sizeof name, "%s%d", tag, h << kl | l);
            line[h << kl | l] = gate(b, G_AND, name, lo[l], hi[h]);
        }
}

void gen_beta(Program *p, BetaCells *bc)
{
    const uint8_t *rm = beta_rom();
    Builder *b = b_new();
    uint32_t zero = CELL_ZERO, one = CELL_ONE;
    char name[48];

    /* the state */
    for (int a = 0; a < BETA_ROM; a++)
        for (int i = 0; i < 8; i++)
            bc->mem[a][i] = cellf(b, "m%02x_%d", a, i);
    for (int i = 0; i < 8; i++) {
        bc->a[i] = cellf(b, "a%d", i, 0);
        bc->x[i] = cellf(b, "x%d", i, 0);
        bc->pc[i] = cellf(b, "pc%d", i, 0);
        bc->in[i] = cellf(b, "in%d", i, 0);
        bc->out[i] = cellf(b, "out%d", i, 0);
    }
    bc->c = b_cell(b, "cy");
    bc->in_valid = b_cell(b, "iv");
    bc->out_strobe = b_cell(b, "os");
    bc->halted = b_cell(b, "halt");
    bc->waiting = b_cell(b, "wait");
    for (int i = 0; i < 8; i++)
        b_input(b, bc->in[i]);
    b_input(b, bc->in_valid);
    for (int i = 0; i < 8; i++)
        b_output(b, bc->out[i]);
    b_output(b, bc->out_strobe);
    b_output(b, bc->halted);
    b_output(b, bc->waiting);

    /* the true PC: the cells hold PC ^ BETA_MON */
    uint32_t pc[8], npc[8];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "pcn%d", i);
        uint32_t inv = not1(b, name, bc->pc[i]);
        pc[i] = (BETA_MON >> i) & 1 ? inv : bc->pc[i];
        npc[i] = (BETA_MON >> i) & 1 ? bc->pc[i] : inv;
    }

    /* fetch: the instruction word at PC / 2 */
    uint32_t fsel[128], ins[16], val[128];
    decode(b, pc + 1, npc + 1, 7, fsel, "fs");
    for (int j = 0; j < 14; j++) {  /* op byte bits 0-5, then the argument byte */
        int bit = j < 6 ? j : j - 6, second = j >= 6;
        for (int w = 0; w < 128; w++) {
            int addr = 2 * w + second;
            val[w] = addr < BETA_ROM ? bc->mem[addr][bit]
                                     : ((rm[addr] >> bit) & 1 ? CELL_ONE : CELL_ZERO);
        }
        ins[j] = cellf(b, "ins%d", j, 0);
        b_select(b, ins[j], fsel, val, 128);
    }
    const uint32_t *arg = ins + 6;
    uint32_t nins[6], op[16], mode[4];
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof name, "insn%d", i);
        nins[i] = not1(b, name, ins[i]);
    }
    decode(b, ins, nins, 4, op, "op");
    decode(b, ins + 4, nins + 4, 2, mode, "md");

    /* effective address: arg, plus X in mode IDX */
    uint32_t ea[8], nea[8], c = b_cell(b, "ea_c");
    b_gate(b, G_BUF, c, &zero, 1);
    for (int i = 0; i < 8; i++) {
        uint32_t xm = gate(b, G_AND, "ea_xm", bc->x[i], mode[BMODE_IDX]);
        ea[i] = cellf(b, "ea%d", i, 0);
        full_adder(b, ea[i], c, arg[i], xm, c);
    }
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "ean%d", i);
        nea[i] = not1(b, name, ea[i]);
    }
    uint32_t esel[256], data[8], v[8], vals[256];
    decode(b, ea, nea, 8, esel, "es");
    for (int j = 0; j < 8; j++) {
        for (int addr = 0; addr < 256; addr++)
            vals[addr] = addr < BETA_ROM ? bc->mem[addr][j]
                                         : ((rm[addr] >> j) & 1 ? CELL_ONE : CELL_ZERO);
        data[j] = cellf(b, "d%d", j, 0);
        b_select(b, data[j], esel, vals, 256);
    }
    /* the operand: #arg, memory, or A */
    uint32_t mem_mode = gate(b, G_OR, "m_mem", mode[BMODE_ABS], mode[BMODE_IDX]);
    for (int j = 0; j < 8; j++) {
        uint32_t s3[3] = {mode[BMODE_IMM], mem_mode, mode[BMODE_A]};
        uint32_t v3[3] = {arg[j], data[j], bc->a[j]};
        v[j] = cellf(b, "v%d", j, 0);
        b_select(b, v[j], s3, v3, 3);
    }

    /* the ALU: A + V, A + V + C, A - V; AND, OR, XOR */
    uint32_t sum[8], ands[8], ors[8], xors[8], co = b_cell(b, "alu_c");
    uint32_t adc_c = gate(b, G_AND, "adc_c", op[BOP_ADC], bc->c);
    b_gate2(b, G_OR, co, adc_c, op[BOP_SUB]);  /* carry in */
    for (int j = 0; j < 8; j++) {
        uint32_t vv = gate(b, G_XOR, "alu_v", v[j], op[BOP_SUB]);
        sum[j] = cellf(b, "sum%d", j, 0);
        full_adder(b, sum[j], co, bc->a[j], vv, co);
        ands[j] = gate(b, G_AND, (snprintf(name, sizeof name, "and%d", j), name), bc->a[j], v[j]);
        ors[j] = gate(b, G_OR, (snprintf(name, sizeof name, "or%d", j), name), bc->a[j], v[j]);
        xors[j] = gate(b, G_XOR, (snprintf(name, sizeof name, "xor%d", j), name), bc->a[j], v[j]);
    }
    uint32_t xinc[8], pc2[8], ea2[8], jt[8];
    for (int i = 0; i < 8; i++) {
        xinc[i] = cellf(b, "xi%d", i, 0);
        pc2[i] = cellf(b, "pc2_%d", i, 0);
        ea2[i] = cellf(b, "ea2_%d", i, 0);
        jt[i] = i ? v[i] : CELL_ZERO;
    }
    increment(b, xinc, bc->x, 0);
    increment(b, pc2, pc, 1);
    increment(b, ea2, ea, 1);
    b_gate(b, G_BUF, ea2[0], &zero, 1);
    uint32_t a_zero = b_cell(b, "a_zero");
    b_gate(b, G_NOR, a_zero, bc->a, 8);

    /* what this instruction does */
    uint32_t io_in = gate(b, G_AND, "s_inop", op[BOP_IO], mode[BIO_IN]);
    uint32_t s_in = gate(b, G_AND, "s_in", io_in, bc->in_valid);
    uint32_t s_out = gate(b, G_AND, "s_out", op[BOP_IO], mode[BIO_OUT]);
    uint32_t s_hlt = gate(b, G_AND, "s_hlt", op[BOP_IO], mode[BIO_HLT]);
    uint32_t s_inx = gate(b, G_AND, "s_inx", op[BOP_REG], mode[BREG_INX]);
    uint32_t s_tax = gate(b, G_AND, "s_tax", op[BOP_REG], mode[BREG_TAX]);
    uint32_t s_txa = gate(b, G_AND, "s_txa", op[BOP_REG], mode[BREG_TXA]);
    uint32_t s_shr = gate(b, G_AND, "s_shr", op[BOP_REG], mode[BREG_SHR]);
    uint32_t sum_ops[3] = {op[BOP_ADD], op[BOP_ADC], op[BOP_SUB]};
    uint32_t s_sum = b_cell(b, "s_sum");
    b_gate(b, G_OR, s_sum, sum_ops, 3);
    uint32_t a_ops[8] = {op[BOP_LDA], s_sum, op[BOP_AND], op[BOP_OR], op[BOP_XOR], s_in, s_txa, s_shr};
    uint32_t a_keep = b_cell(b, "s_akeep");
    b_gate(b, G_NOR, a_keep, a_ops, 8);
    uint32_t x_ops[3] = {op[BOP_LDX], s_inx, s_tax};
    uint32_t x_keep = b_cell(b, "s_xkeep");
    b_gate(b, G_NOR, x_keep, x_ops, 3);
    uint32_t c_ops[2] = {s_sum, s_shr};
    uint32_t c_keep = b_cell(b, "s_ckeep");
    b_gate(b, G_NOR, c_keep, c_ops, 2);
    uint32_t jz = gate(b, G_AND, "s_jz", op[BOP_JZ], a_zero);
    uint32_t jc = gate(b, G_AND, "s_jc", op[BOP_JC], bc->c);
    uint32_t jumps[3] = {op[BOP_JMP], jz, jc}, s_jump = b_cell(b, "s_jump");
    b_gate(b, G_OR, s_jump, jumps, 3);
    uint32_t no_in = gate(b, G_AND, "s_wait", io_in, not1(b, "ivn", bc->in_valid));
    uint32_t s_stay = gate(b, G_OR, "s_stay", no_in, s_hlt);
    uint32_t pc_ops[3] = {s_jump, op[BOP_JSR], s_stay}, s_next = b_cell(b, "s_next");
    b_gate(b, G_NOR, s_next, pc_ops, 3);

    /* the new registers, from the old ones */
    uint32_t na[8], nx[8], npcv[8], wd[8];
    for (int j = 0; j < 8; j++) {
        uint32_t as[9] = {op[BOP_LDA], s_sum, op[BOP_AND], op[BOP_OR], op[BOP_XOR], s_in, s_txa,
                          s_shr, a_keep};
        uint32_t av[9] = {v[j], sum[j], ands[j], ors[j], xors[j], bc->in[j], bc->x[j],
                          j < 7 ? bc->a[j + 1] : CELL_ZERO, bc->a[j]};
        na[j] = cellf(b, "na%d", j, 0);
        b_select(b, na[j], as, av, 9);
        uint32_t xs[4] = {op[BOP_LDX], s_inx, s_tax, x_keep};
        uint32_t xv[4] = {v[j], xinc[j], bc->a[j], bc->x[j]};
        nx[j] = cellf(b, "nx%d", j, 0);
        b_select(b, nx[j], xs, xv, 4);
        uint32_t ps[4] = {s_jump, op[BOP_JSR], s_stay, s_next};
        uint32_t pv[4] = {jt[j], ea2[j], pc[j], pc2[j]};
        npcv[j] = cellf(b, "np%d", j, 0);
        b_select(b, npcv[j], ps, pv, 4);
        uint32_t ws[3] = {op[BOP_STA], op[BOP_STX], op[BOP_JSR]};
        uint32_t wv[3] = {bc->a[j], bc->x[j], pc2[j]};
        wd[j] = cellf(b, "wd%d", j, 0);
        b_select(b, wd[j], ws, wv, 3);
    }
    uint32_t cs[3] = {s_sum, s_shr, c_keep}, cv[3] = {co, bc->a[0], bc->c};
    uint32_t nc = b_cell(b, "nc");
    b_select(b, nc, cs, cv, 3);

    /* the write port: STA, STX and JSR write one RAM byte */
    uint32_t w_ops[3] = {op[BOP_STA], op[BOP_STX], op[BOP_JSR]}, we = b_cell(b, "we");
    b_gate(b, G_OR, we, w_ops, 3);
    uint32_t wa = b_cell(b, "we_a"), nwa = b_cell(b, "we_an");
    for (int a = 0; a < BETA_ROM; a++) {
        b_gate2(b, G_AND, wa, esel[a], we);
        b_gate(b, G_NOT, nwa, &wa, 1);
        for (int j = 0; j < 8; j++)
            b_mux(b, bc->mem[a][j], wa, nwa, wd[j], bc->mem[a][j]);
    }

    /* commit, last: the ports, then the registers */
    uint32_t nso = not1(b, "s_outn", s_out);
    for (int j = 0; j < 8; j++)
        b_mux(b, bc->out[j], s_out, nso, bc->a[j], bc->out[j]);
    b_gate(b, G_BUF, bc->out_strobe, &s_out, 1);
    b_gate(b, G_BUF, bc->halted, &s_hlt, 1);
    b_gate(b, G_BUF, bc->waiting, &no_in, 1);
    uint32_t nio = not1(b, "s_inopn", io_in);
    b_gate2(b, G_AND, bc->in_valid, bc->in_valid, nio);
    for (int j = 0; j < 8; j++) {
        b_gate(b, G_BUF, bc->a[j], &na[j], 1);
        b_gate(b, G_BUF, bc->x[j], &nx[j], 1);
        b_gate(b, (BETA_MON >> j) & 1 ? G_NOT : G_BUF, bc->pc[j], &npcv[j], 1);
    }
    b_gate(b, G_BUF, bc->c, &nc, 1);
    (void)one;
    b_finish(b, p);
}

/* ---- one lane's state --------------------------------------------------------------------- */

static void put8(LaneMachine *m, const uint32_t *cells, unsigned lane, uint8_t v)
{
    for (int i = 0; i < 8; i++)
        lane_set(m, cells[i], lane, (uint8_t)((v >> i) & 1));
}

static uint8_t get8(const LaneMachine *m, const uint32_t *cells, unsigned lane)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint8_t)(lane_get(m, cells[i], lane) << i);
    return v;
}

void beta_to_lane(LaneMachine *m, const BetaCells *c, unsigned lane, const Beta *s)
{
    for (int a = 0; a < BETA_ROM; a++)
        put8(m, c->mem[a], lane, s->mem[a]);
    put8(m, c->a, lane, s->a);
    put8(m, c->x, lane, s->x);
    put8(m, c->pc, lane, (uint8_t)(s->pc ^ BETA_MON));
    put8(m, c->in, lane, s->in);
    put8(m, c->out, lane, s->out);
    lane_set(m, c->c, lane, s->c);
    lane_set(m, c->in_valid, lane, s->in_valid);
    lane_set(m, c->out_strobe, lane, s->out_strobe);
    lane_set(m, c->halted, lane, s->halted);
    lane_set(m, c->waiting, lane, s->waiting);
}

void beta_from_lane(const LaneMachine *m, const BetaCells *c, unsigned lane, Beta *s)
{
    memcpy(s->mem + BETA_ROM, beta_rom() + BETA_ROM, 256 - BETA_ROM);
    for (int a = 0; a < BETA_ROM; a++)
        s->mem[a] = get8(m, c->mem[a], lane);
    s->a = get8(m, c->a, lane);
    s->x = get8(m, c->x, lane);
    s->pc = (uint8_t)(get8(m, c->pc, lane) ^ BETA_MON);
    s->in = get8(m, c->in, lane);
    s->out = get8(m, c->out, lane);
    s->c = lane_get(m, c->c, lane);
    s->in_valid = lane_get(m, c->in_valid, lane);
    s->out_strobe = lane_get(m, c->out_strobe, lane);
    s->halted = lane_get(m, c->halted, lane);
    s->waiting = lane_get(m, c->waiting, lane);
}

/* ---- the host side of the ports ----------------------------------------------------------- */

static int bit_of(const LaneMachine *m, uint32_t cell, unsigned lane)
{
    return (int)((m->mem[cell].w[lane >> 6] >> (lane & 63)) & 1);
}

static void out_byte(BetaLane *l, uint8_t v)
{
    if (l->nout == l->cap) {
        l->cap = l->cap ? 2 * l->cap : 256;
        l->out = realloc(l->out, l->cap);
        if (!l->out) {
            fputs("out of memory\n", stderr);
            exit(1);
        }
    }
    l->out[l->nout++] = v;
}

void beta_feed(LaneMachine *m, const BetaCells *c, BetaLane *lanes)
{
    for (unsigned k = 0; k < LANES; k++) {
        BetaLane *l = &lanes[k];
        if (l->at < l->nin && !bit_of(m, c->in_valid, k)) {
            put8(m, c->in, k, l->in[l->at++]);
            lane_set(m, c->in_valid, k, 1);
        }
    }
}

int beta_collect(const LaneMachine *m, const BetaCells *c, BetaLane *lanes)
{
    int done = 0;
    for (unsigned k = 0; k < LANES; k++) {
        BetaLane *l = &lanes[k];
        if (bit_of(m, c->out_strobe, k))
            out_byte(l, get8(m, c->out, k));
        l->done = (bit_of(m, c->waiting, k) && l->at == l->nin) || bit_of(m, c->halted, k);
        done += l->done;
    }
    return done;
}

void beta_emu_pass(Beta *s, BetaLane *l)
{
    if (l->at < l->nin && !s->in_valid) {
        s->in = l->in[l->at++];
        s->in_valid = 1;
    }
    beta_step(s);
    if (s->out_strobe)
        out_byte(l, s->out);
    l->done = (s->waiting && l->at == l->nin) || s->halted;
}

size_t beta_boot_input(const BetaImage *img, const uint8_t *extra, size_t nextra, uint8_t *buf,
                       size_t cap)
{
    size_t n = 0, len = img->hi - img->lo;
    if (5 + len + nextra > cap)
        return 0;
    buf[n++] = 'L';
    buf[n++] = (uint8_t)img->lo;
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, img->bytes + img->lo, len);
    n += len;
    buf[n++] = 'G';
    buf[n++] = (uint8_t)img->lo;
    memcpy(buf + n, extra, nextra);
    return n + nextra;
}
