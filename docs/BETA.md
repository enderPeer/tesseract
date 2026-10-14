# Beta — a byte computer built from NAND/STO (step 8)

The NAND/STO machine stays exactly as it is ([ISA.md](ISA.md)): no jumps, no indirect addressing,
one fixed program that repeats every pass. Beta is what that fixed program *is*: the circuit of a
small stored-program computer, generated gate by gate ([src/beta.c](../src/beta.c), `gen_beta`).
Its registers, program counter and memory are cells; **one pass is one beta instruction**.

That is how a real computer works too: the CPU is a fixed circuit, and programs are data in
memory. So everything step 8 asked for — writable program memory, indirect addressing, a loader,
I/O conventions, a resident monitor — lives in beta's memory, not in the NAND/STO machine.

Each of the 512 lanes is its own beta, with its own memory and its own program counter: one lane
group is 512 separate computers that may run different programs and take different branches. No
lane masks, no new instruction — divergence is just data.

## The machine

| | |
|---|---|
| memory | 256 bytes: RAM `0x00`–`0x9F`, ROM `0xA0`–`0xFF` (the monitor; writes are ignored) |
| registers | `A` and `X` (8 bits), `PC` (8 bits, always even), `C` (carry) |
| power on | all cells 0: RAM 0, registers 0, `PC = 0xA0`, the monitor (the PC cells hold `PC XOR 0xA0`) |
| instruction | two bytes at an even address: `[mode:2 op:4]` (bits 6–7 ignored), then `arg` |

Every instruction first forms the **operand** `V` from its mode:

| mode | written | `V` | address `EA` |
|---|---|---|---|
| 0 | `#n` | `arg` | `arg` |
| 1 | `n` | `RAM[arg]` | `arg` |
| 2 | `n,X` | `RAM[arg + X]` | `arg + X` (mod 256) |
| 3 | `A` | `A` | `arg` |

| op | instruction | does |
|---|---|---|
| 0 | `LDA` | `A = V` |
| 1 | `LDX` | `X = V` |
| 2 | `ADD` | `A = A + V`, `C` = carry out |
| 3 | `ADC` | `A = A + V + C`, `C` = carry out |
| 4 | `SUB` | `A = A - V`, `C = 1` if no borrow (`A >= V` before) |
| 5 6 7 | `AND` `OR` `XOR` | `A = A op V` |
| 8 | `STA` | `RAM[EA] = A` (modes 1, 2) |
| 9 | `STX` | `RAM[EA] = X` (modes 1, 2) |
| 10 | `JMP` | `PC = V` (with bit 0 cleared) |
| 11 | `JZ` | if `A == 0`: `PC = V` |
| 12 | `JC` | if `C`: `PC = V` |
| 13 | `JSR` | `RAM[EA] = PC + 2`, `PC = EA + 2` |
| 14 | `IN` `OUT` `HLT` `NOP` | mode 0: take the input byte into `A` (waits, PC unchanged, while there is none); mode 1: send `A`; mode 2: stop (PC unchanged); mode 3: nothing |
| 15 | `INX` `TAX` `TXA` `SHR` | mode 0: `X = X + 1`; 1: `X = A`; 2: `A = X`; 3: `C = A & 1`, `A = A >> 1` |

Otherwise `PC = PC + 2`. Writes to the ROM are ignored.

**Jumps use the operand like everything else.** `JMP loop` (mode 0) jumps to `loop`;
`JMP (ret)` (mode 1) jumps to the address *stored* at `ret` — memory-indirect; `JMP (tab,X)`
jumps through a table; `JMP A` jumps to the address in `A`. `ADD A` doubles `A`, `XOR A` clears it.

**Subroutines** without a stack, as on the PDP-8: `JSR sub` stores the return address in the
routine's first byte and continues after its first two bytes; the routine returns with
`JMP (sub)`. Routines begin with `.byte 0, 0`. Not reentrant.

## I/O: ports to the host

| cell | direction | meaning |
|---|---|---|
| `IN` (8), `IN_VALID` | host → beta | the host puts a byte in and sets `IN_VALID`; `IN` takes it and clears `IN_VALID` |
| `OUT` (8), `OUT_STROBE` | beta → host | `OUT` puts `A` there; `OUT_STROBE` is 1 for that pass only |
| `HALTED`, `WAITING` | beta → host | this pass was `HLT`, or `IN` with nothing to take |

Before every pass the host offers each beta that has taken its last byte the next byte of its
input; after the pass it collects what each sent. A beta is **done** when it waits for input and
the host has none left for it — normally that is the monitor waiting for the next command — or
it has halted.

## The monitor (ROM, `0xA0`–`0xEF`)

Beta powers on in the monitor, which reads commands through `IN`:

| command | bytes | does |
|---|---|---|
| load | `'L' addr n byte*n` | puts `n` bytes at `addr` |
| go | `'G' addr` | runs the program at `addr`; it comes back with `JMP MON` |
| dump | `'D' addr n` | sends the `n` bytes from `addr` through `OUT` |

Any other byte is skipped. The monitor keeps `0x9D`–`0x9F` for itself; programs use `0x00`–`0x9C`.

**Service** `PUTS` (`0xE2`): prints the zero-terminated string at `X`. Entered with the return
address in `A` (a ROM routine cannot hold its own return address):

```
        LDX #msg
        LDA #back
        JMP PUTS
back:   ...
```

The assembler knows `MON` and `PUTS`. `alpha beta monitor` prints the source and the ROM.

## Assembly

```
label:  LDA #'A'+1      ; numbers (decimal, 0x hex), 'c' with \n \t \0, names, + and -
        STA buf,X
        JMP (ret)
NAME = 0x40             ; a constant
        .org 0x20       ; where the next bytes go
        .byte 1, 2, 'x'
        .ascii "text"   ; .asciz adds a 0
        .align          ; to an even address (instructions must be on one)
```

`alpha beta run FILE` loads the bytes from the lowest to the highest address used, starts at the
lowest, and feeds `--input TEXT`, `--byte N` or, with `--lane-input`, each beta its lane number
as the program's input.

## How it is built

`gen_beta` builds the circuit with the gate builder ([src/build.c](../src/build.c)) — 40,852 NAND/STO
instructions, 1,975 cells, one pass per beta step:

- **fetch**: the 7 bits `PC >> 1` decoded into 128 select lines, then each of the 14 instruction bits
  is `OR (select & byte bit)` over the 128 words (`b_select`); ROM bytes are constants, so their
  terms cost nothing or just the select line;
- **operand**: `EA = arg + (X if mode 2)` by a ripple adder, decoded into 256 lines, the data byte
  read the same way;
- **ALU** and next PC: one adder for `ADD`/`ADC`/`SUB` (operand inverted and carry in for `SUB`),
  the logic operations, incrementers for `X + 1`, `PC + 2` and `EA + 2`, all candidates computed
  and the right one selected by the decoded instruction;
- **write port**: for every RAM byte, `byte = (EA selects it and the instruction stores) ? data :
  byte` (`b_mux`) — the only part that grows with every byte of RAM written each pass;
- then the ports and registers take their new values.

Memory is the expensive part: every pass reads 2 bytes through a 128-way multiplexer, 1 byte
through a 256-way one, and rewrites all 1,280 RAM bits. That is the price of addressing on a
machine without addresses.
