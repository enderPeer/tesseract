; The cube as a ring (lib/cube.inc, step 7e): the elementary cellular automaton Rule 110 on a ring
; of 512 cells, one cell per machine, one step per pass. Ring position i is machine i ^ (i >> 1),
; so both neighbours are one link away. Rule 110 is Turing-complete.
;   new = (x | right) & !(left & x & right)
;   alpha check examples\rule110.asm --engine jit   (every machine against the reference cube)
;   alpha cube ring                                 (against plain C, and timed)
.include "../lib/std.inc"
.include "../lib/cube.inc"
.in  x
.out x

    RING_MASKS
    RING_PREV x left
    RING_NEXT x right
    LD   x
    AND  left
    AND  right
    STO  all3
    LD   x
    OR   right
    ANDC all3
    STO  x
