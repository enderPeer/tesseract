# Gamma — a world of cells on the cube

alpha is a 1-bit NAND machine, [beta](BETA.md) an 8-bit computer built from it, and gamma a world
built from it: a 3-D cube of voxels in which cells live. Every law of the world is one NAND/STO
program that runs the same in every machine, once per tick, and every cell will itself be a small
NAND circuit. Status and measurements: [ROADMAP.md](../ROADMAP.md), step 11.

## Why, next to Universe 2

Universe 2, a separate project, is an artificial chemistry on GPUs: codons on a 1-D tape, organisms
in the slots of a 2-D torus, millions of them at about 20 ticks/s on an RTX 4090. Its question is
how life starts. A straight port to alpha would be a worse Universe 2: alpha has at most 65,536
machines, and every byte of RAM a machine holds costs about 180 instructions per step (beta, step
8c). Gamma uses what only alpha has instead: links between machines, physics that runs as one
circuit in lockstep, and tools that prove what a circuit computes.

| | Universe 2 | Gamma |
|---|---|---|
| Physics | CUDA kernels written in C++ | one NAND/STO program, the same in every voxel |
| Space | 2-D torus of slots, genes read along a 1-D tape | 3-D torus, 6 face neighbours, nothing moves faster than 1 voxel per tick |
| Genome | codons, 31 operations | a NAND netlist, which alpha's builder and `.bench` loader take as it is |
| Replication | the gene must compute it: the origin of life is the question | proposed: a physics primitive, so evolution works on behaviour (open, see below) |
| Organism | one slot | many cells, held together by signals and shared energy |
| Analysis | sampling; loopholes found by hand | exhaustive: `verify`/`equiv` over every input a cell reads (2^32 cases in 0.4 s), `super` on small parts |
| Experiments | one world per configuration, a shadow population | a twin world one link away (the 16th dimension): butterfly and control runs in the same pass |
| Hardware | RTX 4090 / 3060 | any AVX2 CPU; AVX-512 (5c), the cluster (7g), an FPGA (9) |
| Viewing | PNG and MP4 after the run | live (11c) |

The two are meant to work side by side: Universe 2 asks how life starts, gamma what space, bodies
and signals do with it once it exists, and lets you prove what evolved.

## Decisions

- **2026-10-14: cells are NAND circuits.** Not one beta per voxel: byte-addressed code breaks
  easily under mutation. Beta stays a candidate for a later "program your own creature" mode.
- **Open: replication.** Proposed: division is part of physics (the cell asks, the world copies its
  genome over the link). The alternative is Universe 2's way, where the cell's own circuit has to do
  the copying. Step 11a does not depend on it; 11b does.

## The world (step 11a, built)

A torus of 2^a × 2^b × 2^c voxels, 512 to 65,536 of them, one per machine. Each axis sits in its
own dimensions of the cube in Gray code, x lowest, so every face neighbour is one link away (step
7e's grids, with a third axis). z = 0 is the sun plane at the top; the last layer is the floor.

Each voxel has `occ` (something is there), light (0 .. 255) and minerals (0 .. 4095: the 8 bits a
cell will see and 4 more below the point). One tick, every voxel at once:

- **Light** falls one voxel per tick. The sun plane gets 255. Every other voxel gets the light of
  the voxel above it, of which an empty voxel passes on l − l/8 and an occupied one l/2, so shade
  forms under whatever is there.
- **Minerals** diffuse up from the floor. The floor gets 4095 and the sun plane 0. In between a
  voxel gets (2m + its 6 neighbours) / 8, rounded down. With whole numbers the rounding stopped the
  diffusion about 8 layers above the floor for good (a fixed point, reached after 84 ticks); with
  4 bits below the point the minerals reach the whole depth.

`src/gamma.c` holds the rules twice: `gamma_step()` in plain C is the definition, and
`gen_gamma()` is the same tick as one NAND/STO program. They are checked against each other bit for
bit on every engine. For now `occ` is set by the host; in 11b the cells set it.

## A cell (step 11b, planned)

- **Genome:** 32 NAND gates, each with two 6-bit input addresses, 384 bits.
- **Inputs:** an address picks one of 32 senses or one of the 32 gate outputs. The gates run in
  order, so a gate that reads itself or a later gate gets last tick's value: memory and oscillators
  come from wiring alone.
- **Senses:** a constant 1, which of the 6 neighbours are occupied, 2 signal bits from each, light,
  minerals and the cell's own energy (their top bits), and a random bit.
- **Actions:** fixed gate outputs: 2 signal bits (the neighbours read them next tick), divide, a
  direction, and give energy to the neighbour in that direction.
- **Division (if it is physics):** the target voxel must be empty, and the target settles
  conflicts. The genome streams over the link 12 bits per tick, so division takes 32 ticks. Each
  copied bit flips with probability 2^-k, from a random generator in each voxel seeded with its
  number.
- **Cost estimate:** about 40,000 instructions per voxel per tick, roughly one beta step. The two
  64-way input selects per gate dominate it.

## Steps

| | | |
|---|---|---|
| 11a | the world: 3-D torus, light and minerals, checked against plain C, timed | ✅ |
| 11b | cells: genome, energy, division and mutation, death; a hand-written plant that survives | ⬜ |
| 11c | `alpha gamma --serve`: a page that draws the cube live; click a cell to see its circuit | ⬜ |
| 11d | the twin world across the 16th dimension: butterfly and control modes | ⬜ |
| 11e | AVX-512 (5c), the world across the cluster (7g) | ⬜ |
