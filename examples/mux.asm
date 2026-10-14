; "if" without jumps: out = sel ? x : y.
; Both sides are computed every pass; logic picks the one that counts.
.include "../lib/std.inc"
.in  sel x y
.out out

    LD   sel
    AND  x
    STO  t          ; t = sel & x
    LDC  sel
    AND  y          ; RR = !sel & y
    OR   t
    STO  out
