#include "jit.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* The generated function is void run(Line *mem, Line *rr, void *save, uint64_t passes): it runs
   the pass `passes` times (at least once). On Windows xmm6-xmm15 belong to the caller, so the ones
   the pass uses are saved to `save` and restored.

   With more than 512 machines (step 7f) the pass is cut into segments, each its own function
   void seg(Line *mem, Line *rr, void *save, Line **peer) that runs once: peer[s] is the memory of
   the group read across peer s (lane_peers), whose address goes through rax. */
#ifdef _WIN32
enum { MEM = X86_RCX, RRP = X86_RDX, SAVE = X86_R8, COUNT = X86_R9, SAVED_FROM = 6 };
#else
enum { MEM = X86_RDI, RRP = X86_RSI, SAVE = X86_RDX, COUNT = X86_RCX, SAVED_FROM = 16 };
#endif
enum { PEERS = COUNT, PEER_BASE = X86_RAX };

/* Vector registers: ymm0 = all ones, ymm1 = scratch, ymm2-ymm15 = 7 pairs. A pair holds one
   512-lane value: lanes 0-255 in the even register, lanes 256-511 in the odd one. */
enum { ONES = 0, SCRATCH = 1, NPAIRS = 7 };
#define REG(k, h) (2 + 2 * (k) + (h))

enum { K_MEM, K_ZERO, K_ONE, K_PAIR };  /* what is known about a cell during the pass */
enum { RR_CONST, RR_MEM, RR_REG };      /* where RR is */

typedef struct {
    char  *s;
    size_t len, cap;
} Text;

typedef struct {
    const Program *p;
    int       literal;
    X86       x;
    Text     *text;       /* listing, or NULL */
    char      head[80];   /* the 1-bit instruction being compiled */
    int       head_used;
    size_t    emitted;    /* x86 instructions emitted for it */

    int       rr_kind;    /* RR_CONST: value rr_val; RR_MEM, RR_REG: true RR = stored ^ rr_inv */
    int       rr_val, rr_pair, rr_inv;
    uint8_t  *kind;       /* per cell, K_* */
    uint8_t  *pair;       /* per cell, if K_PAIR */
    uint32_t *ver;        /* per cell: pair_ver[pair] when mapped; stale = back in memory only */
    uint32_t  pair_ver[NPAIRS];
    int       pair_cells[NPAIRS];
    uint64_t  pair_used[NPAIRS];
    uint64_t  clock;
    int       top;        /* highest vector register written */
    JitStats  st;
    uint32_t  gm[CUBE_MAX_GROUPS];  /* the peers' group masks (lane_peers) */
    unsigned  npeers;
} Jit;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void say(Text *t, const char *fmt, ...)
{
    for (;;) {
        size_t room = t->cap - t->len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(t->s ? t->s + t->len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if ((size_t)n < room) {
            t->len += (size_t)n;
            return;
        }
        t->cap = 2 * (t->len + (size_t)n + 1);
        t->s = realloc(t->s, t->cap);
        if (!t->s) {
            fputs("out of memory\n", stderr);
            exit(1);
        }
    }
}

/* ---- listing and counting ----------------------------------------------------------------- */

static void begin(Jit *j, const char *fmt, ...)
{
    j->head_used = 0;
    j->emitted = 0;
    if (!j->text)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->head, sizeof j->head, fmt, ap);
    va_end(ap);
}

static void logged(Jit *j)
{
    j->st.x86++;
    j->emitted++;
    if (j->text) {
        say(j->text, "%-26s %s\n", j->head_used ? "" : j->head, j->x.text);
        j->head_used = 1;
    }
}

static void note(Jit *j, const char *fmt, ...)
{
    if (!j->text)
        return;
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(j->text, "%-26s ; %s\n", j->head_used ? "" : j->head, buf);
    j->head_used = 1;
}

static const char *cell_name(const Jit *j, uint32_t a, char *buf, size_t n)
{
    if (j->p->cell_names)
        return j->p->cell_names[a];
    snprintf(buf, n, "cell%u", a);
    return buf;
}

static int32_t disp(uint32_t cell, int half)
{
    return (int32_t)(cell * 64u + 32u * (unsigned)half);
}

static void track(Jit *j, int reg)
{
    if (reg > j->top)
        j->top = reg;
}

/* dst = op(src1, src2) */
static void op3(Jit *j, int op, int dst, int src1, int src2)
{
    x86_rrr(&j->x, op, dst, src1, src2);
    track(j, dst);
    if (src1 != src2)  /* x^x and x==x are idioms the CPU resolves without an ALU */
        j->st.alu++;
    logged(j);
}

/* dst = op(src1, cell) */
static void op3m(Jit *j, int op, int dst, int src1, uint32_t cell, int half)
{
    x86_rrm(&j->x, op, dst, src1, MEM, disp(cell, half));
    track(j, dst);
    j->st.alu++;
    j->st.loads++;
    logged(j);
}

static void load(Jit *j, int dst, int base, int32_t d, int aligned)
{
    x86_load(&j->x, dst, base, d, aligned, 1);
    track(j, dst);
    j->st.loads++;
    logged(j);
}

static void store(Jit *j, int src, int base, int32_t d, int aligned)
{
    x86_store(&j->x, src, base, d, aligned, 1);
    j->st.stores++;
    logged(j);
}

static int zero(Jit *j)
{
    op3(j, X86_VPXOR, SCRATCH, SCRATCH, SCRATCH);
    return SCRATCH;
}

/* ---- what the registers hold ------------------------------------------------------------- */

static void touch(Jit *j, int k)
{
    j->pair_used[k] = ++j->clock;
}

static int cell_pair(const Jit *j, uint32_t a)
{
    return j->kind[a] == K_PAIR && j->ver[a] == j->pair_ver[j->pair[a]] ? j->pair[a] : -1;
}

/* Cell a is about to get a new value. */
static void forget(Jit *j, uint32_t a)
{
    int k = cell_pair(j, a);
    if (k >= 0)
        j->pair_cells[k]--;
    j->kind[a] = K_MEM;
}

static void map(Jit *j, uint32_t a, int k)
{
    j->kind[a] = K_PAIR;
    j->pair[a] = (uint8_t)k;
    j->ver[a] = j->pair_ver[k];
    j->pair_cells[k]++;
    touch(j, k);
}

static int rr_in(const Jit *j, int k)
{
    return j->rr_kind == RR_REG && j->rr_pair == k;
}

/* A pair to write a new value into, never `keep`, `keep2` or RR's pair. Prefers an empty pair,
   otherwise the least recently used; its cells then fall back to memory, which already holds them. */
static int pick2(Jit *j, int keep, int keep2)
{
    int best = -1;
    for (int k = 0; k < NPAIRS && best < 0; k++)
        if (k != keep && k != keep2 && !rr_in(j, k) && j->pair_cells[k] == 0)
            best = k;
    if (best < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int k = 0; k < NPAIRS; k++) {
            if (k != keep && k != keep2 && !rr_in(j, k) && j->pair_used[k] < oldest) {
                oldest = j->pair_used[k];
                best = k;
            }
        }
    }
    j->pair_ver[best]++;
    j->pair_cells[best] = 0;
    touch(j, best);
    return best;
}

static int pick(Jit *j, int keep)
{
    return pick2(j, keep, -1);
}

static void rr_set_const(Jit *j, int v)
{
    j->rr_kind = RR_CONST;
    j->rr_val = v;
}

static void rr_set_reg(Jit *j, int k, int inv)
{
    j->rr_kind = RR_REG;
    j->rr_pair = k;
    j->rr_inv = inv;
    touch(j, k);
}

/* RR still lives in memory from the previous pass. */
static void rr_load(Jit *j)
{
    int d = pick(j, -1);
    load(j, REG(d, 0), RRP, 0, 0);
    load(j, REG(d, 1), RRP, 32, 0);
    rr_set_reg(j, d, j->rr_inv);
}

/* ---- reading across links (step 7a) ------------------------------------------------------- */

static void counted(Jit *j, int dst)
{
    track(j, dst);
    j->st.alu++;
    logged(j);
}

/* One of the constant lines after the program's cells (src/lanes.h). */
static int32_t extra_line(const Jit *j, int which)
{
    return (int32_t)((j->p->ncells + (uint32_t)which) * 64u);
}

/* Cell `base` as read across the dimensions in mask, built in a free register pair: lane k gets
   lane k XOR mask. The same steps as permute() in src/lanes.c. Dimensions 9 and up name another
   group: its memory, not this group's registers. */
static int link_value(Jit *j, uint32_t base, unsigned mask)
{
    int from = MEM, src = -1;
    if (mask >> CUBE_DIMS) {
        unsigned s = 0;
        while (j->gm[s] != mask >> CUBE_DIMS)
            s++;
        x86_load64(&j->x, PEER_BASE, PEERS, (int32_t)(8 * s));
        logged(j);
        from = PEER_BASE;
    } else {
        src = cell_pair(j, base);
    }
    int t = pick2(j, j->rr_kind == RR_REG ? j->rr_pair : -1, src);
    int swap = (mask >> 8) & 1;  /* dimension 8: the two halves change places */
    for (int h = 0; h < 2; h++) {
        if (src >= 0) {
            x86_mov(&j->x, REG(t, h), REG(src, h ^ swap));
            track(j, REG(t, h));
            logged(j);
        } else {
            load(j, REG(t, h), from, disp(base, h ^ swap), 1);
        }
    }
    if (src >= 0)
        touch(j, src);
    for (int h = 0; h < 2; h++) {
        int r = REG(t, h);
        for (int d = 0; d < 3; d++) {  /* bits inside each byte: (x & m) << s | (x >> s) & m */
            if (!(mask & (1u << d)))
                continue;
            int32_t m = extra_line(j, LINE_BITS0 + d);
            x86_shiftq(&j->x, 0, SCRATCH, r, 1 << d);
            counted(j, SCRATCH);
            x86_rrm(&j->x, X86_VPAND, SCRATCH, SCRATCH, MEM, m);
            j->st.loads++;
            counted(j, SCRATCH);
            x86_rrm(&j->x, X86_VPAND, r, r, MEM, m);
            j->st.loads++;
            counted(j, r);
            x86_shiftq(&j->x, 1, r, r, 1 << d);
            counted(j, r);
            x86_rrr(&j->x, X86_VPOR, r, r, SCRATCH);
            counted(j, r);
        }
        for (int d = 3; d < 5; d++) {  /* bytes and 16-bit pieces */
            if (mask & (1u << d)) {
                x86_pshufb(&j->x, r, r, MEM, extra_line(j, d == 3 ? LINE_BYTES3 : LINE_BYTES4));
                j->st.loads++;
                counted(j, r);
            }
        }
        if (mask & 32) {  /* 32-bit halves */
            x86_pshufd(&j->x, r, r, 0xB1);
            counted(j, r);
        }
        if (mask & 64) {  /* 64-bit words */
            x86_pshufd(&j->x, r, r, 0x4E);
            counted(j, r);
        }
        if (mask & 128) {  /* 128-bit halves */
            x86_permq(&j->x, r, r, 0x4E);
            counted(j, r);
        }
    }
    return t;
}

/* ---- the two instructions ----------------------------------------------------------------- */

static void literal_nand(Jit *j, uint32_t a)
{
    unsigned mask = 0;
    if (j->p->special)
        a = program_resolve(j->p, a, &mask);
    int t = mask && a >= CELL_FIRST_FREE ? link_value(j, a, mask) : -1;
    for (int h = 0; h < 2; h++) {
        if (t >= 0)
            op3(j, X86_VPAND, REG(0, h), REG(0, h), REG(t, h));
        else
            op3m(j, X86_VPAND, REG(0, h), REG(0, h), a, h);
        op3(j, X86_VPXOR, REG(0, h), REG(0, h), ONES);
    }
}

static void literal_sto(Jit *j, uint32_t a)
{
    if (program_readonly(j->p, a)) {
        note(j, "read-only cell: ignored");
        return;
    }
    for (int h = 0; h < 2; h++)
        store(j, REG(0, h), MEM, disp(a, h), 1);
}

static void compile_nand(Jit *j, uint32_t a)
{
    char nb[16];
    unsigned mask = 0;
    if (j->p->special)  /* a link reads `a` of another lane; constants are the same everywhere */
        a = program_resolve(j->p, a, &mask);
    if (j->kind[a] == K_ZERO) {
        rr_set_const(j, 1);
        note(j, "RR = 1");
        return;
    }
    if (j->kind[a] == K_ONE) {
        if (j->rr_kind == RR_CONST)
            j->rr_val ^= 1;
        else
            j->rr_inv ^= 1;
        note(j, "RR = !RR, kept as a flag");
        return;
    }
    if (j->rr_kind == RR_CONST && j->rr_val == 0) {
        rr_set_const(j, 1);
        note(j, "RR was 0, so RR = 1");
        return;
    }
    if (j->rr_kind == RR_MEM && mask)
        rr_load(j);  /* before link_value picks registers, so RR keeps its pair */
    int xk = mask ? link_value(j, a, mask) : cell_pair(j, a);
    if (j->rr_kind == RR_CONST) {  /* RR = !x */
        if (xk >= 0) {
            rr_set_reg(j, xk, 1);
            note(j, "RR = !%s, already in ymm%d/ymm%d", cell_name(j, a, nb, sizeof nb), REG(xk, 0),
                 REG(xk, 1));
            return;
        }
        int d = pick(j, -1);
        load(j, REG(d, 0), MEM, disp(a, 0), 1);
        load(j, REG(d, 1), MEM, disp(a, 1), 1);
        rr_set_reg(j, d, 1);
        return;
    }
    if (j->rr_kind == RR_MEM)
        rr_load(j);

    int s = j->rr_pair, inv = j->rr_inv;
    if (xk == s) {  /* x is RR's own value: !(v & v) = !v */
        if (inv)
            rr_set_const(j, 1);
        else
            j->rr_inv = 1;
        note(j, "%s is RR itself", cell_name(j, a, nb, sizeof nb));
        return;
    }
    /* RR = !(v & x): keep v' = v & x (or !v & x if RR was already inverted) and set the flag */
    int op = inv ? X86_VPANDN : X86_VPAND;
    int d = j->pair_cells[s] == 0 ? s : pick(j, s);
    for (int h = 0; h < 2; h++) {
        if (xk >= 0)
            op3(j, op, REG(d, h), REG(s, h), REG(xk, h));
        else
            op3m(j, op, REG(d, h), REG(s, h), a, h);
    }
    if (xk >= 0)
        touch(j, xk);
    rr_set_reg(j, d, 1);
}

static void compile_sto(Jit *j, uint32_t a)
{
    if (program_readonly(j->p, a)) {
        note(j, "read-only cell: ignored");
        return;
    }
    forget(j, a);
    if (j->rr_kind == RR_CONST) {
        int r = j->rr_val ? ONES : zero(j);
        store(j, r, MEM, disp(a, 0), 1);
        store(j, r, MEM, disp(a, 1), 1);
        j->kind[a] = j->rr_val ? K_ONE : K_ZERO;
        return;
    }
    if (j->rr_kind == RR_MEM)
        rr_load(j);
    int s = j->rr_pair, v = s;
    if (j->rr_inv) {  /* memory gets the true value; RR itself stays inverted */
        v = pick(j, s);
        op3(j, X86_VPXOR, REG(v, 0), REG(s, 0), ONES);
        op3(j, X86_VPXOR, REG(v, 1), REG(s, 1), ONES);
    }
    store(j, REG(v, 0), MEM, disp(a, 0), 1);
    store(j, REG(v, 1), MEM, disp(a, 1), 1);
    map(j, a, v);
}

/* Writes RR back with its true value. */
static void finish_rr(Jit *j)
{
    if (j->rr_kind == RR_CONST) {
        int r = j->rr_val ? ONES : zero(j);
        store(j, r, RRP, 0, 0);
        store(j, r, RRP, 32, 0);
        return;
    }
    for (int h = 0; h < 2; h++) {
        if (j->rr_kind == RR_MEM && !j->rr_inv)
            break;  /* unchanged */
        int r;
        if (j->rr_kind == RR_MEM) {
            load(j, SCRATCH, RRP, 32 * h, 0);
            r = SCRATCH;
        } else {
            r = REG(j->rr_pair, h);
        }
        if (j->rr_inv) {
            op3(j, X86_VPXOR, SCRATCH, r, ONES);
            r = SCRATCH;
        }
        store(j, r, RRP, 32 * h, 0);
    }
}

/* ---- the whole pass ----------------------------------------------------------------------- */

/* Where a pass over more than 512 machines must be cut (step 7f). Each segment runs for every group
   before the next segment starts anywhere, but the groups do it one after another, or at the same
   time on different threads. That is the same as lockstep as long as no segment both writes a cell
   and reads it across dimension 9 or up: then nobody reads that cell while anybody writes it, and
   every read sees exactly the writes of the segments before. Cuts go in as late as possible.
   first[k] is where segment k starts, first[n] = len; returns n. */
static size_t find_segments(const Program *p, size_t *first)
{
    uint32_t *stored = calloc(p->ncells, sizeof *stored), *read = calloc(p->ncells, sizeof *read);
    if (!stored || !read) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    uint32_t seg = 1;
    size_t n = 0;
    first[n++] = 0;
    for (size_t i = 0; i < p->len; i++) {
        const Instr in = p->code[i];
        if (in.op == OP_NAND) {
            unsigned mask;
            uint32_t a = program_resolve(p, in.addr, &mask);
            if (mask >> CUBE_DIMS && !program_readonly(p, a)) {
                if (stored[a] == seg) {
                    seg++;
                    first[n++] = i;
                }
                read[a] = seg;
            }
        } else if (!program_readonly(p, in.addr)) {
            if (read[in.addr] == seg) {
                seg++;
                first[n++] = i;
            }
            stored[in.addr] = seg;
        }
    }
    first[n] = p->len;
    free(stored);
    free(read);
    return n;
}

/* Instructions first .. last-1 as one function: the whole pass with its loop, or a segment. */
static void emit_range(Jit *j, size_t first, size_t last, int loop_back, int save_top)
{
    const Program *p = j->p;
    memset(j->kind, K_MEM, p->ncells);
    j->kind[CELL_ZERO] = K_ZERO;
    j->kind[CELL_ONE] = K_ONE;
    memset(j->pair_ver, 0, sizeof j->pair_ver);
    memset(j->pair_cells, 0, sizeof j->pair_cells);
    memset(j->pair_used, 0, sizeof j->pair_used);
    j->clock = 0;
    j->rr_kind = RR_MEM;
    j->rr_inv = 0;

    begin(j, loop_back ? "entry" : "segment entry");
    for (int r = SAVED_FROM; r <= save_top; r++) {
        x86_store(&j->x, r, SAVE, 16 * (r - SAVED_FROM), 0, 0);
        logged(j);
    }
    op3(j, X86_VPCMPEQD, ONES, ONES, ONES);
    size_t loop = j->x.len;  /* each pass starts here with RR in memory */
    if (j->literal)
        rr_load(j);

    for (size_t i = first; i < last; i++) {
        const Instr in = p->code[i];
        char nb[16];
        begin(j, "%6zu  %-4s %s", i, in.op == OP_NAND ? "NAND" : "STO",
              cell_name(j, in.addr, nb, sizeof nb));
        if (in.op == OP_NAND && j->literal)
            literal_nand(j, in.addr);
        else if (in.op == OP_NAND)
            compile_nand(j, in.addr);
        else if (j->literal)
            literal_sto(j, in.addr);
        else
            compile_sto(j, in.addr);
        if (j->emitted == 0)
            j->st.free++;
    }

    begin(j, loop_back ? "end of pass" : "end of segment");
    finish_rr(j);
    if (loop_back) {
        x86_dec(&j->x, COUNT);
        logged(j);
        x86_jnz(&j->x, loop);
        logged(j);
    }

    begin(j, "exit");
    for (int r = SAVED_FROM; r <= save_top; r++) {
        x86_load(&j->x, r, SAVE, 16 * (r - SAVED_FROM), 0, 0);
        logged(j);
    }
    x86_vzeroupper(&j->x);
    logged(j);
    x86_ret(&j->x);
    logged(j);
}

/* The whole program: one function, or with nseg segments one function each at seg_off[k]. */
static void emit_pass(Jit *j, int save_top, size_t nseg, const size_t *first, size_t *seg_off)
{
    j->x.len = 0;
    memset(&j->st, 0, sizeof j->st);
    j->top = -1;
    if (!nseg) {
        emit_range(j, 0, j->p->len, 1, save_top);
        return;
    }
    for (size_t k = 0; k < nseg; k++) {
        seg_off[k] = j->x.len;
        emit_range(j, first[k], first[k + 1], 0, save_top);
    }
}

static void *exec_alloc(const uint8_t *code, size_t n)
{
#ifdef _WIN32
    void *m = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m)
        return NULL;
    memcpy(m, code, n);
    DWORD old;
    if (!VirtualProtect(m, n, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(m, 0, MEM_RELEASE);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), m, n);
    return m;
#else
    void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return NULL;
    memcpy(m, code, n);
    if (mprotect(m, n, PROT_READ | PROT_EXEC)) {
        munmap(m, n);
        return NULL;
    }
    return m;
#endif
}

int jit_compile(const Program *p, int literal, JitCode *out, char **listing, char *err,
                size_t errlen)
{
    memset(out, 0, sizeof *out);
    if (listing)
        *listing = NULL;
    if ((uint64_t)(p->ncells + LINE_EXTRA) * 64 > INT32_MAX) {
        snprintf(err, errlen, "too many cells for the compiler (limit %d)", INT32_MAX / 64);
        return 0;
    }

    Jit j;
    memset(&j, 0, sizeof j);
    j.p = p;
    j.literal = literal;
    j.kind = xmalloc(p->ncells);
    j.pair = xmalloc(p->ncells);
    j.ver = xmalloc(p->ncells * sizeof *j.ver);
    j.npeers = lane_peers(p, j.gm);

    size_t nseg = 0;  /* more than 512 machines: the pass in segments */
    if (program_dims(p) > CUBE_DIMS) {
        out->seg_first = xmalloc((p->len + 2) * sizeof *out->seg_first);
        out->seg_off = xmalloc((p->len + 1) * sizeof *out->seg_off);
        nseg = find_segments(p, out->seg_first);
    }
    out->nseg = (unsigned)nseg;

    /* The first pass finds out which registers are used, the second one saves exactly those. */
    emit_pass(&j, -1, nseg, out->seg_first, out->seg_off);
    Text text = {0};
    j.text = listing ? &text : NULL;
    emit_pass(&j, j.top, nseg, out->seg_first, out->seg_off);

    out->code = exec_alloc(j.x.b, j.x.len);
    out->size = j.x.len;
    out->prog = p;
    out->stats = j.st;
    out->stats.bytes = j.x.len;
    free(j.kind);
    free(j.pair);
    free(j.ver);
    x86_free(&j.x);
    if (!out->code) {
        free(text.s);
        free(out->seg_first);
        free(out->seg_off);
        snprintf(err, errlen, "cannot allocate executable memory");
        return 0;
    }
    if (listing)
        *listing = text.s;
    return 1;
}

void jit_free(JitCode *j)
{
    free(j->seg_first);
    free(j->seg_off);
    if (j->code) {
#ifdef _WIN32
        VirtualFree(j->code, 0, MEM_RELEASE);
#else
        munmap(j->code, j->size);
#endif
    }
    memset(j, 0, sizeof *j);
}

typedef void (*RunFn)(Line *mem, Line *rr, void *save, uint64_t passes);

void jit_run(const JitCode *j, LaneMachine *m, uint64_t passes)
{
    if (j->nseg) {  /* more than 512 machines: only as part of the whole cube */
        assert(!"jit_run on a program for more than 512 machines: use jit_cube_run");
        return;
    }
    if (passes && m->pc != 0) {  /* a pass started with lane_step: let the interpreter finish it */
        lane_tick(m);
        passes--;
    }
    if (!passes)
        return;
    RunFn fn;
    memcpy(&fn, &j->code, sizeof fn);
    _Alignas(16) uint8_t save[160];
    fn(m->mem, &m->rr, save, passes);
    m->steps += passes * m->prog->len;
    m->ticks += passes;
}

void jit_tick(const JitCode *j, LaneMachine *m)
{
    jit_run(j, m, 1);
}

/* ---- more than 512 machines (step 7f) ----------------------------------------------------- */

typedef void (*SegFn)(Line *mem, Line *rr, void *save, Line **peer);

void jit_segment(const JitCode *j, unsigned k, LaneMachine *m)
{
    SegFn fn;
    void *at = (uint8_t *)j->code + j->seg_off[k];
    memcpy(&fn, &at, sizeof fn);
    _Alignas(16) uint8_t save[160];
    fn(m->mem, &m->rr, save, m->peer);
}

void jit_cube_run(const JitCode *j, LaneCube *c, uint64_t passes)
{
    if (!j->nseg) {  /* 512 machines */
        jit_run(j, &c->g[0], passes);
        return;
    }
    if (passes && c->g[0].pc != 0) {
        lane_cube_tick(c);
        passes--;
    }
    for (uint64_t n = 0; n < passes; n++)
        for (unsigned k = 0; k < j->nseg; k++)
            for (unsigned g = 0; g < c->groups; g++)
                jit_segment(j, k, &c->g[g]);
    for (unsigned g = 0; g < c->groups; g++) {
        c->g[g].steps += passes * c->prog->len;
        c->g[g].ticks += passes;
    }
}

void jit_cube_tick(const JitCode *j, LaneCube *c)
{
    jit_cube_run(j, c, 1);
}

