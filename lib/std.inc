; Standard macros for ISA v1 (NAND/STO).
; The comment on each .macro line gives its effect and its cost in machine instructions.
; @names are scratch cells: every call of a macro gets fresh ones.
;
; Every macro here is the shortest NAND/STO program that does its job: alpha super tried every
; shorter one (ROADMAP step 10). Shared cells, where allowed, are written after their last read.

.macro SET              ; RR = 1                                1
    NAND ZERO
.endm

.macro NOT              ; RR = !RR                              1
    NAND ONE
.endm

.macro CLR              ; RR = 0                                2
    NAND ZERO
    NAND ONE
.endm

.macro LD x             ; RR = x                                3
    NAND ZERO
    NAND x
    NAND ONE
.endm

.macro LDC x            ; RR = !x                               2
    NAND ZERO
    NAND x
.endm

.macro AND x            ; RR = RR & x                           2
    NAND x
    NAND ONE
.endm

.macro OR x             ; RR = RR | x                           4
    NAND ONE            ; !RR
    STO  @t
    NAND x              ; !(!RR & x) = RR | !x
    NAND @t             ; !((RR | !x) & !RR) = RR | x
.endm

.macro NOR x            ; RR = !(RR | x)                        5
    OR   x
    NOT
.endm

.macro ANDC x           ; RR = RR & !x                          4
    STO  @t
    NAND x              ; !(RR & x)
    NAND @t             ; !(!(RR & x) & RR) = !RR | x
    NAND ONE            ; RR & !x
.endm

.macro ORC x            ; RR = RR | !x                          2
    NAND ONE            ; !RR
    NAND x              ; !(!RR & x)
.endm

.macro XOR x            ; RR = RR ^ x                           7
    STO  @a
    NAND ONE            ; !a
    NAND x              ; p = !(!a & x) = a | !x
    STO  @p
    NAND x              ; !(p & x) = !(a & x)
    NAND @a             ; !(!(a & x) & a) = !a | x
    NAND @p             ; !((!a | x) & (a | !x)) = a ^ x
.endm

.macro XNOR x           ; RR = !(RR ^ x)                        8
    NOT
    XOR  x              ; !a ^ x
.endm

.macro STOC x           ; x = !RR, RR unchanged                 3
    NAND ONE
    STO  x
    NAND ONE
.endm

.macro MOV dst src      ; dst = src, RR = src                   4
    LD   src
    STO  dst
.endm

; Full adder: s = a ^ b ^ c, then c = carry out.                          19
; s may be the same cell as a or b.
.macro FA a b c s
    NAND ZERO
    NAND a              ; !a
    STO  @na
    NAND ONE
    NAND b              ; n = !(a & b)
    STO  @n
    NAND b
    NAND @na            ; a | b
    STO  @o
    NAND @n             ; !(a ^ b)
    NAND c              ; q = (a ^ b) | !c
    STO  @q
    NAND c              ; !((a ^ b) & c)
    NAND @n             ; carry = (a ^ b) & c | a & b
    STO  c
    NAND ONE            ; !carry
    NAND @o             ; carry | !(a | b)
    NAND @q             ; s = a ^ b ^ c
    STO  s
.endm
