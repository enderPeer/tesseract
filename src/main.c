/* alpha: command line for the assembler, the reference machine and the 512-lane engine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "check.h"
#include "jit.h"
#include "lanes.h"
#include "parallel.h"
#include "net.h"
#include "platform.h"
#include "ref.h"
#include "tester.h"
#include "work.h"

static void usage(void)
{
    fputs("usage:\n"
          "  alpha asm   <file.asm>              show the machine code bit by bit\n"
          "  alpha jit   <file.asm> [--literal]  show the x86 code the compiler makes of it\n"
          "  alpha run   <file.asm> [options]    run on the reference machine\n"
          "  alpha check <file.asm> [options]    verify a 512-lane engine against the reference\n"
          "  alpha bench <file.asm> [options]    measure every engine on one core\n"
          "  alpha scale <file.asm> [options]    compiled code on 1 thread .. every thread\n"
          "  alpha sweep <file.asm> [options]    one thread, more and more memory (cache levels)\n"
          "  alpha random --cells N [--len L] [--seed S]   print a random program\n"
          "  alpha tester1 [options]             what do random programs do? (see below)\n"
          "\n"
          "engines: ref (reference), lanes (512-lane interpreter),\n"
          "         literal (compiled one instruction at a time), jit (compiled, optimized)\n"
          "\n"
          "run options:\n"
          "  --ticks N         passes through the program (default 1)\n"
          "  --set name=v      set a cell before the first pass (repeatable)\n"
          "  --trace           print every instruction as it executes\n"
          "  --table           run every combination of the .in cells and print a truth table\n"
          "  --engine E        with --table: ref (default), or lanes / literal / jit for 512 rows\n"
          "                    per pass\n"
          "\n"
          "check options:\n"
          "  --engine E        lanes (default), literal or jit\n"
          "  --ticks N         passes to compare (default 8)\n"
          "  --seed S          random starting state (default 1)\n"
          "  --steps           lanes only: compare after every instruction instead of every pass\n"
          "\n"
          "bench options:\n"
          "  --seconds S       time per engine (default 1)\n"
          "\n"
          "scale and sweep options:\n"
          "  --seconds S       time per row (default 2 for scale, 1 for sweep)\n"
          "  --exclude LIST    leave these processors alone, e.g. 3,11 or 8-15\n"
          "  --idle            lowest priority: use only cycles nothing else wants\n"
          "  --literal         measure the literal compile instead of the optimized one\n"
          "  --total           scale: only the row with every thread\n"
          "  --max-mb M        sweep: stop at this much memory (default 256)\n"
          "\n"
          "tester1 options (every random program runs from 512 random states):\n"
          "  --cells LIST      machine sizes (default 8,16,32,64,128,256)\n"
          "  --ratio R         instructions per cell (default 4)\n"
          "  --programs P      programs per size (default 64)\n"
          "  --seconds S       run for S seconds instead (programs unlimited)\n"
          "  --max-passes T    give up on a cycle after T passes (default 1048576)\n"
          "  --seed S          which random programs (default 1)\n"
          "  --threads N, --exclude LIST, --idle   as for scale\n"
          "  --raw             machine-readable output; --merge FILES adds such outputs up\n"
          "\n",
          stderr);
    work_usage();
}

enum { ENG_REF, ENG_LANES, ENG_LITERAL, ENG_JIT };

static int parse_engine(const char *s)
{
    static const char *names[] = {"ref", "lanes", "literal", "jit"};
    for (int i = 0; i < 4; i++)
        if (!strcmp(s, names[i]))
            return i;
    fprintf(stderr, "unknown engine '%s' (ref, lanes, literal or jit)\n", s);
    return -1;
}

static int compile(const Program *p, int literal, JitCode *jc)
{
    char err[256];
    if (jit_compile(p, literal, jc, NULL, err, sizeof err))
        return 1;
    fprintf(stderr, "%s\n", err);
    return 0;
}

static int need_avx2(void)
{
    if (cpu_has_avx2())
        return 1;
    fputs("the 512-lane engine needs a CPU with AVX2\n", stderr);
    return 0;
}

/* Parses a positive integer option value. */
static int parse_count(const char *opt, const char *s, unsigned long long *v)
{
    char *end;
    *v = strtoull(s, &end, 10);
    if (*s && !*end && *v > 0)
        return 1;
    fprintf(stderr, "%s expects a positive number, got '%s'\n", opt, s);
    return 0;
}

/* ---- asm ---------------------------------------------------------------------------------- */

static int has(const uint32_t *list, size_t n, uint32_t a)
{
    for (size_t i = 0; i < n; i++)
        if (list[i] == a)
            return 1;
    return 0;
}

static void print_bits(uint32_t v, unsigned bits, int width)
{
    for (int b = (int)bits - 1; b >= 0; b--)
        putchar('0' + (int)((v >> b) & 1));
    printf("%*s", width - (int)bits, "");
}

static int cmd_asm(const Program *p, const char *path)
{
    unsigned ab = addr_bits(p->ncells);
    int w = ab > 4 ? (int)ab : 4;
    printf("%s: %zu instructions, %u cells\n", path, p->len, (unsigned)p->ncells);
    printf("encoding: [op:1][addr:%u] = %u bits per instruction, %zu bits in total\n\n", ab,
           ab + 1, p->len * (ab + 1));

    printf("  pc  op  %-*s  asm\n", w, "addr");
    for (size_t i = 0; i < p->len; i++) {
        const Instr *in = &p->code[i];
        printf("%4zu  %u   ", i, (unsigned)in->op);
        print_bits(in->addr, ab, w);
        printf("  %-4s %s\n", in->op == OP_NAND ? "NAND" : "STO", p->cell_names[in->addr]);
    }

    printf("\n  %-*s  cell\n", w, "addr");
    for (uint32_t a = 0; a < p->ncells; a++) {
        int i = has(p->inputs, p->ninputs, a), o = has(p->outputs, p->noutputs, a);
        char kind[96] = "";
        if (p->special && p->special[a].kind == CK_ID)
            snprintf(kind, sizeof kind, "lane id: bit %u of the machine's number",
                     p->special[a].dim);
        else if (p->special && p->special[a].kind == CK_LINK)
            snprintf(kind, sizeof kind, "link: %s of machine k XOR %u (dimension %u)",
                     p->cell_names[p->special[a].src], 1u << p->special[a].dim, p->special[a].dim);
        const char *role = a == CELL_ZERO ? "constant 0"
                         : a == CELL_ONE  ? "constant 1"
                         : *kind          ? kind
                         : i && o         ? "in, out"
                         : i              ? "in"
                         : o              ? "out"
                                          : "";
        printf("  ");
        print_bits(a, ab, w);
        if (*role)
            printf("  %-14s %s\n", p->cell_names[a], role);
        else
            printf("  %s\n", p->cell_names[a]);
    }
    return 0;
}

/* ---- run ---------------------------------------------------------------------------------- */

static void tick(RefMachine *m, int trace)
{
    const Program *p = m->prog;
    if (!trace || p->len == 0) {
        ref_tick(m);
        return;
    }
    printf("-- tick %llu\n", (unsigned long long)m->ticks + 1);
    do {
        size_t pc = m->pc;
        const Instr *in = &p->code[pc];
        const char *name = p->cell_names[in->addr];
        uint8_t read = ref_get(m, in->addr);
        ref_step(m);
        if (in->op == OP_NAND)
            printf("%6zu  NAND %-14s read  %s=%u  RR=%u\n", pc, name, name, read, m->rr);
        else
            printf("%6zu  STO  %-14s write %s=%u\n", pc, name, name, ref_get(m, in->addr));
    } while (m->pc != 0);
}

/* One pass of a whole cube, tracing machine `lane`. */
static void tick_cube(RefMachine *cube, unsigned lane, int trace)
{
    RefMachine *m = &cube[lane];
    const Program *p = m->prog;
    if (!trace || p->len == 0) {
        ref_cube_tick(cube);
        return;
    }
    printf("-- tick %llu, machine %u\n", (unsigned long long)m->ticks + 1, lane);
    do {
        size_t pc = m->pc;
        const Instr *in = &p->code[pc];
        const char *name = p->cell_names[in->addr];
        uint8_t read = ref_get(m, in->addr);
        ref_cube_step(cube);
        if (in->op == OP_NAND)
            printf("%6zu  NAND %-14s read  %s=%u  RR=%u\n", pc, name, name, read, m->rr);
        else
            printf("%6zu  STO  %-14s write %s=%u\n", pc, name, name, ref_get(m, in->addr));
    } while (m->pc != 0);
}

/* After a run the .out cells are shown, or RR if the program declares none. */
static size_t ncolumns(const Program *p)
{
    return p->noutputs ? p->noutputs : 1;
}

static const char *column_name(const Program *p, size_t i)
{
    return p->noutputs ? p->cell_names[p->outputs[i]] : "RR";
}

static void print_named(const Program *p, const uint8_t *vals)
{
    for (size_t i = 0; i < ncolumns(p); i++)
        printf(" %s=%u", column_name(p, i), vals[i]);
}

static void ref_outputs(const RefMachine *m, uint8_t *vals)
{
    const Program *p = m->prog;
    if (!p->noutputs)
        vals[0] = m->rr;
    for (size_t i = 0; i < p->noutputs; i++)
        vals[i] = ref_get(m, p->outputs[i]);
}

static void lane_outputs(const LaneMachine *m, unsigned lane, uint8_t *vals)
{
    const Program *p = m->prog;
    if (!p->noutputs)
        vals[0] = lane_rr(m, lane);
    for (size_t i = 0; i < p->noutputs; i++)
        vals[i] = lane_get(m, p->outputs[i], lane);
}

static unsigned input_bit(size_t n, size_t k, uint64_t row)
{
    return (unsigned)((row >> (n - 1 - k)) & 1);  /* the first .in cell is the high bit */
}

static void print_table_header(const Program *p)
{
    for (size_t k = 0; k < p->ninputs; k++)
        printf("%s ", p->cell_names[p->inputs[k]]);
    printf("|");
    for (size_t i = 0; i < ncolumns(p); i++)
        printf(" %s", column_name(p, i));
    putchar('\n');
}

static void print_table_row(const Program *p, uint64_t row, const uint8_t *vals)
{
    for (size_t k = 0; k < p->ninputs; k++)
        printf("%*u ", (int)strlen(p->cell_names[p->inputs[k]]), input_bit(p->ninputs, k, row));
    printf("|");
    for (size_t i = 0; i < ncolumns(p); i++)
        printf(" %*u", (int)strlen(column_name(p, i)), vals[i]);
    putchar('\n');
}

static void table_ref(const Program *p, unsigned long long ticks, int trace, uint8_t *vals)
{
    RefMachine m;
    if (!ref_init(&m, p))
        return;
    for (uint64_t row = 0; row < (1ull << p->ninputs); row++) {
        ref_reset(&m);
        for (size_t k = 0; k < p->ninputs; k++)
            ref_set(&m, p->inputs[k], (uint8_t)input_bit(p->ninputs, k, row));
        for (unsigned long long t = 0; t < ticks; t++)
            tick(&m, trace);
        ref_outputs(&m, vals);
        print_table_row(p, row, vals);
    }
    ref_free(&m);
}

/* Row number base + lane goes to lane `lane`: one pass computes 512 rows. Passes run compiled if
   jc is given, otherwise on the lane interpreter. */
static void table_lanes(const Program *p, const JitCode *jc, unsigned long long ticks,
                        uint8_t *vals)
{
    LaneMachine m;
    if (!lane_init(&m, p))
        return;
    uint64_t rows = 1ull << p->ninputs;
    for (uint64_t base = 0; base < rows; base += LANES) {
        unsigned count = rows - base < LANES ? (unsigned)(rows - base) : LANES;
        lane_reset(&m);
        for (unsigned l = 0; l < count; l++)
            for (size_t k = 0; k < p->ninputs; k++)
                lane_set(&m, p->inputs[k], l, (uint8_t)input_bit(p->ninputs, k, base + l));
        for (unsigned long long t = 0; t < ticks; t++) {
            if (jc)
                jit_tick(jc, &m);
            else
                lane_tick(&m);
        }
        for (unsigned l = 0; l < count; l++) {
            lane_outputs(&m, l, vals);
            print_table_row(p, base + l, vals);
        }
    }
    lane_free(&m);
}

static int set_cell(RefMachine *m, const char *arg)
{
    const char *eq = strchr(arg, '=');
    char name[256];
    if (!eq || (size_t)(eq - arg) >= sizeof name || (strcmp(eq + 1, "0") && strcmp(eq + 1, "1"))) {
        fprintf(stderr, "--set expects name=0 or name=1, got '%s'\n", arg);
        return 0;
    }
    memcpy(name, arg, (size_t)(eq - arg));
    name[eq - arg] = '\0';
    uint32_t a;
    if (!program_find_cell(m->prog, name, &a)) {
        fprintf(stderr, "no cell named '%s'\n", name);
        return 0;
    }
    if (a < CELL_FIRST_FREE) {
        fprintf(stderr, "%s is a constant cell\n", name);
        return 0;
    }
    ref_set(m, a, (uint8_t)(eq[1] - '0'));
    return 1;
}

static int cmd_run(const Program *p, int argc, char **argv)
{
    unsigned long long ticks = 1, lane = 0;
    int trace = 0, table = 0, engine = ENG_REF, nset = 0, rc = 1;
    RefMachine *cube = NULL;
    RefMachine m;
    uint8_t *vals = malloc(ncolumns(p));
    if (!vals || !ref_init(&m, p)) {
        fputs("out of memory\n", stderr);
        free(vals);
        return 1;
    }

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) {
            if (!parse_count("--ticks", argv[++i], &ticks))
                goto done;
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            if (!set_cell(&m, argv[++i]))
                goto done;
            nset++;
        } else if (!strcmp(argv[i], "--trace")) {
            trace = 1;
        } else if (!strcmp(argv[i], "--table")) {
            table = 1;
        } else if (!strcmp(argv[i], "--engine") && i + 1 < argc) {
            if ((engine = parse_engine(argv[++i])) < 0)
                goto done;
        } else if (!strcmp(argv[i], "--lane") && i + 1 < argc) {
            char *end;
            lane = strtoull(argv[++i], &end, 10);
            if (*end || lane >= ref_cube_machines(p)) {
                fprintf(stderr, "--lane expects 0 to %u\n", ref_cube_machines(p) - 1);
                goto done;
            }
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            goto done;
        }
    }

    if (p->special) {
        /* id and link cells: run all 512 machines of the cube, --set going to every one */
        if (table || engine != ENG_REF) {
            fputs("this program's machines read each other (id or link cells), so they run as a "
                  "cube: --table and --engine do not apply\n", stderr);
            goto done;
        }
        unsigned n = ref_cube_machines(p);
        cube = calloc(n, sizeof *cube);
        if (!cube || !ref_cube_init(cube, p)) {
            fputs("out of memory\n", stderr);
            goto done;
        }
        for (unsigned k = 0; k < n; k++)
            for (uint32_t a = CELL_FIRST_FREE; a < p->ncells; a++)
                ref_set(&cube[k], a, m.mem[a]);
        printf("cube of %u machines, showing machine %llu\n", n, lane);
        for (unsigned long long t = 0; t < ticks; t++) {
            tick_cube(cube, (unsigned)lane, trace);
            ref_outputs(&cube[lane], vals);
            printf("tick %llu:", (unsigned long long)cube[lane].ticks);
            print_named(p, vals);
            putchar('\n');
        }
        rc = 0;
        goto done;
    }

    if (engine != ENG_REF && (!table || trace)) {
        fputs("the 512-lane engines work with --table and without --trace; "
              "use 'alpha check' and 'alpha bench' for the rest\n", stderr);
        goto done;
    }
    if (table) {
        if (nset) {
            fputs("--table sets the inputs itself and cannot be combined with --set\n", stderr);
            goto done;
        }
        if (p->ninputs == 0 || p->ninputs > 20) {
            fprintf(stderr, "--table needs between 1 and 20 .in cells, this program has %zu\n",
                    p->ninputs);
            goto done;
        }
        if (engine != ENG_REF && !need_avx2())
            goto done;
        JitCode jc;
        if (engine >= ENG_LITERAL && !compile(p, engine == ENG_LITERAL, &jc))
            goto done;
        print_table_header(p);
        if (engine == ENG_REF)
            table_ref(p, ticks, trace, vals);
        else
            table_lanes(p, engine >= ENG_LITERAL ? &jc : NULL, ticks, vals);
        if (engine >= ENG_LITERAL)
            jit_free(&jc);
        rc = 0;
        goto done;
    }

    for (unsigned long long t = 0; t < ticks; t++) {
        tick(&m, trace);
        ref_outputs(&m, vals);
        printf("tick %llu:", (unsigned long long)m.ticks);
        print_named(p, vals);
        putchar('\n');
    }
    rc = 0;
done:
    if (cube) {
        ref_cube_free(cube);
        free(cube);
    }
    ref_free(&m);
    free(vals);
    return rc;
}

/* ---- jit ---------------------------------------------------------------------------------- */

static int cmd_jit(const Program *p, const char *path, int argc, char **argv)
{
    int literal = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--literal")) {
            literal = 1;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }
    JitCode jc;
    char *listing, err[256];
    if (!jit_compile(p, literal, &jc, &listing, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    const JitStats *s = &jc.stats;
    printf("%s: %zu instructions -> %zu x86 instructions, %zu bytes (%s)\n", path, p->len, s->x86,
           s->bytes, literal ? "literal" : "optimized");
    printf("one pass for 512 lanes: %zu vector logic ops, %zu loads, %zu stores; "
           "%zu of %zu instructions need no x86 code\n\n",
           s->alu, s->loads, s->stores, s->free, p->len);
    fputs(listing ? listing : "", stdout);
    free(listing);
    jit_free(&jc);
    return 0;
}

/* ---- check -------------------------------------------------------------------------------- */

static const char *const engine_names[] = {"reference machine", "lane interpreter",
                                           "literal compiled code", "compiled code"};

static int cmd_check(const Program *p, int argc, char **argv)
{
    unsigned long long ticks = 8, seed = 1;
    int steps = 0, engine = ENG_LANES;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) {
            if (!parse_count("--ticks", argv[++i], &ticks))
                return 1;
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            if (!parse_count("--seed", argv[++i], &seed))
                return 1;
        } else if (!strcmp(argv[i], "--steps")) {
            steps = 1;
        } else if (!strcmp(argv[i], "--engine") && i + 1 < argc) {
            if ((engine = parse_engine(argv[++i])) < 0)
                return 1;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }
    if (engine == ENG_REF) {
        fputs("check compares a 512-lane engine against the reference: lanes, literal or jit\n",
              stderr);
        return 1;
    }
    if (steps && engine != ENG_LANES) {
        fputs("--steps needs --engine lanes: compiled code has no state between instructions\n",
              stderr);
        return 1;
    }
    if (!need_avx2())
        return 1;

    JitCode jc;
    if (engine >= ENG_LITERAL && !compile(p, engine == ENG_LITERAL, &jc))
        return 1;
    CheckResult r;
    check_lanes(p, engine >= ENG_LITERAL ? &jc : NULL, seed, (unsigned)ticks, steps, &r);
    if (engine >= ENG_LITERAL)
        jit_free(&jc);

    if (r.ok) {
        unsigned n = ref_cube_machines(p);
        if (n > LANES)
            printf("OK: the %s matches the reference cube in all %u machines (%u groups of %d) "
                   "after every %s\n",
                   engine_names[engine], n, n / LANES, LANES, steps ? "instruction" : "tick");
        else
            printf("OK: the %s matches the reference machine in all %d lanes after every %s\n",
                   engine_names[engine], LANES, steps ? "instruction" : "tick");
        printf("    %llu ticks from random state (seed %llu), RR + %u cells per lane, "
               "%llu lane states compared\n",
               ticks, seed, (unsigned)p->ncells, (unsigned long long)r.compared);
        return 0;
    }
    printf("MISMATCH in tick %llu", (unsigned long long)r.tick);
    if (r.pc >= 0)
        printf(" after pc %ld", r.pc);
    printf(", %s %u, %s: reference %u, %s %u\n", ref_cube_machines(p) > LANES ? "machine" : "lane",
           r.lane,
           r.cell < 0 ? "RR" : p->cell_names[r.cell], r.want, engine_names[engine], r.got);
    return 1;
}

/* ---- bench -------------------------------------------------------------------------------- */

typedef uint64_t (*TickFn)(void *);  /* runs some passes, returns how many */

typedef struct {
    const JitCode *jc;
    LaneMachine   *m;
} Compiled;

static uint64_t tick_ref(void *m)
{
    ref_tick(m);
    return 1;
}

static uint64_t tick_lanes(void *m)
{
    lane_tick(m);
    return 1;
}

static uint64_t tick_jit(void *c)
{
    const Compiled *x = c;
    jit_run(x->jc, x->m, 64);  /* the loop runs inside the compiled code */
    return 64;
}

/* Finds a batch of calls that takes at least 50 ms, then runs for about `seconds`. */
static double ticks_per_second(TickFn fn, void *m, double seconds)
{
    uint64_t n = 1;
    double dt;
    for (;;) {
        double t0 = now_seconds();
        for (uint64_t i = 0; i < n; i++)
            fn(m);
        dt = now_seconds() - t0;
        if (dt >= 0.05)
            break;
        n *= 2;
    }
    uint64_t calls = (uint64_t)((double)n * seconds / dt) + 1, passes = 0;
    double t0 = now_seconds();
    for (uint64_t i = 0; i < calls; i++)
        passes += fn(m);
    return (double)passes / (now_seconds() - t0);
}

static const char *si(double v, char *buf, size_t len)
{
    const char *unit = "";
    if (v >= 1e12)      { v /= 1e12; unit = " T"; }
    else if (v >= 1e9)  { v /= 1e9;  unit = " G"; }
    else if (v >= 1e6)  { v /= 1e6;  unit = " M"; }
    else if (v >= 1e3)  { v /= 1e3;  unit = " k"; }
    snprintf(buf, len, "%.2f%s", v, unit);
    return buf;
}

static uint64_t tick_ref_cube(void *cube)
{
    ref_cube_tick(cube);
    return 1;
}

/* base: machine-passes per second of the reference, for the "vs ref" column */
static void bench_row(const char *name, double tps, double base, size_t len, unsigned lanes)
{
    char a[32], b[32], c[32];
    double ips = tps * (double)len;
    printf("%-14s %10s %15s %14s %15.3f %10.0fx\n", name, si(tps, a, sizeof a),
           si(ips, b, sizeof b), si(ips * lanes, c, sizeof c), 1e9 / ips, tps * lanes / base);
}

static int cmd_bench(const Program *p, const char *path, int argc, char **argv)
{
    double seconds = 1.0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
            if (seconds <= 0) {
                fputs("--seconds expects a positive number\n", stderr);
                return 1;
            }
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }
    if (p->len == 0) {
        fputs("nothing to measure: the program is empty\n", stderr);
        return 1;
    }
    if (!need_avx2())
        return 1;

    /* programs with id or link cells: the reference is the whole cube of 512 machines */
    RefMachine rm, *cube = p->special ? calloc(ref_cube_machines(p), sizeof *cube) : NULL;
    LaneMachine lm;
    JitCode lit, opt;
    if ((p->special ? !cube || !ref_cube_init(cube, p) : !ref_init(&rm, p)) || !lane_init(&lm, p)) {
        fputs("out of memory\n", stderr);
        return 1;
    }
    double t0 = now_seconds();
    if (!compile(p, 1, &lit))
        return 1;
    double t1 = now_seconds();
    if (!compile(p, 0, &opt))
        return 1;
    double t2 = now_seconds();

    int perf;
    int cpu = pin_to_fast_core(&perf);
    printf("%s: %zu instructions, %u cells\n", path, p->len, (unsigned)p->ncells);
    if (cpu >= 0)
        printf("one thread on logical processor %d%s, %.1f s per engine\n\n", cpu,
               perf ? " (performance core)" : "", seconds);
    else
        printf("one thread (not pinned), %.1f s per engine\n\n", seconds);

    Compiled cl = {&lit, &lm}, co = {&opt, &lm};
    double ref_tps = cube ? ticks_per_second(tick_ref_cube, cube, seconds)
                          : ticks_per_second(tick_ref, &rm, seconds);
    unsigned ref_lanes = cube ? LANES : 1;
    double base = ref_tps * ref_lanes;
    double lane_tps = ticks_per_second(tick_lanes, &lm, seconds);
    double lit_tps = ticks_per_second(tick_jit, &cl, seconds);
    double opt_tps = ticks_per_second(tick_jit, &co, seconds);

    printf("%-14s %10s %15s %14s %15s %11s\n", "engine", "ticks/s", "instructions/s",
           "1-bit ops/s", "ns/instruction", "vs ref");
    bench_row(cube ? "ref cube x512" : "reference", ref_tps, base, p->len, ref_lanes);
    bench_row("lanes x512", lane_tps, base, p->len, LANES);
    bench_row("literal x512", lit_tps, base, p->len, LANES);
    bench_row("jit x512", opt_tps, base, p->len, LANES);

    char a[32];
    printf("\ncompiled pass, literal:   %6zu x86 instructions, %6zu vector logic ops "
           "(compiled in %.2f ms)\n", lit.stats.x86, lit.stats.alu, (t1 - t0) * 1e3);
    printf("compiled pass, optimized: %6zu x86 instructions, %6zu vector logic ops "
           "(compiled in %.2f ms)\n", opt.stats.x86, opt.stats.alu, (t2 - t1) * 1e3);
    printf("jit is %.2fx the lane interpreter and %.2fx the literal compile\n",
           opt_tps / lane_tps, opt_tps / lit_tps);
    printf("memory: %u lines of 64 bytes = %.1f KB for all 512 lanes; jit moves %sB/s\n",
           (unsigned)p->ncells, p->ncells * 64 / 1024.0,
           si(opt_tps * (double)(opt.stats.loads + opt.stats.stores) * 32, a, sizeof a));

    jit_free(&lit);
    jit_free(&opt);
    if (cube) {
        ref_cube_free(cube);
        free(cube);
    } else {
        ref_free(&rm);
    }
    lane_free(&lm);
    return 0;
}

/* ---- scale and sweep: compiled code on many threads / much memory ------------------------- */

#define MAX_CPUS 1024

typedef struct {
    double        seconds;
    int           idle, literal, total_only;
    double        max_mb;
    unsigned char excluded[MAX_CPUS];
} ParOpts;

/* "3,11" or "8-15,24" -> excluded[id] = 1 */
static int parse_cpu_list(const char *s, unsigned char *flag)
{
    const char *p = s;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            return 0;
        p = end;
        if (*p == '-') {
            b = strtol(p + 1, &end, 10);
            if (end == p + 1)
                return 0;
            p = end;
        }
        if (a < 0 || b >= MAX_CPUS || a > b)
            return 0;
        for (long i = a; i <= b; i++)
            flag[i] = 1;
        if (*p == ',')
            p++;
        else if (*p)
            return 0;
    }
    return 1;
}

static int parse_par_opts(int argc, char **argv, ParOpts *o, double seconds)
{
    memset(o, 0, sizeof *o);
    o->seconds = seconds;
    o->max_mb = 256;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            o->seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--max-mb") && i + 1 < argc) {
            o->max_mb = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) {
            if (!parse_cpu_list(argv[++i], o->excluded)) {
                fprintf(stderr, "--exclude expects processor numbers like 3,11 or 8-15\n");
                return 0;
            }
        } else if (!strcmp(argv[i], "--idle")) {
            o->idle = 1;
        } else if (!strcmp(argv[i], "--literal")) {
            o->literal = 1;
        } else if (!strcmp(argv[i], "--total")) {
            o->total_only = 1;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 0;
        }
    }
    if (o->seconds <= 0 || o->max_mb <= 0) {
        fputs("--seconds and --max-mb expect positive numbers\n", stderr);
        return 0;
    }
    return 1;
}

static void print_machine(const Cpu *c, int n, const ParOpts *o)
{
    char host[128], brand[96];
    host_name(host, sizeof host);
    cpu_brand(brand, sizeof brand);
    int cores = n ? c[n - 1].core + 1 : 0, fast = 0, slow = 0, numa = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].smt == 0 && c[i].fast)
            fast++;
        else if (c[i].smt == 0)
            slow++;
        if (c[i].numa + 1 > numa)
            numa = c[i].numa + 1;
    }
    printf("%s: %s, %d cores / %d threads", host, brand, cores, n);
    if (slow)
        printf(" (%d P-cores, %d E-cores)", fast, slow);
    if (numa > 1)
        printf(", %d NUMA nodes", numa);
    printf("\n");
    int first = 1;
    for (int i = 0; i < n; i++) {
        if (o->excluded[c[i].id]) {
            printf("%s%d", first ? "left free: processors " : ",", c[i].id);
            first = 0;
        }
    }
    if (!first)
        printf("\n");
    printf("%s priority\n", o->idle ? "idle (only cycles nothing else wants)" : "normal");
}

/* Processor for single-thread runs: first free thread of the last fast core. */
static int fast_cpu(const Cpu *c, int n, const ParOpts *o)
{
    int best = -1, best_core = -1;
    for (int i = 0; i < n; i++)
        if (c[i].fast && !o->excluded[c[i].id] && c[i].core > best_core) {
            best_core = c[i].core;
            best = c[i].id;
        }
    for (int i = 0; i < n; i++)  /* lowest thread of that core */
        if (c[i].core == best_core && !o->excluded[c[i].id]) {
            best = c[i].id;
            break;
        }
    return best;
}

static double ops_per_second(const JitCode *jc, const ParResult *r)
{
    return (double)r->passes * (double)jc->prog->len * LANES / r->seconds;
}

static int cmd_scale(const Program *p, int argc, char **argv)
{
    ParOpts o;
    if (!parse_par_opts(argc, argv, &o, 2.0) || !need_avx2())
        return 1;
    if (p->len == 0) {
        fputs("nothing to measure: the program is empty\n", stderr);
        return 1;
    }
    static Cpu c[MAX_CPUS];
    int n = cpu_topology(c, MAX_CPUS);
    if (n == 0) {
        fputs("cannot read the processor topology\n", stderr);
        return 1;
    }
    JitCode jc;
    if (!compile(p, o.literal, &jc))
        return 1;
    print_machine(c, n, &o);
    printf("%zu instructions, %u cells, %s compile, %.1f s per row\n\n", p->len,
           (unsigned)p->ncells, o.literal ? "literal" : "optimized", o.seconds);

    int hybrid = 0, ref_core = -1, ref_avail = 0;
    for (int i = 0; i < n; i++)
        hybrid |= !c[i].fast;
    const char *big = hybrid ? "P-core" : "core";
    for (int core = 0; core <= c[n - 1].core; core++) {
        int avail = 0, fast = 0;
        for (int i = 0; i < n; i++)
            if (c[i].core == core && !o.excluded[c[i].id]) {
                avail++;
                fast = c[i].fast;
            }
        if (fast && avail >= ref_avail) {
            ref_core = core;
            ref_avail = avail;
        }
    }
    if (ref_core < 0) {
        fputs("every fast core is excluded\n", stderr);
        jit_free(&jc);
        return 1;
    }

    int *ids = malloc((size_t)n * sizeof *ids), *prev = malloc((size_t)n * sizeof *prev);
    int nprev = 0, best_k = 0;
    double first = 0, best = 0;
    printf("%7s  %-34s %14s %12s %8s\n", "threads", "where", "1-bit ops/s", "per thread",
           "scaling");
    /* Rows 5 and 6 are the two ways to use the whole machine: every hardware thread, or one
       thread per core (better where SMT costs more than it gives). --total measures only these
       and reports the better one. */
    for (int row = 0; row < 7; row++) {
        int k = 0, cores = 0, lastcore = -1, two = 0;
        char label[64];
        for (int i = 0; i < n; i++) {
            if (o.excluded[c[i].id])
                continue;
            int take = row == 0 ? c[i].core == ref_core && k == 0
                     : row == 1 ? c[i].core == ref_core
                     : row == 2 ? c[i].fast && c[i].core != lastcore
                     : row == 3 ? c[i].fast
                     : row == 4 ? !c[i].fast
                     : row == 5 ? 1
                                : c[i].core != lastcore;
            if (!take)
                continue;
            two |= c[i].core == lastcore;
            cores += c[i].core != lastcore;
            lastcore = c[i].core;
            ids[k++] = c[i].id;
        }
        if (row == 0)
            snprintf(label, sizeof label, "1 %s", big);
        else if (row == 1)
            snprintf(label, sizeof label, "1 %s, both hardware threads", big);
        else if (row == 2)
            snprintf(label, sizeof label, "%d %ss, 1 thread each", cores, big);
        else if (row == 3)
            snprintf(label, sizeof label, "%d %ss, all hardware threads", cores, big);
        else if (row == 4)
            snprintf(label, sizeof label, "%d E-cores", cores);
        else if (row == 5)
            snprintf(label, sizeof label, "everything");
        else
            snprintf(label, sizeof label, "everything, 1 thread per core (%d)", cores);
        /* skip rows that are empty or repeat the previous one; row 6 repeats row 2 unless hybrid */
        int same = k == nprev && memcmp(ids, prev, (size_t)k * sizeof *ids) == 0;
        if (k == 0 || same || (row == 1 && !two) || (row == 3 && !two) || (row == 4 && !hybrid) ||
            (o.total_only && row < 5) || (!o.total_only && row == 6 && !hybrid))
            continue;
        memcpy(prev, ids, (size_t)k * sizeof *ids);
        nprev = k;

        ParResult r;
        par_measure(&jc, k, ids, 1, o.idle, o.seconds, &r);
        double ops = ops_per_second(&jc, &r);
        if (first == 0)
            first = ops / k;
        char a[32], b[32];
        printf("%7d  %-34s %14s %12s %7.2fx%s\n", k, label, si(ops, a, sizeof a),
               si(ops / k, b, sizeof b), ops / first, r.pinned < k ? "  (not all pinned)" : "");
        fflush(stdout);
        if (row >= 5 && ops > best) {
            best = ops;
            best_k = k;
        }
    }
    if (o.total_only) {
        char host[128];
        host_name(host, sizeof host);
        printf("TOTAL %s %.6e %d\n", host, best, best_k);
    }
    free(ids);
    free(prev);
    jit_free(&jc);
    return 0;
}

static int cmd_sweep(const Program *p, int argc, char **argv)
{
    ParOpts o;
    if (!parse_par_opts(argc, argv, &o, 1.0) || !need_avx2())
        return 1;
    if (p->len == 0) {
        fputs("nothing to measure: the program is empty\n", stderr);
        return 1;
    }
    static Cpu c[MAX_CPUS];
    int n = cpu_topology(c, MAX_CPUS), cpu = fast_cpu(c, n, &o);
    JitCode jc;
    if (!compile(p, o.literal, &jc))
        return 1;
    print_machine(c, n, &o);
    printf("one thread on processor %d, %zu instructions, %u cells, %.1f s per row\n\n", cpu,
           p->len, (unsigned)p->ncells, o.seconds);
    printf("%8s %10s %12s %14s %8s\n", "groups", "lanes", "memory", "1-bit ops/s", "vs 1");

    double first = 0, group_bytes = (double)(p->ncells + 1) * 64;
    for (int groups = 1; groups * group_bytes <= o.max_mb * 1048576; groups *= 4) {
        ParResult r;
        par_measure(&jc, 1, cpu >= 0 ? &cpu : NULL, groups, o.idle, o.seconds, &r);
        double ops = ops_per_second(&jc, &r), bytes = groups * group_bytes;
        if (first == 0)
            first = ops;
        char a[32], mem[32];
        if (bytes < 1048576)
            snprintf(mem, sizeof mem, "%.1f KB", bytes / 1024);
        else
            snprintf(mem, sizeof mem, "%.1f MB", bytes / 1048576);
        printf("%8d %10d %12s %14s %7.2fx\n", groups, groups * LANES, mem, si(ops, a, sizeof a),
               ops / first);
        fflush(stdout);
    }
    jit_free(&jc);
    return 0;
}

/* ---- random programs and tester 1 --------------------------------------------------------- */

static int parse_u64(const char *opt, const char *s, unsigned long long *v)
{
    char *end;
    *v = strtoull(s, &end, 0);  /* decimal or 0x... */
    if (*s && !*end)
        return 1;
    fprintf(stderr, "%s expects a number, got '%s'\n", opt, s);
    return 0;
}

static int cmd_random(int argc, char **argv)
{
    unsigned long long cells = 0, len = 0, seed = 1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--cells") && i + 1 < argc) {
            if (!parse_u64("--cells", argv[++i], &cells))
                return 1;
        } else if (!strcmp(argv[i], "--len") && i + 1 < argc) {
            if (!parse_u64("--len", argv[++i], &len))
                return 1;
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            if (!parse_u64("--seed", argv[++i], &seed))
                return 1;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }
    if (cells == 0 || cells > 1000000) {
        fputs("--cells expects a number of cells (1 to 1000000)\n", stderr);
        return 1;
    }
    if (len == 0)
        len = 4 * cells;
    Program p;
    random_program(&p, (uint32_t)cells, (size_t)len, seed);
    printf("; random program: %llu cells, %llu instructions\n", cells, len);
    printf(";   alpha random --cells %llu --len %llu --seed 0x%llx\n", cells, len, seed);
    printf("; every opcode bit is a coin flip; NAND reads any cell, STO writes any of c0..c%llu\n",
           cells - 1);
    for (uint32_t a = CELL_FIRST_FREE; a < p.ncells; a++)
        printf("%s%s", (a - CELL_FIRST_FREE) % 16 == 0 ? (a > CELL_FIRST_FREE ? "\n.out " : ".out ")
                                                       : " ",
               p.cell_names[a]);
    printf("\n\n");
    for (size_t i = 0; i < p.len; i++)
        printf("    %-4s %s\n", p.code[i].op == OP_NAND ? "NAND" : "STO",
               p.cell_names[p.code[i].addr]);
    program_free(&p);
    return 0;
}

static int cmd_tester1(int argc, char **argv)
{
    static unsigned char excluded[MAX_CPUS];
    T1Config cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *cells = "8,16,32,64,128,256";
    cfg.ratio = 4;
    cfg.max_passes = 1 << 20;
    cfg.seed = 1;
    cfg.excluded = excluded;
    unsigned long long programs = 64, v;
    int programs_set = 0, raw = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--merge")) {
            T1Size s[T1_SIZES];
            double seconds;
            int threads;
            uint64_t max_passes;
            int n = t1_merge(argv + i + 1, argc - i - 1, s, &seconds, &threads, &max_passes);
            if (n < 0) {
                fputs("cannot read a file to merge\n", stderr);
                return 1;
            }
            t1_print(s, n, seconds, threads, max_passes);
            return 0;
        } else if (!strcmp(argv[i], "--cells") && i + 1 < argc) {
            cells = argv[++i];
        } else if (!strcmp(argv[i], "--ratio") && i + 1 < argc) {
            cfg.ratio = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--programs") && i + 1 < argc) {
            if (!parse_u64("--programs", argv[++i], &programs))
                return 1;
            programs_set = 1;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            cfg.seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--max-passes") && i + 1 < argc) {
            if (!parse_u64("--max-passes", argv[++i], &v))
                return 1;
            cfg.max_passes = v;
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            if (!parse_u64("--seed", argv[++i], &v))
                return 1;
            cfg.seed = v;
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            cfg.threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) {
            if (!parse_cpu_list(argv[++i], excluded)) {
                fputs("--exclude expects processor numbers like 3,11 or 8-15\n", stderr);
                return 1;
            }
        } else if (!strcmp(argv[i], "--idle")) {
            cfg.idle = 1;
        } else if (!strcmp(argv[i], "--raw")) {
            raw = 1;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }
    for (const char *p = cells; *p;) {
        char *end;
        unsigned long n = strtoul(p, &end, 10);
        if (end == p || n == 0 || n > 100000 || cfg.nsizes == T1_SIZES) {
            fputs("--cells expects a list like 8,16,32 (1 to 100000 cells, at most 32 sizes)\n",
                  stderr);
            return 1;
        }
        cfg.cells[cfg.nsizes++] = (uint32_t)n;
        p = *end == ',' ? end + 1 : end;
    }
    cfg.programs = cfg.seconds > 0 && !programs_set ? 0 : programs;
    if (cfg.ratio <= 0 || cfg.max_passes == 0 || (cfg.programs == 0 && cfg.seconds <= 0)) {
        fputs("--ratio, --max-passes and --programs or --seconds must be positive\n", stderr);
        return 1;
    }
    if (!need_avx2())
        return 1;

    T1Size s[T1_SIZES];
    int threads;
    double wall = t1_run(&cfg, s, &threads);
    if (raw)
        t1_print_raw(s, cfg.nsizes, wall, threads, cfg.max_passes);
    else
        t1_print(s, cfg.nsizes, wall, threads, cfg.max_passes);
    return 0;
}

/* ---- main --------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int rc = work_main(argc, argv);
    if (rc >= 0)
        return rc;
    if (argc >= 2 && !strcmp(argv[1], "random"))
        return cmd_random(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "tester1"))
        return cmd_tester1(argc - 2, argv + 2);

    static const char *cmds[] = {"asm", "jit", "run", "check", "bench", "scale", "sweep"};
    int cmd = -1;
    for (int i = 0; argc >= 3 && i < 7; i++)
        if (!strcmp(argv[1], cmds[i]))
            cmd = i;
    if (cmd < 0 || (cmd == 0 && argc != 3)) {
        usage();
        return 2;
    }

    Program p;
    char err[1024];
    if (!load_program(argv[2], &p, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (cmd >= 4 && program_dims(&p) > CUBE_DIMS) {
        fprintf(stderr, "%s reaches dimension %u: its %u machines only run as one cube "
                        "(alpha run, check; alpha cube --dims for speed)\n",
                argv[2], program_dims(&p) - 1, ref_cube_machines(&p));
        program_free(&p);
        return 1;
    }
    rc = cmd == 0 ? cmd_asm(&p, argv[2])
           : cmd == 1 ? cmd_jit(&p, argv[2], argc - 3, argv + 3)
           : cmd == 2 ? cmd_run(&p, argc - 3, argv + 3)
           : cmd == 3 ? cmd_check(&p, argc - 3, argv + 3)
           : cmd == 4 ? cmd_bench(&p, argv[2], argc - 3, argv + 3)
           : cmd == 5 ? cmd_scale(&p, argc - 3, argv + 3)
                      : cmd_sweep(&p, argc - 3, argv + 3);
    program_free(&p);
    return rc;
}
