; The cube: every machine with x = 1 learns its place among them (p = how many machines below it
; have x = 1) and every machine learns how many there are (t). This is what packs the hits of a
; search into a list. 9 steps across the cube, 10-bit counts.
;   alpha run examples\cube_rank.asm --set x=1 --lane 300      (all 512 have x = 1: p = 300)
.include "../lib/std.inc"
.include "../lib/cube.inc"
.in  x
.out p0 p1 p2 p3 p4 p5 p6 p7 p8 p9 t0 t1 t2 t3 t4 t5 t6 t7 t8 t9

    PCOUNT x p t
