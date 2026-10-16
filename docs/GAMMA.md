# Gamma — a world of cells on the cube

alpha is a 1-bit NAND machine, [beta](BETA.md) an 8-bit computer built from it, and gamma a world
built from it: a 3-D cube of voxels in which cells live and evolve. Every law of the world is one
NAND/STO program that runs the same in every machine, once per tick, and every cell is itself a
small NAND circuit. Status and measurements: [ROADMAP.md](../ROADMAP.md), step 11. The exact rules
are at the top of [src/gamma.h](../src/gamma.h).

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
| Replication | the gene must compute it: the origin of life is the question | physics: a cell only decides when and where, so evolution works on behaviour |
| Start | an unseeded chemistry | a soup of random genomes; nothing hand-written |
| Organism | one slot | many cells, held together by signals (and later shared energy) |
| Analysis | sampling; loopholes found by hand | exhaustive: `verify`/`equiv` over every input a cell reads (2^32 cases in 0.4 s), `super` on small parts |
| Experiments | one world per configuration, a shadow population | a twin world one link away (the 16th dimension): butterfly and control runs in the same pass |
| Hardware | RTX 4090 / 3060 | any AVX2 CPU; AVX-512 (5c), the cluster (7g), an FPGA (9) |
| Viewing | PNG and MP4 after the run | live (11c) |

The two are meant to work side by side: Universe 2 asks how life starts, gamma what space, bodies
and signals do with it once it exists, and lets you prove what evolved.

## Decisions

- **2026-10-14: cells are NAND circuits.** Not one beta per voxel: byte-addressed code breaks
  easily under mutation. Beta stays a candidate for a later "program your own creature" mode.
- **2026-10-14: division is physics, and everything evolves from a random soup.** Nothing in the
  world is written by hand. A random circuit's divide output is 1 about half the time, so a soup of
  random genomes starts dividing at once and selection takes over. Had the cell's own circuit to
  copy its genome (Universe 2's way), random genomes would almost never manage it, and the world
  would need a hand-written ancestor. A hand-written cell exists only in the tests, as a fixture
  whose every step can be predicted.
- **2026-10-16: no cost for asking; bodies instead.** The competition for space is enough once
  the tick no longer handicaps the cells that look (see Bodies). Cells stay one voxel; organisms
  grow by division and sharing energy.

## The world (step 11a)

A torus of 2^a × 2^b × 2^c voxels, 512 to 65,536 of them, one per machine. Each axis sits in its
own dimensions of the cube in Gray code, x lowest, so every face neighbour is one link away (step
7e's grids, with a third axis). z = 0 is the sun plane at the top; the last layer is the floor.

- **Light** (0 .. 255) falls one voxel per tick. The sun plane gets 255. Every other voxel gets the
  light of the voxel above it, of which an empty voxel passes on l − l/8 and one with a cell in it
  l/2, so shade forms under the cells.
- **Minerals** (0 .. 4095: the 8 bits a cell sees and 4 more below the point) diffuse up from the
  floor. The floor gets 4095 and the sun plane 0. In between a voxel gets (2m + its 6 neighbours) /
  8, rounded down, less what a cell eats there, plus what a dying cell gives back. With whole
  numbers the rounding stopped the diffusion about 8 layers above the floor for good (a fixed
  point, reached after 84 ticks); with 4 bits below the point the minerals reach the whole depth.

## Cells (step 11b)

- **Genome:** 32 NAND gates, each with two 6-bit input addresses, 384 bits. An address picks one of
  32 senses or one of the 32 gate outputs. The gates run in order, so a gate that reads itself or a
  later gate gets last tick's value: memory and oscillators come from wiring alone.
- **Senses:** a constant 1, three random bits, which of the 6 neighbours hold a cell, 2 signal bits
  from each neighbour, the top 3 bits of the cell's energy, of the light and of the minerals where
  it is, and "I was just born".
- **Actions:** gates 26 and 27 are the signals the neighbours see; gate 25 gives energy, gate 28
  asks to divide, gates 29–31 say in which direction, for both.
- **Division:** a cell asks when it is free and has energy 32 or more. An empty voxel grants one
  of the cells asking for it, the lowest direction first, and the cell learns it in the same tick.
  For the next 32 ticks both are busy and do not run their gates: every tick the parent's genome
  turns by one gate and the new cell takes that gate in at its end, so after 32 ticks it holds the
  parent's genome, each gate flipped in one bit with probability 1/64 on the way. The new cell gets
  half the parent's energy, and dies if its parent does.
- **Energy:** every tick a cell gains light/32 + minerals/512 (0 .. 7 each, and eats those
  minerals), at most 255, and pays 1. It dies with nothing left, or by chance (1/2048 per tick),
  and gives back 512 minerals.
- **Dice:** every voxel has its own xorshift32 generator, so the whole world is deterministic and
  every run can be replayed bit for bit.

One tick is about 55,000 NAND/STO instructions per voxel; two thirds of it are the gates' 64-way
input selects.

## Bodies (step 11c)

A cell can't be bigger than a voxel: a voxel is one machine, and every machine runs the same
program on the same fixed memory. But an organism can be many voxels. Division already puts the
same genome into the neighbouring voxel, and the signals let cells tell kin from strangers. What
makes a cluster of related cells a body is sharing energy:

- **Giving:** a free cell with gate 25 on offers an eighth of its energy (what it had at the start
  of the tick) to the cell in its direction, if there is one. The receiver gets it this tick, as a
  gain; the giver loses it. What does not fit under 255 is lost. Nothing is given to an empty
  voxel.
- **The tick, in order:** the gates run first, then every cell asks and offers, then the empty
  voxels grant, all in one tick. In 11b the grants used the previous tick's request, so a cell that
  saw a gap and turned to it was always a tick behind a cell that had pointed there blindly.

Why this and not a cost for asking: the war for space is already there. What kept it from rewarding
the senses was the order of the tick, and that nothing could be done with surplus energy. The world
has light at the top and minerals at the floor with 24 empty layers between; a body that spans them
is the first thing that can live there. And sharing creates something to cheat on.

## What it does so far

From a soup of 10 % random cells the world fills the sunlit top layers and the mineral-rich floor;
the twilight between them stays empty. Selection is visible at once: the share of free cells that
ask to divide rises from about 26 % in the soup to about 90 %.

With the tick in the right order (11c) the common genomes read their senses: they turn towards a
free neighbour, and give on conditions. But the habitable zone is full, so the only vacancies are
the few deaths per tick, and the empty voxels a cell can see are the dark under the canopy, where
a child starves. Space is the limit, not energy, which sits near the cap. An eighth of the cells
give energy away and clonal bodies of up to 13 cells form, but no body feeds a cell in the dark
yet. The numbers are in ROADMAP step 11c.

## Steps

| | | |
|---|---|---|
| 11a | the world: 3-D torus, light and minerals, checked against plain C, timed | ✅ |
| 11b | cells: genome, senses, division with mutation, energy, death; evolution from a random soup | ✅ |
| 11c | bodies: energy given from cell to cell, and asking, granting and being born in one tick | ✅ |
| 11d | `alpha gamma --serve`: a page that draws the cube live; click a cell to see its circuit | ⬜ |
| 11e | the twin world across the 16th dimension: butterfly and control modes | ⬜ |
| 11f | AVX-512 (5c), the world across the cluster (7g) | ⬜ |
