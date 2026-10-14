/* alpha beta: the byte computer of step 8 (src/beta.c, docs/BETA.md) from the command line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "beta.h"
#include "jit.h"
#include "parallel.h"
#include "platform.h"
#include "work.h"

static void usage(void)
{
    fputs("usage:\n"
          "  alpha beta asm FILE.bs        assemble: addresses, bytes, instructions\n"
          "  alpha beta monitor            the monitor in ROM\n"
          "  alpha beta run FILE.bs [--input TEXT] [--byte N]... [--lane-input] [--show LANE]\n"
          "                     [--engine lanes|jit] [--max PASSES]\n"
          "                                512 betas boot, the monitor loads and runs the program\n"
          "                                in each; every one checked against the emulator\n"
          "  alpha beta bench [--seconds S] [--threads N]   betas per second, 1 and all threads\n",
          stderr);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(s);
        fprintf(stderr, "cannot read %s\n", path);
        return NULL;
    }
    s[n] = '\0';
    fclose(f);
    text_normalize(s, (size_t)n);
    return s;
}

static int load(const char *path, BetaImage *img)
{
    char *src = read_file(path), err[512];
    if (!src)
        return 0;
    int ok = beta_assemble(src, path, img, err, sizeof err);
    free(src);
    if (!ok)
        fprintf(stderr, "%s\n", err);
    return ok;
}

static void listing(const BetaImage *img)
{
    for (unsigned a = img->lo; a < img->hi;) {
        if (img->insn[a] && a + 1 < 256) {
            char text[32];
            beta_disasm(img->bytes[a], img->bytes[a + 1], text, sizeof text);
            printf("  0x%02X  %02X %02X   %s\n", a, img->bytes[a], img->bytes[a + 1], text);
            a += 2;
            continue;
        }
        unsigned n = 0;  /* data: up to 8 bytes, as bytes and as text */
        char text[16];
        printf("  0x%02X ", a);
        for (; n < 8 && a + n < img->hi && !img->insn[a + n]; n++) {
            uint8_t v = img->bytes[a + n];
            printf(" %02X", v);
            text[n] = v >= 32 && v < 127 ? (char)v : '.';
        }
        text[n] = '\0';
        printf("%*s  .byte  \"%s\"\n", (int)(3 * (8 - n)), "", text);
        a += n;
    }
}

static int cmd_asm(int argc, char **argv)
{
    BetaImage img;
    if (argc != 1) {
        usage();
        return 2;
    }
    if (!load(argv[0], &img))
        return 1;
    printf("%s: %u bytes at 0x%02X .. 0x%02X\n",
           argv[0], img.hi - img.lo, img.lo, img.hi ? img.hi - 1 : 0);
    listing(&img);
    return 0;
}

static int cmd_monitor(void)
{
    fputs(beta_monitor_source(), stdout);
    BetaImage img;
    memset(&img, 0, sizeof img);
    memcpy(img.bytes, beta_rom(), 256);
    memset(img.used + BETA_ROM, 1, 256 - BETA_ROM);
    for (unsigned a = BETA_ROM; a < 0xF0; a += 2)  /* the monitor's code; zeros after it */
        img.insn[a] = 1;
    img.lo = BETA_ROM;
    img.hi = 256;
    printf("\nROM, 0x%02X .. 0xFF:\n", BETA_ROM);
    listing(&img);
    return 0;
}

static int build(Program *p, BetaCells *cells, JitCode *jc, int literal)
{
    char err[256];
    gen_beta(p, cells);
    if (!jit_compile(p, literal, jc, NULL, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 0;
    }
    return 1;
}

static int cmd_run(int argc, char **argv)
{
    const char *path = NULL;
    uint8_t extra[1024];
    size_t nextra = 0;
    int lane_input = 0, jit = 1;
    unsigned long show = 0, max = 50000000;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            const char *t = argv[++i];
            for (; *t && nextra < sizeof extra; t++)
                extra[nextra++] = (uint8_t)*t;
        } else if (!strcmp(argv[i], "--byte") && i + 1 < argc && nextra < sizeof extra) {
            extra[nextra++] = (uint8_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--lane-input")) {
            lane_input = 1;
        } else if (!strcmp(argv[i], "--show") && i + 1 < argc) {
            show = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
            max = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--engine") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "lanes"))
                jit = 0;
            else if (strcmp(argv[i], "jit")) {
                fprintf(stderr, "--engine is lanes or jit\n");
                return 1;
            }
        } else if (argv[i][0] != '-' && !path) {
            path = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (!path || show >= LANES) {
        usage();
        return 2;
    }
    if (!cpu_has_avx2()) {
        fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
        return 1;
    }
    BetaImage img;
    if (!load(path, &img))
        return 1;
    if (img.hi > 0x9D) {
        fprintf(stderr, "%s reaches 0x%02X: programs go below 0x9D (the monitor keeps 0x9D .. "
                        "0x9F, the ROM starts at 0x%02X)\n", path, img.hi - 1, BETA_ROM);
        return 1;
    }

    /* every lane: the commands that load and start the program, then its input */
    static uint8_t inputs[LANES][1300];
    static BetaLane lanes[LANES], emu[LANES];
    for (unsigned k = 0; k < LANES; k++) {
        uint8_t mine[1025];
        memcpy(mine, extra, nextra);
        size_t n = nextra;
        if (lane_input)
            mine[n++] = (uint8_t)k;
        memset(&lanes[k], 0, sizeof lanes[k]);
        lanes[k].in = inputs[k];
        lanes[k].nin = beta_boot_input(&img, mine, n, inputs[k], sizeof inputs[k]);
        emu[k] = lanes[k];
    }

    Program p;
    BetaCells cells;
    JitCode jc;
    if (!build(&p, &cells, &jc, 0))
        return 1;
    LaneMachine m;
    lane_init(&m, &p);  /* every cell 0: every beta powers on in the monitor */
    printf("beta on NAND/STO: %zu instructions per beta step, 512 betas per pass (%s)\n", p.len,
           jit ? "compiled" : "interpreter");
    unsigned long passes = 0;
    double t0 = now_seconds();
    int done = 0;
    while (done < LANES && passes < max) {
        beta_feed(&m, &cells, lanes);
        if (jit)
            jit_tick(&jc, &m);
        else
            lane_tick(&m);
        done = beta_collect(&m, &cells, lanes);
        passes++;
    }
    double t = now_seconds() - t0;

    /* the same input to 512 emulators, as many steps: the same output and the same state */
    int bad = -1;
    for (unsigned k = 0; k < LANES; k++) {
        Beta s, got;
        beta_reset(&s);
        for (unsigned long i = 0; i < passes; i++)
            beta_emu_pass(&s, &emu[k]);
        beta_from_lane(&m, &cells, k, &got);
        if (bad < 0 && (emu[k].nout != lanes[k].nout ||
                        memcmp(emu[k].out, lanes[k].out, lanes[k].nout) ||
                        memcmp(&s, &got, sizeof s)))
            bad = (int)k;
    }

    BetaLane *l = &lanes[show];
    printf("beta %lu printed %zu bytes:\n", show, l->nout);
    fwrite(l->out, 1, l->nout, stdout);
    if (l->nout && l->out[l->nout - 1] != '\n')
        putchar('\n');
    unsigned same = 0;
    for (unsigned k = 0; k < LANES; k++)
        same += lanes[k].nout == l->nout && !memcmp(lanes[k].out, l->out, l->nout);
    printf("%u of 512 betas printed the same as beta %lu; %d done after %lu steps (%.3f s, %.3g "
           "beta steps/s with the host's I/O)\n",
           same, show, done, passes, t, (double)passes * LANES / t);
    if (bad >= 0)
        printf("MISMATCH: beta %d differs from the emulator\n", bad);
    else
        printf("OK: all 512 betas match the emulator: the same output, and the same state after "
               "%lu steps\n", passes);
    if (done < LANES)
        printf("(stopped at --max %lu: %d betas still running)\n", max, LANES - done);
    for (unsigned k = 0; k < LANES; k++) {
        free(lanes[k].out);
        free(emu[k].out);
    }
    lane_free(&m);
    jit_free(&jc);
    program_free(&p);
    return bad >= 0 ? 3 : 0;
}

static int cmd_bench(int argc, char **argv)
{
    double seconds = 2;
    int threads = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = atoi(argv[++i]);
        else {
            usage();
            return 2;
        }
    }
    if (!cpu_has_avx2()) {
        fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
        return 1;
    }
    Program p;
    BetaCells cells;
    JitCode jc;
    if (!build(&p, &cells, &jc, 0))
        return 1;
    LaneMachine m;
    lane_init(&m, &p);
    uint64_t n = 1;
    double t;
    for (;;) {
        double t0 = now_seconds();
        jit_run(&jc, &m, n);
        t = now_seconds() - t0;
        if (t > 0.3)
            break;
        n *= 2;
    }
    printf("beta: %zu NAND/STO instructions per step, %u cells, %zu bytes of x86 code\n", p.len,
           (unsigned)p.ncells, jc.stats.bytes);
    printf("1 thread:   %.3g steps/s for each of 512 betas, %.3g beta steps/s, %.1f T 1-bit ops/s\n",
           (double)n / t, (double)n * LANES / t, (double)n * (double)p.len * LANES / t / 1e12);
    static unsigned char excluded[1024];
    int *ids = malloc(1024 * sizeof *ids), k = cpu_pick(ids, 1024, excluded, threads);
    ParResult r;
    par_measure(&jc, k ? k : 1, k ? ids : NULL, 1, 0, seconds, &r);
    printf("%d threads: %.3g beta steps/s (%d betas), %.1f T 1-bit ops/s\n", k ? k : 1,
           (double)r.passes * LANES / r.seconds, (k ? k : 1) * LANES,
           (double)r.passes * (double)p.len * LANES / r.seconds / 1e12);
    free(ids);
    lane_free(&m);
    jit_free(&jc);
    program_free(&p);
    return 0;
}

int cmd_beta(int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return 2;
    }
    if (!strcmp(argv[0], "asm"))
        return cmd_asm(argc - 1, argv + 1);
    if (!strcmp(argv[0], "monitor"))
        return cmd_monitor();
    if (!strcmp(argv[0], "run"))
        return cmd_run(argc - 1, argv + 1);
    if (!strcmp(argv[0], "bench"))
        return cmd_bench(argc - 1, argv + 1);
    usage();
    return 2;
}
