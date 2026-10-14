# Tutorial: Tesseract on your own PC

An hour from a fresh clone to running your own 1-bit programs on 512 machines at once, proving
circuits correct for every input, booting a computer that is built out of NAND gates, and asking
the superoptimizer whether you could have done better. Every step shows the command and what you
should see; the numbers will be different on your machine.

## What you need

- **An x86-64 CPU with AVX2**: Intel since Haswell (2013), AMD since Excavator/Zen. The compiler
  turns programs into AVX2 machine code, so ARM machines (Apple Silicon, Raspberry Pi) can build
  the reference parts but not run the fast engines.
- **Windows 10/11** with *Visual Studio 2022 Build Tools* (the free "Desktop development with C++"
  workload), or **Linux** with `gcc` or `clang`.
- `git`, and about 50 MB of disk.

## 1. Get it and build it

```
git clone https://github.com/enderPeer/tesseract
cd tesseract
```

Windows, in PowerShell (the build finds Visual Studio by itself):

```
.\build.bat test
```

Linux:

```
./build.sh test
```

This builds `bin/alpha` and runs the test suites. Each one ends with a line like
`1199 checks, 0 failures`; together they are over 2,600 checks, a minute or two. If they pass,
every engine on your machine is bit for bit the same as the reference model.

The commands below are written `bin/alpha ...`; that works in PowerShell and in Linux shells (in
the old `cmd.exe`, write `bin\alpha`).

## 2. The machine in five minutes

The whole machine:

- a 1-bit register **RR** and memory cells of 1 bit each; cell `ZERO` is always 0 and `ONE` always 1;
- **`NAND x`**: `RR = !(RR & x)`;
- **`STO x`**: `x = RR`;
- no jumps: the program runs from top to bottom, then starts again (one *pass*).

That is all. Here is XOR, in [examples/xor.asm](../examples/xor.asm):

```
bin/alpha run examples/xor.asm --table
```

```
a b | out
0 0 | 0
0 1 | 1
1 0 | 1
1 1 | 0
```

Watch it work, one instruction at a time:

```
bin/alpha run examples/xor.asm --set a=1 --set b=0 --trace
```

and see the actual machine code, one opcode bit and the address bits per instruction:

```
bin/alpha asm examples/xor.asm
```

## 3. Your first program

Make a file `maj.asm` in the top folder: the majority of three bits, `m = 1` when at least two of
`a`, `b`, `c` are 1. The macros in [lib/std.inc](../lib/std.inc) (`LD`, `AND`, `OR`, ...) expand
into NAND and STO:

```
; majority: m = 1 when at least two of a, b, c are 1
.include "lib/std.inc"
.in  a b c
.out m

    LD   a
    OR   b          ; a | b
    AND  c          ; c & (a | b)
    STO  t
    LD   a
    AND  b          ; a & b
    OR   t          ; a & b | c & (a | b)
    STO  m
```

```
bin/alpha run maj.asm --table
bin/alpha asm maj.asm
```

The table shows 1 in the four rows with two or three ones, and `asm` says the program is 20
instructions long. Is that good? Ask the **superoptimizer**, which tries every shorter program
there is:

```
bin/alpha super "m = a&b | a&c | b&c"
```

It prints the shortest program and the proof that nothing shorter exists (it tried every shorter
one). Copy its instructions into `maj.asm` in place of yours and check the table again. More in
lesson 10.

## 4. 512 machines at once

Everything so far ran on the *reference machine*, the slow and obviously correct model. The real
engines run 512 copies of the machine side by side, one per bit of a 64-byte cache line, and a
compiler turns your program into AVX2 code:

```
bin/alpha check examples/adder8.asm --engine jit
bin/alpha bench examples/adder8.asm
bin/alpha jit examples/xor.asm
```

`check` starts all 512 lanes in random states and compares every cell of every lane with 512
reference machines after every pass. `bench` times all engines on one core: the compiled code does
trillions of 1-bit operations per second (`T`). `jit` shows the x86 code it made of XOR.

`bin/alpha scale examples/adder8.asm` runs it on 1, 2, ... all of your threads.

## 5. Prove a circuit correct for every input

Generate a 12 × 12-bit multiplier as a NAND/STO program and check it against plain C for all
2^24 inputs:

```
bin/alpha gen mul --bits 12 > mul12.asm
bin/alpha verify mul12.asm --spec "p = a * b"
```

It reports `16777216 of 16777216 inputs` with the time it took, then
`OK: the outputs equal the spec for every input tried`. Now give it a wrong specification and see
the inputs where it fails:

```
bin/alpha verify mul12.asm --spec "p = a * b + 1"
```

Search runs a program backwards, by trying every input: which input gives this toy hash?

```
bin/alpha search examples/toyhash32.asm --want "h = 0x165c2a07"
```

That is 2^32 inputs; on an 8-core PC it takes seconds. `equiv` checks two programs against each
other the same way, and `.bench` netlists from the ISCAS benchmarks load directly
(`bin/alpha verify examples/c17.bench --spec "..."`).

## 6. Machines that talk: the cube

The 512 machines can read each other's cells. Machine k is wired to machine k XOR 2^d for d = 0
.. 8: the corners of a 9-dimensional cube. A link cell `.link n = r @ d` reads cell `r` of the
neighbour across dimension d, and `ID0` .. `ID8` tell a machine its own number. In
[examples/cube_any.asm](../examples/cube_any.asm) every machine finds out, in 9 steps, whether any
machine has `x = 1`:

```
bin/alpha run examples/cube_any.asm --set x=1 --lane 300
bin/alpha check examples/cube_any.asm --engine jit
```

[lib/cube.inc](../lib/cube.inc) has the collectives built this way: OR/AND/XOR over all machines,
broadcast, running counts, maximum, sorting, packing, any permutation. See them checked and timed:

```
bin/alpha cube
bin/alpha cube sort
```

`bin/alpha cube --dims 12` does it on 4,096 machines (dimensions 9 and up join groups of 512).

## 7. The Game of Life

```
bin/alpha life --size 16 --gens 50 --show
bin/alpha life --cube --show
```

The first runs 512 separate 16 × 16 boards, one per lane, and compares every one with plain C. The
second spreads one 512 × 1024 board over the 512 machines as tiles that swap their edges over the
links every generation. `--dims 14` makes it 4,096 × 4,096 on 16,384 machines.

## 8. A computer built from NAND: beta

Beta is an 8-bit computer — registers, 256 bytes of memory, 16 instructions — whose whole CPU is one
NAND/STO program: one pass is one beta instruction. Every lane is its own beta. A monitor in ROM
loads programs and runs them ([docs/BETA.md](BETA.md)):

```
bin/alpha beta run examples/beta/hello.bs
bin/alpha beta run examples/beta/fib.bs
```

Write your own: make `stars.bs`, which reads a number and prints that many stars:

```
; prints N stars, N from the input
        .org 0
        IN              ; A = N
loop:   JZ  done
        STA n
        LDA #'*'
        OUT
        LDA n
        SUB #1
        JMP loop
done:   LDA #'\n'
        OUT
        JMP MON         ; back to the monitor
n:      .byte 0
```

```
bin/alpha beta run stars.bs --byte 5
bin/alpha beta run stars.bs --lane-input --show 7
```

With `--lane-input`, beta number k gets k as its input, so the 512 betas print 512 different
things, each finishing at its own time, and every one is checked against an emulator in C.

## 9. A world: gamma

Gamma is a 3-D world of voxels, one per machine, with light falling from a sun plane and minerals
diffusing up from the floor ([docs/GAMMA.md](GAMMA.md)). The laws are one NAND/STO program:

```
bin/alpha gamma --show
bin/alpha gamma --occupied 20 --show
```

`--occupied` puts things into the world; watch the shade under them in the light slice.

## 10. The superoptimizer

`alpha super` takes a specification — outputs as expressions of inputs — and finds the shortest
NAND/STO program, with the proof:

```
bin/alpha super "RR = RR ^ x"
bin/alpha super "o = a ^ b ^ c"
bin/alpha super "r{a,b} = s ? a : b"
bin/alpha super --library
```

`RR` on the right is the register's value at the start, on the left its value at the end. `r{a,b}`
means r may be the same cell as a or b, so the program must not write it before its last read of
them. `name := expression` gives a cell that already holds a value. The program it prints can
go straight into a `.asm` file; `--macro` names the scratch cells `@t1`, ... for a macro body.

`--library` takes about two minutes: every building block of the library, what it cost before
the superoptimizer existed (`was`) and its shortest form, which the library uses now. A search
grows by roughly 7× per instruction; 15 to 20 instructions take seconds to an hour.

## 11. Experiments

Ideas, from a few minutes to a weekend:

- **What do random programs do?** `bin/alpha tester1 --seconds 30` runs random NAND/STO programs
  from random states and measures how long until they repeat. How does that grow with the number
  of cells?
- **Beat the library.** Write a two-bit adder or a comparator as a specification and let `super`
  find it; can you get anywhere near it by hand?
- **Your own circuit.** Write an ALU, a counter or a shift register in assembly, check it with
  `run --table` and `check`, then `verify` it for all inputs against a C expression.
- **Find a preimage.** Make a bigger toy hash (`bin/alpha gen hash --bits 36 --rounds 8 > h.asm`)
  and `search` for an output. How long does 2^36 take on your PC?
- **Rule 110 on a ring.** [examples/rule110.asm](../examples/rule110.asm) runs the Turing-complete
  automaton on a ring of 512 machines; try other rules.
- **512 different beta programs.** Give every beta a different number with `--lane-input` and print
  something that depends on it: primes below N, a countdown, a pattern.
- **Change the world.** In gamma, how far down does the light reach through 20 % occupied voxels?
- **Measure your CPU.** `bin/alpha scale examples/adder8.asm` and `bin/alpha sweep
  examples/adder8.asm` show how your cores and caches compare; E-cores, hyper-threads and RAM all
  show up.

## 12. More than one PC (optional)

`tools/cluster.sh` (from Git Bash on Windows: `tools\cluster.cmd`) copies the project to Linux
machines over SSH, builds and tests it there, measures them, and splits `search`, `verify` and
`equiv` between all of them by speed. Copy `tools/cluster.local.example.sh` to
`tools/cluster.local.sh` and name your machines; everything runs there at idle priority.

```
tools/cluster.sh deploy
tools/cluster.sh test
tools/cluster.sh search examples/toyhash40.asm --want "h = 0xb3c369a491"
```

## When something goes wrong

- **`build.bat`: Visual Studio not found** — install *Build Tools for Visual Studio 2022* with the
  C++ workload; the script finds it through `vswhere`.
- **"the 512-lane engine needs a CPU with AVX2"** — the reference machine (`run`, `asm`, the
  emulators) still works; the fast engines need AVX2.
- **A test fails** — please open an issue with the failing line and your CPU model
  (`bin/alpha bench` prints it): every engine is meant to be bit-exact everywhere.

Where it is going, with every measurement so far: [ROADMAP.md](../ROADMAP.md). The machine itself:
[docs/ISA.md](ISA.md).
