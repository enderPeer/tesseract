#include "gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static uint32_t cellf(Builder *b, const char *fmt, int i, int j)
{
    char name[64];
    snprintf(name, sizeof name, fmt, i, j);
    return b_cell(b, name);
}

/* s = x ^ y, k = x & y; s may be x */
static void half_adder(Builder *b, uint32_t s, uint32_t k, uint32_t x, uint32_t y)
{
    b_half_adder(b, s, k, x, y);
}

/* s = x ^ y ^ c, k = carry; s may be x or y and k may be c */
static void full_adder(Builder *b, uint32_t s, uint32_t k, uint32_t x, uint32_t y, uint32_t c)
{
    b_full_adder(b, s, k, x, y, c);
}

/* pc[0 .. 2n-1] = a * bv, n bits each; pc is neither a nor bv */
static void mul_full(Builder *b, const uint32_t *pc, const uint32_t *a, const uint32_t *bv, int n)
{
    uint32_t pp = b_cell(b, "pp"), carry = b_cell(b, "carry"), zero = CELL_ZERO;
    for (int i = 0; i < n; i++)  /* row 0: p = a * b0, the upper half 0 */
        b_gate2(b, G_AND, pc[i], a[i], bv[0]);
    for (int k = n; k < 2 * n; k++)
        b_gate(b, G_BUF, pc[k], &zero, 1);
    for (int j = 1; j < n; j++) {  /* row j: p[j ..] += a * bj, carry out into p[j + n] */
        for (int i = 0; i < n; i++) {
            b_gate2(b, G_AND, pp, a[i], bv[j]);
            if (i == 0)
                half_adder(b, pc[j], carry, pc[j], pp);
            else
                full_adder(b, pc[i + j], i == n - 1 ? pc[j + n] : carry, pc[i + j], pp, carry);
        }
    }
}

void gen_mul(Program *p, int n)
{
    Builder *b = b_new();
    uint32_t *a = xmalloc((size_t)n * sizeof *a), *bv = xmalloc((size_t)n * sizeof *bv);
    uint32_t *pc = xmalloc(2 * (size_t)n * sizeof *pc);
    for (int i = 0; i < n; i++)
        b_input(b, a[i] = cellf(b, "a%d", i, 0));
    for (int i = 0; i < n; i++)
        b_input(b, bv[i] = cellf(b, "b%d", i, 0));
    for (int k = 0; k < 2 * n; k++)
        pc[k] = cellf(b, "p%d", k, 0);
    mul_full(b, pc, a, bv, n);
    for (int k = 0; k < 2 * n; k++)
        b_output(b, pc[k]);
    b_finish(b, p);
    free(a);
    free(bv);
    free(pc);
}

/* ---- toy hash ----------------------------------------------------------------------------- */

static int gcd(int a, int b)
{
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int perm_mult(int n)
{
    static const int m[] = {5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61};
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (gcd(m[i], n) == 1)
            return m[i];
    return 0;
}

static uint64_t round_constant(int r, int bits)
{
    uint64_t z = (uint64_t)(r + 1) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return bits == 64 ? z : z & ((1ull << bits) - 1);
}

uint64_t toyhash(uint64_t x, int n, int rounds)
{
    int m = perm_mult(n);
    for (int r = 0; r < rounds; r++) {
        uint64_t y = 0, z = 0;
        for (int i = 0; i < n; i++) {
            uint64_t xi = (x >> i) & 1, x1 = (x >> ((i + 1) % n)) & 1, x2 = (x >> ((i + 2) % n)) & 1;
            y |= (xi ^ (~x1 & x2 & 1)) << i;
        }
        for (int i = 0; i < n; i++)
            z |= ((y >> ((m * i + 3) % n)) & 1) << i;
        x = z ^ round_constant(r, n);
    }
    return x;
}

int gen_hash(Program *p, int n, int rounds)
{
    int m = perm_mult(n);
    if (!m)
        return 0;
    Builder *b = b_new();
    uint32_t *cur = xmalloc((size_t)n * sizeof *cur), *nxt = xmalloc((size_t)n * sizeof *nxt);
    for (int i = 0; i < n; i++)
        b_input(b, cur[i] = cellf(b, "x%d", i, 0));
    uint32_t nt = b_cell(b, "nt"), tt = b_cell(b, "tt");
    for (int r = 0; r < rounds; r++) {
        int bank = r % 2;  /* round r writes one bank and reads the other */
        uint64_t rc = round_constant(r, n);
        for (int i = 0; i < n; i++) {
            uint32_t y = cellf(b, "y%d_%d", bank, i);
            b_gate(b, G_NOT, nt, &cur[(i + 1) % n], 1);
            b_gate2(b, G_AND, tt, nt, cur[(i + 2) % n]);
            b_gate2(b, G_XOR, y, cur[i], tt);
        }
        for (int i = 0; i < n; i++) {
            uint32_t src = cellf(b, "y%d_%d", bank, (m * i + 3) % n);
            if ((rc >> i) & 1) {
                nxt[i] = cellf(b, "k%d_%d", bank, i);
                b_gate(b, G_NOT, nxt[i], &src, 1);
            } else {
                nxt[i] = src;
            }
        }
        memcpy(cur, nxt, (size_t)n * sizeof *cur);
    }
    for (int i = 0; i < n; i++) {
        uint32_t h = cellf(b, "h%d", i, 0);
        b_gate(b, G_BUF, h, &cur[i], 1);
        b_output(b, h);
    }
    b_finish(b, p);
    free(cur);
    free(nxt);
    return 1;
}

/* ---- Game of Life ------------------------------------------------------------------------- */

/* One generation of Life on the w x h cells board[] (registers: they change at the end of the
   pass). halo[(r + 1) * (w + 2) + c + 1] is the cell at row r, column c for r = -1 .. h and
   c = -1 .. w: the board itself inside, the edges of the neighbouring boards around it. */
static void life_step(Builder *b, const uint32_t *board, const uint32_t *halo, int w, int h)
{
    uint32_t s0 = b_cell(b, "s0"), k0 = b_cell(b, "k0"), s1 = b_cell(b, "s1"),
             k1 = b_cell(b, "k1"), s2 = b_cell(b, "s2"), k2 = b_cell(b, "k2"),
             c0 = b_cell(b, "c0"), k3 = b_cell(b, "k3"), t = b_cell(b, "t"), u = b_cell(b, "u"),
             c1 = b_cell(b, "c1"), v = b_cell(b, "v"), r1 = b_cell(b, "r1"), r2 = b_cell(b, "r2");
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            uint32_t nb[8];
            int k = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++)
                    if (dr || dc)
                        nb[k++] = halo[(r + 1 + dr) * (w + 2) + c + 1 + dc];
            /* count the 8 neighbours: bits c0 (1), c1 (2), and u, v (4 each) */
            full_adder(b, s0, k0, nb[0], nb[1], nb[2]);
            full_adder(b, s1, k1, nb[3], nb[4], nb[5]);
            half_adder(b, s2, k2, nb[6], nb[7]);
            full_adder(b, c0, k3, s0, s1, s2);
            full_adder(b, t, u, k0, k1, k2);
            half_adder(b, c1, v, t, k3);
            /* alive next = count is 2 or 3 (c1 set, no 4s) and (count is odd or alive now) */
            uint32_t self = board[r * w + c], next = cellf(b, "n%d_%d", r, c);
            uint32_t all[3] = {c1, r1, r2};
            b_gate2(b, G_OR, r1, c0, self);
            b_gate2(b, G_NOR, r2, u, v);
            b_gate(b, G_AND, next, all, 3);
            b_reg(b, self, next);
        }
    }
}

static uint32_t *board_cells(Builder *b, int w, int h, uint32_t *board)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            board[r * w + c] = cellf(b, "x%d_%d", r, c);
    return xmalloc((size_t)(w + 2) * (size_t)(h + 2) * sizeof(uint32_t));
}

void gen_life(Program *p, int w, int h, uint32_t *board)
{
    Builder *b = b_new();
    uint32_t *halo = board_cells(b, w, h, board);
    for (int r = -1; r <= h; r++)  /* a torus: the board's own opposite edges */
        for (int c = -1; c <= w; c++)
            halo[(r + 1) * (w + 2) + c + 1] = board[((r + h) % h) * w + (c + w) % w];
    life_step(b, board, halo, w, h);
    free(halo);
    b_finish(b, p);
}

/* ---- grids across the cube (step 7e) ------------------------------------------------------ */

unsigned grid_machine(unsigned row, unsigned col, int cbits)
{
    return (row ^ row >> 1) << cbits | (col ^ col >> 1);
}

/* The axis of dimensions lo .. lo+k-1, positions numbered in Gray code: position i sits in the
   machines with i ^ (i >> 1) in those bits. next[j] (prev[j]) becomes 1 in the machines whose next
   (previous) position is across dimension lo + j. From an even position (even parity of the bits)
   the next one is across lo and the previous one across the dimension above the lowest 1 bit; from
   an odd position the other way round; the last and the first position meet across the top. */
void grid_masks(Builder *b, int lo, int k, uint32_t *next, uint32_t *prev, const char *tag)
{
    char name[32];
    uint32_t id[CUBE_DIMS], v[4], zero = CELL_ZERO;
    for (int j = 0; j < k; j++) {
        id[j] = cellf(b, "ID%d", lo + j, 0);
        snprintf(name, sizeof name, "%s_n%d", tag, j);
        next[j] = b_cell(b, name);
        snprintf(name, sizeof name, "%s_v%d", tag, j);
        prev[j] = b_cell(b, name);
    }
    for (int i = 0; i < 4; i++) {
        snprintf(name, sizeof name, "%s_%c", tag, "pqol"[i]);
        v[i] = b_cell(b, name);
    }
    uint32_t odd = v[0], even = v[1], below = v[2], low = v[3];
    b_gate(b, G_XOR, odd, id, k);
    b_gate(b, G_NOT, even, &odd, 1);
    b_gate(b, G_BUF, next[0], &even, 1);
    b_gate(b, G_BUF, prev[0], &odd, 1);
    b_gate(b, G_BUF, below, &zero, 1);  /* some bit below j is 1 */
    for (int j = 0; j < k; j++) {
        b_gate(b, G_NOT, low, &below, 1);
        b_gate2(b, G_AND, low, low, id[j]);  /* the lowest 1 bit is j */
        if (j + 1 < k) {
            b_gate2(b, G_AND, next[j + 1], low, odd);
            b_gate2(b, G_AND, prev[j + 1], low, even);
        } else {  /* only the top bit: the last position, whose next one is the first */
            b_gate2(b, G_AND, low, low, odd);
            b_gate2(b, G_OR, next[j], next[j], low);
        }
        b_gate2(b, G_OR, below, below, id[j]);
    }
    b_gate(b, G_NOT, low, &below, 1);  /* all 0: the first position, whose previous one is the last */
    b_gate2(b, G_OR, prev[k - 1], prev[k - 1], low);
}

/* the new cell `name` = src of the machine that mask[0 .. k-1] points to, across lo .. lo+k-1 */
uint32_t grid_gather(Builder *b, const char *name, uint32_t src, int lo, int k,
                     const uint32_t *mask)
{
    uint32_t t[CUBE_DIMS], out = b_cell(b, name);
    char link[48];
    for (int j = 0; j < k; j++) {
        snprintf(link, sizeof link, "lk%u_%d", (unsigned)src, lo + j);
        uint32_t l = b_has(b, link) ? b_cell(b, link) : b_link(b, link, src, lo + j);
        b_gate2(b, G_NAND, t[j] = cellf(b, "gt%d", j, 0), mask[j], l);
    }
    b_gate(b, G_NAND, out, t, k);  /* the OR of mask & link: only one mask is 1 */
    return out;
}

void gen_life_cube(Program *p, int w, int h, int rbits, int cbits, uint32_t *board)
{
    Builder *b = b_new();
    int W = w + 2;
    uint32_t *halo = board_cells(b, w, h, board), right[CUBE_DIMS], left[CUBE_DIMS],
             down[CUBE_DIMS], up[CUBE_DIMS];
    char name[32];
    if (cbits)
        grid_masks(b, 0, cbits, right, left, "gc");
    if (rbits)
        grid_masks(b, cbits, rbits, down, up, "gr");
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            halo[(r + 1) * W + c + 1] = board[r * w + c];
    /* the left and right edges: the last column of the board on the left, the first on the right */
    for (int r = 0; r < h; r++) {
        uint32_t last = board[r * w + w - 1], first = board[r * w];
        snprintf(name, sizeof name, "hw%d", r);
        halo[(r + 1) * W] = cbits ? grid_gather(b, name, last, 0, cbits, left) : last;
        snprintf(name, sizeof name, "he%d", r);
        halo[(r + 1) * W + w + 1] = cbits ? grid_gather(b, name, first, 0, cbits, right) : first;
    }
    /* the top and bottom edges, corners included: the last row of the board above with the edges
       it just got from its own left and right, and the first row of the board below */
    for (int c = 0; c < W; c++) {
        uint32_t last = halo[h * W + c], first = halo[W + c];
        snprintf(name, sizeof name, "hn%d", c);
        halo[c] = rbits ? grid_gather(b, name, last, cbits, rbits, up) : last;
        snprintf(name, sizeof name, "hs%d", c);
        halo[(h + 1) * W + c] = rbits ? grid_gather(b, name, first, cbits, rbits, down) : first;
    }
    life_step(b, board, halo, w, h);
    free(halo);
    b_finish(b, p);
}

/* ---- running count over a bigger cube (step 7f) ------------------------------------------- */

/* Round d: n = t of the machine across d; the machine with bit d set adds n to p (that half of the
   pair has the smaller numbers), and both add it to t. t lives in two banks, read from one and
   written to the other, so on a cube of more than 512 machines a round across groups needs one
   cut of the compiled pass, not two. */
void gen_rank(Program *p, int dims)
{
    Builder *b = b_new();
    int w = dims + 1;
    uint32_t x = b_cell(b, "x"), pc[CUBE_MAX_DIMS + 1], t[2][CUBE_MAX_DIMS + 1], add[CUBE_MAX_DIMS + 1];
    uint32_t zero = CELL_ZERO, c = b_cell(b, "rk_c"), a = b_cell(b, "rk_a");
    char name[32];
    b_input(b, x);
    for (int i = 0; i < w; i++) {
        pc[i] = cellf(b, "p%d", i, 0);
        t[dims % 2][i] = cellf(b, "t%d", i, 0);   /* the bank written last is the output */
        t[1 - dims % 2][i] = cellf(b, "u%d", i, 0);
    }
    for (int i = 0; i < w; i++) {
        b_gate(b, G_BUF, pc[i], &zero, 1);
        b_gate(b, G_BUF, t[0][i], i ? &zero : &x, 1);
        b_gate(b, G_BUF, t[1][i], &zero, 1);
    }
    for (int d = 0; d < dims; d++) {
        const uint32_t *cur = t[d % 2], *nxt = t[1 - d % 2];
        uint32_t id = cellf(b, "ID%d", d, 0), n[CUBE_MAX_DIMS + 1];
        for (int i = 0; i <= d; i++) {  /* t < 2^(d+1) before round d: bits 0 .. d */
            snprintf(name, sizeof name, "rk%d_%d", d, i);
            n[i] = b_link(b, name, cur[i], d);
        }
        b_gate(b, G_BUF, c, &zero, 1);
        for (int i = 0; i <= d; i++)
            full_adder(b, nxt[i], c, cur[i], n[i], c);
        b_gate(b, G_BUF, nxt[d + 1], &c, 1);
        b_gate(b, G_BUF, c, &zero, 1);
        for (int i = 0; i < w; i++) {
            if (i <= d)
                b_gate2(b, G_AND, add[i] = a, n[i], id);
            else
                add[i] = zero;
            full_adder(b, pc[i], c, pc[i], add[i], c);
        }
    }
    for (int i = 0; i < w; i++)
        b_output(b, pc[i]);
    for (int i = 0; i < w; i++)
        b_output(b, t[dims % 2][i]);
    b_finish(b, p);
}

/* ---- transforms on the cube (step 7d) ----------------------------------------------------- */

/* w <- its Walsh-Hadamard transform over the 512 machines, on `bits`-bit numbers (mod 2^bits).
   Round d pairs machine u (bit d = 0) with v (bit d = 1): u gets u + v, v gets u - v. Both are one
   addition, x + n + c, with x = own value XOR IDd (all ones in v), n = the neighbour's value across
   d and carry in c = IDd. All machines run in lockstep, so each reads its neighbour's bit i before
   anyone writes bit i. */
static void wht(Builder *b, const uint32_t *w, int bits, const char *tag)
{
    char name[64];
    snprintf(name, sizeof name, "%s_x", tag);
    uint32_t x = b_cell(b, name);
    snprintf(name, sizeof name, "%s_c", tag);
    uint32_t c = b_cell(b, name);
    for (int d = 0; d < CUBE_DIMS; d++) {
        snprintf(name, sizeof name, "ID%d", d);
        uint32_t id = b_cell(b, name);
        b_gate(b, G_BUF, c, &id, 1);
        for (int i = 0; i < bits; i++) {
            snprintf(name, sizeof name, "%s_n%d_%d", tag, d, i);
            uint32_t n = b_link(b, name, w[i], d);
            b_gate2(b, G_XOR, x, w[i], id);
            full_adder(b, w[i], c, x, n, c);
        }
    }
}

void gen_wht(Program *p, int bits)
{
    Builder *b = b_new();
    uint32_t f = b_cell(b, "f"), one = CELL_ONE;
    uint32_t *w = xmalloc((size_t)bits * sizeof *w);
    b_input(b, f);
    for (int i = 0; i < bits; i++)  /* (-1)^f = 1 - 2f: bit 0 is 1, the others are f */
        b_gate(b, G_BUF, w[i] = cellf(b, "w%d", i, 0), i == 0 ? &one : &f, 1);
    wht(b, w, bits, "a");
    for (int i = 0; i < bits; i++)
        b_output(b, w[i]);
    b_finish(b, p);
    free(w);
}

/* acc = x * y mod 2^bits (the low half of a multiplier; the same in two's complement) */
static void mul_low(Builder *b, uint32_t *acc, const uint32_t *x, const uint32_t *y, int bits)
{
    uint32_t pp = b_cell(b, "m_pp"), carry = b_cell(b, "m_carry");
    for (int k = 0; k < bits; k++)
        b_gate2(b, G_AND, acc[k], x[k], y[0]);
    for (int j = 1; j < bits; j++) {
        for (int k = j; k < bits; k++) {
            b_gate2(b, G_AND, pp, x[k - j], y[j]);
            if (k == j)
                half_adder(b, acc[k], carry, acc[k], pp);
            else
                full_adder(b, acc[k], carry, acc[k], pp, carry);
        }
    }
}

void gen_xorconv(Program *p)
{
    enum { BITS = 20 };  /* 512 h <= 2^18 fits; everything else may wrap around mod 2^20 */
    Builder *b = b_new();
    uint32_t f = b_cell(b, "f"), g = b_cell(b, "g"), zero = CELL_ZERO;
    uint32_t fw[BITS], gw[BITS], pw[BITS];
    b_input(b, f);
    b_input(b, g);
    for (int i = 0; i < BITS; i++) {
        b_gate(b, G_BUF, fw[i] = cellf(b, "F%d", i, 0), i == 0 ? &f : &zero, 1);
        b_gate(b, G_BUF, gw[i] = cellf(b, "G%d", i, 0), i == 0 ? &g : &zero, 1);
        pw[i] = cellf(b, "P%d", i, 0);
    }
    wht(b, fw, BITS, "F");
    wht(b, gw, BITS, "G");
    mul_low(b, pw, fw, gw, BITS);  /* the spectrum of the convolution */
    wht(b, pw, BITS, "P");         /* back: 512 times the convolution */
    for (int i = 0; i < 10; i++) {
        uint32_t h = cellf(b, "h%d", i, 0);
        b_gate(b, G_BUF, h, &pw[9 + i], 1);  /* divide by 512 */
        b_output(b, h);
    }
    b_finish(b, p);
}

/* ---- polynomials mod x^512 + 1 with the number-theoretic transform ------------------------ */

enum { QB = 14 };          /* bits of a number mod q */
#define NO_CELL UINT32_MAX

/* out = x + y + cin over n bits, the carry out into cout unless it is NO_CELL; out may be x or y */
static void add_n(Builder *b, const uint32_t *out, const uint32_t *x, const uint32_t *y,
                  uint32_t cin, int n, uint32_t cout)
{
    uint32_t c = b_cell(b, "add_c");
    b_gate(b, G_BUF, c, &cin, 1);
    for (int i = 0; i < n; i++)
        full_adder(b, out[i], c, x[i], y[i], c);
    if (cout != NO_CELL)
        b_gate(b, G_BUF, cout, &c, 1);
}

/* out = sel ? x1 : x0 over n bits; out may be x0 or x1 */
static void mux_n(Builder *b, const uint32_t *out, uint32_t sel, const uint32_t *x1,
                  const uint32_t *x0, int n)
{
    uint32_t t = b_cell(b, "mux_t");
    for (int i = 0; i < n; i++) {
        b_gate2(b, G_XOR, t, x0[i], x1[i]);
        b_gate2(b, G_AND, t, t, sel);
        b_gate2(b, G_XOR, out[i], x0[i], t);
    }
}

/* r = x * y / 2^14 mod q (Montgomery), all in 0 .. q-1; r may be x or y. With t = x * y and
   m = t / q mod 2^14, t - m q is a multiple of 2^14, so (t - m q) / 2^14 = t / 2^14 - m q / 2^14
   (rounded down both), between -q and q. q = 3 * 2^12 + 1 makes this cheap: 1/q = 2^12 + 1
   mod 2^14, so m differs from t only in bits 12 and 13, and m q = (3m + m / 2^12) * 2^12 +
   m mod 2^12, whose upper part is (3m + m / 2^12) / 4. */
static void montmul(Builder *b, const uint32_t *r, const uint32_t *x, const uint32_t *y)
{
    uint32_t t[2 * QB], m[QB], m1[16], m2[16], s[16], th[15], sh[15], h[15], qm[QB];
    uint32_t zero = CELL_ZERO, one = CELL_ONE, k = b_cell(b, "mm_k");
    for (int i = 0; i < 2 * QB; i++)
        t[i] = cellf(b, "mm_t%d", i, 0);
    for (int i = 0; i < 16; i++)
        s[i] = cellf(b, "mm_s%d", i, 0);
    for (int i = 0; i < 15; i++)
        h[i] = cellf(b, "mm_h%d", i, 0);
    mul_full(b, t, x, y, QB);  /* t < q^2 < 2^28 */
    for (int i = 0; i < 12; i++)
        m[i] = t[i];
    m[12] = b_cell(b, "mm_m12");
    m[13] = b_cell(b, "mm_m13");
    b_gate2(b, G_AND, k, t[12], t[0]);  /* m = t + (t << 12) mod 2^14 */
    b_gate2(b, G_XOR, m[12], t[12], t[0]);
    b_gate2(b, G_XOR, m[13], t[13], t[1]);
    b_gate2(b, G_XOR, m[13], m[13], k);
    for (int i = 0; i < 16; i++) {  /* s = m + m / 2^12 + 2m */
        m1[i] = i < QB ? m[i] : zero;
        m2[i] = i < 2 ? m[12 + i] : zero;
    }
    add_n(b, s, m1, m2, zero, 16, NO_CELL);
    for (int i = 0; i < 16; i++)
        m2[i] = i >= 1 && i <= QB ? m[i - 1] : zero;
    add_n(b, s, s, m2, zero, 16, NO_CELL);
    for (int i = 0; i < 15; i++) {  /* h = t / 2^14 - s / 4, 15-bit two's complement */
        th[i] = i < QB ? t[QB + i] : zero;
        if (i < QB)
            b_gate(b, G_NOT, sh[i] = cellf(b, "mm_n%d", i, 0), &s[2 + i], 1);
        else
            sh[i] = one;
    }
    add_n(b, h, th, sh, one, 15, NO_CELL);
    for (int i = 0; i < QB; i++)  /* r = h + (h < 0 ? q : 0) */
        qm[i] = ((NTT_Q >> i) & 1) ? h[14] : zero;
    add_n(b, r, h, qm, zero, QB, NO_CELL);
}

/* out = sel ? x - y : x + y, mod q, all in 0 .. q-1; out may be x or y */
static void addsub_q(Builder *b, const uint32_t *out, uint32_t sel, const uint32_t *x,
                     const uint32_t *y)
{
    uint32_t s[15], v[15], d[15], k[15], x15[15], zero = CELL_ZERO, one = CELL_ONE;
    uint32_t cy = b_cell(b, "as_cy");
    for (int i = 0; i < 15; i++) {
        s[i] = cellf(b, "as_s%d", i, 0);
        d[i] = cellf(b, "as_d%d", i, 0);
        x15[i] = i < QB ? x[i] : zero;
        k[i] = ((NTT_Q >> i) & 1) ? sel : zero;
    }
    add_n(b, s, x15, k, zero, 15, NO_CELL);  /* s = x + (sel ? q : 0) */
    for (int i = 0; i < QB; i++)
        b_gate2(b, G_XOR, v[i] = cellf(b, "as_v%d", i, 0), y[i], sel);
    v[QB] = sel;
    add_n(b, s, s, v, sel, 15, NO_CELL);  /* s += sel ? -y : y: x + y or x + q - y, below 2q */
    for (int i = 0; i < 15; i++)
        k[i] = (((32768 - NTT_Q) >> i) & 1) ? one : zero;
    add_n(b, d, s, k, zero, 15, cy);  /* d = s - q, carrying out exactly when s >= q */
    mux_n(b, out, cy, d, s, QB);
}

/* a <- its negacyclic NTT, machine k holding a_k: rounds d = 8 .. 0 of Cooley-Tukey butterflies,
   a_u, a_v -> a_u + z a_v, a_u - z a_v for the machines u, v = u + 2^d, with z from input z[d] */
static void ntt_forward(Builder *b, const uint32_t *a, uint32_t z[CUBE_DIMS][QB], const char *tag)
{
    uint32_t t[QB], x[QB], y[QB], la[QB], lt[QB];
    char name[64];
    for (int i = 0; i < QB; i++) {
        t[i] = cellf(b, "nt_t%d", i, 0);
        x[i] = cellf(b, "nt_x%d", i, 0);
        y[i] = cellf(b, "nt_y%d", i, 0);
    }
    for (int d = CUBE_DIMS - 1; d >= 0; d--) {
        uint32_t id = cellf(b, "ID%d", d, 0);
        montmul(b, t, a, z[d]);  /* used in the machines with bit d set */
        for (int i = 0; i < QB; i++) {
            snprintf(name, sizeof name, "%s_a%d_%d", tag, d, i);
            la[i] = b_link(b, name, a[i], d);
            snprintf(name, sizeof name, "%s_t%d_%d", tag, d, i);
            lt[i] = b_link(b, name, t[i], d);
        }
        mux_n(b, x, id, la, a, QB);  /* x = a_u */
        mux_n(b, y, id, t, lt, QB);  /* y = z a_v */
        addsub_q(b, a, id, x, y);
    }
}

/* the way back, without the factor 1/512: rounds d = 0 .. 8 of Gentleman-Sande butterflies,
   a_u, a_v -> a_u + a_v, (a_u - a_v) / z, the multiplier from input w[d] (1 in machine u) */
static void ntt_inverse(Builder *b, const uint32_t *a, uint32_t w[CUBE_DIMS][QB], const char *tag)
{
    uint32_t x[QB], y[QB], la[QB];
    char name[64];
    for (int i = 0; i < QB; i++) {
        x[i] = cellf(b, "nt_x%d", i, 0);
        y[i] = cellf(b, "nt_y%d", i, 0);
    }
    for (int d = 0; d < CUBE_DIMS; d++) {
        uint32_t id = cellf(b, "ID%d", d, 0);
        for (int i = 0; i < QB; i++) {
            snprintf(name, sizeof name, "%s_a%d_%d", tag, d, i);
            la[i] = b_link(b, name, a[i], d);
        }
        mux_n(b, x, id, la, a, QB);  /* x = a_u */
        mux_n(b, y, id, a, la, QB);  /* y = a_v */
        addsub_q(b, a, id, x, y);
        montmul(b, a, a, w[d]);
    }
}

static uint32_t pow_q(uint32_t x, uint32_t e)
{
    uint64_t r = 1, y = x;
    for (; e; e >>= 1, y = y * y % NTT_Q)
        if (e & 1)
            r = r * y % NTT_Q;
    return (uint32_t)r;
}

static unsigned rev9(unsigned k)
{
    unsigned r = 0;
    for (int i = 0; i < CUBE_DIMS; i++)
        r |= (k >> i & 1) << (CUBE_DIMS - 1 - i);
    return r;
}

void ntt_twiddles(uint16_t zf[CUBE_DIMS][CUBE_SIZE], uint16_t zi[CUBE_DIMS][CUBE_SIZE])
{
    uint32_t g = 2, mont = (1u << QB) % NTT_Q;
    while (pow_q(g, (NTT_Q - 1) / 2) == 1 || pow_q(g, (NTT_Q - 1) / 3) == 1)
        g++;  /* a generator: 12288 = 2^12 * 3 */
    uint32_t psi = pow_q(g, (NTT_Q - 1) / 1024);  /* a 1024th root of 1: psi^512 = -1 */
    for (int d = 0; d < CUBE_DIMS; d++)
        for (unsigned k = 0; k < CUBE_SIZE; k++) {
            /* the k-th zeta of the usual in-place loop: round d is block k / 2^(d+1) of 2^(8-d) */
            uint32_t z = pow_q(psi, rev9((256u >> d) + (k >> (d + 1))));
            zf[d][k] = (uint16_t)((uint64_t)z * mont % NTT_Q);
            zi[d][k] = (uint16_t)((k >> d) & 1 ? (uint64_t)pow_q(z, NTT_Q - 2) * mont % NTT_Q : mont);
        }
}

void gen_polymul(Program *p)
{
    Builder *b = b_new();
    uint32_t a[QB], bv[QB], c[QB], wa[QB], wb[QB], f[QB], zf[CUBE_DIMS][QB], zi[CUBE_DIMS][QB];
    char name[64];
    for (int i = 0; i < QB; i++)
        b_input(b, a[i] = cellf(b, "a%d", i, 0));
    for (int i = 0; i < QB; i++)
        b_input(b, bv[i] = cellf(b, "b%d", i, 0));
    for (int d = 0; d < CUBE_DIMS; d++)
        for (int i = 0; i < QB; i++) {
            snprintf(name, sizeof name, "zf%d_%d", d, i);
            b_input(b, zf[d][i] = b_cell(b, name));
        }
    for (int d = 0; d < CUBE_DIMS; d++)
        for (int i = 0; i < QB; i++) {
            snprintf(name, sizeof name, "zi%d_%d", d, i);
            b_input(b, zi[d][i] = b_cell(b, name));
        }
    /* 2^28 / 512 mod q: undoes the 1/2^14 of two Montgomery products and the 512 of the way back */
    uint32_t fc = pow_q((1u << QB) % NTT_Q, 2) * (uint32_t)pow_q(CUBE_SIZE, NTT_Q - 2) % NTT_Q;
    for (int i = 0; i < QB; i++) {
        c[i] = cellf(b, "c%d", i, 0);
        b_gate(b, G_BUF, wa[i] = cellf(b, "wa%d", i, 0), &a[i], 1);
        b_gate(b, G_BUF, wb[i] = cellf(b, "wb%d", i, 0), &bv[i], 1);
        f[i] = (fc >> i) & 1 ? CELL_ONE : CELL_ZERO;
    }
    ntt_forward(b, wa, zf, "fa");
    ntt_forward(b, wb, zf, "fb");
    montmul(b, wa, wa, wb);  /* the product, point by point */
    ntt_inverse(b, wa, zi, "ia");
    montmul(b, c, wa, f);
    for (int i = 0; i < QB; i++)
        b_output(b, c[i]);
    b_finish(b, p);
}

void polymul_scalar(const uint16_t *a, const uint16_t *b, uint16_t *c)
{
    uint64_t s[CUBE_SIZE] = {0};
    for (unsigned i = 0; i < CUBE_SIZE; i++)
        for (unsigned j = 0; j < CUBE_SIZE; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] % NTT_Q;
            if (i + j < CUBE_SIZE)
                s[i + j] += t;
            else
                s[i + j - CUBE_SIZE] += NTT_Q - t;  /* x^512 = -1 */
        }
    for (unsigned k = 0; k < CUBE_SIZE; k++)
        c[k] = (uint16_t)(s[k] % NTT_Q);
}

void life_scalar(uint8_t *board, int w, int h)
{
    uint8_t *next = xmalloc((size_t)w * (size_t)h);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++)
                    if (dr || dc)
                        n += board[((r + dr + h) % h) * w + (c + dc + w) % w];
            next[r * w + c] = (uint8_t)(n == 3 || (n == 2 && board[r * w + c]));
        }
    memcpy(board, next, (size_t)w * (size_t)h);
    free(next);
}
