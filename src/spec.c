#include "spec.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_NUM, T_VAR, T_ADD, T_SUB, T_MUL, T_AND, T_OR, T_XOR, T_SHL, T_SHR, T_NOT, T_NEG };

#define MAX_VARS  32
#define MAX_RPN   256
#define MAX_STACK 64

typedef struct {
    int      op;
    uint64_t v;  /* number, or variable index */
} Item;

struct Spec {
    int      width;
    uint32_t out[64];
    int      nvars;
    char     var_name[MAX_VARS][64];
    int      var_bits[MAX_VARS];
    int      var_pos[MAX_VARS][64];  /* bit of the assignment x for each bit of the vector */
    Item     rpn[MAX_RPN];
    int      nrpn, depth, max_depth;
};

typedef struct {
    const char    *s;
    const Program *p;
    Spec          *spec;
    int            inputs_allowed;
    char          *err;
    size_t         errlen;
    int            failed;
} Parser;

static int same_nocase(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

int vec_resolve(const Program *p, const uint32_t *list, size_t n, const char *name, int *idx)
{
    size_t len = strlen(name);
    for (size_t i = 0; i < n; i++) {
        const char *c = p->cell_names[list[i]];
        if (strlen(c) == len && same_nocase(c, name, len)) {
            idx[0] = (int)i;
            return 1;
        }
    }
    int width = 0;
    unsigned long long seen = 0;
    for (size_t i = 0; i < n; i++) {
        const char *c = p->cell_names[list[i]];
        if (strlen(c) <= len || !same_nocase(c, name, len))
            continue;
        const char *d = c + len;
        if ((d[0] == '0' && d[1]) || !isdigit((unsigned char)d[0]))
            continue;
        char *end;
        unsigned long bit = strtoul(d, &end, 10);
        if (*end)
            continue;
        if (bit >= 64)
            return -1;
        idx[bit] = (int)i;
        seen |= 1ull << bit;
        if ((int)bit + 1 > width)
            width = (int)bit + 1;
    }
    if (width && seen != (width == 64 ? ~0ull : (1ull << width) - 1))
        return -1;
    return width;
}

void vec_format(const Program *p, const uint32_t *list, size_t n, const uint8_t *val, char *buf,
                size_t len)
{
    size_t at = 0;
    buf[0] = '\0';
    unsigned char *done = calloc(n + 1, 1);
    for (size_t i = 0; i < n && at < len; i++) {
        if (done[i])
            continue;
        const char *c = p->cell_names[list[i]];
        size_t base = strlen(c);
        while (base > 0 && isdigit((unsigned char)c[base - 1]))
            base--;
        char name[64];
        int idx[64], w = 0;
        if (base > 0 && base < strlen(c) && base < sizeof name) {
            memcpy(name, c, base);
            name[base] = '\0';
            w = vec_resolve(p, list, n, name, idx);  /* 1 if a cell is called exactly `name` */
        }
        if (w > 1) {
            unsigned long long v = 0;
            for (int b = 0; b < w; b++) {
                v |= (unsigned long long)val[idx[b]] << b;
                done[idx[b]] = 1;
            }
            at += (size_t)snprintf(buf + at, len - at, "%s%s=0x%llx", at ? " " : "", name, v);
        } else {
            done[i] = 1;
            at += (size_t)snprintf(buf + at, len - at, "%s%s=%u", at ? " " : "", c, val[i]);
        }
    }
    free(done);
}

/* ---- parser ------------------------------------------------------------------------------- */

static void fail(Parser *ps, const char *fmt, ...)
{
    if (ps->failed)
        return;
    ps->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->err, ps->errlen, fmt, ap);
    va_end(ap);
}

static void skip(Parser *ps)
{
    while (*ps->s == ' ' || *ps->s == '\t')
        ps->s++;
}

static void push(Parser *ps, int op, uint64_t v)
{
    Spec *s = ps->spec;
    if (s->nrpn == MAX_RPN) {
        fail(ps, "the expression is too long");
        return;
    }
    s->rpn[s->nrpn].op = op;
    s->rpn[s->nrpn++].v = v;
    if (op == T_NUM || op == T_VAR)
        s->depth++;
    else if (op != T_NOT && op != T_NEG)
        s->depth--;
    if (s->depth > s->max_depth)
        s->max_depth = s->depth;
    if (s->max_depth > MAX_STACK)
        fail(ps, "the expression is nested too deeply");
}

static int read_name(Parser *ps, char *buf, size_t len)
{
    size_t n = 0;
    if (!isalpha((unsigned char)*ps->s) && *ps->s != '_')
        return 0;
    while (isalnum((unsigned char)*ps->s) || *ps->s == '_') {
        if (n + 1 < len)
            buf[n++] = *ps->s;
        ps->s++;
    }
    buf[n] = '\0';
    return 1;
}

static int variable(Parser *ps, const char *name)
{
    Spec *s = ps->spec;
    for (int k = 0; k < s->nvars; k++)
        if (strlen(s->var_name[k]) == strlen(name) &&
            same_nocase(s->var_name[k], name, strlen(name)))
            return k;
    if (!ps->inputs_allowed) {
        fail(ps, "'%s': the right side must be a number here", name);
        return -1;
    }
    if (s->nvars == MAX_VARS) {
        fail(ps, "too many input vectors");
        return -1;
    }
    int idx[64], w = vec_resolve(ps->p, ps->p->inputs, ps->p->ninputs, name, idx);
    if (w <= 0) {
        fail(ps, w < 0 ? "input vector %s has missing bits or more than 64" : "no input called %s",
             name);
        return -1;
    }
    int k = s->nvars++;
    snprintf(s->var_name[k], sizeof s->var_name[k], "%s", name);
    s->var_bits[k] = w;
    for (int b = 0; b < w; b++)
        s->var_pos[k][b] = (int)ps->p->ninputs - 1 - idx[b];
    return k;
}

static void expr(Parser *ps, int min_prec);

static void primary(Parser *ps)
{
    skip(ps);
    char name[64];
    if (*ps->s == '(') {
        ps->s++;
        expr(ps, 0);
        skip(ps);
        if (*ps->s != ')')
            fail(ps, "expected )");
        else
            ps->s++;
    } else if (isdigit((unsigned char)*ps->s)) {
        char *end;
        unsigned long long v = strtoull(ps->s, &end, 0);
        ps->s = end;
        push(ps, T_NUM, v);
    } else if (read_name(ps, name, sizeof name)) {
        int k = variable(ps, name);
        if (k >= 0)
            push(ps, T_VAR, (uint64_t)k);
    } else {
        fail(ps, *ps->s ? "unexpected '%c'" : "the expression ends too early", *ps->s);
    }
}

static void unary(Parser *ps)
{
    skip(ps);
    if (*ps->s == '~' || *ps->s == '-') {
        int op = *ps->s == '~' ? T_NOT : T_NEG;
        ps->s++;
        unary(ps);
        push(ps, op, 0);
    } else {
        primary(ps);
    }
}

/* Binary operator at s: its op, precedence (higher binds tighter) and length; 0 if none. */
static int binop(const char *s, int *prec, int *len)
{
    *len = 1;
    switch (*s) {
    case '|': *prec = 1; return T_OR;
    case '^': *prec = 2; return T_XOR;
    case '&': *prec = 3; return T_AND;
    case '<': *len = 2; *prec = 4; return s[1] == '<' ? T_SHL : 0;
    case '>': *len = 2; *prec = 4; return s[1] == '>' ? T_SHR : 0;
    case '+': *prec = 5; return T_ADD;
    case '-': *prec = 5; return T_SUB;
    case '*': *prec = 6; return T_MUL;
    default:  return 0;
    }
}

static void expr(Parser *ps, int min_prec)
{
    unary(ps);
    while (!ps->failed) {
        skip(ps);
        int prec, len, op = binop(ps->s, &prec, &len);
        if (!op || prec < min_prec)
            break;
        ps->s += len;
        expr(ps, prec + 1);
        push(ps, op, 0);
    }
}

Spec *spec_parse(const char *text, const Program *p, int inputs_allowed, char *err, size_t errlen)
{
    Spec *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    Parser ps = {text, p, s, inputs_allowed, err, errlen, 0};

    /* left side: output vectors joined with ':', the first one highest */
    int outs[64][64], widths[64], nout = 0, total = 0;
    for (;;) {
        char name[64];
        skip(&ps);
        if (!read_name(&ps, name, sizeof name)) {
            fail(&ps, "expected an output name before =");
            break;
        }
        int w = vec_resolve(p, p->outputs, p->noutputs, name, outs[nout]);
        if (w <= 0) {
            fail(&ps, w < 0 ? "output vector %s has missing bits or more than 64"
                            : "no output called %s", name);
            break;
        }
        widths[nout++] = w;
        total += w;
        skip(&ps);
        if (*ps.s != ':' || nout == 64)
            break;
        ps.s++;
    }
    if (!ps.failed && total > 64)
        fail(&ps, "the left side has %d bits, at most 64 work", total);
    if (!ps.failed) {
        for (int k = nout - 1; k >= 0; k--)
            for (int b = 0; b < widths[k]; b++)
                s->out[s->width++] = p->outputs[outs[k][b]];
        skip(&ps);
        if (*ps.s != '=')
            fail(&ps, "expected = after the outputs");
        else
            ps.s++;
    }
    if (!ps.failed)
        expr(&ps, 0);
    if (!ps.failed) {
        skip(&ps);
        if (*ps.s)
            fail(&ps, "unexpected '%c'", *ps.s);
    }
    if (ps.failed) {
        free(s);
        return NULL;
    }
    return s;
}

void spec_free(Spec *s)
{
    free(s);
}

int spec_width(const Spec *s)
{
    return s->width;
}

uint32_t spec_out_cell(const Spec *s, int bit)
{
    return s->out[bit];
}

uint64_t spec_value(const Spec *s, uint64_t x)
{
    uint64_t var[MAX_VARS], st[MAX_STACK];
    for (int k = 0; k < s->nvars; k++) {
        uint64_t v = 0;
        for (int b = 0; b < s->var_bits[k]; b++)
            v |= ((x >> s->var_pos[k][b]) & 1) << b;
        var[k] = v;
    }
    int sp = 0;
    for (int i = 0; i < s->nrpn; i++) {
        const Item *it = &s->rpn[i];
        uint64_t b;
        switch (it->op) {
        case T_NUM: st[sp++] = it->v; break;
        case T_VAR: st[sp++] = var[it->v]; break;
        case T_NOT: st[sp - 1] = ~st[sp - 1]; break;
        case T_NEG: st[sp - 1] = 0 - st[sp - 1]; break;
        default:
            b = st[--sp];
            switch (it->op) {
            case T_ADD: st[sp - 1] += b; break;
            case T_SUB: st[sp - 1] -= b; break;
            case T_MUL: st[sp - 1] *= b; break;
            case T_AND: st[sp - 1] &= b; break;
            case T_OR:  st[sp - 1] |= b; break;
            case T_XOR: st[sp - 1] ^= b; break;
            case T_SHL: st[sp - 1] = b < 64 ? st[sp - 1] << b : 0; break;
            case T_SHR: st[sp - 1] = b < 64 ? st[sp - 1] >> b : 0; break;
            }
        }
    }
    return s->width == 64 ? st[0] : st[0] & ((1ull << s->width) - 1);
}
