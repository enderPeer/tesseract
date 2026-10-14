; 1-bit full adder: s = a + b + c, carry out goes back into c.
; FA (lib/std.inc) is 19 machine instructions, the fewest possible (alpha super).
.include "../lib/std.inc"
.in  a b c
.out s c

    FA a b c s
