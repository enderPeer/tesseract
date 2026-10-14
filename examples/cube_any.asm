; The cube (docs/ISA.md): does any of the 512 machines have x = 1? Every machine finds out in 9
; steps. After step d, machine k holds the OR of x over all machines whose number differs from k
; only in bits 0..d, so after step 8 every machine holds the OR over all 512.
;   alpha run examples\cube_any.asm --set x=1        (x = 1 everywhere)
;   alpha check examples\cube_any.asm --engine jit   (random x in every machine)
.include "../lib/std.inc"
.in  x
.out any

.link n0 = r @ 0        ; r of machine k XOR 1
.link n1 = r @ 1        ; ... k XOR 2
.link n2 = r @ 2
.link n3 = r @ 3
.link n4 = r @ 4
.link n5 = r @ 5
.link n6 = r @ 6
.link n7 = r @ 7
.link n8 = r @ 8        ; ... k XOR 256

    MOV  r x
    LD   r
    OR   n0             ; every machine reads its neighbour's r before anyone writes r again
    STO  r
    LD   r
    OR   n1
    STO  r
    LD   r
    OR   n2
    STO  r
    LD   r
    OR   n3
    STO  r
    LD   r
    OR   n4
    STO  r
    LD   r
    OR   n5
    STO  r
    LD   r
    OR   n6
    STO  r
    LD   r
    OR   n7
    STO  r
    LD   r
    OR   n8
    STO  any
