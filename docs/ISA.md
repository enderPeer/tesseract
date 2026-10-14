# ISA v1 — the NAND/STO machine

A 1-bit machine with a 1-bit opcode. This document is the contract: every engine in this project
(reference, bitsliced, compiled, FPGA) must produce exactly the same bits.

## State

| Name | Width | Power-on value |
|---|---|---|
| `RR` | 1 bit | 0 |
| `PC` | enough bits to index the program | 0 |
| `M[0..N-1]` | N cells of 1 bit | 0, except `M[1]` = 1 |

## Instructions

Encoding: `[op:1][addr:n]` — one opcode bit, then an n-bit cell address, where n is the smallest
width that fits every cell the program uses. `alpha asm` prints the exact bits.

| op | Mnemonic | Effect |
|---|---|---|
| 0 | `NAND a` | `RR ← ¬(RR ∧ M[a])` |
| 1 | `STO a` | `M[a] ← RR` |

NAND alone can build every other logic function, and STO writes results back — together the
smallest complete instruction set for a machine with addressed memory.

## Memory

- Every address selects one bit.
- Cell 0 `ZERO` always reads 0 and cell 1 `ONE` always reads 1. Writes to them are ignored by the
  machine and rejected by the assembler.
- Every other name gets the next free cell, in order of first appearance.
- Input cells (`.in`) are written by the host before a pass, output cells (`.out`) are read after
  it. A cell may be both.

## Execution

- Instructions run in order. After the last one the PC wraps to 0; one full pass is a **tick**.
- There are no jumps. Every tick executes every instruction, so a program behaves like one clock
  cycle of a logic circuit. Cells keep their value between ticks and act as state (see
  `examples/counter4.asm`).
- `RR` also keeps its value from one tick to the next.

## Assembly language

```
; comment
.in   a b c            ; input cells
.out  s c              ; output cells
.include "std.inc"     ; path relative to the including file

.macro NAME p1 p2      ; parameters are replaced by the call's arguments
    NAND p1
    STO  @tmp          ; @names are local: a fresh cell for every call
.endm

    NAND a
    STO  b
    NAME a b           ; macro call
```

- Names, mnemonics and macro names are case-insensitive. A name is letters, digits and `_`, not
  starting with a digit.
- Operands may be separated by spaces or commas.
- Macros may call other macros (nesting limit 64, so no recursion). Every call allocates new cells
  for its `@names` (listed as `@<call>:<name>`).
- Inside a macro, `#` joins pieces of a name, and pieces that are parameters are replaced first:
  with `v` = `sum` and `d` = `3`, `v#0` is `sum0` and `ID#d` is `ID3`. This is how macros work
  on multi-bit values (cells `sum0`, `sum1`, ...). Numbers may be passed as macro arguments.
- `lib/std.inc` has the standard macros: `SET CLR NOT LD LDC AND OR NOR ANDC ORC XOR XNOR STOC MOV FA`,
  each with its cost in machine instructions.

## The cube: 512 machines wired together (step 7a)

The instructions and their encoding do not change. What changes is the memory map: a program may
read cells of other machines.

**512 machines in lockstep.** A cube is 512 copies of the machine, numbered 0–511, all running
the same program. Machine k is wired to the 9 machines **k XOR 2^d, d = 0..8**: the corners of a
9-dimensional cube, one dimension per bit of the number. All 512 run instruction t before any of
them runs instruction t + 1. A `NAND` only reads and a `STO` only writes the machine's own cells,
so every read sees the other machines' cells exactly as they were before that instruction.

**Lane-id cells** `ID0` .. `ID8`: in machine k, `IDd` reads bit d of k. They are read-only. They
are the only way for a machine to tell which corner it is.

**Link cells**, declared before their first use:

```
.link y = x @ 3        ; reading y in machine k reads x in machine k XOR 8
```

- `x` is any cell: a plain one, `ZERO`/`ONE`, an id cell or another link. Links in a row add up:
  if `z = y @ 5`, then `z` reads `x` in machine k XOR 8 XOR 32. A link that leads back to itself
  is an error.
- Link cells are read-only. They take up an address like any other cell, so programs are still
  `[op:1][addr:n]`.

**Read-only cells.** `ZERO`, `ONE`, id cells and link cells: the assembler rejects `STO` to them and
`.in` / `.out` on them, and the machines ignore writes to them, as before for the constants.

A program without id or link cells behaves exactly as before: its 512 machines never meet.

### Bigger cubes (step 7f)

Dimensions go up to 15: `ID9` .. `ID15` and `.link y = x @ 9` .. `@ 15`. A program's cube has
2^D machines, D = 9 or one more than the highest dimension its id and link cells name — up to
65,536 machines. Nothing else changes: all of them run instruction t before any runs t + 1, and
machine k is wired to k XOR 2^d for every d < D.

How the engines keep that promise (not part of the machine): machines 512 g .. 512 g + 511 form
group g, one 512-lane line per cell as before. Dimensions 0–8 move lanes inside the line,
dimensions 9 and up read the same cell in another group's memory. The groups run one after
another, or on different threads, so the compiler cuts the pass into segments: wherever a
segment would both write a cell and read it across dimension 9 or up, a new segment starts. Each
segment runs in every group before the next one starts anywhere — then every read sees exactly
what lockstep would show it. The reference machine needs none of this: it steps all 2^D machines
one instruction at a time.

## Step 8: a computer on top

The machine never gets more than this. [BETA.md](BETA.md) builds a stored-program byte computer
out of it, as one fixed NAND/STO program: programs, jumps and indirect addressing live in that
computer's memory, not in the machine.
