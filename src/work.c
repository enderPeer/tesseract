#include "work.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "enum.h"
#include "gen.h"
#include "jit.h"
#include "lanes.h"
#include "net.h"
#include "parallel.h"
#include "platform.h"
#include "ref.h"
#include "spec.h"

void work_usage(void)
{
    fputs("workloads:\n"
          "  alpha gen mul --bits N                  N x N multiplier (inputs a, b; output p)\n"
          "  alpha gen hash --bits N --rounds R      toy hash (input x; output h)\n"
          "  alpha gen life --size N                 Game of Life, N x N, one generation per pass\n"
          "  alpha gen wht [--bits N]                the cube: Walsh-Hadamard spectrum of f (w)\n"
          "  alpha gen xorconv                       the cube: h(k) = #x with f(x) = g(x^k) = 1\n"
          "  alpha gen polymul                       the cube: c = a * b mod x^512 + 1, 12289 (NTT)\n"
          "  alpha net2asm <file.bench>              an ISCAS .bench netlist as assembly\n"
          "  alpha toyhash --bits N --rounds R X     the toy hash of X, computed in plain C\n"
          "  alpha verify <prog> --spec \"p = a * b\"  run every input, compare with the spec\n"
          "  alpha equiv <prog1> <prog2>             run every input, compare the two programs\n"
          "  alpha search <prog> --want \"h = 0x..\"   find every input that gives these outputs\n"
          "  alpha life [--size N] [--gens G]        512 boards against plain C, then speed\n"
          "  alpha life --cube [--rows R]            one board over all 512 machines, N x N tiles\n"
          "  alpha life --dims D [--rows R]          the same over 2^D machines, D = 10 .. 16\n"
          "  alpha cube [DEMO]                       the cube's operations, checked and timed: any,\n"
          "                                          rank, max, sort, pack, perm, anf, walsh, xorconv,\n"
          "                                          polymul, ring, life\n"
          "  alpha beta asm|monitor|run|bench        beta, a byte computer built from NAND/STO\n"
          "  alpha super \"SPEC\" | --library          the shortest NAND/STO program, proven\n"
          "  alpha gamma [--size X,Y,Z] [--show]     gamma, a 3-D world of cells on the cube that\n"
          "                                          evolve from a random soup; checked against\n"
          "                                          plain C, then run with a census\n"
          "\n"
          "Programs may be .asm or .bench files. verify, equiv and search take --ticks T,\n"
          "--threads N, --exclude LIST, --idle, and for the cluster --part I/K with --raw, and\n"
          "--merge FILE... to add up raw outputs.\n",
          stderr);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static int num(const char *opt, const char *s, unsigned long long *v)
{
    char *end;
    *v = strtoull(s, &end, 0);
    if (*s && !*end)
        return 1;
    fprintf(stderr, "%s expects a number, got '%s'\n", opt, s);
    return 0;
}

static int cpu_list(const char *s, unsigned char *flag)
{
    while (*s) {
        char *end;
        long a = strtol(s, &end, 10), b = a;
        if (end == s)
            return 0;
        s = end;
        if (*s == '-') {
            b = strtol(s + 1, &end, 10);
            if (end == s + 1)
                return 0;
            s = end;
        }
        if (a < 0 || b >= 1024 || a > b)
            return 0;
        for (long i = a; i <= b; i++)
            flag[i] = 1;
        if (*s == ',')
            s++;
        else if (*s)
            return 0;
    }
    return 1;
}

/* ---- gen, net2asm, toyhash ---------------------------------------------------------------- */

static int cmd_gen(int argc, char **argv)
{
    if (argc < 1) {
        work_usage();
        return 2;
    }
    const char *what = argv[0];
    unsigned long long bits = ~0ull, rounds = 8, size = 16;  /* bits: 8, or 12 for wht */
    for (int i = 1; i < argc; i++) {
        unsigned long long *v = !strcmp(argv[i], "--bits")     ? &bits
                              : !strcmp(argv[i], "--rounds")   ? &rounds
                              : !strcmp(argv[i], "--size")     ? &size
                                                               : NULL;
        if (!v || i + 1 == argc) {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        }
        const char *opt = argv[i];
        if (!num(opt, argv[++i], v))
            return 1;
    }
    if (bits == ~0ull)
        bits = !strcmp(what, "wht") ? 12 : 8;
    Program p;
    if (!strcmp(what, "mul")) {
        if (bits < 1 || bits > 32) {
            fputs("--bits must be 1 to 32\n", stderr);
            return 1;
        }
        gen_mul(&p, (int)bits);
        printf("; %llu x %llu array multiplier: p = a * b\n;   alpha gen mul --bits %llu\n", bits,
               bits, bits);
    } else if (!strcmp(what, "hash")) {
        if (bits < 1 || bits > 64 || rounds > 64 || !gen_hash(&p, (int)bits, (int)rounds)) {
            fputs("--bits must be 1 to 64 and --rounds 0 to 64\n", stderr);
            return 1;
        }
        printf("; toy hash, %llu bits, %llu rounds: h = toyhash(x)\n"
               ";   alpha gen hash --bits %llu --rounds %llu\n"
               "; each round: x_i ^= !x_(i+1) & x_(i+2), permute the bits, add a round constant\n",
               bits, rounds, bits, rounds);
    } else if (!strcmp(what, "life")) {
        if (size < 3 || size > 256) {
            fputs("--size must be 3 to 256\n", stderr);
            return 1;
        }
        uint32_t *board = xmalloc((size_t)(size * size) * sizeof *board);
        gen_life(&p, (int)size, (int)size, board);
        free(board);
        printf("; Game of Life on a %llu x %llu torus, one generation per pass\n"
               ";   alpha gen life --size %llu\n; cell x<row>_<col>\n",
               size, size, size);
    } else if (!strcmp(what, "wht")) {
        if (bits < 2 || bits > 32) {
            fputs("--bits must be 2 to 32\n", stderr);
            return 1;
        }
        gen_wht(&p, (int)bits);
        printf("; the cube: machine k holds f(k) and gets w = the Walsh-Hadamard spectrum of f at k,\n"
               "; the sum over x of (-1)^(f(x) + popcount(x & k)), %llu-bit two's complement\n"
               ";   alpha gen wht --bits %llu\n",
               bits, bits);
    } else if (!strcmp(what, "polymul")) {
        gen_polymul(&p);
        printf("; the cube: c = a * b in Z_12289[x]/(x^512 + 1), machine k holding coefficient k,\n"
               "; by NTT, product and inverse NTT. The twiddle factors zf<d>_, zi<d>_ are inputs\n"
               "; (ntt_twiddles in src/gen.c).\n"
               ";   alpha gen polymul\n");
    } else if (!strcmp(what, "xorconv")) {
        gen_xorconv(&p);
        printf("; the cube: machine k holds f(k) and g(k) and gets h(k) = the number of x with\n"
               "; f(x) = g(x ^ k) = 1, by spectrum, product and inverse spectrum\n"
               ";   alpha gen xorconv\n");
    } else {
        fprintf(stderr, "gen makes mul, hash, life, wht, xorconv or polymul, not '%s'\n", what);
        return 1;
    }
    printf("; %zu instructions, %u cells\n", p.len, (unsigned)p.ncells);
    program_print(stdout, &p);
    program_free(&p);
    return 0;
}

static int cmd_net2asm(int argc, char **argv)
{
    if (argc != 1) {
        work_usage();
        return 2;
    }
    Program p;
    char err[512];
    if (!net_load(argv[0], &p, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    printf("; from %s: %zu instructions, %u cells\n", argv[0], p.len, (unsigned)p.ncells);
    program_print(stdout, &p);
    program_free(&p);
    return 0;
}

static int cmd_toyhash(int argc, char **argv)
{
    unsigned long long bits = 32, rounds = 8, x = 0;
    int have_x = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--bits") && i + 1 < argc) {
            if (!num("--bits", argv[++i], &bits))
                return 1;
        } else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) {
            if (!num("--rounds", argv[++i], &rounds))
                return 1;
        } else if (!have_x && num("value", argv[i], &x)) {
            have_x = 1;
        } else {
            return 1;
        }
    }
    if (!have_x || bits < 1 || bits > 64 || rounds > 64) {
        fputs("usage: alpha toyhash --bits N --rounds R X\n", stderr);
        return 1;
    }
    if (bits < 64)
        x &= (1ull << bits) - 1;
    printf("0x%llx\n", (unsigned long long)toyhash(x, (int)bits, (int)rounds));
    return 0;
}

/* ---- verify, equiv, search ---------------------------------------------------------------- */

typedef struct {
    Enum          e;
    unsigned char excluded[1024];
    int           raw;
    char        **merge;
    int           nmerge;
    const char   *text;  /* --spec / --want */
} EnOpts;

static int parse_en(int argc, char **argv, EnOpts *o, const char *text_flag)
{
    for (int i = 0; i < argc; i++) {
        unsigned long long v;
        if (text_flag && !strcmp(argv[i], text_flag) && i + 1 < argc) {
            o->text = argv[++i];
        } else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) {
            if (!num("--ticks", argv[++i], &v) || v < 1)
                return 0;
            o->e.ticks = (unsigned)v;
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            if (!num("--threads", argv[++i], &v))
                return 0;
            o->e.threads = (int)v;
        } else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) {
            if (!cpu_list(argv[++i], o->excluded)) {
                fputs("--exclude expects processor numbers like 3,11 or 8-15\n", stderr);
                return 0;
            }
        } else if (!strcmp(argv[i], "--idle")) {
            o->e.idle = 1;
        } else if (!strcmp(argv[i], "--part") && i + 1 < argc) {
            const char *s = argv[++i];
            if (sscanf(s, "%d-%d/%d", &o->e.part, &o->e.part_end, &o->e.parts) != 3) {
                if (sscanf(s, "%d/%d", &o->e.part, &o->e.parts) != 2) {
                    fputs("--part expects I/K or I-J/K (shares I to J of K), e.g. 2/5\n", stderr);
                    return 0;
                }
                o->e.part_end = o->e.part;
            }
        } else if (!strcmp(argv[i], "--raw")) {
            o->raw = 1;
        } else if (!strcmp(argv[i], "--merge")) {
            o->merge = argv + i + 1;
            o->nmerge = argc - i - 1;
            break;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 0;
        }
    }
    return 1;
}

/* Runs p from the power-on state on assignment x; inputs in a's order go to cells in_cells. */
static void run_one(const Program *p, const uint32_t *in_cells, size_t n, uint64_t x,
                    unsigned ticks, uint8_t *outs)
{
    RefMachine m;
    if (!ref_init(&m, p))
        return;
    for (size_t k = 0; k < n; k++)
        ref_set(&m, in_cells[k], (uint8_t)((x >> (n - 1 - k)) & 1));
    for (unsigned t = 0; t < ticks; t++)
        ref_tick(&m);
    for (size_t o = 0; o < p->noutputs; o++)
        outs[o] = ref_get(&m, p->outputs[o]);
    ref_free(&m);
}

static void show_hit(const EnOpts *o, uint64_t x)
{
    const Enum *e = &o->e;
    const Program *a = e->a;
    size_t n = a->ninputs;
    uint8_t *ins = xmalloc(n + 1), *outa = xmalloc(a->noutputs + 1);
    char bi[1024], bo[1024];
    for (size_t k = 0; k < n; k++)
        ins[k] = (uint8_t)((x >> (n - 1 - k)) & 1);
    vec_format(a, a->inputs, n, ins, bi, sizeof bi);
    run_one(a, a->inputs, n, x, e->ticks, outa);
    vec_format(a, a->outputs, a->noutputs, outa, bo, sizeof bo);
    if (e->mode == EN_SEARCH) {
        printf("  %s  gives  %s\n", bi, bo);
    } else if (e->mode == EN_VERIFY) {
        printf("  %s  gives  %s, the spec says 0x%llx\n", bi, bo,
               (unsigned long long)spec_value(e->spec, x));
    } else {
        int *im = xmalloc((n + 1) * sizeof *im), *om = xmalloc((a->noutputs + 1) * sizeof *om);
        char err[256];
        if (enum_match(a, e->b, im, om, err, sizeof err)) {
            uint32_t *cells = xmalloc((n + 1) * sizeof *cells);
            uint8_t *outb = xmalloc(e->b->noutputs + 1), *ordered = xmalloc(a->noutputs + 1);
            for (size_t k = 0; k < n; k++)
                cells[k] = e->b->inputs[im[k]];
            run_one(e->b, cells, n, x, e->ticks, outb);
            for (size_t k = 0; k < a->noutputs; k++)
                ordered[k] = outb[om[k]];
            char bb[1024];
            vec_format(a, a->outputs, a->noutputs, ordered, bb, sizeof bb);
            printf("  %s\n      first:  %s\n      second: %s\n", bi, bo, bb);
            free(cells);
            free(outb);
            free(ordered);
        }
        free(im);
        free(om);
    }
    free(ins);
    free(outa);
}

static int merge_raw(EnOpts *o, EnumResult *r)
{
    memset(r, 0, sizeof *r);
    for (int f = 0; f < o->nmerge; f++) {
        FILE *in = fopen(o->merge[f], "r");
        if (!in) {
            fprintf(stderr, "cannot open %s\n", o->merge[f]);
            return 0;
        }
        char line[512];
        while (fgets(line, sizeof line, in)) {
            unsigned long long space, tried, hits, ops, x;
            double sec;
            int mode, threads;
            if (sscanf(line, "EN %d %llu %llu %llu %lf %d %llu", &mode, &space, &tried, &hits, &sec,
                       &threads, &ops) == 7) {
                r->space = space;
                r->tried += tried;
                r->hits += hits;
                r->ops += ops;
                r->threads += threads;
                if (sec > r->seconds)
                    r->seconds = sec;
            } else if (sscanf(line, "ENHIT %llx", &x) == 1) {
                enum_add_hit(r, x);
            }
        }
        fclose(in);
    }
    return 1;
}

static int cmd_enum(int mode, int argc, char **argv)
{
    static const char *verbs[] = {"verify", "equiv", "search"};
    int nprog = mode == EN_EQUIV ? 2 : 1;
    if (argc < nprog) {
        work_usage();
        return 2;
    }
    EnOpts o;
    memset(&o, 0, sizeof o);
    o.e.mode = mode;
    o.e.ticks = 1;
    o.e.parts = 1;
    o.e.excluded = o.excluded;
    if (!parse_en(argc - nprog, argv + nprog, &o,
                  mode == EN_VERIFY ? "--spec" : mode == EN_SEARCH ? "--want" : NULL))
        return 1;
    if (mode != EN_EQUIV && !o.text) {
        fprintf(stderr, "%s needs %s \"outputs = ...\"\n", verbs[mode],
                mode == EN_VERIFY ? "--spec" : "--want");
        return 1;
    }

    Program pa, pb;
    char err[512];
    int rc = 1;
    if (!load_program(argv[0], &pa, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    o.e.a = &pa;
    if (mode == EN_EQUIV) {
        if (!load_program(argv[1], &pb, err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            program_free(&pa);
            return 1;
        }
        o.e.b = &pb;
    }
    Spec *spec = NULL;
    if (o.text) {
        spec = spec_parse(o.text, &pa, mode == EN_VERIFY, err, sizeof err);
        if (!spec) {
            fprintf(stderr, "%s: %s\n", mode == EN_VERIFY ? "--spec" : "--want", err);
            goto done;
        }
        o.e.spec = spec;
    }

    EnumResult r;
    if (o.nmerge) {
        if (!merge_raw(&o, &r))
            goto done;
    } else {
        if (!cpu_has_avx2()) {
            fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
            goto done;
        }
        if (!enum_run(&o.e, &r, err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            goto done;
        }
        if (o.raw) {
            printf("EN %d %llu %llu %llu %.6f %d %llu\n", mode, (unsigned long long)r.space,
                   (unsigned long long)r.tried, (unsigned long long)r.hits, r.seconds, r.threads,
                   (unsigned long long)r.ops);
            for (int h = 0; h < r.nhit; h++)
                printf("ENHIT %llx\n", (unsigned long long)r.hit[h]);
            rc = 0;
            goto done;
        }
    }

    printf("%s %s%s%s%s%s\n", verbs[mode], argv[0], mode == EN_EQUIV ? " and " : "",
           mode == EN_EQUIV ? argv[1] : "", o.text ? ": " : "", o.text ? o.text : "");
    printf("%llu of %llu inputs (%zu input bits), %d threads, %.2f s, %.3g 1-bit operations "
           "(%.2f T/s)\n",
           (unsigned long long)r.tried, (unsigned long long)r.space, pa.ninputs, r.threads,
           r.seconds, (double)r.ops, r.seconds > 0 ? (double)r.ops / r.seconds / 1e12 : 0);
    if (mode == EN_SEARCH) {
        if (r.hits == 0)
            printf("no input gives %s\n", o.text);
        else
            printf("%llu input%s give%s %s%s\n", (unsigned long long)r.hits, r.hits == 1 ? "" : "s",
                   r.hits == 1 ? "s" : "", o.text, r.hits > 10 ? "; the first ones:" : ":");
    } else if (r.hits == 0) {
        printf("OK: %s for every input tried\n",
               mode == EN_VERIFY ? "the outputs equal the spec" : "both programs give the same outputs");
    } else {
        printf("%s for %llu input%s; the first ones:\n",
               mode == EN_VERIFY ? "WRONG" : "DIFFERENT", (unsigned long long)r.hits,
               r.hits == 1 ? "" : "s");
    }
    for (int h = 0; h < r.nhit && h < 10; h++)
        show_hit(&o, r.hit[h]);
    if (r.tried < r.space)
        printf("(only %llu of %llu inputs were tried: the other --part shares are missing)\n",
               (unsigned long long)r.tried, (unsigned long long)r.space);
    rc = mode == EN_SEARCH || r.hits == 0 ? 0 : 3;
done:
    spec_free(spec);
    program_free(&pa);
    if (mode == EN_EQUIV)
        program_free(&pb);
    return rc;
}

/* ---- Game of Life ------------------------------------------------------------------------- */

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int cmd_life(int argc, char **argv)
{
    unsigned long long size = 32, gens = 0, v;
    double seconds = 3;
    int threads = 0, idle = 0, show = 0, cube = 0, rbits = -1, dims = CUBE_DIMS;
    static unsigned char excluded[1024];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (!num("--size", argv[++i], &size))
                return 1;
        } else if (!strcmp(argv[i], "--gens") && i + 1 < argc) {
            if (!num("--gens", argv[++i], &gens))
                return 1;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            if (!num("--threads", argv[++i], &v))
                return 1;
            threads = (int)v;
        } else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) {
            if (!cpu_list(argv[++i], excluded))
                return 1;
        } else if (!strcmp(argv[i], "--idle")) {
            idle = 1;
        } else if (!strcmp(argv[i], "--show")) {
            show = 1;
        } else if (!strcmp(argv[i], "--cube")) {
            cube = 1;
        } else if (!strcmp(argv[i], "--rows") && i + 1 < argc) {
            if (!num("--rows", argv[++i], &v))
                return 1;
            if (v > CUBE_MAX_DIMS) {
                fputs("--rows must be 0 to --dims (2^R rows of machines)\n", stderr);
                return 1;
            }
            rbits = (int)v;
        } else if (!strcmp(argv[i], "--dims") && i + 1 < argc) {
            if (!num("--dims", argv[++i], &v))
                return 1;
            if (v < CUBE_DIMS || v > CUBE_MAX_DIMS) {
                fprintf(stderr, "--dims must be %d to %d\n", CUBE_DIMS, CUBE_MAX_DIMS);
                return 1;
            }
            dims = (int)v;
            cube = 1;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!gens)
        gens = dims > CUBE_DIMS ? 10 : 100;  /* plain C needs a while for a big board */
    if (rbits < 0)
        rbits = dims / 2;
    if (size < (cube ? 1u : 3u) || size > 256 || rbits > dims) {
        fprintf(stderr, "--size must be %d to 256, --rows at most --dims\n", cube ? 1 : 3);
        return 1;
    }
    if (!cpu_has_avx2()) {
        fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
        return 1;
    }
    int w = (int)size, h = (int)size, cells = w * h, cbits = dims - rbits;
    uint32_t *board = xmalloc((size_t)cells * sizeof *board);
    Program p;
    if (cube)
        gen_life_cube(&p, w, h, rbits, cbits, board);
    else
        gen_life(&p, w, h, board);
    JitCode jc;
    char err[256];
    if (!jit_compile(&p, 0, &jc, NULL, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    /* the cube: one board of 2^rbits x 2^cbits tiles, tile (R, C) in machine grid_machine */
    int gw = w << cbits, gh = h << rbits;
    LaneCube lc;
    if (!lane_cube_init(&lc, &p)) {
        fputs("out of memory\n", stderr);
        return 1;
    }
    unsigned machines = lc.groups * LANES;
    if (cube && lc.groups > 1)
        printf("Game of Life on the cube: one %d x %d torus, a %d x %d tile in each of %u machines "
               "(%u groups of 512), %zu NAND/STO instructions per generation\n",
               gh, gw, h, w, machines, lc.groups, p.len);
    else if (cube)
        printf("Game of Life on the cube: one %d x %d torus, a %d x %d tile in each of the 512 "
               "machines, %zu NAND/STO instructions per generation\n", gh, gw, h, w, p.len);
    else
        printf("Game of Life, %d x %d torus: %zu NAND/STO instructions per generation, one lane "
               "per board\n", w, h, p.len);
    if (jc.nseg > 1)
        printf("the pass is cut into %u segments (on several threads, a barrier after each)\n",
               jc.nseg);

    /* random boards, one thread, against plain C */
    uint64_t s = 42;
    for (unsigned g = 0; g < lc.groups; g++)
        for (int i = 0; i < cells; i++)
            for (int k = 0; k < 8; k++)
                lc.g[g].mem[board[i]].w[k] = splitmix64(&s);
    uint8_t *start = xmalloc((size_t)machines * (size_t)cells), *b = xmalloc((size_t)cells);
    for (unsigned l = 0; l < machines; l++)
        for (int i = 0; i < cells; i++)
            start[(size_t)l * (size_t)cells + (size_t)i] = lane_cube_get(&lc, board[i], l);
    double t0 = now_seconds();
    jit_cube_run(&jc, &lc, gens);
    double t1 = now_seconds();
    int bad = -1;
    if (cube) {  /* the whole board in plain C */
        uint8_t *big = xmalloc((size_t)gw * (size_t)gh);
        for (int R = 0; R < gh; R++)
            for (int C = 0; C < gw; C++)
                big[(size_t)R * (size_t)gw + (size_t)C] =
                    start[(size_t)grid_machine((unsigned)(R / h), (unsigned)(C / w), cbits) *
                              (size_t)cells + (size_t)((R % h) * w + C % w)];
        for (unsigned long long g = 0; g < gens; g++)
            life_scalar(big, gw, gh);
        for (int R = 0; R < gh && bad < 0; R++)
            for (int C = 0; C < gw; C++)
                if (big[(size_t)R * (size_t)gw + (size_t)C] !=
                    lane_cube_get(&lc, board[(R % h) * w + C % w],
                                  grid_machine((unsigned)(R / h), (unsigned)(C / w), cbits))) {
                    bad = R;
                    break;
                }
        if (bad >= 0)
            printf("MISMATCH: row %d differs from the plain C Game of Life after %llu "
                   "generations\n", bad, gens);
        else
            printf("OK: the board equals the plain C Game of Life after %llu generations\n", gens);
        if (show) {
            int rows = gh < 48 ? gh : 48, cols = gw < 128 ? gw : 128;
            printf("the top left %d x %d after %llu generations:\n", rows, cols, gens);
            for (int R = 0; R < rows; R++) {
                for (int C = 0; C < cols; C++)
                    putchar(big[(size_t)R * (size_t)gw + (size_t)C] ? '#' : '.');
                putchar('\n');
            }
        }
        free(big);
    }
    LaneMachine *m = &lc.g[0];
    for (int l = 0; l < LANES && bad < 0 && !cube; l++) {
        memcpy(b, start + (size_t)l * (size_t)cells, (size_t)cells);
        for (unsigned long long g = 0; g < gens; g++)
            life_scalar(b, w, h);
        for (int i = 0; i < cells; i++)
            if (b[i] != lane_get(m, board[i], (unsigned)l)) {
                bad = l;
                break;
            }
    }
    if (bad >= 0 && !cube)
        printf("MISMATCH: board %d differs from the plain C Game of Life after %llu generations\n",
               bad, gens);
    else if (!cube)
        printf("OK: all 512 boards equal the plain C Game of Life after %llu generations\n", gens);
    printf("one thread: %.3g cell updates/s (%llu generations %s in %.3f s, %.3g per second)\n",
           (double)gens * cells * machines / (t1 - t0), gens,
           cube ? "of the board" : "of 512 boards", t1 - t0, (double)gens / (t1 - t0));
    if (show && !cube) {
        int alive = 0;
        for (int i = 0; i < cells; i++)
            alive += lane_get(m, board[i], 0);
        printf("board 0 after %llu generations (%d alive):\n", gens, alive);
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++)
                putchar(lane_get(m, board[r * w + c], 0) ? '#' : '.');
            putchar('\n');
        }
    }
    free(start);
    free(b);

    if (seconds > 0) {
        if (lc.groups > 1 && !threads)  /* lockstep: leave the second hardware threads free */
            threads = cpu_cores(excluded);
        int *ids = xmalloc(1024 * sizeof *ids), k = cpu_pick(ids, 1024, excluded, threads);
        if (lc.groups > 1) {  /* one board on all threads, in lockstep */
            uint64_t n = 1;
            double t;
            int pinned;
            for (;;) {
                t = par_cube_run(&jc, &lc, k ? k : 1, k ? ids : NULL, idle, n, &pinned);
                if (t > seconds / 2 || n > (1ull << 40))
                    break;
                n *= 2;
            }
            printf("%d threads on the one board: %.3g cell updates/s, %.2f T 1-bit ops/s (%.3g "
                   "generations per second)\n",
                   k ? k : 1, (double)n * cells * machines / t,
                   (double)n * (double)p.len * machines / t / 1e12, (double)n / t);
        } else {
            ParResult r;
            par_measure(&jc, k ? k : 1, k ? ids : NULL, 1, idle, seconds, &r);
            double ups = (double)r.passes * cells * LANES / r.seconds;
            printf("%d threads: %.3g cell updates/s, %.2f T 1-bit ops/s (%.0f generations of %d "
                   "boards)\n",
                   k ? k : 1, ups, (double)r.passes * (double)p.len * LANES / r.seconds / 1e12,
                   (double)r.passes, (cube ? 1 : LANES) * (k ? k : 1));
        }
        free(ids);
    }
    lane_cube_free(&lc);
    jit_free(&jc);
    program_free(&p);
    free(board);
    return bad >= 0 ? 3 : 0;
}

int work_main(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    const char *c = argv[1];
    if (!strcmp(c, "gen"))
        return cmd_gen(argc - 2, argv + 2);
    if (!strcmp(c, "net2asm"))
        return cmd_net2asm(argc - 2, argv + 2);
    if (!strcmp(c, "toyhash"))
        return cmd_toyhash(argc - 2, argv + 2);
    if (!strcmp(c, "verify"))
        return cmd_enum(EN_VERIFY, argc - 2, argv + 2);
    if (!strcmp(c, "equiv"))
        return cmd_enum(EN_EQUIV, argc - 2, argv + 2);
    if (!strcmp(c, "search"))
        return cmd_enum(EN_SEARCH, argc - 2, argv + 2);
    if (!strcmp(c, "life"))
        return cmd_life(argc - 2, argv + 2);
    if (!strcmp(c, "cube"))
        return cmd_cube(argv[0], argc - 2, argv + 2);
    if (!strcmp(c, "beta"))
        return cmd_beta(argc - 2, argv + 2);
    if (!strcmp(c, "super"))
        return cmd_super(argc - 2, argv + 2);
    if (!strcmp(c, "gamma"))
        return cmd_gamma(argc - 2, argv + 2);
    return -1;
}
