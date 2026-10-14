#include "build.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct Builder {
    Instr    *code;
    size_t    len, cap;
    char    **names;
    uint32_t  ncells, capcells;
    uint32_t *in, *out, *rq, *rd;
    size_t    nin, capin, nout, capout, nreg, capreg;
    uint32_t *slot;     /* name hash table: cell + 1, 0 = empty */
    size_t    nslot;
    uint32_t  scratch[6];
    CellInfo *info;     /* per cell: plain, lane id or link (the cube, docs/ISA.md) */
    uint32_t  capinfo;
    int       special;
};

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return q;
}

#define GROW(arr, n, cap)                                                \
    do {                                                                 \
        if ((n) == (cap)) {                                              \
            (cap) = (cap) ? 2 * (cap) : 64;                              \
            (arr) = xrealloc((arr), (size_t)(cap) * sizeof *(arr));      \
        }                                                                \
    } while (0)

/* ---- names (case-insensitive, like the assembler) ----------------------------------------- */

static uint64_t hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (uint64_t)tolower((unsigned char)*s)) * 1099511628211ull;
    return h;
}

static int same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static size_t find_slot(const Builder *b, const char *name)
{
    size_t i = (size_t)(hash(name) & (b->nslot - 1));
    while (b->slot[i] && !same(b->names[b->slot[i] - 1], name))
        i = (i + 1) & (b->nslot - 1);
    return i;
}

static void rehash(Builder *b)
{
    free(b->slot);
    b->nslot = b->nslot ? 2 * b->nslot : 1024;
    b->slot = calloc(b->nslot, sizeof *b->slot);
    if (!b->slot) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    for (uint32_t c = 0; c < b->ncells; c++)
        b->slot[find_slot(b, b->names[c])] = c + 1;
}

uint32_t b_cell(Builder *b, const char *name)
{
    size_t i = find_slot(b, name);
    if (b->slot[i])
        return b->slot[i] - 1;
    GROW(b->names, b->ncells, b->capcells);
    GROW(b->info, b->ncells, b->capinfo);
    size_t n = strlen(name) + 1;
    b->names[b->ncells] = memcpy(xrealloc(NULL, n), name, n);
    CellInfo *in = &b->info[b->ncells];
    memset(in, 0, sizeof *in);
    int d = program_id_name(name);
    if (d >= 0) {  /* ID0 .. ID15 */
        in->kind = CK_ID;
        in->dim = (uint8_t)d;
        b->special = 1;
    }
    b->slot[i] = ++b->ncells;
    if (2 * (size_t)b->ncells > b->nslot)
        rehash(b);
    return b->ncells - 1;
}

int b_has(const Builder *b, const char *name)
{
    return b->slot[find_slot(b, name)] != 0;
}

uint32_t b_link(Builder *b, const char *name, uint32_t src, int dim)
{
    assert(!b_has(b, name) && src < b->ncells && dim >= 0 && dim < CUBE_MAX_DIMS);
    uint32_t c = b_cell(b, name);
    b->info[c].kind = CK_LINK;
    b->info[c].dim = (uint8_t)dim;
    b->info[c].src = src;
    b->special = 1;
    return c;
}

Builder *b_new(void)
{
    Builder *b = calloc(1, sizeof *b);
    if (!b) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    rehash(b);
    b_cell(b, "ZERO");
    b_cell(b, "ONE");
    return b;
}

static uint32_t scratch(Builder *b, int i)
{
    static const char *const name[6] = {"__t0", "__t1", "__t2", "__t3", "__t4", "__t5"};
    if (!b->scratch[i])
        b->scratch[i] = b_cell(b, name[i]);
    return b->scratch[i];
}

void b_input(Builder *b, uint32_t cell)
{
    GROW(b->in, b->nin, b->capin);
    b->in[b->nin++] = cell;
}

void b_output(Builder *b, uint32_t cell)
{
    GROW(b->out, b->nout, b->capout);
    b->out[b->nout++] = cell;
}

void b_reg(Builder *b, uint32_t q, uint32_t d)
{
    if (b->nreg == b->capreg) {
        b->capreg = b->capreg ? 2 * b->capreg : 64;
        b->rq = xrealloc(b->rq, b->capreg * sizeof *b->rq);
        b->rd = xrealloc(b->rd, b->capreg * sizeof *b->rd);
    }
    b->rq[b->nreg] = q;
    b->rd[b->nreg++] = d;
}

/* ---- the two instructions, and a few runs of them ----------------------------------------- */

static void emit(Builder *b, int op, uint32_t cell)
{
    assert(cell < b->ncells &&
           (op == OP_NAND || (cell >= CELL_FIRST_FREE && b->info[cell].kind == CK_PLAIN)));
    GROW(b->code, b->len, b->cap);
    b->code[b->len].op = (uint8_t)op;
    b->code[b->len].addr = cell;
    b->len++;
}

static void nand(Builder *b, uint32_t c) { emit(b, OP_NAND, c); }
static void sto(Builder *b, uint32_t c) { emit(b, OP_STO, c); }
static void ld(Builder *b, uint32_t c) { nand(b, CELL_ZERO); nand(b, c); nand(b, CELL_ONE); }
static void ldc(Builder *b, uint32_t c) { nand(b, CELL_ZERO); nand(b, c); }

/* RR = x ^ y in 7 instructions, the fewest possible (alpha super); uses scratch 0 */
static void xor2(Builder *b, uint32_t x, uint32_t y)
{
    uint32_t t = scratch(b, 0);
    nand(b, CELL_ZERO);
    nand(b, x);  /* !x */
    nand(b, y);  /* x | !y */
    sto(b, t);
    nand(b, y);  /* !(x & y) */
    nand(b, x);  /* !x | y */
    nand(b, t);  /* !((!x | y) & (x | !y)) = x ^ y */
}

void b_gate(Builder *b, int type, uint32_t out, const uint32_t *in, int n)
{
    assert(n >= 1 && out >= CELL_FIRST_FREE);
    switch (type) {
    case G_BUF:
        ld(b, in[0]);
        break;
    case G_NOT:
        ldc(b, in[0]);
        break;
    case G_AND:
    case G_NAND:
        ld(b, in[0]);
        for (int i = 1; i < n; i++) {
            nand(b, in[i]);  /* RR = !(in0 & .. & in i) */
            if (i < n - 1)
                nand(b, CELL_ONE);
        }
        if ((n == 1) == (type == G_NAND))  /* one input: RR = in0; more: RR = NAND of all */
            nand(b, CELL_ONE);
        break;
    case G_OR:
    case G_NOR: {
        uint32_t t = scratch(b, 0);
        ldc(b, in[0]);  /* RR = !in0 */
        for (int i = 1; i < n; i++) {
            if (i > 1)
                nand(b, CELL_ONE);  /* !(in0 | .. | in i-1) */
            sto(b, t);
            nand(b, in[i]);  /* (in0 | ..) | !in i */
            nand(b, t);      /* in0 | .. | in i: two inputs in 5, the fewest possible */
        }
        if ((n == 1) == (type == G_OR))  /* one input: RR = !in0; more: RR = OR of all */
            nand(b, CELL_ONE);
        break;
    }
    case G_XOR:
    case G_XNOR:
        if (n == 1) {
            ld(b, in[0]);
        } else {
            uint32_t acc = scratch(b, 2);
            xor2(b, in[0], in[1]);
            for (int i = 2; i < n; i++) {
                sto(b, acc);
                xor2(b, acc, in[i]);
            }
        }
        if (type == G_XNOR)
            nand(b, CELL_ONE);
        break;
    default:
        assert(0);
    }
    sto(b, out);
}

/* 8 instructions, the fewest possible (alpha super) */
void b_mux(Builder *b, uint32_t out, uint32_t s, uint32_t ns, uint32_t a1, uint32_t a0)
{
    assert(out >= CELL_FIRST_FREE);
    uint32_t t = scratch(b, 0);
    nand(b, CELL_ZERO);
    nand(b, s);   /* !s */
    nand(b, a0);  /* t = s | !a0 */
    sto(b, t);
    nand(b, ns);  /* s | a0 */
    nand(b, a1);  /* !((s | a0) & a1) */
    nand(b, t);   /* (s | a0) & a1 | !s & a0 = s ? a1 : a0 */
    sto(b, out);
}

/* 11 instructions, the fewest possible (alpha super) */
void b_half_adder(Builder *b, uint32_t s, uint32_t k, uint32_t x, uint32_t y)
{
    assert(s >= CELL_FIRST_FREE && k >= CELL_FIRST_FREE);
    uint32_t t = scratch(b, 0);
    nand(b, CELL_ZERO);
    nand(b, x);         /* !x */
    nand(b, y);         /* t = x | !y */
    sto(b, t);
    nand(b, y);         /* !(x & y) */
    nand(b, CELL_ONE);
    sto(b, k);          /* x & y */
    nand(b, CELL_ONE);
    nand(b, x);         /* !x | y */
    nand(b, t);         /* x ^ y */
    sto(b, s);
}

/* 19 instructions, the fewest possible (alpha super; the gates took 45) */
void b_full_adder(Builder *b, uint32_t s, uint32_t k, uint32_t x, uint32_t y, uint32_t c)
{
    assert(s >= CELL_FIRST_FREE && k >= CELL_FIRST_FREE);
    uint32_t nx = scratch(b, 0), n = scratch(b, 1), o = scratch(b, 2), q = scratch(b, 3);
    nand(b, CELL_ZERO);
    nand(b, x);         /* !x */
    sto(b, nx);
    nand(b, CELL_ONE);
    nand(b, y);         /* n = !(x & y) */
    sto(b, n);
    nand(b, y);
    nand(b, nx);        /* x | y */
    sto(b, o);
    nand(b, n);         /* !(x ^ y) */
    nand(b, c);         /* q = (x ^ y) | !c */
    sto(b, q);
    nand(b, c);         /* !((x ^ y) & c) */
    nand(b, n);         /* carry = (x ^ y) & c | x & y */
    sto(b, k);
    nand(b, CELL_ONE);
    nand(b, o);
    nand(b, q);         /* x ^ y ^ c */
    sto(b, s);
}

void b_select(Builder *b, uint32_t out, const uint32_t *sel, const uint32_t *val, int n)
{
    assert(out >= CELL_FIRST_FREE);
    uint32_t acc = scratch(b, 0);
    int terms = 0;
    for (int i = 0; i < n; i++) {
        if (val[i] == CELL_ZERO)
            continue;
        if (val[i] == CELL_ONE) {
            ldc(b, sel[i]);  /* RR = !(sel & 1) */
        } else {
            ld(b, sel[i]);
            nand(b, val[i]);  /* RR = !(sel & val) */
        }
        if (terms++) {
            nand(b, acc);
            nand(b, CELL_ONE);  /* RR = acc & !(sel & val) */
        }
        sto(b, acc);  /* acc = no term so far is 1 */
    }
    if (terms)
        ldc(b, acc);
    else
        ld(b, CELL_ZERO);
    sto(b, out);
}

void b_gate2(Builder *b, int type, uint32_t out, uint32_t x, uint32_t y)
{
    uint32_t in[2] = {x, y};
    b_gate(b, type, out, in, 2);
}

size_t b_len(const Builder *b)
{
    return b->len;
}

/* Registers whose input is another register's output go through a staging cell first, so a
   chain of registers (a shift register) moves by one place, not all the way through. */
void b_finish(Builder *b, Program *p)
{
    unsigned char *is_q = calloc(b->ncells + 1, 1);
    for (size_t r = 0; r < b->nreg; r++)
        is_q[b->rq[r]] = 1;
    uint32_t *stage = xrealloc(NULL, (b->nreg + 1) * sizeof *stage);
    for (size_t r = 0; r < b->nreg; r++) {
        stage[r] = 0;
        if (is_q[b->rd[r]]) {
            char name[32];
            snprintf(name, sizeof name, "__r%zu", r);
            stage[r] = b_cell(b, name);
            ld(b, b->rd[r]);
            sto(b, stage[r]);
        }
    }
    for (size_t r = 0; r < b->nreg; r++) {
        if (!stage[r]) {
            ld(b, b->rd[r]);
            sto(b, b->rq[r]);
        }
    }
    for (size_t r = 0; r < b->nreg; r++) {
        if (stage[r]) {
            ld(b, stage[r]);
            sto(b, b->rq[r]);
        }
    }
    free(stage);
    free(is_q);

    memset(p, 0, sizeof *p);
    p->code = b->code;
    p->len = b->len;
    p->ncells = b->ncells;
    p->cell_names = b->names;
    p->inputs = b->in;
    p->ninputs = b->nin;
    p->outputs = b->out;
    p->noutputs = b->nout;
    if (b->special) {
        p->special = b->info;
    } else {
        free(b->info);
        p->special = NULL;
    }
    free(b->rq);
    free(b->rd);
    free(b->slot);
    free(b);
}

static const char *const names[G_COUNT] = {"BUF", "NOT", "AND", "NAND", "OR", "NOR", "XOR", "XNOR"};

const char *gate_name(int type)
{
    return type >= 0 && type < G_COUNT ? names[type] : "?";
}

int gate_type(const char *name)
{
    if (same(name, "BUFF"))
        return G_BUF;
    for (int t = 0; t < G_COUNT; t++)
        if (same(name, names[t]))
            return t;
    return -1;
}

static void print_list(FILE *f, const char *directive, const Program *p, const uint32_t *cells,
                       size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i % 16 == 0)
            fprintf(f, "%s%s", i ? "\n" : "", directive);
        else
            fputc(' ', f);
        fputs(p->cell_names[cells[i]], f);
    }
    if (n)
        fputc('\n', f);
}

void program_print(FILE *f, const Program *p)
{
    print_list(f, ".in  ", p, p->inputs, p->ninputs);
    print_list(f, ".out ", p, p->outputs, p->noutputs);
    for (uint32_t c = 0; p->special && c < p->ncells; c++)  /* a link only reads older cells */
        if (p->special[c].kind == CK_LINK)
            fprintf(f, ".link %s = %s @ %u\n", p->cell_names[c], p->cell_names[p->special[c].src],
                    p->special[c].dim);
    fputc('\n', f);
    for (size_t i = 0; i < p->len; i++)
        fprintf(f, "    %-4s %s\n", p->code[i].op == OP_NAND ? "NAND" : "STO",
                p->cell_names[p->code[i].addr]);
}
