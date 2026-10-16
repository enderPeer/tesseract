# Roadmap

**Tesseract** is a 1-bit computer: a 1-bit opcode and two instructions, `NAND` and `STO`. All the
rest — 512 machines per cache line, a compiler to x86 machine code, machines wired into a hypercube
of up to 65,536, a byte computer and a world of cells built out of them, proofs of the shortest
programs — comes from the engines, the compiler, the macro library and the host, never from new
instructions. Every engine is checked bit for bit against one reference model. The machine and
its command are called `alpha`. New here? Start with [docs/TUTORIAL.md](docs/TUTORIAL.md).

Measurements are from the author's workstation (i9-13900K: 8 P + 16 E cores, AVX2, no AVX-512) and
a small LAN cluster of four Ubuntu machines; yours will differ.

| # | Step | Status | Highlights |
|---|---|---|---|
| 1 | Reference machine | ✅ | the golden model: one byte per cell, one instruction at a time |
| 2 | 512-lane bitsliced engine | ✅ | 512 machines in one 64-byte line, AVX2 |
| 3 | ~~ISA v2: native opcodes~~ | dropped | the machine stays NAND/STO |
| 4 | Compiler to AVX2 | ✅ | 5.2 T 1-bit ops/s on one core |
| 5 | All cores, then the cluster | 5a ✅ 5b ✅ 5c ⬜ | 74 T/s on the workstation, ≈ 200 T/s with the cluster |
| 6 | Workloads | ✅ | all 2^32 inputs of a 16-bit multiplier in 9.8 s; a 2^40 search in 60 s |
| 7 | Hypercube: connecting the machines | 7a–7f ✅ 7g ⬜ | collectives, sort, any permutation, NTT, Life on one board, 65,536 machines |
| 8 | Toward an operating system | 8a ✅ 8b ✅ 8c ⬜ | beta, a byte computer with a monitor: 16,384 of them, 1.3 × 10^9 instructions/s |
| 9 | Real 1-bit hardware (FPGA) | ⬜ | |
| 10 | Superoptimizer | ✅ | every library building block provably shortest; full adder 28 → 19, sort 2× faster |
| 11 | Gamma: a world of cells on the cube | 11a ✅ 11b ✅ 11c ✅ 11d ✅ 11e–11g ⬜ | cells that are NAND circuits evolve from a random soup, divide, feed each other, and live under day and night, manna, a term and the lot; ~11,000 ticks/s for 32,768 voxels |
| – | ~~Control flow with lane masks~~ | parked | needs a new instruction (see the end) |

**Open:** gamma's ecology (the floor knows no weather and holds 60 % of the cells: see 11d) and
11e (a live view), 10 (optimize for the compiler's cost, not only the length), 7g (one cube across
the cluster), 5c (AVX-512), 8c (more memory, networked betas), 9 (FPGA).

## 1. Reference machine ✅

The golden model. Slow on purpose — one byte per cell, one instruction at a time — so it is
obviously correct. Every faster engine is tested against it.

- ISA v1 spec: [docs/ISA.md](docs/ISA.md). 1-bit opcode (`NAND`, `STO`), bit-addressed cells,
  constant cells `ZERO`/`ONE`, no jumps, the program repeats every tick.
- Assembler with macros and `@local` scratch cells; standard macros in [lib/std.inc](lib/std.inc).
- `alpha asm` prints the machine code bit by bit; `alpha run` runs, traces and prints truth tables.
- Examples: xor, mux, full adder, 8-bit adder, 4-bit counter.
- Done: `build test` passes 214 checks, including all 131,072 inputs of the 8-bit adder.

## 2. 512-lane bitsliced engine ✅

Same ISA, 512 independent machines at once: [src/lanes.c](src/lanes.c).

- Memory transposed: cell `a` of all 512 machines is 64-byte line `a` (64-byte aligned), lane k is
  bit k of the line. `RR` is two 256-bit registers.
- `NAND a` = 2× `vpand` + 2× `vpxor`, `STO a` = 2× `vmovdqa`. Every instruction touches exactly one
  cache line.
- Each lane gets its own inputs: `alpha run --table --engine lanes` computes 512 rows of a truth
  table per pass.
- `alpha check` gives every lane its own random state (all cells and RR) and compares all 512 lanes
  with 512 reference machines after every tick, or every instruction with `--steps`.
- Done: `build test` passes 149 more checks — all examples and macros, 100 random programs, every
  lane, after every instruction. Two deliberately injected engine bugs were both caught. The
  131,072-row adder table is byte-identical from both engines.

**First benchmark** (`alpha bench examples\adder8.asm`, one thread pinned to a P-core):

| engine | instructions/s | 1-bit ops/s | ns per instruction |
|---|---|---|---|
| reference | 0.90 G | 0.90 G | 1.12 |
| lanes x512 | 1.52 G | **778 G** | 0.66 |

868× more 1-bit operations per second than the reference. The whole state of 512 machines is
4.2 KB, so it lives in L1, and the engine reads or writes 97 GB/s there — more than the RAM could
deliver (~77 GB/s).

Where the remaining time goes: one P-core can do 768 1-bit operations per clock (3 vector ALUs ×
256 bits), about 4.4 T/s; this engine reaches about 18% of that. Three limits, attacked in step 4:

1. NAND costs two vector operations (AND, then invert).
2. Every instruction waits for the previous one through RR: at least 2 cycles each.
3. The interpreter loads, decodes and branches per instruction, and scratch cells make a round
   trip through memory (store, then load the same line a few instructions later).

## 3. ~~ISA v2: native opcodes~~ (dropped)

Dropped by decision: the machine keeps its 1-bit opcode and only `NAND` and `STO`. The speed this
step would have brought comes from the compiler in step 4 instead, which translates NAND/STO into
whatever x86 does fastest without changing the machine.

## 4. Compiler to AVX2 ✅

[src/jit.c](src/jit.c) turns a NAND/STO program into x86-64 machine code for one pass of all 512
lanes, written straight into executable memory: no C compiler at run time, about 0.1 ms per
compile. [src/x86.c](src/x86.c) encodes the handful of AVX2 instructions it needs.

x86 has no NAND instruction, so every engine builds NAND from AND / AND-NOT / XOR. Two
translations:

- **literal** (`--literal`): one instruction at a time. NAND = AND + invert, STO = store. Removes
  only the interpreter's decoding.
- **optimized** (default): the compiler tracks at compile time what every register holds during
  the pass and emits only the work that is left.
  - RR carries an "inverted" flag, so NAND x is a single AND (or AND-NOT) and `NAND ONE` is free.
  - Constants fold: `NAND ZERO`, and constants stored and read back, cost nothing.
  - Cells stored during the pass stay in 7 register pairs for later reads (least recently used goes
    first); every store still goes to memory.

`alpha jit file.asm` prints the generated code next to each NAND/STO instruction.

Done:
- The encoder matches Microsoft's assembler (ml64) byte for byte on 19 reference encodings
  (registers 8-15, 2- and 3-byte VEX, negative / 8-bit / 32-bit displacements).
- `build test` passes 664 more checks: both translations of every example, every macro and 300
  random programs (up to 3,000 instructions and 400 cells), all 512 lanes after every pass, from
  random state. Two injected compiler bugs were both caught. The 131,072-row adder table is
  byte-identical to the reference's.

**Benchmark** (`alpha bench examples\adder8.asm`, one thread on a P-core):

| engine | 1-bit ops/s | ns per instruction | vector logic ops per pass |
|---|---|---|---|
| reference | 0.93 G | 1.07 | – |
| lanes x512 (interpreter) | 763 G | 0.67 | 672 + decoding |
| literal x512 | 1.55 T | 0.33 | 672 |
| **jit x512** | **4.93 T** | **0.10** | **258** |

The optimized code is 6.5× the interpreter and 3.2× the literal translation: 5,279× the
reference machine, on one core. A pass of the 8-bit adder for 512 machines takes 23 ns.

Interleaving two 512-lane groups to overlap their RR chains was left for later. Step 5a measured the
same effect with hyper-threading (two threads per P-core): +3%, so it is not worth building.

## 5. All cores, then the whole cluster

The lanes are independent machines, so the work splits with no communication at all: every thread
runs its own 512-lane groups, every node its own threads. Hardware: four Ubuntu machines on a LAN
plus the workstation, 64 cores / 104 threads, all x86-64 with AVX2 (the CPUs are in 5b's table).

### 5a. This workstation ✅

[src/parallel.c](src/parallel.c) runs compiled code on many threads. Each thread is pinned to one
processor and allocates its own 512-lane groups (so they land in its NUMA node); all threads start
and stop together. The compiled code now loops over passes itself (`jit_run`), so a call costs
nothing per pass. `alpha scale` and `alpha sweep` measure; `build test` checks the loop (671
compiler checks, including ml64-verified `dec`/`jnz` encodings).

`alpha scale examples\adder8.asm` (i9-13900K):

| threads | where | 1-bit ops/s | per thread |
|---:|---|---:|---:|
| 1 | 1 P-core | 5.22 T | 5.22 T |
| 2 | 1 P-core, both hardware threads | 5.37 T | 2.68 T |
| 8 | 8 P-cores, 1 thread each | 42.5 T | 5.31 T |
| 16 | 8 P-cores, all hardware threads | 43.2 T | 2.70 T |
| 16 | 16 E-cores | 37.3 T | 2.33 T |
| **32** | **everything** | **74.3 T** | 2.32 T |

- Hyper-threading adds 3%: the compiled code already keeps a P-core busy, so interleaving two
  groups in the compiler would gain little. Dropped from step 4's leftovers.
- An E-core does 45% of a P-core. All 24 cores together reach 74 T, a bit less than P + E
  measured apart (80 T): the chip's power limit lowers the clocks when everything runs.

`alpha sweep examples\adder8.asm` (one P-core thread, more and more 512-lane groups):

| memory | where it lives | 1-bit ops/s | vs L1 |
|---:|---|---:|---:|
| 4.2 KB | L1 (48 KB) | 5.27 T | 1.00× |
| 17 KB | L1 | 4.77 T | 0.90× |
| 68 KB – 1.1 MB | L2 (2 MB) | 2.3–2.4 T | 0.44× |
| 4.2 – 17 MB | L3 (36 MB) | 1.3 T | 0.25× |
| 68 – 272 MB | RAM | 0.52 T | 0.10× |

From 4 groups on, each pass is its own call, which accounts for most of the 10% drop at 17 KB.
Keeping each thread's machines in L1 is worth 10× over RAM.

### 5b. Linux build and the cluster nodes ✅

[build.sh](build.sh) builds on Linux (gcc 15.2 on every node). [tools/cluster.sh](tools/cluster.sh)
copies, builds, tests and measures on the nodes (`deploy`, `test`, `scale`, `sweep`, `total`);
`tools\cluster.cmd` runs it from PowerShell.

Done (2026-10-13):
- The first Linux build passed every test on all four nodes: 214 + 149 + 671 checks each. The
  compiler's Linux calling convention and `mmap` path work, and every engine is bit-exact on every
  machine.
- A long GPU simulation sharing one of the nodes kept running at full speed, and its core stayed
  free.

`tools\cluster.cmd scale`, then `total` (adder8, idle priority, 2 s per row):

| machine | CPU | 1 core | whole machine |
|---|---|---:|---:|
| workstation | i9-13900K, 8 P + 16 E | 5.22 T | 74.3 T (32 threads) |
| adler40 | i9-12900K, 8 P + 8 E, one P-core left to a protected job | 4.90 T | 50.8 T (22 threads) |
| falke64 | Ryzen 5 7600X (Zen 4) | 5.20 T | 30.1 T (12 threads) |
| specht32 | i7-8700K (Skylake) | 4.47 T | 25.4 T (12 threads) |
| knecht24 | Threadripper 1920X (Zen 1), 2 NUMA nodes | 2.04 T | 22.0 T (12 threads) |
| **all five** | **102 hardware threads** | | **≈ 200 T** |

The four nodes running at the same moment gave 126.4 T on 70 threads; the workstation's 74.3 T
was measured on its own.

- The 256-bit core designs (Raptor/Golden Cove, Zen 4, Skylake) land within 15 % of each other per
  core. Zen 1 reaches 40 %: its 128-bit datapath splits every AVX2 op in two, as expected.
- Hyper-threading: +2–3 % on the Intel cores and Zen 4, but −9 % on Zen 1 (24 threads: 20.0 T,
  12 threads: 22.0 T). `--total` now measures both "every thread" and "one thread per core" and
  keeps the better one, and `total` runs the workstation at the same time as the nodes.

How it runs:

- Each thread is pinned (`sched_setaffinity`; P/E cores from `/sys/devices/cpu_core/cpus`) and
  allocates its own groups, so on knecht24 each thread's memory sits in its own NUMA node.
- The nodes are shared. Everything runs as `nice -n 19 ionice -c3` at `SCHED_IDLE`, so it only
  gets cycles nothing else wants. A protected process (`PROTECT` in `tools/cluster.local.sh`)
  comes first: the core it runs on is also left free (`--exclude`, found at run time).

### 5c. AVX-512 translation for falke64

falke64 (Zen 4) is the only machine with AVX-512. The machine stays NAND/STO; only the translation
changes.

- One zmm register holds one cache line: all 512 lanes, no low/high halves.
- `vpternlogd` computes any function of 3 inputs in one instruction: NAND becomes one op, and the
  compiler can fuse a run of NAND/STO steps that depends on at most 3 values into one op.
- Needs EVEX encoding in [src/x86.c](src/x86.c), checked against an assembler like the VEX one.
- Done when: bit-exact on falke64 and measured against its AVX2 translation.

## 6. Workloads

### 6a. Tester 1: what do random 1-bit computers do? (first run done)

[src/tester.c](src/tester.c), `alpha tester1`, `tools\cluster.cmd tester1`.

- A random program is a random string of instructions: every opcode bit is a coin flip, NAND
  reads any cell, STO writes any writable cell (`alpha random` prints one). Each pass maps the
  state (RR and all cells) to the next one; with finitely many states, every run ends in a cycle.
- Every program starts from 512 random states at once (one per lane). Brent's algorithm finds the
  cycle length of all 512 runs in step on the compiled code; big states are first compared on a
  64-cell sample. Programs are handed out to every thread; on the cluster every machine gets its
  own seed and the results are added up.
- Checked: on 60 small programs the cycle length of every lane equals an exhaustive count of
  visited states on the reference machine (30,720 runs); on 8 programs with 70–301 cells, every
  lane equals a scalar Brent on the reference machine (73 checks in `test_tester`).

Workstation, 32 threads, 4 instructions per cell, up to 1,048,576 passes:

| cells | cycle length 1 | 2 | 3–16 | 17–256 | 257–64k | longer | none found | median |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 8 | 74 % | 25 % | 1 % | | | | | 1 |
| 64 | 36 % | 40 % | 24 % | | | | | 2 |
| 256 | 15 % | 19 % | 56 % | 10 % | | | | 3–4 |
| 512 | 6 % | 9 % | 35 % | 48 % | 2 % | | | 17–32 |
| 1024 | 2 % | 2 % | 7 % | 24 % | 65 % | 0.2 % | | 513–1024 |
| 2048 | | | | | 13 % | 16 % | 70 % | > 1 M |
| 4096, 8192 | | | | | | | 100 % | > 1 M |

What it shows:
- Small random machines are orderly: most runs freeze (cycle 1) or flip between two states. One
  reason: `NAND x` with x = 0 sets RR to 1 no matter what came before, so each stored value only
  depends on the last few cells read.
- Past a few hundred cells the cycles grow exponentially: from 256 to 512 to 1024 cells the median
  goes from ~4 to ~24 to ~700, roughly doubling every 100 cells. By that rough fit, 2048 cells
  would need around a million passes and 4096 cells ~10^12, which matches what was (not) found.
- The longest cycle found: 316,193 passes (1024 cells, program 0xf5f874b42b80f41a).

**Cluster run** (2026-10-13, 21:44–21:54): all five machines, 102 threads, 10 minutes, up to
16,777,216 passes: 536 programs, 274,432 runs, 2.13 × 10^16 1-bit operations (35.5 T/s; these
machines are 64–144 KB each, so they run from L2 rather than L1).

| cells | median cycle length | longer than 65,536 | none found | fit: 2^((cells − 50) / 102) |
|---:|---|---:|---:|---:|
| 1024 | 513–1024 | 0.2 % | 0.0 % | 749 |
| 1280 | 2049–4096 | 6.5 % | 0.0 % | 4,270 |
| 1536 | 16,385–32,768 | 29 % | 0.2 % | 24,300 |
| 1792 | 65,537–131,072 | 56 % | 4.7 % | 138,000 |
| 2048 | 524,289–1,048,576 | 58 % | 25 % | 790,000 |
| 2304 | not found | 40 % | 52 % | 4.4 M |

- One rule fits every size from 256 to 2048 cells, across both runs: the median cycle length is
  about 2^((cells − 50) / 102). Each added cell makes cycles 0.7 % longer; 100 cells double them.
- At 2304 cells most runs reach the search limit. Brent's algorithm can only see cycles up to half
  the passes it is given (8,388,608 here): every "longest" from 1536 cells on sits just below
  that, so those are the ceiling, not the true longest.
- For comparison, a random map on the same number of states would cycle after about 2^(cells / 2)
  passes. 2^(cells / 102) is what such a map would give on only ~2 % of the cells. One reading,
  to be tested: most cells end up frozen or copying others, and only about 1 in 50 stays free.
- With the fit, 4096 cells means cycles of ~10^12 passes. Each pass needs the one before, so one
  program's search cannot be split across threads or machines: at this run's ~0.35 T/s per thread
  it would take over a year (the cluster could run about 100 such programs side by side).

Next: a tester 2 that measures order vs chaos directly (flip one bit, does the difference die out
or spread?) and counts the cells that stay frozen, to check the 1-in-50 reading.

### 6b. Circuits, exhaustive checks, search, Game of Life ✅

Everything still runs as NAND/STO; each workload is checked against plain C (`test_work`, 113
checks; an injected XOR-gate bug was caught).

**Circuits** ([src/build.c](src/build.c), [src/net.c](src/net.c)). A builder turns gates (AND,
NAND, OR, NOR, XOR, XNOR, NOT, BUF, any number of inputs) into NAND/STO; flip-flops update together
at the end of the pass. `.bench` netlists (the ISCAS benchmark format) load directly — every command
takes them — and `alpha net2asm` prints them as assembly. `alpha gen mul|hash|life` builds a
multiplier, a toy hash or a Game of Life board. Example: [examples/c17.bench](examples/c17.bench),
ISCAS-85's smallest circuit, becomes 30 instructions.

**Exhaustive checks** ([src/enum.c](src/enum.c), [src/spec.c](src/spec.c)). `alpha verify P --spec
"p = a * b"` runs every input, 512 per pass on every thread, and compares with a C expression over
the input vectors; `alpha equiv A B` compares two programs; both list the first counterexamples.

- 16 × 16 multiplier ([examples/mul16.asm](examples/mul16.asm), 12,010 instructions — the size of
  ISCAS c6288): **all 4,294,967,296 input pairs equal `a * b`, in 9.8 s** on the workstation. The C
  comparison per input is the limit here (5.3 T/s).
- A wrong spec (`c:s = a + b` for the 8-bit adder) is caught in exactly the 65,536 inputs with a
  carry in.

**Brute-force search**. `alpha search P --want "h = 0x..."` lists every input that gives these
outputs. Needs no per-input C, so it runs near full speed:

| toy hash ([examples/toyhash32.asm](examples/toyhash32.asm), 8 rounds) | inputs | time | speed |
|---|---:|---:|---:|
| 32 bits | 4.3 × 10^9 | 0.39 s | 67.9 T/s |
| 36 bits | 6.9 × 10^10 | 11.3 s | 42.0 T/s |
| **40 bits, whole cluster** (102 threads) | **1.1 × 10^12** | **59.5 s** | **141.8 T/s** |

Each found exactly the one secret input. `tools\cluster.cmd search ...` splits the inputs between
the nodes and the workstation by their speed (`--part I-J/K`, then `--merge`). The 40-bit run
(2026-10-14) would take about 3 minutes on the workstation alone.

**Game of Life** (`alpha life --size N`). Every lane is its own board: 512 boards per pass, one
generation per pass, all 512 checked against a plain C Life.

| board | instructions per generation | cell updates/s, 32 threads |
|---:|---:|---:|
| 16 × 16 | 62,720 | 302 × 10^9 (74 T/s) |
| 32 × 32 | 250,880 | 162 × 10^9 (40 T/s) |
| 64 × 64 | 1,003,520 | 116 × 10^9 (28 T/s) |

Bigger boards are bigger programs: the compiled code outgrows the CPU's instruction caches.

Not needed so far: GFNI bit transposes for getting data in and out of the bitsliced layout. Inputs
are generated directly in the layout, and reading results back is a small part of the time.

## 7. Hypercube: connecting the 512 lanes

Today the 512 lanes of a group are 512 separate machines. Numbered 0–511 they are the corners of a
9-dimensional cube (512 = 2^9): lane k is wired to the 9 lanes **k XOR 2^i, i = 0..8**, the ones
whose number differs in exactly one bit. Every lane reaches every other in at most 9 hops, each hop
along one dimension. It is the network of the Connection Machine (1985): 65,536 one-bit processors
in lockstep on a hypercube.

**No new instruction.** The opcode stays one bit, `NAND` and `STO`, and a program is still
`[op:1][addr:n]`. The links go into the memory map, like `ZERO`/`ONE` and the I/O cells:

- **Link cells**: `.link y = x @ 3` declares a read-only cell `y`. Reading `y` in lane k gives
  cell `x` of lane k XOR 8 (dimension 3).
- **Lane-id cells** `ID0`..`ID8`: read-only, bit i of the lane's own number. A lane needs them to
  know which side of a link it is on (who keeps the minimum, who adds).

**Well defined because there are no jumps.** All lanes run the same instruction at the same moment.
A NAND only reads and a STO only writes the lane's own cells, so a read across a link always sees
the neighbour's state just before that instruction. Jumps would let lanes drift apart and break
exactly this, which is one more reason to keep them parked.

**Cost.** Reading across a dimension is a fixed bit permutation of the 64-byte line; the compiler
emits it in place of a plain load:

| dimension | what swaps | AVX2 |
|---:|---|---|
| 8 | the two 256-bit halves | free: use the other register |
| 7 | 128-bit halves | 1 op (`vpermq`) |
| 6 | 64-bit words | 1 op (`vpshufd`) |
| 3–5 | bytes, 16-bit, 32-bit pieces | 1 op (`vpshufb` / `vpshufd`) |
| 0–2 | bits inside each byte | 4 ops (shift and mask); 1 with GFNI |

### 7a. Links in the machine ✅

- [docs/ISA.md](docs/ISA.md) "The cube": `.link y = x @ d`, `ID0`..`ID8`, lockstep, read-only
  cells. The opcode and the instruction format are unchanged.
- Assembler: id cells on first use, `.link` (links to links add up; a link may only read cells that
  already exist, so links can never loop), STO / `.in` / `.out` on read-only cells rejected.
- Reference: `ref_cube_*` runs 512 reference machines in lockstep; a link read looks into the
  neighbour's memory. `alpha run` uses it for such programs (`--lane L` picks the machine shown).
- Interpreter: instructions are decoded once; a link read permutes the line. Compiler: the same
  permutation in registers (`vpshufb`, `vpshufd`, `vpermq`, shifts; dimension 8 free), from the
  cached cell when it is in registers. Six new x86 instructions, byte-identical to ml64.
- [examples/cube_any.asm](examples/cube_any.asm): every machine learns whether any of the 512 has
  `x = 1`, in 9 link steps — 85 instructions, **7.4 ns per pass for all 512 machines** on one
  thread (5.9 T/s), 15,000× the reference cube.

Done: `test_cube` (469 checks) — every one of the 512 lane permutations against plain C; id cells,
links and chains of links in the reference against plain C; 150 random programs with id and link
cells (links to links, writes to read-only cells mixed in) on the interpreter after every
instruction and on both compiles after every pass, all matching the reference cube bit for bit.
Two injected bugs (a wrong shuffle in the compiler, a wrong mask in the interpreter) were caught.
`verify` / `equiv` / `search` refuse programs whose lanes read each other.

### 7b. Collectives: all 512 lanes at once, in 9 steps ✅

[lib/cube.inc](lib/cube.inc), plain NAND/STO macros, one dimension per step, the result in every
machine:

| macro | what every machine gets | instructions | one pass, 512 machines, 1 thread |
|---|---|---:|---:|
| `ALL_OR` / `ALL_AND` / `ALL_XOR` | OR / AND / parity of x over all 512 | 85 / 58 / 130 | 7.3 ns (`ALL_OR`) |
| `BCAST0` / `BCAST` | x of machine 0 / of machine s (s chosen at run time) | 148 / 274 | |
| `PXOR` | XOR of x over machines 0..k, and over all | 278 | |
| `PCOUNT` | how many machines below k have x = 1 (its place in a packed list), and how many in all | 5,659 | 0.94 µs |
| `MAX8` / `MIN8` | the largest / smallest of 512 bytes | 3,434 | 0.32 µs |

Prefixes use the classic hypercube scan: every machine keeps the total of its subcube so far, and a
machine with bit d set also adds the neighbour's total across d. Multi-bit values needed one
assembler addition, `#` to join names inside macros (`v#3`, `ID#d`) — the machine is unchanged.

Examples: [examples/cube_rank.asm](examples/cube_rank.asm) (`alpha run ... --set x=1 --lane 300`
gives p = 300, t = 512), [examples/cube_max.asm](examples/cube_max.asm).

Done: `test_cube` now 622 checks — every collective against plain C in every machine, on the
reference cube and on compiled code, for x densities from none to all (a single machine, 1 %, 2 %,
50 %, 99 %, all), broadcasts from machines 0, 511 and a random one, and 8-bit values including all
equal; every collective program also bit-exact on all engines from random states. An injected
bug in the library (one flipped condition in the prefix step) was caught on both engines.

### 7c. Sorting and moving data ✅

More plain NAND/STO macros in [lib/cube.inc](lib/cube.inc):

- `SORT8 v`: bitonic sort, 45 compare-exchange rounds (`CMPX8`), each across one dimension.
  Afterwards machine k holds the k-th smallest byte.
- `PACK8 v h p`: the values of the machines with h = 1 move to machines 0, 1, 2, ... in order, p
  being each one's place from `PCOUNT`. Each step moves a value across dimension d if bit d of its
  place and its target differ; values packed in order never meet, so nothing is lost or needs
  waiting.
- `PERM8 v c`: any permutation, through a Beneš network of 17 rounds (dimensions 0 .. 8 .. 0). The
  switch settings are data: [src/benes.c](src/benes.c) computes them in C for a given permutation
  (the looping algorithm) and they go in as inputs `c0`..`c16`.

`alpha cube` runs all of step 7 on compiled code — random inputs checked against plain C in all
512 machines, then timed (workstation, 32 threads):

| operation | instructions | one pass, 1 thread | passes per second, all threads |
|---|---:|---:|---:|
| OR over 512 machines | 85 | 7.6 ns | 1.5 × 10^9 |
| running count (10 bits) | 5,659 | 0.97 µs | 1.3 × 10^7 |
| largest of 512 bytes | 3,434 | 0.33 µs | 2.7 × 10^7 |
| **sort 512 bytes** | 18,135 | 3.3 µs | **3.8 million sorts/s** |
| gather the hits | 8,422 | 1.4 µs | 9.1 × 10^6 |
| **any permutation of 512 bytes** | 2,176 | 0.21 µs | **66 million/s** |

Done: `test_cube` 675 checks — the Beneš routing of 206 permutations (reverse, bit reversal,
rotation, perfect shuffle, 16 × 32 transpose, 200 random) in plain C, with both machines of every
pair always set alike; `SORT8` on random, equal, sorted and reversed bytes; `PACK8` with no, one,
10 %, 50 % and all hits; `PERM8` for reverse, bit reversal, transpose and random permutations — all
in every machine, on the reference cube and on compiled code, plus bit-exact engine checks. Two
injected bugs (an inverted output switch in the routing, a flipped comparison in the sort) were
caught.

### 7d. Transforms ✅

The hypercube is the shape of the FFT's data flow: round d of a radix-2 transform pairs entries k
and k XOR 2^d — exactly the machines one link apart. One entry per machine, one round per
dimension, nothing but NAND and STO.

- `MOBIUS f` ([lib/cube.inc](lib/cube.inc)): the truth table of a 9-input Boolean function, one
  entry per machine, becomes its algebraic normal form (machine k: the coefficient of the monomial
  whose variables are the bits of k). 9 rounds of `f ^= ID_d & f across d`; it is its own inverse.
- `alpha gen wht --bits N`: the Walsh–Hadamard spectrum, Σ_x (−1)^(f(x) + x·k), in two's
  complement. Each round is one ripple adder per machine: u + v in the lower machine, u − v in the
  upper one, the same circuit with the operand inverted by `ID_d` and `ID_d` as carry in.
- `alpha gen xorconv`: h(k) = #{x : f(x) = g(x ⊕ k) = 1} by spectrum, product (20-bit multiplier),
  spectrum again and a shift by 9 — the convolution theorem on the cube.
- `alpha gen polymul`: **c = a · b in Z_12289[x]/(x^512 + 1)**, the ring of Falcon-512, with one
  coefficient per machine. Negacyclic NTT of a and b (9 Cooley–Tukey rounds), product point by
  point, inverse (9 Gentleman–Sande rounds), scale. Montgomery multiplication with R = 2^14 is
  cheap because q = 3·2^12 + 1: 1/q ≡ 2^12 + 1 mod 2^14, so m = t/q mod 2^14 differs from t only
  in bits 12 and 13, and the upper half of m·q is (3m + m/2^12)/4. The twiddle factors are inputs
  (`ntt_twiddles` in [src/gen.c](src/gen.c)), like the Beneš switch settings: the program never
  writes them, so they are set once and stay.

Workstation, compiled code (32 threads):

| transform | instructions | one pass, 1 thread | passes per second, all threads |
|---|---:|---:|---:|
| normal form of a 9-input function | 144 | 19 ns | 8.1 × 10^8 |
| Walsh–Hadamard spectrum (12 bits) | 6,348 | 0.77 µs | 2.2 × 10^7 |
| XOR convolution of two functions | 40,944 | 4.7 µs | 3.1 × 10^6 |
| **polynomial product mod (x^512 + 1, 12289)** | 442,449 | 57 µs | **308,000 /s** |

The polynomial product runs at 70 T 1-bit ops/s, the machine's full speed, but it is a lot of
1-bit operations: one 14-bit Montgomery product is about 13,000 NAND/STO instructions here, a
handful of word instructions on the CPU itself, so an ordinary C NTT would be faster on the same
core (not measured). What the cube shows is that the transform maps onto the links exactly — one
round per dimension, no shuffles — built from nothing but NAND.

Done: `test_cube` 790 checks — `MOBIUS` on constant, single-point, random 10 % and 50 % tables
against plain C, and applied twice gives the table back; the spectrum on 12 and on 10 bits (where
±512 wrap around), with Parseval checked on the C side; XOR convolution with all ones, two single
points, random and empty functions; the polynomial product times 1, times x (x^512 = −1), with all
coefficients q − 1, sparse and random, against schoolbook multiplication — every one in all 512
machines on the reference cube and compiled code, plus bit-exact engine checks. Four injected bugs
(a zero carry in the spectrum's subtraction, OR for XOR in `MOBIUS_DIM`, a missing row in the
multiplier, a dropped carry in the Montgomery reduction) were all caught.

### 7e. Grids across the lanes ✅

A ring of 512 machines, or a 2^a × 2^b torus with a + b = 9 (16 × 32, 8 × 64, ...), sits inside
the cube with every grid neighbour one link away: position i of an axis goes to the machines with
i ^ (i >> 1) — the Gray code — in that axis' bits. The catch for a lockstep machine: the link to
the next position is across a *different* dimension in different machines (from an even position
across the lowest bit, from an odd one across the bit above the lowest 1). So every machine works
out its own masks from its lane-id bits, once per pass, and a neighbour read takes all the axis'
links and keeps the one its mask picks. Still nothing but NAND and STO.

- [lib/cube.inc](lib/cube.inc): `RING_MASKS` (324 instructions) and `RING_NEXT x r` /
  `RING_PREV x r` (60 each) for the ring of all 512 machines.
  [examples/rule110.asm](examples/rule110.asm): the Turing-complete cellular automaton Rule 110 on
  a ring of 512 cells, one per machine, 466 instructions per step.
- `alpha life --cube [--rows R] [--size N]`: **one Game of Life board across all 512 machines**,
  an N × N tile in each, 2^R × 2^(9−R) tiles (default 16 × 32 tiles of 32 × 32: one 512 × 1024
  torus). Each pass first fetches the left and right edge columns from the machines left and right,
  then the rows above and below — corners included, since those machines already have their own
  side edges — and then does one generation. Diagonals need no extra hop.

| | instructions per generation | one thread | all threads |
|---|---:|---:|---:|
| 512 separate 32 × 32 boards (step 6b) | 250,880 | 1.99 × 10^10 cell updates/s | 2.85 × 10^11 |
| **one 512 × 1024 board** | 255,551 | 1.93 × 10^10 (36,800 generations/s) | 2.75 × 10^11 |

Exchanging the edges costs 1.9 % more instructions and about 3 % of the speed. "All threads" is
still 32 independent boards, one per thread; one board across threads is step 7f.

`alpha cube` checks and times both: `ring` (Rule 110, one step) 38 ns and 3.5 × 10^8 steps/s;
`life` (the 512 × 1024 board, one generation) 28 µs and 5.1 × 10^5 generations/s.

The masks are a constant of each machine but are recomputed every pass (324 of Rule 110's 466
instructions); with no jumps, the only way around that would be to set them once from the host,
like the NTT's twiddle factors. Diffusion and waves were not built: they are stencils like Life and
would use the same edge exchange.

Done: `test_cube` 858 checks — the ring closes in Gray code one bit at a time; `RING_NEXT` and
`RING_PREV` (also in place) with a single 1 at either end of the wrap and random bits; Rule 110
from `examples/rule110.asm` for 6 steps; Life on one board for 8 shapes — one cell per machine on
16 × 32, 1 × 512 and 512 × 1 grids, tiles of 3 × 2, 2 × 3, 4 × 4, 5 × 3 and 8 × 8 on grids from
2 × 256 to 256 × 2 — for 3 to 6 generations against plain C on the whole board, on the reference
cube and compiled code, plus bit-exact engine checks. Two injected bugs (the first position's
link back to the last dropped from the masks, a flipped parity in `RING_MASK_DIM`) were caught.

### 7f. Beyond 9 dimensions ✅ (one computer)

**Up to 65,536 machines in lockstep — a 16-dimensional cube the size of a full Connection
Machine, on the workstation.** `ID9` .. `ID15` and `.link y = x @ 9` .. `@ 15` (see
[docs/ISA.md](docs/ISA.md)); a program's cube is as big as the highest dimension it names. Still
nothing but NAND and STO: the bigger cube is memory map and engines.

- Machines 512 g .. 512 g + 511 are group g, one ordinary 512-lane line per cell. Across dimension
  9 and up a read is a plain load from another group's memory (`mov rax, [peers + 8 s]` once, then
  the usual `vmovdqa`): no shuffle at all.
- **Keeping lockstep.** The groups run one after another, or on different threads. The compiler
  cuts the pass into segments wherever one segment would both write a cell and read it across
  dimension 9 or up; each segment runs in every group before the next starts anywhere. Then every
  read sees exactly what lockstep shows it. Each segment is its own compiled function (RR goes to
  memory at its end). How programs are written decides the cuts: `CMPX8_FAR` copies the
  neighbour's value before writing its own (2 cuts per round instead of one per bit), and
  `gen_rank` reads one bank and writes the other (1 cut per round).
- **Threads.** `par_cube_run`: a dissemination barrier after every segment (no counter all
  threads fight over: 0.7 µs per segment on 24 threads); each thread runs a fixed share of three
  quarters of the groups, so their memory stays in its caches, and the last quarter goes to
  whoever is free, so P-cores do more than E-cores. Default: one thread per physical core. All 32
  hardware threads in lockstep collapse to ~1 % — anything else Windows wants to run then has to
  preempt one of them, and every other thread waits at the next barrier.
- The reference cube runs 2^D reference machines; `alpha check` compares every machine.

`alpha cube --dims 16`, every result checked against plain C in all 65,536 machines:

| one pass of 65,536 machines | instructions | segments | 1 thread | 24 threads, one cube |
|---|---:|---:|---:|---:|
| OR of one bit | 148 | 15 | 9 µs | 11–18 µs |
| running count, 17-bit results | 19,572 | 8 | 205 µs | 66–80 µs |
| **sort 65,536 bytes** (bitonic, 136 rounds) | 55,704 | 57 | 4.8 ms | 1.9–2.6 ms |
| **Life on one 8192 × 8192 board** (67 M cells) | 259,086 | 3 | 4.2 ms (240 generations/s) | 0.56–0.95 ms (1,050–1,770 /s) |

What the numbers say: on one thread the big cube costs little — Life on 65,536 machines runs at
1.6 × 10^10 cell updates/s, against 1.9 × 10^10 on 512. Across threads, lockstep is the price:
- a barrier costs ~0.7 µs, so a pass with little work per segment (the OR) is faster on one thread;
- the P-cores scale almost linearly (8 threads: 1,700 Life generations/s), the E-cores add little,
  because every segment ends with everyone waiting for the slowest group;
- the sort's cross-group rounds read bytes another core just wrote, a cache-line transfer each
  (~100 ns), so it gains only 1.9–2.6×;
- with other programs on the computer the timings vary ±30 % between runs: any interruption of
  one thread holds up all of them. For comparison, all 32 hardware threads running 32 *separate*
  512 × 1024 boards reach 2.9 × 10^11 cell updates/s (73 T); one shared board on 24 threads gets
  0.7–1.2 × 10^11.

Done: `test_cube` 1,199 checks — 60 random programs on 1,024 to 4,096 machines (half their links
across groups, chains of links, id cells up to the top dimension, writes to read-only cells; up
to 9 segments, 454 in all): interpreter after every instruction or pass, literal and optimized
compiled code, and 2 to 6 threads bit for bit equal to one thread; the OR over 1,024 to 4,096
machines, `gen_rank`, the bitonic sort with `CMPX8_FAR`, and Life on one board across 1,024 and
2,048 machines (5 and 7 threads), all against plain C on the reference cube, one thread and 4
threads. `test_jit`: the new `mov r64, [m]` against ml64. Three injected bugs — a missing cut
before a write, a barrier one round short (the threads lose step and hang), groups wired to the
wrong neighbour — were all caught.

### 7g. Across the cluster

The same segments across computers: each node runs a range of groups, and at every cut the edge
lines go over the LAN. The cuts show the cost: Life needs 3 per generation, the sort 57. Only
for programs with few cuts and a lot of work per segment, and never at idle priority — one
preempted node would hold up the whole cube.

## 8. Toward an operating system

- Indirect addressing, writable program memory, a program loader, I/O conventions.
- A small monitor that loads and runs other programs on the machine.
- Done when: the monitor loads and runs the examples.

### 8a. Beta: a stored-program computer built from NAND/STO ✅

The decision: the NAND/STO machine does not change. Indirect addressing in the machine itself
would need a new instruction or new semantics (like steps 3 and the parked jumps). Instead the
fixed NAND/STO program is the circuit of a small computer — the way any CPU is a fixed circuit
and programs are data. Specification: [docs/BETA.md](docs/BETA.md).

- **Beta**: 8-bit accumulator machine, 256 bytes (160 RAM, 96 ROM), `A`, `X`, `PC`, carry;
  16 two-byte instructions with four operand modes (`#n`, `n`, `n,X`, `A`) — indexed addressing,
  memory-indirect and computed jumps, PDP-8-style `JSR` and `IN`/`OUT`/`HLT`.
- [src/beta.c](src/beta.c): the reference emulator (the definition), an assembler, the generator
  `gen_beta` — **40,852 NAND/STO instructions, 1,975 cells, one pass = one beta instruction** —
  and the host side of the ports. Two new builder parts: `b_select` (OR of select & value: the
  memory reads) and `b_mux` (the memory writes).
- Power on (all cells 0) starts beta in the monitor: the PC cells hold `PC XOR 0xA0`.
- Every lane is its own beta. 512 betas per lane group, each with its own program counter:
  different programs and branches with no lane masks — the divergence the parked step 7 worried
  about is just data here.

### 8b. Ports, the monitor, programs ✅

- **I/O**: `IN`/`IN_VALID` from the host, `OUT`/`OUT_STROBE` to it, `HALTED`, `WAITING`; the host
  offers each beta its next byte before a pass and collects after it.
- **The monitor** (80 bytes of ROM): `L addr n bytes` loads, `G addr` runs (programs return with
  `JMP MON`), `D addr n` dumps; the service `PUTS` prints a string.
- **Programs** ([examples/beta](examples/beta)): `hello` (through `PUTS`), `fib` (decimal output, nested
  subroutines), `primes` (reads N, trial division) — loaded by the monitor and run, in 512 betas
  at once. With `--lane-input` each beta gets its own N: 512 different outputs, each beta
  finishing at its own time.

`alpha beta run examples\beta\primes.bs --lane-input`: 512 betas, 173,430 steps, 0.9 s with the
host feeding and collecting every pass; then 512 emulators given the same input end with the same
output and the same state.

| beta | steps per second |
|---|---:|
| one beta | 215,000 (one thread runs 512 of them) |
| 512 betas, 1 thread | 1.1 × 10^8 |
| **16,384 betas, 32 threads** | **1.28 × 10^9** (52 T 1-bit ops/s) |

Each beta instruction costs 40,852 NAND/STO instructions — most of it memory: three reads
through 128- and 256-way multiplexers and 1,280 RAM bits rewritten every pass. Addressing is
what a machine without addresses pays for.

Done: `test_beta` 42 checks — every instruction and flag on the emulator; the assembler's
encodings for every mode, ten error cases, and disassembly → assembly for every instruction;
the monitor loading, running, dumping and skipping; **the NAND/STO beta equal to the emulator, the
whole state after every step, for 300 steps of random programs in all 512 lanes** (with input
arriving at random), compiled and interpreted, and on the reference engine; `hello`, `fib` and
`primes` booted through the monitor in 512 lanes against the right output. Two injected bugs (a
`JSR` saving the wrong return address in the circuit, a `SUB` off by one in the emulator) were
caught.

### 8c. Next

- More memory: bank switching through a port, or a 16-bit address space — every RAM byte costs
  about 180 NAND/STO instructions per step (fetch 49, data read 56, write 79: 29,400 of the
  40,852), so 1 KB of RAM would be about 5× slower.
- A file store on the host, reached through the ports; a shell in ROM.
- Several programs per beta (time slicing by a host-driven interrupt line), and messages between
  betas over the cube's links (step 7) — 512 networked computers.

## 9. Real 1-bit hardware (FPGA)

- The same ISA in Verilog, checked against the reference with exported test vectors.
- Flashed to an FPGA board (e.g. Tang Nano 9K): a physical 1-bit ALU and 1-bit memory cells.
- Done when: the board runs the examples with the same results as the reference.

## 10. Superoptimizer: provably shortest programs ✅

`alpha super "SPEC"` finds the shortest NAND/STO program for a small specification and proves
that nothing shorter exists; `alpha super --library` does it for every building block of
[lib/std.inc](lib/std.inc), [lib/cube.inc](lib/cube.inc) and the gate builder. Specifications are
outputs as expressions of inputs ([src/super.h](src/super.h)): `RR = RR ^ x`,
`s{a,b} = a ^ b ^ c; c = a&b | a&c | b&c` (`{a,b}`: s may be the same cell as a or b),
`o = s ? a1 : a0; ns := !s` (a given cell).

**The search** ([src/super.c](src/super.c)). A program is a chain: every NAND combines RR with one
stored value, so the search picks one operand per step; a value is stored only when a later NAND
first reads it (one STO, charged then). Iterative deepening on the cost — every program of cost
c − 1 is ruled out before cost c is tried — so the first program found is the shortest. Every
candidate step updates a truth table holding all 2^k input rows in one 64-bit word: all inputs
at once, the same bitslicing as the lane engine. What keeps it exact and small: equal operand
values tried once, steps that change nothing skipped, interchangeable inputs used in a fixed
order, a lower bound (outputs still to store, values still to make), and a transposition table
(128-bit keys) for states reached in different orders. The rules for programs: inputs not
written keep their values; RR, outputs and scratch cells start as garbage and are never read
before being written; an output that may share an input's cell is written after that input's
last read. Threads split the top of the tree; `--part I/K` splits it over the cluster.

**The library, now** (`alpha super --library`, workstation, 24 threads):

| building block | before | shortest | proof |
|---|---:|---:|---|
| `SET` `NOT` `CLR` `LD` `LDC` `AND` `ORC` `STOC` `MOV` | 1 1 2 3 2 2 2 3 4 | the same | already optimal |
| `OR` / `NOR` / `ANDC` | 5 / 6 / 5 | **4 / 5 / 4** | < 0.1 s |
| `XOR` / `XNOR` | 10 / 11 | **7 / 8** | < 0.1 s |
| `FA` (full adder, c in and out, s may share a cell with a or b) | 28 | **19** | 1.2 × 10^10 states, 25 s |
| `MUX` (and `BCAST0_DIM`) | 16 | **9** | < 0.1 s |
| `OR_DIM` / `AND_DIM` / `XOR_DIM` | 9 / 6 / 14 | **6** / 6 / **8** | < 0.1 s |
| `BCAST_DIM`: r = (id ^ s) ? n : r | 30 | **14** | 1.4 s |
| `MOBIUS_DIM`: f ^= id & n | 16 | **12** | < 0.1 s |
| `GE8`, one bit (was LDC, STO and a 28-instruction FA) | 31 | **9** (`GE_BIT`) | < 0.1 s |
| `PXOR_DIM`: p ^= id & n; t ^= n | 30 | **20** | nothing ≤ 19: 1.8 × 10^12 states, 68 min |
| `PCOUNT_DIM` bit, `RING_SEL` term, `b_select` term | 6, 7, 7 | the same | already optimal |
| builder: XOR / OR / AND gate | 13 / 7 / 6 | **8 / 6** / 6 | < 0.1 s |
| builder: `b_mux` (given !s) | 11 | **8** | < 0.1 s |
| builder: half adder / full adder | 19 / 45 | **11 / 19** | full adder 47 s |

The full adder ([lib/std.inc](lib/std.inc)): `!a`, `!(a & b)`, `a | b` and `(a ^ b) | !c` are
stored, the carry and the sum come out of three NANDs each — 19 instructions, where the textbook
nine-gate circuit takes 28.

**What it does to everything built on them** — the old binary and library against the new, same
machine, best of two runs:

| | instructions | one thread | all threads |
|---|---:|---:|---:|
| sort 512 bytes | 18,135 → 7,425 | 3.38 → 1.79 µs (**1.9×**) | 3.6 → 7.6 M/s (**2.1×**) |
| largest of 512 bytes | 3,434 → 1,346 | 325 → 230 ns (1.4×) | 27 → 58 M/s (**2.2×**) |
| polynomial product, NTT | 442,449 → 220,758 | 55 → 33 µs (**1.65×**) | 1.4× |
| Walsh–Hadamard; XOR convolution | 6,348 → 3,000; 40,944 → 19,606 | 1.4×; 1.4× | 1.3×; 1.3× |
| Life, 512 boards of 32 × 32 | 250,880 → 125,952 | 1.84 → 2.63 × 10^10 cells/s (1.4×) | 2.84 → 3.76 × 10^11 (1.3×) |
| running count; gather | 5,659 → 4,039; 8,422 → 5,686 | 1.1×; 1.2× | 1.2×; 1.2× |
| beta (step 8) | 40,852 → 36,346 | 1.14 → 1.21 × 10^8 steps/s (1.06×) | 1.02× |
| OR over 512 machines | 85 → 58 | 7.4 → 8.5 ns (**15 % slower**) | 12 % slower |
| Möbius transform | 144 → 108 | 19.2 → 20.9 ns (9 % slower) | 15 % slower |
| permutation (Beneš) | 2,176 → 1,224 | 196 → 204 ns (4 % slower) | about the same |

Fewer instructions is not always faster compiled code: the compiler gets `NAND ZERO` and
`NAND ONE` for free and turns a NAND right after `NAND ZERO` into a plain load. The old
`OR_DIM` read its neighbour there; the shortest one reads it with RR live, an AND of two vector
operations more. For the interpreter, the reference machine and real hardware, instructions are
the cost, and every block is now the shortest there is. Beta gains little because its cost is
the memory reads, whose step (`b_select`) was already optimal.

Done: `test_super` 109 checks — the specification parser; **a brute force that knows none of the
search's rules** (every NAND/STO sequence, inputs writable, outputs readable before written, three
scratch cells) agrees on the optimum of the 17 blocks of up to 6 instructions, and for the 5 whose
optimum is 7 to 9 finds nothing of 6 or fewer either; every program found run on the reference NAND/STO machine for every input,
with garbage, in every allowed sharing of cells; the 19-instruction full adder, and that nothing of
15 or fewer exists; three deeper searches that must find their program exactly at the first cost
not ruled out. Injected bugs: an overestimating lower bound and a transposition table pruning one
instruction too early. Neither was caught at first — the tests compared only the final length,
and small searches never reach the table — so the search now reports any program found below a
cost it had ruled out (impossible if every budget was searched in full), and the deeper tests run
single-threaded; then both were caught. The whole suite passes with the new library (2,589 checks).

Next: superoptimize for the compiler's cost (x86 operations, with `NAND ZERO`/`NAND ONE` free) as a
second measure, and give the library the cheaper program where the two differ; let `--part` run
bigger blocks (two-bit adders, comparators) on the cluster — `PXOR_DIM`, at 4 inputs and 20
instructions, already took 68 minutes on the workstation.

## 11. Gamma: a world of cells on the cube

A product on top of the machine: a 3-D world in which cells live. Every law of it is one NAND/STO
program that runs the same in every machine, once per tick, and every cell (from 11b) is itself a
small NAND circuit. The plan, the decisions and how it differs from a GPU life simulation are in
[docs/GAMMA.md](docs/GAMMA.md).

### 11a. The world: light and minerals on a 3-D torus ✅

`alpha gamma`: a torus of 2^a × 2^b × 2^c voxels, 512 to 65,536 of them, one per machine. Each
axis sits in its own dimensions in Gray code, x lowest. These are step 7e's grids with a third
axis, so all 6 face neighbours are one link away ([src/gamma.c](src/gamma.c); `grid_masks` and
`grid_gather` now come out of [src/gen.c](src/gen.c)). z = 0 is the sun plane, the last layer the
floor. One tick:

- **light** falls one voxel per tick. The sun plane gets 255. Every other voxel gets the light of
  the voxel above it, of which an empty voxel passes on l − l/8 and an occupied one l/2. Shade
  forms under whatever is there.
- **minerals** diffuse up from the floor. The floor gets the maximum and the sun plane 0. Every
  other voxel gets (2m + its 6 neighbours) / 8, rounded down.
- `occ` (something is there) was set by the host in 11a. Since 11b the cells are what is there.

`gamma_step()` in plain C is the definition. `gen_gamma()` is the same tick as one program and is
checked against it after every tick.

**Whole numbers were not enough.** With 8-bit minerals the rounding stopped the diffusion for good
8 layers above the floor. It reached a fixed point after 84 ticks with every layer above at 0, and
the profile after 20,000 ticks was the same as after 200. A 1-D model of the rule (32 layers)
picked the fix:

| minerals | layers between sun plane and floor that get any | fixed point after |
|---|---:|---:|
| 8 bits, rounded down | 8 of 30 | 84 ticks |
| 8 bits, rounded to nearest | 12 | 128 |
| 8 + 2 bits below the point | 16 | 336 |
| 8 + 3 bits | 22 | 665 |
| **8 + 4 bits** (chosen) | **all 30** | 1,294 |

So minerals are 12-bit fixed point (0 .. 4095), and a cell will see the top 8 bits. The cost is
1,384 more instructions per tick, mostly the 24 more neighbour reads.

One tick of the 32 × 32 × 32 world (15 dimensions, 64 groups):

| part | instructions |
|---|---:|
| where am I: link masks for 3 axes, sun plane, floor | 616 |
| neighbour reads: light and occ from above, minerals from all 6 (81 bits, 36 each) | 2,916 |
| light | 286 |
| minerals | 1,586 |
| register copies | 80 |
| **one tick** | **5,484**, in 2 segments (one cut, before the registers) |

Speed on the cluster at idle priority. On every node the check against plain C passed first
(200 ticks, every voxel, every tick):

| CPU | 1 thread | all cores, one world in lockstep |
|---|---:|---:|
| i9-12900K | 56.0 µs per tick (17,900 ticks/s) | 16 threads: 10.9 µs (91,600 ticks/s, 3.0 × 10^9 voxel updates/s, 16.5 T) |
| Ryzen 5 7600X | 53.1 µs (18,800/s) | 6 threads: 12.3 µs (81,400/s, 14.6 T) |
| i7-8700K | 79.4 µs (12,600/s) | 6 threads: 21.7 µs (46,100/s, 8.3 T) |

On the workstation, with other jobs running, 8 threads still did a tick in 8.1 µs (with 8-bit
minerals). At 16 and 24 threads it collapsed to milliseconds per tick: any thread the scheduler
takes away holds all the others up at the next barrier. 7f found the same, so timings belong on
quiet machines.

Done: `test_gamma`, 33 checks:
- plain C against hand-worked cases:
  - light from nothing, falling 255, 224, … 38 through 15 empty voxels;
  - the shadow under one occupied voxel and down its column;
  - minerals that only grow, up to a fixed point with flat layers, richer with depth and no empty
    layer below the sun plane;
  - a blob that gives 1/8 to each face neighbour across the edges and nothing to a diagonal.
- every voxel its own machine and every face neighbour one link away along its own axis (5
  shapes); bad sizes refused.
- the reference cube (512 and 1,024 machines) against plain C, from random worlds.
- the interpreter and both compilers against the reference cube, from random states of every
  cell, up to 2,048 machines.
- compiled code against plain C after every tick on 9 shapes from 8 × 8 × 8 to 64 × 32 × 32,
  including a 2 × 2 × 512 column and a 512 × 64 × 2 slab, with 3 and 5 threads in lockstep among
  them.

The whole suite was 2,622 checks under gcc on two of the cluster's nodes. Four injected bugs were
all caught, with 10 or 11 failures each: light read from below, the 2m term dropped from the sum,
plain C without the wrap at the +x edge, and the floor in the wrong layer.

Next: 11b, the cells. The link masks are 11 % of a tick and are recomputed every pass. Set once
from the host, like the NTT's twiddle factors, they would cost nothing, but that only matters until
11b's cells (about 40,000 instructions each) dwarf them.

### 11b. Cells that evolve from a random soup ✅

A voxel now holds a cell or nothing. A cell is 32 NAND gates over 32 senses: a constant, 3 random
bits, which neighbours hold a cell, 2 signal bits from each, the top bits of its energy, light and
minerals, and "just born". They run in order every tick, in place, so wiring alone gives memory.
Gates 26–27 are the signals the neighbours see, 28 asks to divide, 29–31 give the direction.
Every rule is at the top of [src/gamma.h](src/gamma.h).

**Division is physics, and nothing is hand-written** (the decision is in
[docs/GAMMA.md](docs/GAMMA.md)). A cell with energy 32 or more asks; an empty voxel grants the first
cell asking for it, by direction, and the cell learns it in the same tick (one cut: the grant is
read in the tick it is computed). For the next 32 ticks both are busy. The parent's genome turns by
one gate per tick and the new cell takes that gate in at its end, so nothing in lockstep has to
compute an address: the copy is one multiplexer per genome bit and 12 reads over one link. Each
gate copied flips one bit with probability 1/64, from the new cell's own dice (xorshift32 in every
voxel). Energy: light/32 + minerals/512 in, 1 out, at most 255. A cell dies with nothing left or by
chance (1/2048 per tick) and gives back 512 minerals; a new cell dies with its parent.

One tick of the 32 × 32 × 32 world:

| part | instructions |
|---|---:|
| where am I | 616 |
| dice (xorshift32) | 488 |
| neighbour reads (122 bits, 36 each) | 4,392 |
| grants | 583 |
| partner reads: the other one of a division, across any of the 15 dimensions (22 bits) | 2,683 |
| **the gates: 64 inputs, each a 64-way select** | **33,964** |
| energy and state | 1,403 |
| genome copy | 3,437 |
| light | 286 |
| minerals | 1,828 |
| register copies | 164 |
| **one tick** | **49,844**, in 3 segments |

The brain is two thirds of a tick: in lockstep every voxel reads its own addresses, so each gate
input is a tree of 63 multiplexers. 16 threads of the i9-12900K (cluster, idle priority) ran
13,300 ticks/s for 32,768 voxels over a million ticks: 75 µs per tick, 4.4 × 10^8 voxel updates/s,
21.7 T.

A million ticks from a soup of 10 % random cells (`alpha gamma --ticks 1000000`, seed 1):

| tick | cells | dividing | genomes | asking | cells in z 0–7 / 24–31 |
|---:|---:|---:|---:|---:|---:|
| 100 | 6,373 | 2,673 | 4,237 | 25.7 % | 1,910 / 2,227 |
| 100,000 | 6,210 | 199 | 3,385 | 89.6 % | 2,784 / 3,426 |
| 1,000,000 | 6,235 | 203 | 3,433 | 94.1 % | 3,035 / 3,200 |

- Life fills the 3 sunlit layers and the 4 mineral-rich ones above the floor; the 24 between stay
  empty. Each layer of cells halves the light (255, 127, 63, 31), and below 32 there is nothing to
  gain: the cells shade themselves into a canopy 3 layers deep.
- Selection works: the share of free cells that ask to divide rose from 26 % to 94 %.
- But the winner is blind. A voxel freed by a death goes to whoever already points at it; a cell
  that senses the gap and turns to it comes one tick late, and asking costs nothing. About 0.1 %
  of the requests point at an empty voxel, while 5 % of the neighbours are empty.
- About 3,400 distinct genomes among 6,000 cells, the most common at 0.2 %: mostly neutral drift,
  since most gates do not matter to a cell that always asks.

Done: `test_gamma`, 45 checks:
- the 11a checks;
- a hand-written cell (it exists only in the tests) that divides upwards:
  - the request, the grant and the energy split;
  - 32 ticks of copying, with the mutations replayed from the new cell's dice;
  - the parent's genome back in place, and "born" for one tick;
- grant priority;
- starving, and the minerals given back;
- a new cell dying with its parent;
- a soup that gets going by itself;
- the reference cube and every engine against plain C, from random states of everything (any mix of alive, busy, partner, counter, ...);
- compiled code against plain C after every tick, on 12 shapes, from random states and from soups, with 3 and 5 threads among them.

The whole suite is 2,634 checks under gcc. Four injected bugs were all caught, with 13–15 failures each, each naming the part that differs:
- the mutation position read one bit too low;
- the parent keeping all its energy;
- plain C forgetting that a new cell dies with its parent;
- requests that ignore the energy minimum.

Next, the ecology: make the senses pay. Make asking cost energy, make energy scarcer, and later let cells eat or feed each other.

`alpha gamma --genes N` (added after 11b) prints the N most common genomes as circuits: every gate
with its two sources by name, the gates that feed the actions marked, and which actions are
constant whatever the senses say. The most common genomes after a million ticks hold only 10 to 12
cells each, and all three read their senses; the "blind" asking of the census is a canopy cell
whose light and energy bits never change.

### 11c. Bodies: energy from cell to cell, and the tick in the right order ✅

Two changes to the rules, decided against a cost for asking ([docs/GAMMA.md](docs/GAMMA.md)):

- **Giving.** Gate 25 is a new action: a free cell with it on offers an eighth of its energy to the
  cell in its direction (gates 29–31, shared with dividing), if there is one. The receiver gets it
  in the same tick as a gain, the giver loses it; what does not fit under 255 is lost. In the
  circuit the offer is 5 plain cells written after the brain and read over the links by the
  neighbours, who add up what points at them (at most 6 × 31): 24 more link reads and two more
  adders, about 1,100 instructions.
- **The tick in the right order.** The gates run first, then every cell asks and offers, then the
  empty voxels grant, then the rest: asking, being granted and the split all happen in the tick
  the decision is made. In 11b the grants used the previous tick's request, so a cell that saw a
  gap and turned to it was always a tick behind one that had pointed there blindly. The signals
  the neighbours read are now registers (the gates as they ended the last tick), so the brain
  can run whole in the first segment; the pass has 4 segments (one more barrier): 52,084
  instructions.

The bug the check caught: in the plain C, a dying cell's gates were cleared in place while
later-indexed neighbours still read its direction to take its offer; the circuit reads them before
the clearing. The two disagreed once in about 2,000 ticks per cell, which `test_gamma` found at
tick 38 of a 512-voxel world and at tick 1 of a 65,536-voxel one. The clearing is a fourth pass
now. (And the generator's energy sum had one bit less room than the adder makes.)

The same world under both rules, seed 1, a million ticks, on the same machine in the same
session (`alpha gamma --ticks 1000000`; the 11b binary kept for it):

| at tick 1,000,000 | 11b | 11c |
|---|---:|---:|
| cells | 6,235 | 5,575 |
| distinct genomes | 3,433 | 3,149 |
| energy per cell | 206 | 207 |
| free cells asking to divide | 94.1 % | 90.4 % |
| asks that get a voxel | 0.1–0.2 % (asks at an empty voxel) | 0.0–0.1 % (granted) |
| free cells giving energy | — | 12.8 %, rising from 7 % |
| energy given, per tick | — | 13,900 (2.5 per cell) |
| bodies of 2 or more cells with one genome / the largest | — | 1,318 / 9–13 |
| cells in the 24 layers between canopy and floor | 0 | 0 |
| ticks/s, 16 threads | 11,800 | 9,650 |

What it says:
- **The senses are used now.** All three most common genomes point by what they see: direction
  bit 1 is NAND(cell at −y, cell at +y) in every one of them, so the cell turns along y when a
  y-neighbour is free, and bit 0 reads whether −x is free. Giving is conditional too: "give unless
  there is a cell at −x", "give when the light is dim".
- **But space, not energy, is the limit, and the senses can't make space.** Energy sits at 207 of
  255. The habitable zone is full; the only vacancies are the ~3 deaths per tick, for ~5,000 asks,
  so at most 0.1 % of asks can ever succeed, sensing or not. The empty voxels a cell can see are
  the dark layer under the canopy, where a child starves. Aiming well at them does not pay.
- **Sharing happens at scale, bodies don't yet.** An eighth of the cells give, 2.5 energy per
  cell per tick flows, and clonal bodies of up to 13 cells form, but nothing lives in the dark
  middle: no body feeds a cell down there. Whether the gifts go to kin or to anyone is not
  measured yet.
- Fewer cells than under 11b (−11 %): gifts at the cap are lost, and a gift to a stranger is a
  loss to the giver.

Done: `test_gamma`, 49 checks: the 11b checks with the division timeline one tick shorter, and
energy given from cell to cell by hand-written cells: an eighth of 80 is 10, a chain of three (the
middle one takes 10 and gives 5 of its own), nothing to an empty voxel, the next tick's eighth, and
a full cell that keeps 254. The whole suite is 2,638 checks under gcc. Four injected bugs were all
caught, with 14–15 failures each: the gift not taken from the giver, a gift made to an empty voxel,
the grant read from the wrong side, and plain C not capping the sum of gifts.

Next: measure where the gifts go (kin or strangers), and give the dark a way to be worth it; a live
view to watch it.

### 11d. Weather, from the three books ✅

The user asked what the Torah, the Quran and the Bible have for this world. Five rules, chosen
for a world where space was the limit and nothing in a cell's senses changed; the passages and
the reasons are in [docs/GAMMA.md](docs/GAMMA.md), the exact rules in [src/gamma.h](src/gamma.h):

- **Day and night.** A clock in every voxel, 8 bits, incremented at the start of every tick; the
  sun plane's light is a triangle wave of it, 0 at midnight, 254 at noon. The clock costs 92
  instructions (8 half adders in place, no register, no segment cut); the wave 7 XORs and 7
  multiplexers where the "OR with sun" was.
- **Manna.** At midnight whatever a cell holds above 127 rots. Upkeep stays 1.
- **An appointed term.** Each cell draws its term, 256–511 ticks, from its own dice in the tick it
  is born, and dies when it is up. Death by chance is gone. A 9-bit countdown in place, loaded
  through a multiplexer from the dice; 257 instructions, against 24 for the dice-bit death.
- **The lot.** A contested vacancy goes to the asker in direction d with the smallest d XOR k, k
  three dice bits of the empty voxel; 11b–11c gave it to the lowest d. A tree of ORs over groups
  of directions and one NAND per bit and group: 151 instructions, 57 more than the scan it
  replaces.
- **The measure.** A ledger of every tick in the plain C, with two identities (energy and cells)
  that the tests check after every tick of running worlds and the census prints: it is the
  conservation law the 11c bugs would have failed.

**How it was built.** With ultracode on: four independent critics (ecology, circuit, tests,
fidelity to the texts) attacked the first design before a line was written, and two of them
changed it: manna as a per-tick levy on wealth starves bodies, so it became the midnight loss;
one term for all phase-locks the world into a death wave, so each cell draws its own. Then the
contract went into gamma.h, four implementers built the plain C, the circuit, the tests and the
census from it in parallel worktrees, five reviewers tried to refute each patch and cross-checked
plain C against circuit, and the four patches merged with disjoint hunks. The merged program
passed `test_gamma` on the first run.

One tick of the 32 × 32 × 32 world: 52,628 instructions in 4 segments (11c: 52,084): where am I
616, clock 92, dice 488, neighbour reads 3,528, gates 33,964, asking and offering 180, grants
2,539, partner reads 2,683, energy and state 2,749, genome copy 3,437, light 356, minerals 1,828,
registers 168.

The same world under both rules, seed 1, a million ticks, on the same machine in the same
session (11c rebuilt from its commit next to 11d):

| at tick 1,000,000 | 11c | 11d |
|---|---:|---:|
| cells | 5,575 | 6,182 |
| cells dividing | 148 | 1,242 |
| distinct genomes | 3,149 | 3,325 |
| energy per cell | 207 | 145 (84 at midnight, 153 by day) |
| deaths per tick | ~3 (by chance) | 17.4: term 13.9, starved 2.1, aborted 1.4 |
| free cells asking | 90 % | 69 % |
| asks that get a voxel | 0.0–0.1 % | 0.4–0.9 % |
| free cells giving | 12.8 % | 1–2 % |
| energy given per tick | 13,900 | 300–900, of which 3–17 % to a cell of the giver's genome |
| what midnight takes, per day | — | ~340,000 at noon (55 per cell) |
| gains per tick at noon: light / minerals / lost at the cap | — | 10,700 / 11,800 / 12,200 |
| grants per tick by the winner's direction (+x −x +y −y down up) | — | 1.4 2.7 2.2 3.0 7.3 2.9 |
| cells in the 24 layers between canopy and floor | 0 | 0 |
| cells in the canopy band / the floor band | 2,389 / 3,186 | 2,468 / 3,714 |
| ticks/s, 16 threads | 11,500 | 11,000 |

What it says:
- **The world turns over six times faster**, and it is bigger, not smaller: a tenth more cells.
  The term alone kills 14 a tick, the nights starve 2, and 7 % of divisions abort because the
  parent's term ends during the copy, as the critics predicted.
- **Asking pays now.** Five to ten times more asks succeed. Every one of the three most common
  genomes reads the occupancy of its neighbours on all three axes to pick a direction, and they
  share a family: gate 5 = NAND(cell at −x, cell at −y), gate 10 reading itself (a 1-bit memory),
  direction bit 1 = NAND(cell at −y, cell at +y). Most grants go to the cell below the vacancy:
  cells divide upward, towards the light at the canopy and away from the floor.
- **Unconditional giving was selected away** at the first nights, as predicted: an eighth of the
  free cells gave in 11c, one to two in a hundred do now, and they give on conditions ("unless
  there is a cell below", "unless both up and down are taken"). The gifts that remain go to kin
  far above chance (a random neighbour shares the giver's genome in 0.2 % of cases).
- **Energy is still not scarce.** By day the cells sit at the cap and 12,000 a tick are lost
  there, more than all the upkeep; the night only makes it swing. The floor knows no weather
  (minerals have no day) and holds 60 % of the cells; nothing lives in the dark middle.

Done: `test_gamma`, 75 checks, every expected value computed by an independent model of the
contract: the sun at seven clocks and the light of an empty column after 1, 16, 143 and 300 ticks;
the division with the new cell's term read from its dice; a cell with term 2; a parent whose term
is up in the tick it is granted, and its embryo's abort; the lot decided by the voxel's dice; the
midnight on a cell at 200, at 100, and on a newborn; the ledger after every tick of two soups,
both identities from the arrays and received = given, no term past 511, the clock the same
everywhere; the engine checks from random states of everything including clock and term, and
compiled against plain C on 14 shapes, through midnights and term deaths, with threads. The
whole suite is 2,664 checks under gcc. Five injected bugs were all caught, with 14–22 failures
each naming the field: the lot and the term read from the wrong dice bits, the term up one tick
late, the sun wave folded on the wrong clock bit, a midnight that never rots.

Next: the floor. Minerals know no day and the floor zone sits at the cap; a season for the
minerals, or a finite floor that must rest (Leviticus 25:4), would give the weather the other half
of the world. And the live view.

### 11e–11g ⬜

- 11e `alpha gamma --serve`: a live page of the cube; click a cell to see its circuit.
- 11f the twin world across the 16th dimension: butterfly and control modes.
- 11g AVX-512 (5c), the world across the cluster (7g).

Details in [docs/GAMMA.md](docs/GAMMA.md).

## Parked

### Control flow with lane masks (was step 7)

Parked by decision (2026-10-14), like step 3: a conditional jump is a new instruction and would
change the 1-bit-opcode design. It would also let lanes take different paths, which breaks the
lockstep the hypercube (step 7) depends on.

- Conditional jump plus a per-lane enable mask (like the MC14500B's OEN): lanes that do not take a
  path stop writing.
- Measure what it costs when lanes disagree (divergence).
