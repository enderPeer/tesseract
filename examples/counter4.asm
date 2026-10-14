; 4-bit counter: q3..q0 goes up by one every pass.
; Sequential logic without jumps: the cells keep their value between passes.
; The high bits are updated first so each update still sees the old low bits.
;   alpha run examples\counter4.asm --ticks 16
.include "../lib/std.inc"
.out q3 q2 q1 q0

    LD   q0
    AND  q1
    AND  q2
    XOR  q3
    STO  q3         ; q3 ^= q0 & q1 & q2
    LD   q0
    AND  q1
    XOR  q2
    STO  q2         ; q2 ^= q0 & q1
    LD   q0
    XOR  q1
    STO  q1         ; q1 ^= q0
    LDC  q0
    STO  q0         ; q0 = !q0
