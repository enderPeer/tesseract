/* Tests for the compiler: the x86 encoder against bytes from Microsoft's assembler (ml64), and
   compiled code in every lane against the reference machine. Run from the project root. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "check.h"
#include "jit.h"
#include "lanes.h"
#include "platform.h"
#include "x86.h"

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

/* ---- encoder ------------------------------------------------------------------------------ */

static void expect_bytes(X86 *x, size_t *at, const char *hex)
{
    char got[64] = "";
    for (size_t i = *at; i < x->len && 2 * (i - *at) + 2 < sizeof got; i++)
        snprintf(got + 2 * (i - *at), 3, "%02X", x->b[i]);
    CHECK(strcmp(got, hex) == 0, "%s: encoded %s, ml64 says %s", x->text, got, hex);
    *at = x->len;
}

static void test_encoder(void)
{
    X86 x = {0};
    size_t at = 0;
    x86_rrm(&x, X86_VPAND, 0, 1, X86_RCX, 0x40);           expect_bytes(&x, &at, "C5F5DB4140");
    x86_rrr(&x, X86_VPXOR, 2, 3, 0);                       expect_bytes(&x, &at, "C5E5EFD0");
    x86_rrm(&x, X86_VPANDN, 9, 10, X86_RCX, 0x80);         expect_bytes(&x, &at, "C52DDF8980000000");
    x86_rrr(&x, X86_VPAND, 12, 13, 14);                    expect_bytes(&x, &at, "C44115DBE6");
    x86_rrr(&x, X86_VPXOR, 3, 4, 15);                      expect_bytes(&x, &at, "C4C15DEFDF");
    x86_rrr(&x, X86_VPCMPEQD, 0, 0, 0);                    expect_bytes(&x, &at, "C5FD76C0");
    x86_load(&x, 2, X86_RCX, 0x80, 1, 1);                  expect_bytes(&x, &at, "C5FD6F9180000000");
    x86_store(&x, 13, X86_RCX, 0x1000, 1, 1);              expect_bytes(&x, &at, "C57D7FA900100000");
    x86_load(&x, 1, X86_RDX, 0, 0, 1);                     expect_bytes(&x, &at, "C5FE6F0A");
    x86_store(&x, 11, X86_RDX, 0x20, 0, 1);                expect_bytes(&x, &at, "C57E7F5A20");
    x86_store(&x, 6, X86_R8, 0x10, 0, 0);                  expect_bytes(&x, &at, "C4C17A7F7010");
    x86_load(&x, 15, X86_R8, 0x90, 0, 0);                  expect_bytes(&x, &at, "C4417A6FB890000000");
    x86_store(&x, 9, X86_R8, 0, 0, 0);                     expect_bytes(&x, &at, "C4417A7F08");
    x86_rrm(&x, X86_VPAND, 4, 5, X86_RCX, -8);             expect_bytes(&x, &at, "C5D5DB61F8");
    x86_load(&x, 7, X86_RDI, 0x7f, 1, 1);                  expect_bytes(&x, &at, "C5FD6F7F7F");
    x86_rrm(&x, X86_VPANDN, 8, 8, X86_RSI, 0x12345678);    expect_bytes(&x, &at, "C53DDF8678563412");
    x86_rrm(&x, X86_VPXOR, 10, 2, X86_R8, 0x40);           expect_bytes(&x, &at, "C4416DEF5040");
    x86_vzeroupper(&x);                                    expect_bytes(&x, &at, "C5F877");
    x86_ret(&x);                                           expect_bytes(&x, &at, "C3");
    x86_free(&x);

    /* the pass loop: ml64 assembled "top: db 200 dup (90h) / dec r9 / dec rcx / jnz top" */
    X86 y = {0};
    for (int i = 0; i < 200; i++) {
        if (y.len == y.cap) {
            y.cap = 4096;
            y.b = realloc(y.b, y.cap);
        }
        y.b[y.len++] = 0x90;
    }
    at = y.len;
    x86_dec(&y, X86_R9);   expect_bytes(&y, &at, "49FFC9");
    x86_dec(&y, X86_RCX);  expect_bytes(&y, &at, "48FFC9");
    x86_jnz(&y, 0);        expect_bytes(&y, &at, "0F852CFFFFFF");
    x86_free(&y);

    /* the peer table of a bigger cube (step 7f), also from ml64 */
    X86 q = {0};
    at = 0;
    x86_load64(&q, X86_RAX, X86_R9, 8);       expect_bytes(&q, &at, "498B4108");
    x86_load64(&q, X86_RAX, X86_RCX, 0x10);   expect_bytes(&q, &at, "488B4110");
    x86_load64(&q, X86_RAX, X86_RDX, 0x400);  expect_bytes(&q, &at, "488B8200040000");
    x86_load64(&q, X86_R10, X86_RDI, 0);      expect_bytes(&q, &at, "4C8B17");
    x86_load64(&q, X86_RAX, X86_R13, 0);      expect_bytes(&q, &at, "498B4500");
    x86_free(&q);

    /* moving lanes across links (step 7a), also from ml64 */
    X86 z = {0};
    at = 0;
    x86_mov(&z, 2, 13);                               expect_bytes(&z, &at, "C4C17D6FD5");
    x86_mov(&z, 12, 3);                               expect_bytes(&z, &at, "C57D6FE3");
    x86_pshufd(&z, 4, 4, 0xB1);                       expect_bytes(&z, &at, "C5FD70E4B1");
    x86_pshufd(&z, 9, 14, 0x4E);                      expect_bytes(&z, &at, "C4417D70CE4E");
    x86_permq(&z, 5, 5, 0x4E);                        expect_bytes(&z, &at, "C4E3FD00ED4E");
    x86_permq(&z, 10, 11, 0x4E);                      expect_bytes(&z, &at, "C443FD00D34E");
    x86_pshufb(&z, 6, 6, X86_RCX, 0x1000);            expect_bytes(&z, &at, "C4E24D00B100100000");
    x86_pshufb(&z, 12, 7, X86_RCX, 0x40);             expect_bytes(&z, &at, "C46245006140");
    x86_shiftq(&z, 1, 3, 3, 1);                       expect_bytes(&z, &at, "C5E573F301");
    x86_shiftq(&z, 0, 1, 8, 2);                       expect_bytes(&z, &at, "C4C17573D002");
    x86_shiftq(&z, 1, 13, 2, 4);                      expect_bytes(&z, &at, "C59573F204");
    x86_rrr(&z, X86_VPOR, 4, 4, 1);                   expect_bytes(&z, &at, "C5DDEBE1");
    x86_rrr(&z, X86_VPOR, 11, 12, 9);                 expect_bytes(&z, &at, "C4411DEBD9");
    x86_rrm(&z, X86_VPAND, 1, 1, X86_RDI, 0x12345);   expect_bytes(&z, &at, "C5F5DB8F45230100");
    x86_free(&z);
}

/* ---- compiled code against the reference machine ----------------------------------------- */

static void expect_match(const char *what, const Program *p, unsigned ticks, uint64_t seed)
{
    for (int literal = 0; literal < 2; literal++) {
        JitCode jc;
        char err[256];
        if (!jit_compile(p, literal, &jc, NULL, err, sizeof err)) {
            CHECK(0, "%s: %s", what, err);
            continue;
        }
        CheckResult r;
        check_lanes(p, &jc, seed, ticks, 0, &r);
        CHECK(r.ok, "%s (%s): tick %llu lane %u cell %ld: reference %u, compiled %u", what,
              literal ? "literal" : "optimized", (unsigned long long)r.tick, r.lane, r.cell,
              r.want, r.got);
        jit_free(&jc);
    }
}

static void test_examples(void)
{
    static const char *files[] = {"examples/xor.asm", "examples/mux.asm", "examples/full_adder.asm",
                                  "examples/adder8.asm", "examples/counter4.asm"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        Program p;
        char err[512];
        if (!asm_file(files[i], &p, err, sizeof err)) {
            CHECK(0, "%s", err);
            continue;
        }
        expect_match(files[i], &p, 40, 1 + i);
        program_free(&p);
    }
}

static void test_std(void)
{
    static const char *ops[] = {"SET",    "CLR",    "NOT",    "LD x",    "LDC x",
                                "AND x",  "OR x",   "NOR x",  "ANDC x",  "ORC x",
                                "XOR x",  "XNOR x", "STOC x", "MOV x y", "FA a b c s"};
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        char src[256], err[256];
        snprintf(src, sizeof src, ".include \"lib/std.inc\"\n%s\n", ops[i]);
        Program p;
        if (!asm_string(src, "<test>", ".", &p, err, sizeof err)) {
            CHECK(0, "%s", err);
            continue;
        }
        expect_match(ops[i], &p, 8, 200 + i);
        program_free(&p);
    }
}

static uint64_t next(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* Random programs: STO to constants, reading cells just stored, more live cells than registers,
   and addresses far enough apart for 32-bit displacements. */
static void test_random_programs(void)
{
    uint64_t s = 0x9E3779B9;
    for (int k = 0; k < 300; k++) {
        uint32_t ncells = 2 + (uint32_t)(next(&s) % (k < 200 ? 24 : 400));
        size_t len = 1 + (size_t)(next(&s) % (k < 200 ? 120 : 3000));
        Instr *code = malloc(len * sizeof *code);
        for (size_t i = 0; i < len; i++) {
            code[i].op = (uint8_t)(next(&s) & 1);
            code[i].addr = (uint32_t)(next(&s) % ncells);
        }
        Program p = {code, len, ncells, NULL, NULL, 0, NULL, 0};
        char what[80];
        snprintf(what, sizeof what, "random program %d (%zu instructions, %u cells)", k, len, ncells);
        expect_match(what, &p, 3, 500 + (uint64_t)k);
        free(code);
    }
}

/* ---- calling convention ------------------------------------------------------------------- */

/* Keeps many doubles alive across calls of compiled code. MSVC tends to keep such values in
   xmm6-xmm15 across a call, and the compiled adder8 uses those registers too, so a wrong result
   here means they were not restored. (A pass is not guaranteed to fail if they were not.) */
static double survive(const JitCode *jc, LaneMachine *m, int calls)
{
    double v[12];
    for (int i = 0; i < 12; i++)
        v[i] = 1.0 + i * 0.25;
    for (int n = 0; n < calls; n++) {
        jit_tick(jc, m);
        for (int i = 0; i < 12; i++)
            v[i] = v[i] * 1.0000001 + (double)(i + n % 3);
    }
    double sum = 0;
    for (int i = 0; i < 12; i++)
        sum += v[i] * (i + 1);
    return sum;
}

static double survive_plain(int calls)
{
    double v[12];
    for (int i = 0; i < 12; i++)
        v[i] = 1.0 + i * 0.25;
    for (int n = 0; n < calls; n++)
        for (int i = 0; i < 12; i++)
            v[i] = v[i] * 1.0000001 + (double)(i + n % 3);
    double sum = 0;
    for (int i = 0; i < 12; i++)
        sum += v[i] * (i + 1);
    return sum;
}

static void test_callee_saved(void)
{
    Program p;
    char err[512];
    if (!asm_file("examples/adder8.asm", &p, err, sizeof err)) {
        CHECK(0, "%s", err);
        return;
    }
    JitCode jc;
    LaneMachine m;
    if (!jit_compile(&p, 0, &jc, NULL, err, sizeof err) || !lane_init(&m, &p)) {
        CHECK(0, "%s", err);
        program_free(&p);
        return;
    }
    CHECK(survive(&jc, &m, 1000) == survive_plain(1000), "xmm6-xmm15 survive a call");
    lane_free(&m);
    jit_free(&jc);
    program_free(&p);
}

/* ---- what the optimizer does -------------------------------------------------------------- */

static void test_optimizer(void)
{
    Program p;
    char err[512];
    if (!asm_file("examples/adder8.asm", &p, err, sizeof err)) {
        CHECK(0, "%s", err);
        return;
    }
    JitCode lit, opt;
    if (jit_compile(&p, 1, &lit, NULL, err, sizeof err) &&
        jit_compile(&p, 0, &opt, NULL, err, sizeof err)) {
        /* literal: every NAND is AND + invert on both halves, plus the ones-constant */
        size_t nands = 0;
        for (size_t i = 0; i < p.len; i++)
            nands += p.code[i].op == OP_NAND;
        CHECK(nands == 104 && lit.stats.alu == 4 * nands, "literal adder8: %zu vector ops for %zu NANDs",
              lit.stats.alu, nands);
        /* the full adder is the shortest program there is, so less is left to fold away than in
           the old one (LD = NAND ZERO, NAND x, NAND ONE cost one load there) */
        CHECK(opt.stats.alu * 10 < lit.stats.alu * 6, "optimized uses under 60%%: %zu vs %zu",
              opt.stats.alu, lit.stats.alu);
        CHECK(opt.stats.free > 0, "some instructions cost nothing (%zu)", opt.stats.free);
        jit_free(&lit);
        jit_free(&opt);
    } else {
        CHECK(0, "%s", err);
    }
    program_free(&p);
}

/* ---- a pass started on the interpreter is finished by it ---------------------------------- */

static void test_partial_tick(void)
{
    Program p;
    char err[512];
    if (!asm_file("examples/adder8.asm", &p, err, sizeof err)) {
        CHECK(0, "%s", err);
        return;
    }
    JitCode jc;
    LaneMachine a, b;
    jit_compile(&p, 0, &jc, NULL, err, sizeof err);
    lane_init(&a, &p);
    lane_init(&b, &p);
    for (int i = 0; i < 37; i++)
        lane_step(&a);
    jit_tick(&jc, &a);  /* finishes the pass */
    jit_tick(&jc, &a);  /* a full compiled pass */
    lane_tick(&b);
    lane_tick(&b);
    CHECK(a.ticks == 2 && a.pc == 0 &&
              memcmp(a.mem, b.mem, p.ncells * sizeof(Line)) == 0 &&
              memcmp(&a.rr, &b.rr, sizeof a.rr) == 0,
          "steps, then compiled passes, give the interpreter's state");
    lane_free(&a);
    lane_free(&b);
    jit_free(&jc);
    program_free(&p);
}

/* ---- many passes in one call -------------------------------------------------------------- */

static void test_run_loop(void)
{
    static const char *files[] = {"examples/adder8.asm", "examples/counter4.asm"};
    for (size_t f = 0; f < 2; f++) {
        Program p;
        char err[512];
        if (!asm_file(files[f], &p, err, sizeof err)) {
            CHECK(0, "%s", err);
            continue;
        }
        for (int literal = 0; literal < 2; literal++) {
            JitCode jc;
            LaneMachine a, b;
            jit_compile(&p, literal, &jc, NULL, err, sizeof err);
            lane_init(&a, &p);
            lane_init(&b, &p);
            for (unsigned l = 0; l < LANES; l++)
                for (size_t k = 0; k < p.ninputs; k++) {
                    uint8_t v = (uint8_t)((l * 2654435761u >> (k + 3)) & 1);
                    lane_set(&a, p.inputs[k], l, v);
                    lane_set(&b, p.inputs[k], l, v);
                }
            jit_run(&jc, &a, 37);
            for (int i = 0; i < 37; i++)
                lane_tick(&b);
            CHECK(a.ticks == 37 && memcmp(a.mem, b.mem, p.ncells * sizeof(Line)) == 0 &&
                      memcmp(&a.rr, &b.rr, sizeof a.rr) == 0,
                  "%s (%s): 37 passes in one call = 37 interpreter passes", files[f],
                  literal ? "literal" : "optimized");
            lane_free(&a);
            lane_free(&b);
            jit_free(&jc);
        }
        program_free(&p);
    }
}

int main(void)
{
    test_encoder();
    if (!cpu_has_avx2()) {
        printf("%d checks, %d failures (compiled code skipped: no AVX2)\n", checks, failures);
        return failures != 0;
    }
    test_examples();
    test_std();
    test_random_programs();
    test_callee_saved();
    test_optimizer();
    test_partial_tick();
    test_run_loop();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
