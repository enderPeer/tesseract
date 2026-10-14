#include "net.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "build.h"

#define DFF (-2)  /* gate type of a flip-flop */

typedef struct {
    int     type;  /* G_* or DFF */
    int     out;   /* signal */
    int    *in;
    int     nin;
    int     line;
} Gate;

typedef struct {
    char **name;      /* signal names as written */
    int   *driver;    /* gate index, -1 = not driven, -2 = primary input */
    int    n, cap;
    Gate  *g;
    int    ng, capg;
    int   *inputs, *outputs;
    int    nin, nout, capin, capout;
    char  *err;
    size_t errlen;
    const char *file;
} Net;

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static int fail(Net *net, int line, const char *fmt, ...)
{
    int k = snprintf(net->err, net->errlen, "%s:%d: ", net->file, line);
    if (k > 0 && (size_t)k < net->errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(net->err + k, net->errlen - (size_t)k, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static int sig(Net *net, const char *name)
{
    for (int i = 0; i < net->n; i++)  /* netlists here are small enough for a linear search */
        if (!strcmp(net->name[i], name))
            return i;
    if (net->n == net->cap) {
        net->cap = net->cap ? 2 * net->cap : 64;
        net->name = xrealloc(net->name, (size_t)net->cap * sizeof *net->name);
        net->driver = xrealloc(net->driver, (size_t)net->cap * sizeof *net->driver);
    }
    size_t len = strlen(name) + 1;
    net->name[net->n] = memcpy(xrealloc(NULL, len), name, len);
    net->driver[net->n] = -1;
    return net->n++;
}

static int add_index(int **arr, int *n, int *cap, int v)
{
    if (*n == *cap) {
        *cap = *cap ? 2 * *cap : 16;
        *arr = xrealloc(*arr, (size_t)*cap * sizeof **arr);
    }
    (*arr)[(*n)++] = v;
    return v;
}

/* Copies the next name (letters, digits and _ . [ ] $) at *p into buf; 0 if there is none. */
static int read_name(const char **p, char *buf, size_t len)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = 0;
    while (isalnum((unsigned char)*s) || (*s && strchr("_.[]$", *s))) {
        if (n + 1 < len)
            buf[n++] = *s;
        s++;
    }
    buf[n] = '\0';
    *p = s;
    return n > 0;
}

static int expect(const char **p, char c)
{
    while (**p == ' ' || **p == '\t')
        (*p)++;
    if (**p != c)
        return 0;
    (*p)++;
    return 1;
}

static int parse_line(Net *net, char *line, int ln)
{
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';
    const char *p = line;
    char word[256], arg[256];
    if (!read_name(&p, word, sizeof word)) {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        return *p ? fail(net, ln, "cannot read this line") : 1;
    }
    int is_in = !strcmp(word, "INPUT") || !strcmp(word, "input");
    int is_out = !strcmp(word, "OUTPUT") || !strcmp(word, "output");
    if (is_in || is_out) {
        if (!expect(&p, '(') || !read_name(&p, arg, sizeof arg) || !expect(&p, ')'))
            return fail(net, ln, "expected %s(name)", word);
        int s = sig(net, arg);
        if (is_in) {
            if (net->driver[s] != -1)
                return fail(net, ln, "%s is driven twice", arg);
            net->driver[s] = -2;
            add_index(&net->inputs, &net->nin, &net->capin, s);
        } else {
            add_index(&net->outputs, &net->nout, &net->capout, s);
        }
        return 1;
    }

    char type[64];
    if (!expect(&p, '=') || !read_name(&p, type, sizeof type) || !expect(&p, '('))
        return fail(net, ln, "expected name = GATE(inputs)");
    int t = gate_type(type);
    if (t < 0 && (!strcmp(type, "DFF") || !strcmp(type, "dff")))
        t = DFF;
    if (t == -1)
        return fail(net, ln, "unknown gate %s", type);
    int out = sig(net, word);
    if (net->driver[out] != -1)
        return fail(net, ln, "%s is driven twice", word);
    if (net->ng == net->capg) {
        net->capg = net->capg ? 2 * net->capg : 64;
        net->g = xrealloc(net->g, (size_t)net->capg * sizeof *net->g);
    }
    Gate *g = &net->g[net->ng];
    memset(g, 0, sizeof *g);
    g->type = t;
    g->out = out;
    g->line = ln;
    int cap = 0;
    do {
        if (!read_name(&p, arg, sizeof arg))
            return fail(net, ln, "expected an input name");
        add_index(&g->in, &g->nin, &cap, sig(net, arg));
    } while (expect(&p, ','));
    if (!expect(&p, ')'))
        return fail(net, ln, "expected )");
    if ((t == G_NOT || t == G_BUF || t == DFF) && g->nin != 1)
        return fail(net, ln, "%s takes one input", type);
    net->driver[out] = net->ng++;
    return 1;
}

/* A name the assembler accepts: letters, digits and _, not starting with a digit, and not one of
   the constant cells or the builder's scratch cells. */
static int same_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static void cell_name(const char *s, char *buf, size_t len)
{
    size_t n = 0;
    if (isdigit((unsigned char)*s) || same_nocase(s, "ZERO") || same_nocase(s, "ONE") ||
        !strncmp(s, "__", 2))
        buf[n++] = 'n';
    for (; *s && n + 1 < len; s++)
        buf[n++] = isalnum((unsigned char)*s) || *s == '_' ? *s : '_';
    buf[n] = '\0';
}

static void free_net(Net *net)
{
    for (int i = 0; i < net->n; i++)
        free(net->name[i]);
    for (int i = 0; i < net->ng; i++)
        free(net->g[i].in);
    free(net->name);
    free(net->driver);
    free(net->g);
    free(net->inputs);
    free(net->outputs);
}

static int build(Net *net, Program *out)
{
    for (int s = 0; s < net->n; s++)
        if (net->driver[s] == -1)
            return fail(net, 0, "signal %s is used but never driven", net->name[s]);

    /* sort the gates (Kahn): a gate is ready once every gate it reads from is placed; inputs and
       flip-flop outputs are ready from the start */
    int *wait = calloc((size_t)net->ng + 1, sizeof *wait), *order = malloc(((size_t)net->ng + 1) * sizeof *order);
    int *users = NULL, nusers = 0, capusers = 0, *first = malloc(((size_t)net->n + 1) * sizeof *first);
    int *next = NULL, nnext = 0, capnext = 0;
    for (int s = 0; s < net->n; s++)
        first[s] = -1;
    for (int i = 0; i < net->ng; i++) {
        if (net->g[i].type == DFF)
            continue;
        for (int k = 0; k < net->g[i].nin; k++) {
            int d = net->driver[net->g[i].in[k]];
            if (d >= 0 && net->g[d].type != DFF) {
                wait[i]++;  /* edge e: gate users[e] reads signal; next[e] = next edge */
                add_index(&users, &nusers, &capusers, i);
                add_index(&next, &nnext, &capnext, first[net->g[d].out]);
                first[net->g[d].out] = nusers - 1;
            }
        }
    }
    int head = 0, tail = 0;
    for (int i = 0; i < net->ng; i++)
        if (net->g[i].type != DFF && wait[i] == 0)
            order[tail++] = i;
    while (head < tail) {
        int g = order[head++];
        for (int e = first[net->g[g].out]; e >= 0; e = next[e])
            if (--wait[users[e]] == 0)
                order[tail++] = users[e];
    }
    int combinational = 0;
    for (int i = 0; i < net->ng; i++)
        combinational += net->g[i].type != DFF;
    int ok = tail == combinational;
    if (!ok) {
        for (int i = 0; i < net->ng; i++)
            if (net->g[i].type != DFF && wait[i] > 0) {
                fail(net, net->g[i].line, "%s is part of a loop without a DFF",
                     net->name[net->g[i].out]);
                break;
            }
    }

    if (ok) {
        Builder *b = b_new();
        uint32_t *cell = malloc(((size_t)net->n + 1) * sizeof *cell);
        for (int s = 0; ok && s < net->n; s++) {
            char nm[300];
            cell_name(net->name[s], nm, sizeof nm);
            if (b_has(b, nm)) {
                ok = fail(net, 0, "two signals both become cell %s", nm);
                break;
            }
            cell[s] = b_cell(b, nm);
        }
        if (ok) {
            for (int i = 0; i < net->nin; i++)
                b_input(b, cell[net->inputs[i]]);
            uint32_t *in = NULL;
            int capin = 0;
            for (int k = 0; k < tail; k++) {
                const Gate *g = &net->g[order[k]];
                if (g->nin > capin) {
                    capin = g->nin;
                    in = xrealloc(in, (size_t)capin * sizeof *in);
                }
                for (int j = 0; j < g->nin; j++)
                    in[j] = cell[g->in[j]];
                b_gate(b, g->type, cell[g->out], in, g->nin);
            }
            free(in);
            for (int i = 0; i < net->ng; i++)
                if (net->g[i].type == DFF)
                    b_reg(b, cell[net->g[i].out], cell[net->g[i].in[0]]);
            for (int i = 0; i < net->nout; i++)
                b_output(b, cell[net->outputs[i]]);
        }
        b_finish(b, out);
        if (!ok)
            program_free(out);
        free(cell);
    }
    free(wait);
    free(order);
    free(users);
    free(first);
    free(next);
    return ok;
}

int net_parse(const char *text, const char *name, Program *out, char *err, size_t errlen)
{
    Net net;
    memset(&net, 0, sizeof net);
    memset(out, 0, sizeof *out);
    net.err = err;
    net.errlen = errlen;
    net.file = name;
    size_t len = strlen(text);
    char *copy = memcpy(xrealloc(NULL, len + 1), text, len + 1);
    int ok = 1, ln = 1;
    for (char *line = copy; ok && line; ln++) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        ok = parse_line(&net, line, ln);
        line = nl ? nl + 1 : NULL;
    }
    free(copy);
    if (ok)
        ok = build(&net, out);
    free_net(&net);
    return ok;
}

int net_load(const char *path, Program *out, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        memset(out, 0, sizeof *out);
        snprintf(err, errlen, "%s: cannot open file", path);
        return 0;
    }
    size_t len = 0, cap = 4096;
    char *buf = xrealloc(NULL, cap);
    for (size_t got; (got = fread(buf + len, 1, cap - len - 1, f)) > 0;) {
        len += got;
        if (len + 1 == cap)
            buf = xrealloc(buf, cap *= 2);
    }
    fclose(f);
    buf[len] = '\0';
    text_normalize(buf, len);
    int ok = net_parse(buf, path, out, err, errlen);
    free(buf);
    return ok;
}

int load_program(const char *path, Program *out, char *err, size_t errlen)
{
    size_t n = strlen(path);
    if (n > 6 && (!strcmp(path + n - 6, ".bench") || !strcmp(path + n - 6, ".BENCH")))
        return net_load(path, out, err, errlen);
    return asm_file(path, out, err, errlen);
}
