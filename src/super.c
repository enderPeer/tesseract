#include "super.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "isa.h"
#include "platform.h"

/* ---- the specification -------------------------------------------------------------------- */

typedef struct {
    const char *s;
    SuperSpec  *sp;
    char       *err;
    size_t      errlen;
    int         failed;
    char        names[SUPER_CELLS + 1][SUPER_NAME];  /* the variables, in order */
    int         nnames;
} Parser;

static void perr(Parser *p, const char *fmt, ...)
{
    if (p->failed)
        return;
    p->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, p->errlen, fmt, ap);
    va_end(ap);
}

static const char *skipws(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    return s;
}

static int read_name(const char **s, char *out)
{
    size_t n = 0;
    *s = skipws(*s);
    if (!isalpha((unsigned char)**s) && **s != '_')
        return 0;
    while ((isalnum((unsigned char)**s) || **s == '_') && n + 1 < SUPER_NAME)
        out[n++] = *(*s)++;
    out[n] = '\0';
    return 1;
}

static int is_rr(const char *name)
{
    return (name[0] == 'R' || name[0] == 'r') && (name[1] == 'R' || name[1] == 'r') && !name[2];
}

/* The truth table of variable i over n variables: row r has bit i of r. */
static uint64_t var_table(int i)
{
    uint64_t t = 0;
    for (int r = 0; r < 64; r++)
        if ((r >> i) & 1)
            t |= 1ull << r;
    return t;
}

static int find_cell(const SuperSpec *sp, const char *name)
{
    for (int i = 0; i < sp->ncells; i++)
        if (!strcmp(sp->cell[i].name, name))
            return i;
    return -1;
}

/* Pass 1 notes the names; pass 2 (tables != NULL) evaluates. */
static uint64_t expr(Parser *p, int pass);

static uint64_t primary(Parser *p, int pass)
{
    char name[SUPER_NAME];
    p->s = skipws(p->s);
    if (*p->s == '(') {
        p->s++;
        uint64_t v = expr(p, pass);
        p->s = skipws(p->s);
        if (*p->s++ != ')')
            perr(p, "expected )");
        return v;
    }
    if (*p->s == '0' || *p->s == '1') {
        uint64_t v = *p->s++ == '1' ? ~0ull : 0;
        return v;
    }
    if (*p->s == '!' || *p->s == '~') {
        p->s++;
        return ~primary(p, pass);
    }
    if (!read_name(&p->s, name)) {
        perr(p, "expected a name, 0, 1 or ( at \"%.20s\"", p->s);
        return 0;
    }
    if (pass == 1) {
        for (int i = 0; i < p->nnames; i++)
            if (!strcmp(p->names[i], name))
                return 0;
        if (p->nnames == SUPER_CELLS + 1) {
            perr(p, "too many names");
            return 0;
        }
        snprintf(p->names[p->nnames++], SUPER_NAME, "%s", name);
        return 0;
    }
    if (is_rr(name))
        return var_table(0);
    int c = find_cell(p->sp, name);
    if (c < 0) {
        perr(p, "unknown name %s", name);
        return 0;
    }
    return p->sp->cell[c].init;
}

static uint64_t conj(Parser *p, int pass)
{
    uint64_t v = primary(p, pass);
    for (p->s = skipws(p->s); *p->s == '&'; p->s = skipws(p->s)) {
        p->s++;
        v &= primary(p, pass);
    }
    return v;
}

static uint64_t exor(Parser *p, int pass)
{
    uint64_t v = conj(p, pass);
    for (p->s = skipws(p->s); *p->s == '^'; p->s = skipws(p->s)) {
        p->s++;
        v ^= conj(p, pass);
    }
    return v;
}

static uint64_t disj(Parser *p, int pass)
{
    uint64_t v = exor(p, pass);
    for (p->s = skipws(p->s); *p->s == '|'; p->s = skipws(p->s)) {
        p->s++;
        v |= exor(p, pass);
    }
    return v;
}

static uint64_t expr(Parser *p, int pass)
{
    uint64_t c = disj(p, pass);
    p->s = skipws(p->s);
    if (*p->s != '?')
        return c;
    p->s++;
    uint64_t a = expr(p, pass);
    p->s = skipws(p->s);
    if (*p->s++ != ':')
        perr(p, "expected : in ?:");
    uint64_t b = expr(p, pass);
    return (c & a) | (~c & b);
}

typedef struct {
    char        lhs[SUPER_NAME];
    char        alias[4][SUPER_NAME];
    int         nalias, given;
    const char *rhs;
} Stmt;

/* f with variables i and j swapped */
static uint64_t swap_vars(uint64_t f, int i, int j, int n)
{
    uint64_t g = 0;
    for (int r = 0; r < 1 << n; r++) {
        int bi = (r >> i) & 1, bj = (r >> j) & 1;
        int q = (r & ~(1 << i) & ~(1 << j)) | bj << i | bi << j;
        if ((f >> r) & 1)
            g |= 1ull << q;
    }
    return g;
}

static void find_symmetry(SuperSpec *sp, const int *var_of)
{
    for (int i = 0; i < sp->ncells; i++)
        sp->cell[i].sym_prev = -1;
    for (int i = 0; i < sp->ncells; i++) {
        if (!sp->cell[i].in || sp->cell[i].out || var_of[i] < 0)
            continue;
        for (int j = i + 1; j < sp->ncells; j++) {
            if (!sp->cell[j].in || sp->cell[j].out || var_of[j] < 0 || sp->cell[j].sym_prev >= 0)
                continue;
            int vi = var_of[i], vj = var_of[j], ok = 1;
            if (sp->rr_out && swap_vars(sp->rr_target, vi, vj, sp->nvars) != sp->rr_target)
                ok = 0;
            for (int c = 0; c < sp->ncells && ok; c++) {
                const uint64_t m = sp->mask;
                if (sp->cell[c].out &&
                    (swap_vars(sp->cell[c].target, vi, vj, sp->nvars) & m) != (sp->cell[c].target & m))
                    ok = 0;
                if (sp->cell[c].in && var_of[c] < 0 &&  /* given cells: no symmetry with them */
                    (swap_vars(sp->cell[c].init, vi, vj, sp->nvars) & m) != (sp->cell[c].init & m))
                    ok = 0;
                uint32_t a = sp->cell[c].alias, bi = 1u << i, bj = 1u << j;
                if (((a & bi) != 0) != ((a & bj) != 0))
                    ok = 0;
            }
            if (ok) {  /* j joins i's group, after the last member so far */
                int last = i;
                for (int k = 0; k < sp->ncells; k++)
                    if (sp->cell[k].sym_prev == last)
                        last = k, k = -1;
                sp->cell[j].sym_prev = last;
            }
        }
    }
}

int super_parse(const char *text, SuperSpec *sp, char *err, size_t errlen)
{
    Parser p;
    memset(&p, 0, sizeof p);
    memset(sp, 0, sizeof *sp);
    p.sp = sp;
    p.err = err;
    p.errlen = errlen;
    err[0] = '\0';
    Stmt st[SUPER_CELLS];
    int nst = 0;
    /* statements: name [{a,b}] (= | :=) expression ; ... */
    for (const char *s = text; *skipws(s) && !p.failed;) {
        if (nst == SUPER_CELLS) {
            perr(&p, "too many statements");
            break;
        }
        Stmt *t = &st[nst++];
        memset(t, 0, sizeof *t);
        if (!read_name(&s, t->lhs)) {
            perr(&p, "expected a name at \"%.20s\"", s);
            break;
        }
        s = skipws(s);
        if (*s == '{') {
            s++;
            for (;;) {
                if (t->nalias == 4 || !read_name(&s, t->alias[t->nalias++])) {
                    perr(&p, "expected names in { }");
                    break;
                }
                s = skipws(s);
                if (*s == ',') {
                    s++;
                    continue;
                }
                if (*s++ != '}')
                    perr(&p, "expected }");
                break;
            }
            s = skipws(s);
        }
        if (s[0] == ':' && s[1] == '=') {
            t->given = 1;
            s += 2;
        } else if (*s == '=') {
            s++;
        } else {
            perr(&p, "expected = after %s", t->lhs);
            break;
        }
        t->rhs = s;
        p.s = s;
        expr(&p, 1);
        s = skipws(p.s);
        if (*s == ';')
            s++;
        else if (*s)
            perr(&p, "unexpected \"%.20s\"", s);
    }
    if (p.failed)
        return 0;

    /* variables: RR first if it is read, then the names read, in order; given cells are not */
    int var_of[SUPER_CELLS];
    for (int i = 0; i < SUPER_CELLS; i++)
        var_of[i] = -1;
    for (int i = 0; i < p.nnames; i++)
        if (is_rr(p.names[i]))
            sp->rr_in = 1;
    sp->nvars = sp->rr_in;
    for (int i = 0; i < p.nnames && !p.failed; i++) {
        if (is_rr(p.names[i]))
            continue;
        int given = 0;
        for (int k = 0; k < nst; k++)
            if (st[k].given && !strcmp(st[k].lhs, p.names[i]))
                given = 1;
        if (given)
            continue;
        if (sp->nvars == SUPER_VARS) {
            perr(&p, "more than %d inputs", SUPER_VARS);
            break;
        }
        int c = sp->ncells++;
        snprintf(sp->cell[c].name, SUPER_NAME, "%s", p.names[i]);
        sp->cell[c].in = 1;
        var_of[c] = sp->nvars;
        sp->cell[c].init = var_table(sp->nvars++);
    }
    sp->mask = sp->nvars == 6 ? ~0ull : (1ull << (1 << sp->nvars)) - 1;
    /* given cells, then outputs */
    for (int pass = 0; pass < 2 && !p.failed; pass++)
        for (int k = 0; k < nst && !p.failed; k++) {
            Stmt *t = &st[k];
            if (t->given != (pass == 0))
                continue;
            p.s = t->rhs;
            uint64_t v = expr(&p, 2) & sp->mask;
            if (is_rr(t->lhs)) {
                if (t->given || sp->rr_out || t->nalias)
                    perr(&p, "RR is set once, with =");
                sp->rr_out = 1;
                sp->rr_target = v;
                continue;
            }
            int c = find_cell(sp, t->lhs);
            if (t->given) {
                if (c >= 0) {
                    perr(&p, "%s is defined twice", t->lhs);
                    break;
                }
                c = sp->ncells++;
                snprintf(sp->cell[c].name, SUPER_NAME, "%s", t->lhs);
                sp->cell[c].in = 1;
                sp->cell[c].init = v;
                continue;
            }
            if (c >= 0 && sp->cell[c].out) {
                perr(&p, "%s is set twice", t->lhs);
                break;
            }
            if (c < 0) {
                if (sp->ncells == SUPER_CELLS) {
                    perr(&p, "too many cells");
                    break;
                }
                c = sp->ncells++;
                snprintf(sp->cell[c].name, SUPER_NAME, "%s", t->lhs);
            }
            if (var_of[c] < 0 && sp->cell[c].in) {
                perr(&p, "%s is a given cell", t->lhs);
                break;
            }
            sp->cell[c].out = 1;
            sp->cell[c].target = v;
            for (int a = 0; a < t->nalias; a++) {
                int ac = find_cell(sp, t->alias[a]);
                if (ac < 0 || !sp->cell[ac].in || ac == c)
                    perr(&p, "%s{%s}: %s must be another input", t->lhs, t->alias[a], t->alias[a]);
                else
                    sp->cell[c].alias |= 1u << ac;
            }
        }
    for (int c = 0; c < sp->ncells && !p.failed; c++)
        if (sp->cell[c].out)
            for (int a = 0; a < sp->ncells; a++)
                if (((sp->cell[c].alias >> a) & 1) && sp->cell[a].out)
                    perr(&p, "%s may share a cell only with inputs that are not written", sp->cell[c].name);
    if (!p.failed && !sp->rr_out) {
        int any = 0;
        for (int c = 0; c < sp->ncells; c++)
            any |= sp->cell[c].out;
        if (!any)
            perr(&p, "nothing to compute: no RR = ... and no cell = ...");
    }
    if (p.failed)
        return 0;
    find_symmetry(sp, var_of);
    return 1;
}

/* ---- the search --------------------------------------------------------------------------- */

/* A program is a chain: hist[0] is RR at the start (if it is an input), and NAND i makes
   hist[i] = !(hist[i-1] & operand). STO only stores the current RR, so where a value is needed
   later it is stored right after it appears: lazily, when a later NAND first reads it, which
   costs one STO then. Scratch cells are never overwritten (there are as many as needed), outputs
   are written once, with their final value. */
enum { K_ZERO, K_ONE, K_CELL, K_TEMP, K_LAZY };

typedef struct {
    uint64_t v;
    uint8_t  kind, extra;
    int8_t   ref;
} Cand;

typedef struct {
    uint8_t cand, mask;
} Choice;

enum { GEN_DEPTH = 6 };

/* The transposition table: states already searched in full, with how many more instructions
   were allowed then. The same state is reached in many orders (independent steps swapped), and
   its future depends only on: RR, which cells can be read and are written, the symmetric inputs
   used so far, and the values a NAND could read with what each costs. 128-bit keys: a false
   match, which could hide a program, is out of the question in practice. */
typedef struct {
    uint64_t k1, k2;
    int32_t  rem;
} TTEntry;

enum { TT_BITS = 19 };

typedef struct {
    Choice c[GEN_DEPTH];
    uint8_t len, root;
} Path;

typedef struct {
    const SuperSpec *sp;
    int              budget, n, cost, def0;
    uint64_t         hist[SUPER_LEN + 1];
    uint8_t          stored[SUPER_LEN + 1];
    uint8_t          op_kind[SUPER_LEN + 1];
    int8_t           op_ref[SUPER_LEN + 1];
    int8_t           out_at[SUPER_CELLS];
    uint32_t         readable, written, used;
    uint64_t         nodes;
    volatile int    *stop;
    TTEntry         *tt;  /* per thread, kept from budget to budget; NULL: none */
    /* frontier generation */
    int              gen;  /* depth to stop at, or 0 */
    Path             path;
    Path            *paths;
    size_t           npaths, cappaths;
    /* the program found */
    int              found;
    struct {
        int      n, def0;
        uint64_t hist[SUPER_LEN + 1];
        uint8_t  stored[SUPER_LEN + 1], op_kind[SUPER_LEN + 1];
        int8_t   op_ref[SUPER_LEN + 1], out_at[SUPER_CELLS];
        int      cost;
    } sol;
} Ctx;

static uint64_t cell_value(const Ctx *x, int c)
{
    return ((x->written >> c) & 1) ? x->sp->cell[c].target : x->sp->cell[c].init;
}

static int rr_def(const Ctx *x)
{
    return x->n > 0 || x->def0;
}

static int build(const Ctx *x, Cand *out)
{
    const SuperSpec *sp = x->sp;
    int n = 0;
    out[n++] = (Cand){0, K_ZERO, 0, 0};
    out[n++] = (Cand){sp->mask, K_ONE, 0, 0};
    for (int c = 0; c < sp->ncells; c++) {
        if (!((x->readable >> c) & 1))
            continue;
        int prev = sp->cell[c].sym_prev;
        if (prev >= 0 && !((x->used >> prev) & 1))
            continue;  /* symmetry: read the interchangeable inputs in order */
        out[n++] = (Cand){cell_value(x, c), K_CELL, 0, (int8_t)c};
    }
    for (int j = x->def0 ? 0 : 1; j <= x->n; j++)
        out[n++] = (Cand){x->hist[j], x->stored[j] ? K_TEMP : K_LAZY, (uint8_t)!x->stored[j],
                          (int8_t)j};
    /* one candidate per value, the cheapest */
    int m = 0;
    for (int i = 0; i < n; i++) {
        int k;
        for (k = 0; k < m && out[k].v != out[i].v; k++)
            ;
        if (k == m)
            out[m++] = out[i];
        else if (out[i].extra < out[k].extra)
            out[k] = out[i];
    }
    return m;
}

/* the fewest instructions still needed */
static int lower_bound(const Ctx *x)
{
    const SuperSpec *sp = x->sp;
    uint64_t need[SUPER_CELLS + 1];
    int nneed = 0, stores = 0;
    uint64_t rr = x->hist[x->n];
    int def = rr_def(x);
    for (int c = 0; c < sp->ncells; c++) {
        if (!sp->cell[c].out || ((x->written >> c) & 1))
            continue;
        stores++;
        uint64_t t = sp->cell[c].target;
        int k;
        for (k = 0; k < nneed && need[k] != t; k++)
            ;
        if (k == nneed && !(def && t == rr))
            need[nneed++] = t;
    }
    int nands = nneed;
    if (sp->rr_out) {
        int k;
        for (k = 0; k < nneed && need[k] != sp->rr_target; k++)
            ;
        if (!(def && rr == sp->rr_target)) {
            if (k == nneed)
                nands++;
        } else if (nneed) {
            nands++;  /* RR has to change and come back */
        }
    }
    return stores + nands;
}

static int goal(const Ctx *x)
{
    const SuperSpec *sp = x->sp;
    for (int c = 0; c < sp->ncells; c++)
        if (sp->cell[c].out && !((x->written >> c) & 1))
            return 0;
    return !sp->rr_out || (rr_def(x) && x->hist[x->n] == sp->rr_target);
}

static void save(Ctx *x)
{
    x->found = 1;
    *x->stop = 1;
    x->sol.n = x->n;
    x->sol.def0 = x->def0;
    x->sol.cost = x->cost;
    memcpy(x->sol.hist, x->hist, sizeof x->hist);
    memcpy(x->sol.stored, x->stored, sizeof x->stored);
    memcpy(x->sol.op_kind, x->op_kind, sizeof x->op_kind);
    memcpy(x->sol.op_ref, x->op_ref, sizeof x->op_ref);
    memcpy(x->sol.out_at, x->out_at, sizeof x->out_at);
}

/* outputs that could take the current RR now */
static int hits(const Ctx *x, int *list)
{
    int k = 0;
    if (!rr_def(x))
        return 0;
    for (int c = 0; c < x->sp->ncells; c++)
        if (x->sp->cell[c].out && !((x->written >> c) & 1) && x->sp->cell[c].target == x->hist[x->n])
            list[k++] = c;
    return k;
}

typedef struct {
    uint32_t readable, written;
    int      cost;
} Undo;

static void write_outputs(Ctx *x, const int *list, int mask, Undo *u)
{
    u->readable = x->readable;
    u->written = x->written;
    u->cost = x->cost;
    for (int i = 0; (mask >> i) != 0; i++) {
        if (!((mask >> i) & 1))
            continue;
        int c = list[i];
        x->written |= 1u << c;
        x->out_at[c] = (int8_t)x->n;
        x->cost++;
        x->readable &= ~x->sp->cell[c].alias;  /* it may be one of those cells */
        x->readable |= 1u << c;
    }
}

static void unwrite(Ctx *x, const Undo *u)
{
    x->readable = u->readable;
    x->written = u->written;
    x->cost = u->cost;
}

static void push_path(Ctx *x)
{
    if (x->npaths == x->cappaths) {
        x->cappaths = x->cappaths ? 2 * x->cappaths : 1024;
        x->paths = realloc(x->paths, x->cappaths * sizeof *x->paths);
        if (!x->paths) {
            fputs("out of memory\n", stderr);
            exit(1);
        }
    }
    x->paths[x->npaths++] = x->path;
}

static void node(Ctx *x);

/* after NAND x->n: every way of writing outputs that equal RR now, then on */
static void after_nand(Ctx *x)
{
    int list[SUPER_CELLS], k = hits(x, list);
    for (int mask = 0; mask < 1 << k && !*x->stop; mask++) {
        Undo u;
        write_outputs(x, list, mask, &u);
        if (x->gen && x->n > 0)
            x->path.c[x->n - 1].mask = (uint8_t)mask;
        else if (x->gen)
            x->path.root = (uint8_t)mask;
        node(x);
        unwrite(x, &u);
    }
}

static uint64_t mix(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void state_key(const Ctx *x, const Cand *cand, int m, uint64_t *k1, uint64_t *k2)
{
    uint64_t a = mix(x->hist[x->n] ^ (uint64_t)rr_def(x) << 63), b = mix(a + 0x1234567);
    uint64_t f = (uint64_t)x->readable | (uint64_t)x->written << 21 | (uint64_t)x->used << 42;
    a += mix(f + 0x9E3779B97F4A7C15ull);
    b ^= mix(f * 3 + 77);
    for (int i = 0; i < m; i++) {  /* a set: the order does not matter */
        uint64_t v = cand[i].v * 2 + cand[i].extra;
        a += mix(v + 0x51ED27);
        b += mix(v * 0x2545F4914F6CDD1Dull + 0xABCDEF);
    }
    *k1 = a;
    *k2 = b;
}

static void node(Ctx *x)
{
    x->nodes++;
    if (x->cost > x->budget)
        return;
    if (goal(x)) {
        save(x);
        return;
    }
    if (x->cost + lower_bound(x) > x->budget || x->n == SUPER_LEN)
        return;
    if (x->gen && x->n == x->gen) {
        x->path.len = (uint8_t)x->n;
        push_path(x);
        return;
    }
    Cand cand[SUPER_CELLS + SUPER_LEN + 4];
    int m = build(x, cand), def = rr_def(x), rem = x->budget - x->cost;
    uint64_t k1 = 0, k2 = 0;
    TTEntry *e = NULL;
    if (x->tt && !x->gen && rem >= 3) {  /* small subtrees: cheaper to search again */
        state_key(x, cand, m, &k1, &k2);
        e = &x->tt[k1 & ((1u << TT_BITS) - 1)];
        if (e->k1 == k1 && e->k2 == k2 && e->rem >= rem)
            return;  /* searched before with at least this much left: nothing there */
    }
    uint64_t rr = x->hist[x->n], mask = x->sp->mask;
    for (int i = 0; i < m && !*x->stop; i++) {
        const Cand *c = &cand[i];
        if (!def && c->kind != K_ZERO)
            continue;  /* RR is garbage: only NAND ZERO gives something known */
        uint64_t r = ~(rr & c->v) & mask;
        if (def && r == rr)
            continue;  /* changes nothing */
        if (x->cost + 1 + c->extra > x->budget)
            continue;
        uint32_t used = x->used;
        if (c->kind == K_LAZY)
            x->stored[c->ref] = 1;
        if (c->kind == K_CELL)
            x->used |= 1u << c->ref;
        x->cost += 1 + c->extra;
        x->n++;
        x->hist[x->n] = r;
        x->stored[x->n] = 0;
        x->op_kind[x->n] = c->kind;
        x->op_ref[x->n] = c->ref;
        if (x->gen)
            x->path.c[x->n - 1].cand = (uint8_t)i;
        after_nand(x);
        x->n--;
        x->cost -= 1 + c->extra;
        x->used = used;
        if (c->kind == K_LAZY)
            x->stored[c->ref] = 0;
    }
    if (e && !*x->stop) {  /* searched in full, nothing found */
        e->k1 = k1;
        e->k2 = k2;
        e->rem = rem;
    }
}

static void reset(Ctx *x, const SuperSpec *sp, int budget)
{
    x->sp = sp;
    x->budget = budget;
    x->n = 0;
    x->cost = 0;
    x->def0 = sp->rr_in;
    x->hist[0] = sp->rr_in ? var_table(0) & sp->mask : 0;  /* RR is variable 0 */
    x->stored[0] = 0;
    x->readable = 0;
    x->written = 0;
    x->used = 0;
    for (int c = 0; c < sp->ncells; c++) {
        x->out_at[c] = -1;
        if (sp->cell[c].in)
            x->readable |= 1u << c;
    }
}

/* Follows a path from the root; returns 0 if it does not apply (it always should). */
static int replay(Ctx *x, const Path *p)
{
    int list[SUPER_CELLS], k = hits(x, list);
    Undo u;
    if (p->root >> k)
        return 0;
    write_outputs(x, list, p->root, &u);
    for (int s = 0; s < p->len; s++) {
        Cand cand[SUPER_CELLS + SUPER_LEN + 4];
        int m = build(x, cand);
        if (p->c[s].cand >= m)
            return 0;
        const Cand *c = &cand[p->c[s].cand];
        uint64_t r = ~(x->hist[x->n] & c->v) & x->sp->mask;
        if (c->kind == K_LAZY)
            x->stored[c->ref] = 1;
        if (c->kind == K_CELL)
            x->used |= 1u << c->ref;
        x->cost += 1 + c->extra;
        x->n++;
        x->hist[x->n] = r;
        x->stored[x->n] = 0;
        x->op_kind[x->n] = c->kind;
        x->op_ref[x->n] = c->ref;
        k = hits(x, list);
        if (p->c[s].mask >> k)
            return 0;
        write_outputs(x, list, p->c[s].mask, &u);
    }
    return 1;
}

typedef struct {
    const SuperSpec *sp;
    const Path      *paths;
    size_t           npaths;
    int              budget, part, parts;
    volatile long   *next;
    volatile int    *stop;
    Ctx             *ctx;
} Work;

static void worker(void *arg)
{
    Work *w = arg;
    Ctx *x = w->ctx;
    for (;;) {
        long i = atomic_inc(w->next) - 1;
        if ((size_t)i >= w->npaths || *w->stop)
            break;
        if (i % w->parts != w->part)
            continue;
        reset(x, w->sp, w->budget);
        x->stop = w->stop;
        x->gen = 0;
        if (replay(x, &w->paths[i]))
            node(x);
    }
}

static void emit_program(const SuperSpec *sp, const Ctx *x, SuperProg *p)
{
    int temp[SUPER_LEN + 1], ntemps = 0;
    p->len = 0;
    for (int j = 0; j <= x->sol.n; j++)
        temp[j] = x->sol.stored[j] ? ++ntemps : 0;
#define PUT(o, ...)                                                                             \
    do {                                                                                        \
        p->code[p->len].op = (o);                                                               \
        snprintf(p->code[p->len].cell, SUPER_NAME, __VA_ARGS__);                                \
        p->len++;                                                                               \
    } while (0)
    for (int i = 0; i <= x->sol.n; i++) {
        if (i > 0) {
            int k = x->sol.op_kind[i], r = x->sol.op_ref[i];
            if (k == K_ZERO)
                PUT(OP_NAND, "ZERO");
            else if (k == K_ONE)
                PUT(OP_NAND, "ONE");
            else if (k == K_CELL)
                PUT(OP_NAND, "%s", sp->cell[r].name);
            else
                PUT(OP_NAND, "_t%d", temp[r]);
        }
        if (x->sol.stored[i])
            PUT(OP_STO, "_t%d", temp[i]);
        for (int c = 0; c < sp->ncells; c++)
            if (x->sol.out_at[c] == i)
                PUT(OP_STO, "%s", sp->cell[c].name);
    }
#undef PUT
}

void super_search(const SuperSpec *sp, const SuperOpts *o, SuperResult *r)
{
    memset(r, 0, sizeof *r);
    int threads = o->threads > 0 ? o->threads : cpu_cores(NULL);
    int parts = o->parts > 0 ? o->parts : 1;
    Ctx *ctx = calloc((size_t)threads + 1, sizeof *ctx);
    for (int i = 0; i < threads; i++) {
        ctx[i].tt = calloc((size_t)1 << TT_BITS, sizeof(TTEntry));
        for (size_t k = 0; ctx[i].tt && k < (size_t)1 << TT_BITS; k++)
            ctx[i].tt[k].rem = -1;
    }
    double t0 = now_seconds();
    volatile int stop = 0;
    Ctx *root = &ctx[threads];
    reset(root, sp, 0);
    int lb = lower_bound(root);
    r->below = lb;
    for (int budget = lb; budget <= o->max_cost && !stop; budget++) {
        /* the top of the tree, as a list of paths for the threads */
        reset(root, sp, budget);
        root->stop = &stop;
        root->npaths = 0;
        int depth = 1;
        for (;;) {
            root->npaths = 0;
            root->gen = depth;
            reset(root, sp, budget);
            root->gen = depth;
            after_nand(root);  /* the outputs RR could fill before any NAND, then on */
            if (stop || root->npaths >= (size_t)(64 * threads * parts) || depth >= GEN_DEPTH ||
                depth >= budget)
                break;
            depth++;
        }
        r->nodes += root->nodes;
        root->nodes = 0;
        if (root->found) {  /* found near the top already */
            emit_program(sp, root, &r->prog);
            r->found = 1;
            r->cost = root->sol.cost;
            break;
        }
        volatile long next = 0;
        Work *w = calloc((size_t)threads, sizeof *w);
        for (int i = 0; i < threads; i++) {
            ctx[i].found = 0;
            w[i] = (Work){sp, root->paths, root->npaths, budget, o->part, parts, &next, &stop,
                          &ctx[i]};
        }
        Threads *t = threads_start(threads, worker, w, sizeof *w);
        threads_join(t);
        free(w);
        for (int i = 0; i < threads; i++) {
            r->nodes += ctx[i].nodes;
            ctx[i].nodes = 0;
            if (ctx[i].found && !r->found) {
                emit_program(sp, &ctx[i], &r->prog);
                r->found = 1;
                r->cost = ctx[i].sol.cost;
            }
        }
        if (!r->found)
            r->below = budget + 1;
        if (!o->quiet)
            fprintf(stderr, "  cost %d: %s (%.3g nodes so far, %.1f s)\n", budget,
                    r->found ? "found" : "none", (double)r->nodes, now_seconds() - t0);
    }
    /* A correct search finds a program of cost c at budget c and none before: a shorter one
       turning up later means some budget below was not searched in full. */
    r->contradiction = r->found && r->cost != r->below;
    free(root->paths);
    for (int i = 0; i < threads; i++)
        free(ctx[i].tt);
    free(ctx);
    r->seconds = now_seconds() - t0;
}

/* ---- checking and printing ---------------------------------------------------------------- */

static uint64_t rnd(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int super_check(const SuperSpec *sp, const SuperProg *p, char *err, size_t errlen)
{
    /* the cells: the specification's, then the scratch ones the program names */
    char names[SUPER_CELLS + SUPER_LEN][SUPER_NAME];
    int ncells = sp->ncells;
    for (int c = 0; c < sp->ncells; c++)
        snprintf(names[c], SUPER_NAME, "%s", sp->cell[c].name);
    int ref[SUPER_LEN];
    for (int i = 0; i < p->len; i++) {
        const char *n = p->code[i].cell;
        if (!strcmp(n, "ZERO") || !strcmp(n, "ONE")) {
            if (p->code[i].op == OP_STO) {
                snprintf(err, errlen, "instruction %d stores to a constant", i);
                return 0;
            }
            ref[i] = !strcmp(n, "ZERO") ? -1 : -2;
            continue;
        }
        int k;
        for (k = 0; k < ncells && strcmp(names[k], n); k++)
            ;
        if (k == ncells)
            snprintf(names[ncells++], SUPER_NAME, "%s", n);
        ref[i] = k;
    }
    /* every way the outputs may share cells with inputs */
    int outs[SUPER_CELLS], nout = 0, choice[SUPER_CELLS];
    for (int c = 0; c < sp->ncells; c++)
        if (sp->cell[c].out && !sp->cell[c].in)
            outs[nout++] = c;
    memset(choice, 0, sizeof choice);
    uint64_t seed = 12345;
    for (;;) {
        int home[SUPER_CELLS + SUPER_LEN], bad = 0;
        for (int c = 0; c < ncells; c++)
            home[c] = c;
        for (int i = 0; i < nout; i++) {
            int c = outs[i], k = 0;
            if (choice[i])
                for (int a = 0; a < sp->ncells; a++)
                    if (((sp->cell[c].alias >> a) & 1) && ++k == choice[i])
                        home[c] = a;
        }
        for (int i = 0; i < nout; i++)  /* two outputs in one cell is not allowed */
            for (int j = i + 1; j < nout; j++)
                bad |= home[outs[i]] == home[outs[j]];
        for (int trial = 0; trial < 16 && !bad; trial++) {
            uint64_t val[SUPER_CELLS + SUPER_LEN], rr;
            for (int c = 0; c < ncells; c++)
                val[c] = rnd(&seed) & sp->mask;  /* garbage */
            for (int c = 0; c < sp->ncells; c++)
                if (sp->cell[c].in)
                    val[c] = sp->cell[c].init;
            rr = sp->rr_in ? var_table(0) & sp->mask : rnd(&seed) & sp->mask;
            for (int i = 0; i < p->len; i++) {
                int c = ref[i] >= 0 ? home[ref[i]] : ref[i];
                if (p->code[i].op == OP_NAND)
                    rr = ~(rr & (c == -1 ? 0 : c == -2 ? sp->mask : val[c])) & sp->mask;
                else
                    val[c] = rr;
            }
            for (int c = 0; c < sp->ncells; c++) {
                const char *why = NULL;
                uint64_t want = sp->cell[c].out ? sp->cell[c].target : sp->cell[c].init;
                int shared = 0;  /* an input whose cell an output took: its value may change */
                for (int i = 0; i < nout; i++)
                    shared |= home[outs[i]] == c && outs[i] != c;
                if (!shared && val[home[c]] != want)
                    why = sp->cell[c].out ? "does not get its value" : "is not kept";
                if (why) {
                    snprintf(err, errlen, "%s %s (cells shared: variant %d)", sp->cell[c].name, why,
                             choice[0]);
                    return 0;
                }
            }
            if (sp->rr_out && rr != sp->rr_target) {
                snprintf(err, errlen, "RR does not end with its value");
                return 0;
            }
        }
        int i = 0;  /* next variant */
        for (; i < nout; i++) {
            int n = 0;
            for (int a = 0; a < sp->ncells; a++)
                n += (sp->cell[outs[i]].alias >> a) & 1;
            if (++choice[i] <= n)
                break;
            choice[i] = 0;
        }
        if (i == nout)
            break;
    }
    return 1;
}

void super_print(FILE *f, const SuperProg *p, const char *prefix)
{
    for (int i = 0; i < p->len; i++) {
        const char *n = p->code[i].cell;
        int temp = n[0] == '_' && n[1] == 't';  /* scratch cells are _t1, _t2, ... */
        fprintf(f, "    %-4s %s%s\n", p->code[i].op == OP_NAND ? "NAND" : "STO", temp ? prefix : "",
                temp ? n + 1 : n);
    }
}
