/* Beta (step 8): the emulator, the assembler, the monitor, and the NAND/STO beta against the
   emulator in all 512 lanes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beta.h"
#include "check.h"
#include "jit.h"
#include "lanes.h"
#include "platform.h"
#include "ref.h"

static int checks, failures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        checks++;                                            \
        if (!(cond)) {                                       \
            failures++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            putchar('\n');                                   \
        }                                                    \
    } while (0)

static uint64_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static int assemble(const char *src, BetaImage *img)
{
    char err[512];
    if (beta_assemble(src, "<test>", img, err, sizeof err))
        return 1;
    printf("FAIL assembling: %s\n", err);
    failures++;
    return 0;
}

/* Runs src (at its .org) on the emulator from a fresh machine, with input, for up to `steps`
   steps or until it halts; out gets what it printed. */
static Beta run(const char *src, const char *input, int steps, char *out, size_t outlen)
{
    Beta m;
    BetaImage img;
    beta_reset(&m);
    if (assemble(src, &img)) {
        memcpy(m.mem, img.bytes, BETA_ROM);
        m.pc = (uint8_t)img.lo;
    }
    BetaLane l;
    memset(&l, 0, sizeof l);
    l.in = (const uint8_t *)input;
    l.nin = strlen(input);
    for (int i = 0; i < steps && !m.halted; i++)
        beta_emu_pass(&m, &l);
    size_t n = l.nout < outlen - 1 ? l.nout : outlen - 1;
    memcpy(out, l.out, n);
    out[n] = '\0';
    free(l.out);
    return m;
}

/* ---- the instructions --------------------------------------------------------------------- */

static void test_instructions(void)
{
    char out[256];
    Beta m;

    m = run(".org 0\nLDA #200\nADD #100\nSTA 0x50\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.a == 44 && m.c == 1 && m.mem[0x50] == 44 && m.halted && m.pc == 6,
          "ADD: 200 + 100 = 44 with carry (a %u c %u pc %u)", m.a, m.c, m.pc);
    m = run(".org 0\nLDA #200\nADD #100\nLDA #1\nADC #1\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.a == 3 && m.c == 0, "ADC adds the carry: 1 + 1 + 1 = 3 (a %u)", m.a);
    m = run(".org 0\nLDA #5\nSUB #7\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.a == 254 && m.c == 0, "SUB 5 - 7 = 254, borrow: C = 0 (a %u c %u)", m.a, m.c);
    m = run(".org 0\nLDA #7\nSUB #7\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.a == 0 && m.c == 1, "SUB 7 - 7 = 0, no borrow: C = 1");
    m = run(".org 0\nLDA #0xCA\nAND #0x0F\nOR #0x30\nXOR #0xFF\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.a == 0xC5, "AND, OR, XOR: 0x%02X", m.a);
    m = run(".org 0\nLDA #0x81\nSHR\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.a == 0x40 && m.c == 1, "SHR: 0x81 -> 0x40, C = 1");
    m = run(".org 0\nLDA #0x81\nADD A\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.a == 0x02 && m.c == 1, "ADD A doubles: 0x81 -> 0x02 with carry");
    m = run(".org 0\nLDX #3\nLDA t,X\nINX\nTXA\nTAX\nSTX 0x60\nHLT\nt: .byte 9, 8, 7, 6, 5\n", "",
            20, out, sizeof out);
    CHECK(m.a == 4 && m.x == 4 && m.mem[0x60] == 4, "indexed load, INX, TXA, TAX, STX");
    m = run(".org 0\nLDX #2\nLDA #42\nSTA 0x70,X\nLDA 0x72\nHLT\n", "", 20, out, sizeof out);
    CHECK(m.mem[0x72] == 42 && m.a == 42, "indexed store");
    m = run(".org 0\nLDA #0\nJZ yes\nHLT\nyes: LDA #1\nJZ no\nHLT\nno: HLT\n", "", 20, out,
            sizeof out);
    CHECK(m.pc == 10 && m.a == 1, "JZ jumps on 0 only (pc %u)", m.pc);
    m = run(".org 0\nLDA #255\nADD #1\nJC yes\nHLT\nyes: SUB #1\nJC no\nHLT\nno: HLT\n", "", 20, out,
            sizeof out);
    CHECK(m.pc == 12, "JC follows the carry (pc %u)", m.pc);
    m = run(".org 0\nJSR sub\nLDA #7\nHLT\nsub: .byte 0, 0\nLDX #5\nJMP (sub)\n", "", 20, out,
            sizeof out);
    CHECK(m.a == 7 && m.x == 5 && m.mem[6] == 2 && m.pc == 4,
          "JSR keeps the return address in the routine's first byte, JMP () returns");
    m = run(".org 0\nLDA #t2\nJMP A\nHLT\nt2: LDX #9\nLDA #4\nJMP (tab,X)\nHLT\n"
            ".org 0x29\n.byte 0x30\n.org 0x30\nLDA #77\nHLT\ntab = 0x20\n", "", 20, out, sizeof out);
    CHECK(m.a == 77, "JMP A and JMP (table,X): computed jumps (a %u pc %u)", m.a, m.pc);
    m = run(".org 0\nLDA #'X'\nOUT\nIN\nOUT\nIN\nOUT\nHLT\n", "ab", 30, out, sizeof out);
    CHECK(!strcmp(out, "Xab") && m.halted, "OUT and IN: \"%s\"", out);
    m = run(".org 0\nIN\nHLT\n", "", 5, out, sizeof out);
    CHECK(m.waiting && m.pc == 0, "IN without input waits in place");
    m = run(".org 0\nLDA #1\nSTA 0xC0\nSTA 0xA0\nHLT\n", "", 10, out, sizeof out);
    CHECK(m.mem[0xC0] == beta_rom()[0xC0] && m.mem[0xA0] == beta_rom()[0xA0],
          "the ROM ignores writes");
    m = run(".org 0x98\nLDA #3\nJMP 0x9B\n", "", 3, out, sizeof out);
    CHECK(m.pc == 0x9A, "jump targets are even: 0x9B goes to 0x9A (pc %u)", m.pc);

    Beta p;
    beta_reset(&p);
    CHECK(p.pc == BETA_MON && !p.a && !p.mem[0], "power on: in the monitor, RAM 0");
}

/* ---- the assembler ------------------------------------------------------------------------ */

static void test_assembler(void)
{
    BetaImage img;
    if (assemble(".org 4\nlab: LDA #5\nLDA 0x10\nLDA 0x10,X\nLDA A\nSTA lab+1,X\nJMP lab\nJZ (lab)\n"
                 "JC (lab,X)\nJMP A\nIN\nSHR\n.byte 'a', '\\n', -1\n.asciz \"hi\"\n", &img)) {
        static const uint8_t want[] = {0x00, 5,    0x10, 0x10, 0x20, 0x10, 0x30, 0,    0x28, 5,
                                       0x0A, 4,    0x1B, 4,    0x2C, 4,    0x3A, 0,    0x0E, 0,
                                       0x3F, 0,    'a',  '\n', 0xFF, 'h',  'i',  0};
        CHECK(img.lo == 4 && img.hi == 4 + sizeof want && !memcmp(img.bytes + 4, want, sizeof want),
              "encodings of every mode (lo %u hi %u)", img.lo, img.hi);
    }
    if (assemble("x = 'G'-'L'\n.org 0\nLDA #x\nJMP MON\nJMP PUTS\n", &img))
        CHECK(img.bytes[1] == 251 && img.bytes[3] == BETA_MON && img.bytes[5] > BETA_MON,
              "expressions, and the monitor's MON and PUTS");
    static const struct {
        const char *src, *expect;
    } errors[] = {
        {"LDA #300\n", "does not fit"},
        {".byte 1\nLDA #1\n", "odd address"},
        {"JMP nowhere\n", "unknown name"},
        {"STA #3\n", "expected a number"},
        {"FOO 1\n", "unknown instruction"},
        {"LDA\n", "needs an operand"},
        {"IN 3\n", "takes no operand"},
        {".org 0\nLDA #1\n.org 0\nLDA #2\n", "used twice"},
        {"a: .byte 1\na: .byte 2\n", "defined twice"},
        {".org 254\nLDA #1\nLDA #2\n", "past the end"},
    };
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
        char err[512];
        int ok = beta_assemble(errors[i].src, "<test>", &img, err, sizeof err);
        CHECK(!ok && strstr(err, errors[i].expect), "error %zu: expected \"%s\", got \"%s\"", i,
              errors[i].expect, ok ? "(none)" : err);
    }

    /* every instruction the assembler can make comes back from the disassembler */
    int bad = 0;
    for (int b0 = 0; b0 < 64; b0++) {
        unsigned op = b0 & 15, mode = (unsigned)b0 >> 4;
        if (op >= BOP_STA && op <= BOP_STX && mode != BMODE_ABS && mode != BMODE_IDX)
            continue;
        if (op == BOP_JSR && mode != BMODE_ABS && mode != BMODE_IDX)
            continue;
        uint8_t b1 = (op == BOP_IO || op == BOP_REG || mode == BMODE_A) ? 0 : 0x5A;
        char text[48], src[64];
        beta_disasm((uint8_t)b0, b1, text, sizeof text);
        snprintf(src, sizeof src, ".org 0\n%s\n", text);
        char err[256];
        bad += !beta_assemble(src, "<disasm>", &img, err, sizeof err) || img.bytes[0] != b0 ||
               img.bytes[1] != b1;
    }
    CHECK(bad == 0, "disassembly assembles back to the same bytes (%d differ)", bad);
}

/* ---- the monitor, on the emulator --------------------------------------------------------- */

static void test_monitor(void)
{
    BetaImage img;
    if (!assemble(".org 0x10\nLDA #'O'\nOUT\nLDA #'K'\nOUT\nJMP MON\n", &img))
        return;
    uint8_t in[300];
    const uint8_t dump[] = {'D', 0x10, 4, '?', 'G', 0x10};
    size_t n = beta_boot_input(&img, dump, sizeof dump, in, sizeof in);
    Beta m;
    beta_reset(&m);
    BetaLane l;
    memset(&l, 0, sizeof l);
    l.in = in;
    l.nin = n;
    int steps = 0;
    while (!l.done && steps < 10000) {
        beta_emu_pass(&m, &l);
        steps++;
    }
    /* L loads, G runs it (OK), D sends 4 bytes back, '?' is skipped, G runs it again */
    const uint8_t want[] = {'O', 'K', 0x00, 'O', BOP_IO | BIO_OUT << 4, 0x00, 'O', 'K'};
    CHECK(l.done && m.pc == BETA_MON && l.nout == sizeof want && !memcmp(l.out, want, sizeof want),
          "the monitor loads, runs, dumps, skips and runs again (%zu bytes out, pc 0x%02X)", l.nout,
          m.pc);
    free(l.out);
}

/* ---- the NAND/STO beta against the emulator ----------------------------------------------- */

static void same_state(const LaneMachine *lm, const BetaCells *c, const Beta *emu, int *bad,
                       unsigned *first)
{
    for (unsigned k = 0; k < LANES; k++) {
        Beta got;
        beta_from_lane(lm, c, k, &got);
        if (memcmp(&got, &emu[k], sizeof got) && !(*bad)++)
            *first = k;
    }
}

/* Random memory and registers in all 512 lanes (random programs, mostly), random input arriving:
   after every step the whole state equals the emulator's. */
static void test_circuit(const Program *p, const BetaCells *cells, const JitCode *jc)
{
    static Beta emu[LANES];
    LaneMachine lm;
    lane_init(&lm, p);
    uint64_t s = 0xBE7A;
    for (unsigned k = 0; k < LANES; k++) {
        Beta *e = &emu[k];
        beta_reset(e);
        for (int a = 0; a < BETA_ROM; a++) {  /* no HLT, or most would stop within 64 steps */
            e->mem[a] = (uint8_t)rnd(&s);
            if ((e->mem[a] & 0x3F) == (BOP_IO | BIO_HLT << 4))
                e->mem[a] ^= 0x10;
        }
        e->a = (uint8_t)rnd(&s);
        e->x = (uint8_t)rnd(&s);
        e->c = (uint8_t)(rnd(&s) & 1);
        e->pc = (uint8_t)(k % 3 ? rnd(&s) & 0xFE : BETA_MON + 2 * (rnd(&s) % 48));
        e->in = (uint8_t)rnd(&s);
        e->in_valid = (uint8_t)(rnd(&s) & 1);
        e->out = (uint8_t)rnd(&s);
        beta_to_lane(&lm, cells, k, e);
    }
    int bad = 0, outs = 0, ins = 0, jumps = 0;
    unsigned first = 0, step = 0;
    for (step = 0; step < 300 && !bad; step++) {
        for (unsigned k = 0; k < LANES; k++) {  /* the host offers input now and then */
            if (!emu[k].in_valid && rnd(&s) % 3 == 0) {
                emu[k].in = (uint8_t)rnd(&s);
                emu[k].in_valid = 1;
                for (int i = 0; i < 8; i++)
                    lane_set(&lm, cells->in[i], k, (uint8_t)((emu[k].in >> i) & 1));
                lane_set(&lm, cells->in_valid, k, 1);
            }
            uint8_t pc = emu[k].pc;
            beta_step(&emu[k]);
            outs += emu[k].out_strobe;
            ins += (emu[k].mem[pc] & 0x3F) == (BOP_IO | BIO_IN << 4) && !emu[k].waiting;
            jumps += emu[k].pc != (uint8_t)(pc + 2);
        }
        if (jc)
            jit_tick(jc, &lm);
        else
            lane_tick(&lm);
        same_state(&lm, cells, emu, &bad, &first);
    }
    CHECK(bad == 0, "%s beta = emulator in all 512 lanes for %u steps (first wrong: lane %u)",
          jc ? "compiled" : "interpreted", step, first);
    CHECK(outs > 500 && ins > 300 && jumps > 10000, "the random programs did a bit of everything "
          "(%d OUT, %d IN, %d jumps or waits)", outs, ins, jumps);
    lane_free(&lm);
}

/* A few machines on the reference engine itself, for a few steps. */
static void test_reference(const Program *p, const BetaCells *cells)
{
    LaneMachine lm;
    lane_init(&lm, p);
    uint64_t s = 0x5EED;
    Beta e[4];
    RefMachine r[4];
    int bad = 0;
    for (int k = 0; k < 4; k++) {
        beta_reset(&e[k]);
        for (int a = 0; a < BETA_ROM; a++)
            e[k].mem[a] = (uint8_t)rnd(&s);
        e[k].pc = (uint8_t)(rnd(&s) & 0x9E);
        e[k].in_valid = 1;
        e[k].in = 'q';
        beta_to_lane(&lm, cells, (unsigned)k, &e[k]);
        ref_init(&r[k], p);
        for (uint32_t c = CELL_FIRST_FREE; c < p->ncells; c++)
            ref_set(&r[k], c, lane_get(&lm, c, (unsigned)k));
    }
    for (int step = 0; step < 6; step++)
        for (int k = 0; k < 4; k++) {
            beta_step(&e[k]);
            ref_tick(&r[k]);
            for (uint32_t c = CELL_FIRST_FREE; c < p->ncells; c++)
                lane_set(&lm, c, (unsigned)k, ref_get(&r[k], c));
            Beta got;
            beta_from_lane(&lm, cells, (unsigned)k, &got);
            bad += memcmp(&got, &e[k], sizeof got) != 0;
        }
    CHECK(bad == 0, "beta on the reference machine = emulator (%d steps wrong)", bad);
    for (int k = 0; k < 4; k++)
        ref_free(&r[k]);
    lane_free(&lm);
}

/* The examples, booted through the monitor on the compiled beta, every lane its own input. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)n + 1);
    size_t got = fread(t, 1, (size_t)n, f);
    t[got] = '\0';
    fclose(f);
    return t;
}

static void primes_below(unsigned n, char *out)
{
    out[0] = '\0';
    for (unsigned k = 2; k < n; k++) {
        unsigned d = 2;
        while (k % d)
            d++;
        if (d == k)
            out += sprintf(out, "%u ", k);
    }
    strcpy(out, "\n");
}

static void test_examples(const Program *p, const BetaCells *cells, const JitCode *jc)
{
    static const char *const names[] = {"hello", "fib", "primes"};
    for (int e = 0; e < 3; e++) {
        char path[64], err[512];
        snprintf(path, sizeof path, "examples/beta/%s.bs", names[e]);
        char *src = read_file(path);
        BetaImage img;
        if (!src || !beta_assemble(src, path, &img, err, sizeof err)) {
            CHECK(0, "%s: %s", path, src ? err : "cannot read");
            free(src);
            continue;
        }
        free(src);
        static uint8_t in[LANES][400];
        static BetaLane lanes[LANES];
        for (unsigned k = 0; k < LANES; k++) {
            uint8_t mine = (uint8_t)(k / 2);  /* primes: below k / 2 */
            memset(&lanes[k], 0, sizeof lanes[k]);
            lanes[k].in = in[k];
            lanes[k].nin = beta_boot_input(&img, &mine, e == 2, in[k], sizeof in[k]);
        }
        LaneMachine lm;
        lane_init(&lm, p);
        unsigned long passes = 0;
        int done = 0;
        while (done < LANES && passes < 2000000) {
            beta_feed(&lm, cells, lanes);
            jit_tick(jc, &lm);
            done = beta_collect(&lm, cells, lanes);
            passes++;
        }
        int bad = 0;
        unsigned first = 0;
        for (unsigned k = 0; k < LANES; k++) {
            char want[1200];
            if (e == 0)
                strcpy(want, "HELLO FROM BETA\n");
            else if (e == 1)
                strcpy(want, "0 1 1 2 3 5 8 13 21 34 55 89 144 233\n");
            else
                primes_below(k / 2, want);
            if ((lanes[k].nout != strlen(want) || memcmp(lanes[k].out, want, lanes[k].nout)) &&
                !bad++)
                first = k;
        }
        CHECK(done == LANES && bad == 0,
              "%s: all 512 betas loaded it through the monitor, ran it and came back with the "
              "right output (%d done, %d wrong, first lane %u, %lu steps)",
              names[e], done, bad, first, passes);
        for (unsigned k = 0; k < LANES; k++)
            free(lanes[k].out);
        lane_free(&lm);
    }
}

int main(void)
{
    test_instructions();
    test_assembler();
    test_monitor();
    if (!cpu_has_avx2()) {
        printf("%d checks, %d failures (engine tests skipped: no AVX2)\n", checks, failures);
        return failures != 0;
    }
    Program p;
    BetaCells cells;
    gen_beta(&p, &cells);
    JitCode jc;
    char err[128];
    CHECK(jit_compile(&p, 0, &jc, NULL, err, sizeof err), "beta compiles");
    printf("beta: %zu instructions, %u cells\n", p.len, (unsigned)p.ncells);
    test_circuit(&p, &cells, &jc);
    test_circuit(&p, &cells, NULL);
    test_reference(&p, &cells);
    CheckResult r;
    check_lanes(&p, &jc, 31, 3, 0, &r);
    CHECK(r.ok, "compiled beta = reference engine from random cells: tick %llu lane %u cell %ld",
          (unsigned long long)r.tick, r.lane, r.cell);
    test_examples(&p, &cells, &jc);
    jit_free(&jc);
    program_free(&p);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
