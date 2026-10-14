; 8-bit ripple-carry adder: s7..s0 = a7..a0 + b7..b0 + c, carry out goes back into c.
; Eight full adders = 152 machine instructions per pass (19 each, the shortest possible).
.include "../lib/std.inc"
.in  a7 a6 a5 a4 a3 a2 a1 a0  b7 b6 b5 b4 b3 b2 b1 b0  c
.out s7 s6 s5 s4 s3 s2 s1 s0  c

    FA a0 b0 c s0
    FA a1 b1 c s1
    FA a2 b2 c s2
    FA a3 b3 c s3
    FA a4 b4 c s4
    FA a5 b5 c s5
    FA a6 b6 c s6
    FA a7 b7 c s7
