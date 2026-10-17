# Tesseract — a 1-bit computer, 512 at a time

A computer with a **1-bit opcode and two instructions**: `NAND` (RR = !(RR & cell)) and `STO`
(cell = RR). No jumps, no other operations — and yet, on an ordinary PC:

- **512 copies run side by side** in every 64-byte cache line, compiled to AVX2 machine code:
  5 trillion 1-bit operations per second on one core, 74 trillion on a 24-core desktop.
- **Every input, proven.** A 16-bit multiplier written in NAND checked against C for all 2^32
  inputs in 10 s; a 2^32 hash search in 0.4 s.
- **Machines wired into a hypercube** of up to 65,536: they sort 512 bytes in 1.8 µs, apply any
  permutation, multiply polynomials by NTT and run Game of Life on one 8192 × 8192 board.
- **A computer built out of NAND:** beta, an 8-bit machine with a monitor that loads and runs
  programs; 16,384 of them run at once.
- **A world:** gamma, a 3-D torus of voxels with light and minerals, and cells in it that are
  NAND circuits, divide, feed each other and evolve from a soup of random genomes.
- **A superoptimizer** that proves programs shortest: the library's full adder went from 28
  instructions to 19, and nothing shorter exists.

Every engine — reference machine, 512-lane interpreter, two compilers, threads — is checked bit for
bit against one reference model: 2,600+ test checks. The machine and its command are called `alpha`.

## Start

You need an x86-64 CPU with AVX2 and either Windows with *Visual Studio 2022 Build Tools* (C++) or
Linux with gcc/clang.

```
git clone https://github.com/enderPeer/tesseract
cd tesseract
.\build.bat test          (Windows, PowerShell)
./build.sh test           (Linux)
bin/alpha run examples/xor.asm --table
```

Then follow **[the tutorial](docs/TUTORIAL.md)**: your first program, 512 machines at once, proving
a circuit for every input, the cube, Life, beta, gamma, the superoptimizer, and experiments.

| | |
|---|---|
| [docs/TUTORIAL.md](docs/TUTORIAL.md) | from a fresh clone to your own experiments |
| [docs/ISA.md](docs/ISA.md) | the machine: NAND, STO, cells, links, the cube |
| [docs/BETA.md](docs/BETA.md) | beta, the byte computer built from NAND/STO |
| [docs/GAMMA.md](docs/GAMMA.md) | gamma, a world of cells on the cube |
| [ROADMAP.md](ROADMAP.md) | every step so far, with what was measured, and what is open |

## Use

```
bin/alpha asm examples/xor.asm                    machine code, bit by bit
bin/alpha jit examples/xor.asm                    the x86 code the compiler makes of it
bin/alpha run examples/full_adder.asm --table     truth table over every input
bin/alpha run examples/counter4.asm --ticks 16    sequential logic, one line per tick
bin/alpha run examples/xor.asm --set a=1 --set b=0 --trace

bin/alpha run examples/adder8.asm --table --engine jit     512 table rows per pass
bin/alpha check examples/adder8.asm --engine jit  every lane against the reference machine
bin/alpha bench examples/adder8.asm               speed of all engines on one core
bin/alpha scale examples/adder8.asm               compiled code on 1 .. every thread
bin/alpha sweep examples/adder8.asm               one thread, memory from L1 to RAM

bin/alpha random --cells 64 --seed 7              a random program (random bits, valid NAND/STO)
bin/alpha tester1                                 what do random programs do? cycle lengths

bin/alpha verify examples/mul16.asm --spec "p = a * b"     all 2^32 inputs against a C expression
bin/alpha verify examples/c17.bench --spec "..."           .bench netlists load directly
bin/alpha equiv a.asm b.bench                     two programs, every input
bin/alpha search examples/toyhash32.asm --want "h = 0x165c2a07"   every input giving that output
bin/alpha life --size 32 --show                   512 Game of Life boards against plain C, speed
bin/alpha life --cube                             one 512 x 1024 board across all 512 machines
bin/alpha life --dims 16                          one 8192 x 8192 board on 65,536 machines, all cores
bin/alpha gen mul --bits 16                       generators: mul, hash, life, ... (prints assembly)
bin/alpha net2asm examples/c17.bench              a netlist as NAND/STO assembly

bin/alpha run examples/cube_any.asm --set x=1 --lane 300   the cube: 512 machines wired together
bin/alpha check examples/cube_any.asm --engine jit         every lane against the reference cube
bin/alpha cube                                    sort, pack, permute, ...: checked and timed
bin/alpha cube polymul                            a polynomial product mod x^512 + 1, by NTT
bin/alpha cube --dims 16                          65,536 machines in lockstep: OR, count, sort, Life
bin/alpha check examples/rule110.asm --engine jit  the machines as a ring: Rule 110

bin/alpha beta run examples/beta/hello.bs         beta: 512 of them boot, the monitor loads and runs it
bin/alpha beta run examples/beta/primes.bs --lane-input --show 97   each beta its own input
bin/alpha beta asm examples/beta/fib.bs           beta assembly: addresses, bytes, instructions
bin/alpha beta monitor                            the monitor in beta's ROM
bin/alpha beta bench                              beta steps per second, 1 and all threads

bin/alpha gamma                                   gamma: a 32 x 32 x 32 world of cells from a random
                                                  soup, checked against plain C, then a census
bin/alpha gamma --ticks 1000000 --show            a million ticks of evolution; a slice at the end
bin/alpha gamma --serve 8765                      the world live in the browser at
                                                  http://127.0.0.1:8765/; click a cell for its circuit
bin/alpha gamma --need 12 --ramp 16 --predators 256   the social game: cells that must talk to
                                                  live, and predators only a warning escapes

bin/alpha super "RR = RR ^ x"                     the shortest NAND/STO program, proven (7 here)
bin/alpha super "s{a,b} = a ^ b ^ c; c = a&b | a&c | b&c"   a full adder: 19, nothing shorter
bin/alpha super --library                         every building block: before, and its shortest
```

`bin/alpha` works in PowerShell and Linux shells; in `cmd.exe` write `bin\alpha`. Engines: `ref`
(reference), `lanes` (512-lane interpreter), `literal` (compiled one instruction at a time), `jit`
(compiled, optimized).

## Several machines

`tools/cluster.sh` (from PowerShell: `tools\cluster.cmd`, which uses Git Bash) copies the project
to Linux machines over SSH, builds and tests it there and splits work between all of them, always
at idle priority. Name your machines in `tools/cluster.local.sh`, copied from
[tools/cluster.local.example.sh](tools/cluster.local.example.sh).

```
tools/cluster.sh deploy     copy the sources to ~/<folder> on every node
tools/cluster.sh test       build and run the tests there
tools/cluster.sh scale      per-node scaling tables
tools/cluster.sh total      every free thread on every node at once, summed
tools/cluster.sh tester1    tester 1 on every machine at once (T1SECS, T1ARGS to adjust)
tools/cluster.sh gamma      alpha gamma on each node: checked, then timed (GAMMA_ARGS adds options)
tools/cluster.sh search examples/toyhash40.asm --want "h = 0xb3c369a491"
                            the inputs split between all machines by speed (also verify, equiv)
```

## License

Copyright (C) 2026 enderPeer. Free software under the GNU General Public License v3.0: you may
use, study, change and share it; changed versions you distribute must be GPL-3.0 too. See
[LICENSE](LICENSE).

## Layout

```
docs/TUTORIAL.md tutorial: from a fresh clone to your own experiments
docs/ISA.md      ISA v1 specification
docs/BETA.md     beta, the byte computer built from NAND/STO (step 8): instructions, ports, monitor
docs/GAMMA.md    gamma (step 11): a world of cells on the cube: the plan, the world's rules
lib/std.inc      standard macros (LD, AND, OR, XOR, FA, ...), each the shortest possible
lib/cube.inc     the machines together: OR/AND/XOR, broadcast, prefixes, min/max, sort, pack,
                 permute, Möbius transform, the machines as a ring
src/asm.c        assembler
src/ref.c        reference machine (the golden model)
src/lanes.c      512-lane bitsliced interpreter (AVX2), and groups of them as one bigger cube
src/jit.c        compiler: NAND/STO -> x86-64 machine code, cut into segments for bigger cubes
src/x86.c        encoder for the AVX2 instructions the compiler uses
src/check.c      any lane engine vs the reference, machine by machine
src/parallel.c   compiled code on many pinned threads at once; one cube across threads in lockstep
src/tester.c     tester 1: random programs, cycle lengths with Brent's algorithm
src/build.c      circuit builder: gates and flip-flops -> NAND/STO
src/net.c        ISCAS .bench netlists
src/gen.c        generated workloads: multiplier, toy hash, Game of Life, and for the cube the
                 Walsh-Hadamard spectrum, XOR convolution, NTT polynomial product, Life on one
                 board across the machines, the grids (with C versions)
src/spec.c       specs like "c:s = a + b + c"
src/enum.c       every input, 512 at a time: verify, equiv, search
src/work.c       command line for the workloads
src/benes.c      switch settings for any permutation of the 512 machines (Beneš network)
src/cubedemo.c   alpha cube: every cube operation checked and timed
src/beta.c       beta: emulator, assembler, monitor, the CPU as a NAND/STO program, host ports
src/betacmd.c    alpha beta: asm, monitor, run, bench
src/gamma.c      gamma: the world and its cells in plain C, and as one NAND/STO program for the
                 cube
src/gammacmd.c   alpha gamma: the soup, the check against plain C, the run with a census
src/gammaserve.c the live view's server: HTTP on 127.0.0.1 only, Winsock or BSD sockets
lib/gamma.html   the live view: the cube in WebGL, one file, nothing loaded from anywhere
src/super.c      the superoptimizer: shortest programs for a specification, with the proof
src/supercmd.c   alpha super, and the library's building blocks as specifications
src/platform.c   timer, CPU topology, pinning, priority, threads (Windows and Linux)
src/main.c       command line
examples/        xor, mux, full_adder, adder8, counter4, c17.bench, mul16, toyhash32, toyhash40,
                 cube_any, cube_rank, cube_max, rule110; beta/hello.bs, fib.bs, primes.bs
tests/           test_ref.c, test_lanes.c, test_jit.c, test_tester.c, test_work.c, test_cube.c,
                 test_beta.c, test_super.c, test_gamma.c
tools/           cluster.sh and cluster.cmd: deploy, test and measure on Linux machines over SSH
```
