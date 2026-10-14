/* Assembler for ISA v1: source text -> Program. The language is described in docs/ISA.md. */
#include "asm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH  64   /* include + macro nesting */
#define MAX_TOKENS 128

typedef struct {
    char  *name;
    char **params;
    int    nparams;
    char **body;       /* source lines between .macro and .endm */
    int   *body_line;  /* their line numbers */
    int    nbody;
    char  *file;       /* where the macro was defined */
    char  *dir;
} Macro;

typedef struct {
    Instr    *code;   size_t   len, cap;
    char    **cells;  uint32_t ncells, capcells;
    CellInfo *info;   uint32_t capinfo;  /* per cell: plain, id or link */
    int       special;                   /* the program has id or link cells */
    uint32_t *in;     size_t   nin, capin;
    uint32_t *out;    size_t   nout, capout;
    Macro    *macros; int      nmacros, capmacros;
    unsigned  expansions;  /* numbers @locals, one id per macro call */
    char     *err;    size_t   errlen;
    int       failed;
} Ctx;

/* Where the lines being assembled come from. */
typedef struct {
    const char *file;
    const char *dir;       /* for resolving .include */
    const char *from;      /* "file:line" of the outermost macro call, NULL outside macros */
    int         depth;
    int         in_macro;  /* @names are allowed */
} Loc;

typedef struct {
    char *line;
    char *tok[MAX_TOKENS];
    int   n;
} Tokens;

typedef struct {
    char  *s;
    size_t len, cap;
} Str;

static void run_lines(Ctx *c, const Loc *L, char **lines, const int *line_nos, int n);

/* ---- helpers ---------------------------------------------------------------------------- */

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xrealloc(NULL, n), s, n);
}

#define GROW(arr, n, cap)                                                \
    do {                                                                 \
        if ((n) == (cap)) {                                              \
            (cap) = (cap) ? 2 * (cap) : 16;                              \
            (arr) = xrealloc((arr), (size_t)(cap) * sizeof *(arr));      \
        }                                                                \
    } while (0)

static void str_add(Str *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        b->cap = 2 * (b->len + n + 1);
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

static int eq_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int is_ident(const char *s)
{
    if (!isalpha((unsigned char)*s) && *s != '_')
        return 0;
    for (s++; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_')
            return 0;
    return 1;
}

static int is_instruction(const char *s)
{
    return eq_nocase(s, "NAND") || eq_nocase(s, "STO");
}

static void fail(Ctx *c, const Loc *L, int line, const char *fmt, ...)
{
    if (c->failed)
        return;
    c->failed = 1;
    size_t n = 0;
    int k = snprintf(c->err, c->errlen, "%s:%d: ", L->file, line);
    if (k > 0)
        n = (size_t)k;
    if (n < c->errlen) {
        va_list ap;
        va_start(ap, fmt);
        k = vsnprintf(c->err + n, c->errlen - n, fmt, ap);
        va_end(ap);
        if (k > 0)
            n += (size_t)k;
    }
    if (L->from && n < c->errlen)
        snprintf(c->err + n, c->errlen - n, " (in macro called at %s)", L->from);
}

/* Splits a line into tokens. Separators are spaces, tabs and commas, ';' starts a comment and
   "..." is one token. Returns NULL on success, otherwise an error message. */
static const char *tokenize(const char *src, Tokens *t)
{
    char *p = t->line = xstrdup(src);
    t->n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r')
            p++;
        if (*p == '\0' || *p == ';')
            return NULL;
        if (t->n == MAX_TOKENS)
            return "too many tokens on one line";
        if (*p == '"') {
            t->tok[t->n++] = ++p;
            while (*p && *p != '"')
                p++;
            if (*p != '"')
                return "unterminated string";
            *p++ = '\0';
            continue;
        }
        t->tok[t->n++] = p;
        while (*p && !strchr(" \t,\r;", *p))
            p++;
        if (*p == ';') {
            *p = '\0';
            return NULL;
        }
        if (*p)
            *p++ = '\0';
    }
}

static void tokens_free(Tokens *t)
{
    free(t->line);
}

size_t text_normalize(char *buf, size_t len)
{
    unsigned char *b = (unsigned char *)buf;
    if (len >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        int le = b[0] == 0xFF;  /* UTF-16, as Windows PowerShell 5 writes with > */
        size_t n = 0;
        for (size_t i = 2; i + 1 < len; i += 2) {
            unsigned c = le ? (unsigned)b[i] | (unsigned)b[i + 1] << 8
                            : (unsigned)b[i] << 8 | (unsigned)b[i + 1];
            b[n++] = (unsigned char)(c < 128 ? c : '?');
        }
        b[n] = '\0';
        return n;
    }
    if (len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {  /* a UTF-8 byte order mark */
        memmove(b, b + 3, len - 2);
        return len - 3;
    }
    return len;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t len = 0, cap = 4096;
    char *buf = xrealloc(NULL, cap);
    for (;;) {
        size_t got = fread(buf + len, 1, cap - len - 1, f);
        if (got == 0)
            break;
        len += got;
        if (len + 1 == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    fclose(f);
    buf[len] = '\0';
    text_normalize(buf, len);
    return buf;
}

static char *dir_of(const char *path)
{
    const char *slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            slash = p;
    if (!slash)
        return xstrdup(".");
    size_t n = slash == path ? 1 : (size_t)(slash - path);
    char *d = xrealloc(NULL, n + 1);
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static int is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

/* ---- cells -------------------------------------------------------------------------------- */

static int find_cell(const Ctx *c, const char *name)
{
    for (uint32_t i = 0; i < c->ncells; i++)
        if (eq_nocase(c->cells[i], name))
            return (int)i;
    return -1;
}

/* ID0 .. ID15: bit d of the machine's number in the cube, or -1 for any other name */
int program_id_name(const char *name)
{
    if ((name[0] != 'I' && name[0] != 'i') || (name[1] != 'D' && name[1] != 'd') ||
        name[2] < '0' || name[2] > '9' || (name[2] == '0' && name[3]))
        return -1;
    int d = name[2] - '0';
    if (name[3]) {
        if (name[3] < '0' || name[3] > '9' || name[4])
            return -1;
        d = 10 * d + name[3] - '0';
    }
    return d < CUBE_MAX_DIMS ? d : -1;
}

static int id_bit(const char *name)
{
    return program_id_name(name);
}

static uint32_t cell(Ctx *c, const char *name)
{
    int found = find_cell(c, name);
    if (found >= 0)
        return (uint32_t)found;
    GROW(c->cells, c->ncells, c->capcells);
    GROW(c->info, c->ncells, c->capinfo);
    c->cells[c->ncells] = xstrdup(name);
    CellInfo *in = &c->info[c->ncells];
    memset(in, 0, sizeof *in);
    int d = id_bit(name);
    if (d >= 0) {
        in->kind = CK_ID;
        in->dim = (uint8_t)d;
        c->special = 1;
    }
    return c->ncells++;
}

static int readonly(const Ctx *c, uint32_t a)
{
    return a < CELL_FIRST_FREE || c->info[a].kind != CK_PLAIN;
}

static const char *kind_name(const Ctx *c, uint32_t a)
{
    return a < CELL_FIRST_FREE ? "constant" : c->info[a].kind == CK_ID ? "lane-id" : "link";
}

/* @names only ever reach this point from a macro expansion, which already renamed them. */
static int valid_name(Ctx *c, const Loc *L, int line, const char *tok)
{
    if (tok[0] == '@') {
        if (L->in_macro)
            return 1;
        fail(c, L, line, "'%s': @names are only allowed inside macros", tok);
        return 0;
    }
    if (is_ident(tok))
        return 1;
    fail(c, L, line, "'%s' is not a valid cell name", tok);
    return 0;
}

static void add_io(Ctx *c, const Loc *L, int line, const Tokens *t, uint32_t **list, size_t *n,
                   size_t *cap)
{
    if (t->n < 2) {
        fail(c, L, line, "%s expects at least one cell name", t->tok[0]);
        return;
    }
    for (int i = 1; i < t->n; i++) {
        if (!valid_name(c, L, line, t->tok[i]))
            return;
        uint32_t a = cell(c, t->tok[i]);
        if (readonly(c, a)) {
            fail(c, L, line, "%s is a %s cell", t->tok[i], kind_name(c, a));
            return;
        }
        for (size_t k = 0; k < *n; k++) {
            if ((*list)[k] == a) {
                fail(c, L, line, "%s is declared twice", t->tok[i]);
                return;
            }
        }
        GROW(*list, *n, *cap);
        (*list)[(*n)++] = a;
    }
}

/* .link y = x @ d   (spaces optional). y must be new, so a chain of links can never loop. */
static void define_link(Ctx *c, const Loc *L, int line, const Tokens *t)
{
    char buf[512];
    size_t n = 0;
    for (int i = 1; i < t->n; i++) {
        size_t k = strlen(t->tok[i]);
        if (n + k + 1 > sizeof buf) {
            fail(c, L, line, ".link line too long");
            return;
        }
        memcpy(buf + n, t->tok[i], k);
        n += k;
    }
    buf[n] = '\0';
    char *eq = strchr(buf, '='), *at = eq ? strrchr(eq + 1, '@') : NULL;
    if (!eq || !at) {
        fail(c, L, line, ".link expects: .link name = cell @ dimension");
        return;
    }
    *eq = '\0';
    *at = '\0';
    const char *name = buf, *src = eq + 1, *dim = at + 1;
    char *end;
    long d = strtol(dim, &end, 10);
    if (!*dim || *end || d < 0 || d >= CUBE_MAX_DIMS) {
        fail(c, L, line, "the dimension must be 0 to %d, got '%s'", CUBE_MAX_DIMS - 1, dim);
        return;
    }
    if (!valid_name(c, L, line, name) || !valid_name(c, L, line, src))
        return;
    if (find_cell(c, name) >= 0) {
        fail(c, L, line, "%s is already a cell: declare a link before its first use", name);
        return;
    }
    if (id_bit(name) >= 0) {
        fail(c, L, line, "%s is a lane-id cell", name);
        return;
    }
    if (eq_nocase(name, src)) {
        fail(c, L, line, "a link cannot read itself");
        return;
    }
    uint32_t s = cell(c, src), y = cell(c, name);
    c->info[y].kind = CK_LINK;
    c->info[y].dim = (uint8_t)d;
    c->info[y].src = s;
    c->special = 1;
}

/* ---- macros ------------------------------------------------------------------------------- */

static Macro *find_macro(Ctx *c, const char *name)
{
    for (int i = 0; i < c->nmacros; i++)
        if (eq_nocase(c->macros[i].name, name))
            return &c->macros[i];
    return NULL;
}

static int param_index(const Macro *m, const char *tok)
{
    for (int i = 0; i < m->nparams; i++)
        if (eq_nocase(m->params[i], tok))
            return i;
    return -1;
}

static void define_macro(Ctx *c, const Loc *L, int line, const Tokens *t, char **body,
                         const int *body_line, int nbody)
{
    if (t->n < 2 || !is_ident(t->tok[1])) {
        fail(c, L, line, ".macro expects a name");
        return;
    }
    const char *name = t->tok[1];
    if (is_instruction(name)) {
        fail(c, L, line, "cannot redefine instruction %s", name);
        return;
    }
    if (find_macro(c, name)) {
        fail(c, L, line, "macro %s is already defined", name);
        return;
    }
    for (int i = 2; i < t->n; i++) {
        if (!is_ident(t->tok[i]) || is_instruction(t->tok[i])) {
            fail(c, L, line, "'%s' is not a valid parameter name", t->tok[i]);
            return;
        }
        for (int k = 2; k < i; k++) {
            if (eq_nocase(t->tok[k], t->tok[i])) {
                fail(c, L, line, "parameter %s is listed twice", t->tok[i]);
                return;
            }
        }
    }
    GROW(c->macros, c->nmacros, c->capmacros);
    Macro *m = &c->macros[c->nmacros++];
    m->name = xstrdup(name);
    m->nparams = t->n - 2;
    m->params = xrealloc(NULL, (size_t)m->nparams * sizeof *m->params);
    for (int i = 0; i < m->nparams; i++)
        m->params[i] = xstrdup(t->tok[i + 2]);
    m->nbody = nbody;
    m->body = xrealloc(NULL, (size_t)nbody * sizeof *m->body);
    m->body_line = xrealloc(NULL, (size_t)nbody * sizeof *m->body_line);
    for (int i = 0; i < nbody; i++) {
        m->body[i] = xstrdup(body[i]);
        m->body_line[i] = body_line[i];
    }
    m->file = xstrdup(L->file);
    m->dir = xstrdup(L->dir);
}

/* Collects the lines of a .macro block starting at lines[start]; returns the index of .endm. */
static int collect_macro(Ctx *c, const Loc *L, const Tokens *head, char **lines,
                         const int *line_nos, int start, int n)
{
    int line = line_nos ? line_nos[start] : start + 1;
    int end;
    for (end = start + 1; end < n; end++) {
        Tokens t;
        const char *e = tokenize(lines[end], &t);
        int is_end = !e && t.n > 0 && eq_nocase(t.tok[0], ".endm");
        int is_macro = !e && t.n > 0 && eq_nocase(t.tok[0], ".macro");
        tokens_free(&t);
        if (is_macro) {
            fail(c, L, line_nos ? line_nos[end] : end + 1, "macros cannot be defined inside macros");
            return n;
        }
        if (is_end)
            break;
    }
    if (end == n) {
        fail(c, L, line, "missing .endm for macro %s", head->n > 1 ? head->tok[1] : "");
        return n;
    }
    int nbody = end - start - 1;
    int *nums = xrealloc(NULL, (size_t)nbody * sizeof *nums);
    for (int k = 0; k < nbody; k++)
        nums[k] = line_nos ? line_nos[start + 1 + k] : start + 2 + k;
    define_macro(c, L, line, head, lines + start + 1, nums, nbody);
    free(nums);
    return end;
}

/* Substitutes parameters, gives every @local a name unique to this call, then assembles the
   body. */
static void expand_macro(Ctx *c, const Loc *L, int line, const Macro *m, char *const *args)
{
    if (L->depth >= MAX_DEPTH) {
        fail(c, L, line, "macros nested too deeply (does %s call itself?)", m->name);
        return;
    }
    for (int i = 0; i < m->nparams; i++) {
        const char *a = args[i];
        size_t digits = strspn(a, "0123456789");
        if (!(digits > 0 && !a[digits]) && !valid_name(c, L, line, a))  /* numbers are fine too */
            return;
    }

    unsigned id = ++c->expansions;
    char from[512];
    if (!L->from)
        snprintf(from, sizeof from, "%s:%d", L->file, line);
    Loc sub = {m->file, m->dir, L->from ? L->from : from, L->depth + 1, 1};

    /* m may move if the body defines macros (through .include), so copy what is needed. */
    int nbody = m->nbody;
    const int *body_line = m->body_line;
    char **lines = xrealloc(NULL, (size_t)nbody * sizeof *lines);
    for (int b = 0; b < nbody; b++) {
        Tokens t;
        Str out = {0};
        str_add(&out, "");
        const char *e = tokenize(m->body[b], &t);
        if (e)
            fail(c, &sub, body_line[b], "%s", e);
        for (int i = 0; !e && i < t.n; i++) {
            const char *tok = t.tok[i];
            char joined[300], local[320];
            if (strchr(tok, '#')) {  /* v#0 -> the argument for v, then 0; ID#d -> ID3 */
                size_t at = 0;
                for (const char *s = tok;;) {
                    const char *h = strchr(s, '#');
                    size_t n = h ? (size_t)(h - s) : strlen(s);
                    char piece[300];
                    snprintf(piece, sizeof piece, "%.*s", (int)n, s);
                    int p = param_index(m, piece);
                    at += (size_t)snprintf(joined + at, sizeof joined - at, "%s",
                                           p >= 0 ? args[p] : piece);
                    if (!h || at >= sizeof joined)
                        break;
                    s = h + 1;
                }
                tok = joined;
            } else {
                int p = param_index(m, tok);
                if (p >= 0)
                    tok = args[p];
            }
            /* a local name of this call (an outer call's ones already contain ':'; a lone @
               belongs to .link): @t -> @17:t, so a suffix added later stays at the end */
            if (tok[0] == '@' && tok[1] && !strchr(tok, ':')) {
                if (!is_ident(tok + 1)) {
                    fail(c, &sub, body_line[b], "'%s' is not a valid local name", tok);
                    break;
                }
                snprintf(local, sizeof local, "@%u:%s", id, tok + 1);
                tok = local;
            }
            int quote = *tok == '\0' || strchr(tok, ' ') != NULL;
            if (i)
                str_add(&out, " ");
            if (quote)
                str_add(&out, "\"");
            str_add(&out, tok);
            if (quote)
                str_add(&out, "\"");
        }
        tokens_free(&t);
        lines[b] = out.s;
    }
    if (!c->failed)
        run_lines(c, &sub, lines, body_line, nbody);
    for (int b = 0; b < nbody; b++)
        free(lines[b]);
    free(lines);
}

/* ---- lines -------------------------------------------------------------------------------- */

static void run_text(Ctx *c, char *text, const char *file, const char *dir, const char *from,
                     int depth);

static void include_file(Ctx *c, const Loc *L, int line, const char *name)
{
    if (L->depth >= MAX_DEPTH) {
        fail(c, L, line, "includes nested too deeply");
        return;
    }
    char path[1024];
    if (is_absolute(name))
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "%s/%s", L->dir, name);
    char *text = read_file(path);
    if (!text) {
        fail(c, L, line, "cannot open include file '%s'", path);
        return;
    }
    char *dir = dir_of(path);
    run_text(c, text, path, dir, L->from, L->depth + 1);
    free(dir);
    free(text);
}

static void do_line(Ctx *c, const Loc *L, int line, const Tokens *t)
{
    const char *op = t->tok[0];

    if (op[0] == '.') {
        if (eq_nocase(op, ".in"))
            add_io(c, L, line, t, &c->in, &c->nin, &c->capin);
        else if (eq_nocase(op, ".out"))
            add_io(c, L, line, t, &c->out, &c->nout, &c->capout);
        else if (eq_nocase(op, ".link"))
            define_link(c, L, line, t);
        else if (eq_nocase(op, ".include") && t->n == 2)
            include_file(c, L, line, t->tok[1]);
        else if (eq_nocase(op, ".include"))
            fail(c, L, line, ".include expects one file name");
        else if (eq_nocase(op, ".endm"))
            fail(c, L, line, ".endm without .macro");
        else
            fail(c, L, line, "unknown directive %s", op);
        return;
    }

    if (is_instruction(op)) {
        if (t->n != 2) {
            fail(c, L, line, "%s expects exactly one cell", op);
            return;
        }
        if (!valid_name(c, L, line, t->tok[1]))
            return;
        uint32_t a = cell(c, t->tok[1]);
        int sto = eq_nocase(op, "STO");
        if (sto && readonly(c, a)) {
            fail(c, L, line, "cannot store to %s cell %s", kind_name(c, a), t->tok[1]);
            return;
        }
        GROW(c->code, c->len, c->cap);
        c->code[c->len].op = (uint8_t)(sto ? OP_STO : OP_NAND);
        c->code[c->len].addr = a;
        c->len++;
        return;
    }

    const Macro *m = find_macro(c, op);
    if (!m) {
        fail(c, L, line, "unknown instruction or macro '%s'", op);
        return;
    }
    if (t->n - 1 != m->nparams) {
        fail(c, L, line, "macro %s expects %d argument%s, got %d", m->name, m->nparams,
             m->nparams == 1 ? "" : "s", t->n - 1);
        return;
    }
    expand_macro(c, L, line, m, t->tok + 1);
}

static void run_lines(Ctx *c, const Loc *L, char **lines, const int *line_nos, int n)
{
    for (int i = 0; i < n && !c->failed; i++) {
        int line = line_nos ? line_nos[i] : i + 1;
        Tokens t;
        const char *e = tokenize(lines[i], &t);
        if (e)
            fail(c, L, line, "%s", e);
        else if (t.n > 0 && eq_nocase(t.tok[0], ".macro"))
            i = collect_macro(c, L, &t, lines, line_nos, i, n);
        else if (t.n > 0)
            do_line(c, L, line, &t);
        tokens_free(&t);
    }
}

static void run_text(Ctx *c, char *text, const char *file, const char *dir, const char *from,
                     int depth)
{
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text += 3;  /* UTF-8 byte order mark */
    size_t n = 1;
    for (const char *p = text; *p; p++)
        n += *p == '\n';
    char **lines = xrealloc(NULL, n * sizeof *lines);
    size_t k = 0;
    lines[k++] = text;
    for (char *p = text; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            lines[k++] = p + 1;
        }
    }
    Loc L = {file, dir, from, depth, 0};
    run_lines(c, &L, lines, NULL, (int)n);
    free(lines);
}

/* ---- entry points ------------------------------------------------------------------------- */

static void ctx_init(Ctx *c, char *err, size_t errlen)
{
    memset(c, 0, sizeof *c);
    c->err = err;
    c->errlen = errlen;
    if (errlen)
        err[0] = '\0';
    cell(c, "ZERO");
    cell(c, "ONE");
}

static int finish(Ctx *c, Program *out)
{
    for (int i = 0; i < c->nmacros; i++) {
        Macro *m = &c->macros[i];
        for (int k = 0; k < m->nparams; k++)
            free(m->params[k]);
        for (int k = 0; k < m->nbody; k++)
            free(m->body[k]);
        free(m->name);
        free(m->params);
        free(m->body);
        free(m->body_line);
        free(m->file);
        free(m->dir);
    }
    free(c->macros);

    Program p = {c->code, c->len,  c->ncells, c->cells, c->in,
                 c->nin,  c->out,  c->nout,   c->info,  /* kept only if the program needs it */};
    if (!c->special) {
        free(c->info);
        p.special = NULL;
    }
    if (c->failed) {
        program_free(&p);
        memset(out, 0, sizeof *out);
        return 0;
    }
    *out = p;
    return 1;
}

int asm_string(const char *src, const char *name, const char *base_dir, Program *out, char *err,
               size_t errlen)
{
    Ctx c;
    ctx_init(&c, err, errlen);
    char *text = xstrdup(src);
    run_text(&c, text, name, base_dir, NULL, 0);
    free(text);
    return finish(&c, out);
}

int asm_file(const char *path, Program *out, char *err, size_t errlen)
{
    char *text = read_file(path);
    if (!text) {
        memset(out, 0, sizeof *out);
        snprintf(err, errlen, "%s: cannot open file", path);
        return 0;
    }
    Ctx c;
    ctx_init(&c, err, errlen);
    char *dir = dir_of(path);
    run_text(&c, text, path, dir, NULL, 0);
    free(dir);
    free(text);
    return finish(&c, out);
}

void program_free(Program *p)
{
    for (uint32_t i = 0; i < p->ncells; i++)
        free(p->cell_names[i]);
    free(p->cell_names);
    free(p->code);
    free(p->inputs);
    free(p->outputs);
    free(p->special);
    memset(p, 0, sizeof *p);
}

int program_readonly(const Program *p, uint32_t cell)
{
    return cell < CELL_FIRST_FREE || (p->special && p->special[cell].kind != CK_PLAIN);
}

unsigned program_dims(const Program *p)
{
    unsigned dims = CUBE_DIMS;
    for (uint32_t c = 0; p->special && c < p->ncells; c++)
        if (p->special[c].kind != CK_PLAIN && p->special[c].dim + 1u > dims)
            dims = p->special[c].dim + 1u;
    return dims;
}

uint32_t program_resolve(const Program *p, uint32_t cell, unsigned *mask)
{
    *mask = 0;
    while (p->special && p->special[cell].kind == CK_LINK) {
        *mask ^= 1u << p->special[cell].dim;
        cell = p->special[cell].src;
    }
    return cell;
}

int program_find_cell(const Program *p, const char *name, uint32_t *addr)
{
    for (uint32_t i = 0; i < p->ncells; i++) {
        if (eq_nocase(p->cell_names[i], name)) {
            *addr = i;
            return 1;
        }
    }
    return 0;
}

unsigned addr_bits(uint32_t ncells)
{
    unsigned b = 1;
    while (b < 32 && ((uint64_t)1 << b) < ncells)
        b++;
    return b;
}
