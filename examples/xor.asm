; XOR written directly in the two machine instructions: out = a ^ b.
; 8 instructions, no macros: the fewest possible. alpha super "out{a,b} = a ^ b" tried every
; shorter program (the textbook four-NAND XOR takes 13 here).   alpha run examples\xor.asm --table
.in  a b
.out out

    NAND ZERO       ; RR = 1
    NAND a          ; RR = !a
    NAND b          ; RR = !(!a & b) = a | !b
    STO  p
    NAND b          ; RR = !(p & b) = !(a & b)
    NAND a          ; RR = !(!(a & b) & a) = !a | b
    NAND p          ; RR = !((!a | b) & (a | !b)) = a ^ b
    STO  out
