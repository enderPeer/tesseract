; The cube: the largest of 512 numbers, one per machine, ends up in every machine (MIN8 works the
; same way). 9 rounds of compare-and-keep across the cube.
;   alpha check examples\cube_max.asm --engine jit
.include "../lib/std.inc"
.include "../lib/cube.inc"
.in  v0 v1 v2 v3 v4 v5 v6 v7
.out m0 m1 m2 m3 m4 m5 m6 m7

    MAX8 v m
