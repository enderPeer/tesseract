; Collectives for the cube (docs/ISA.md): all 512 machines take part, every machine gets the
; result, one dimension per step, 9 steps. Include lib/std.inc first.
;
; Multi-bit values are cells name0, name1, ... (bit 0 first) and are passed by their name: a
; macro writes v#3 for cell v3. The comment on each macro gives the cost in machine instructions.
;
; The one-bit steps (MUX, OR_DIM, AND_DIM, XOR_DIM, BCAST_DIM, MOBIUS_DIM, GE_BIT, the RING_SEL
; term) are the shortest NAND/STO programs that do their job: alpha super tried every shorter one
; (ROADMAP step 10). So is PXOR_DIM, two of them: 20, and no program of 19 does its job.

; ---- helpers ------------------------------------------------------------------------------

.macro MUX r s a b      ; r = s ? a : b; r may be a or b                          9
    NAND ZERO
    NAND s              ; !s
    NAND ONE            ; s
    NAND a              ; t = !(s & a)
    STO  @t
    NAND s              ; !s | a
    NAND b              ; !((!s | a) & b)
    NAND @t             ; s & a | (!s | a) & b = s ? a : b
    STO  r
.endm

.macro ADD10 a b        ; a += b on 10 bits (a0..a9, b0..b9); b may be a local    193
    CLR
    STO  @c
    FA   a#0 b#0 @c a#0
    FA   a#1 b#1 @c a#1
    FA   a#2 b#2 @c a#2
    FA   a#3 b#3 @c a#3
    FA   a#4 b#4 @c a#4
    FA   a#5 b#5 @c a#5
    FA   a#6 b#6 @c a#6
    FA   a#7 b#7 @c a#7
    FA   a#8 b#8 @c a#8
    FA   a#9 b#9 @c a#9
.endm

; ---- one bit over all machines ------------------------------------------------------------

.macro OR_DIM r d       ; r |= r of the machine across dimension d                6
.link @n = r @ d
    NAND ZERO
    NAND r              ; !r
    STO  @t
    NAND @n             ; r | !n
    NAND @t             ; r | n
    STO  r
.endm

.macro AND_DIM r d      ; r &= r across d                                          6
.link @n = r @ d
    LD   r
    AND  @n
    STO  r
.endm

.macro XOR_DIM r d      ; r ^= r across d                                          8
.link @n = r @ d
    NAND ZERO
    NAND r              ; !r
    NAND @n             ; p = r | !n
    STO  @p
    NAND @n             ; !(r & n)
    NAND r              ; !r | n
    NAND @p             ; r ^ n
    STO  r
.endm

.macro ALL_OR x r       ; r = OR of x over all 512 machines                       58
    MOV  r x
    OR_DIM r 0
    OR_DIM r 1
    OR_DIM r 2
    OR_DIM r 3
    OR_DIM r 4
    OR_DIM r 5
    OR_DIM r 6
    OR_DIM r 7
    OR_DIM r 8
.endm

.macro ALL_AND x r      ; r = AND of x over all machines                          58
    MOV  r x
    AND_DIM r 0
    AND_DIM r 1
    AND_DIM r 2
    AND_DIM r 3
    AND_DIM r 4
    AND_DIM r 5
    AND_DIM r 6
    AND_DIM r 7
    AND_DIM r 8
.endm

.macro ALL_XOR x r      ; r = XOR of x over all machines (parity)                  76
    MOV  r x
    XOR_DIM r 0
    XOR_DIM r 1
    XOR_DIM r 2
    XOR_DIM r 3
    XOR_DIM r 4
    XOR_DIM r 5
    XOR_DIM r 6
    XOR_DIM r 7
    XOR_DIM r 8
.endm

; ---- broadcast ----------------------------------------------------------------------------

.macro BCAST0_DIM r d   ; machines with bit d set take r from across d              9
.link @n = r @ d
    MUX  r ID#d @n r
.endm

.macro BCAST0 x r       ; r = x of machine 0, in every machine                     85
    MOV  r x
    BCAST0_DIM r 0
    BCAST0_DIM r 1
    BCAST0_DIM r 2
    BCAST0_DIM r 3
    BCAST0_DIM r 4
    BCAST0_DIM r 5
    BCAST0_DIM r 6
    BCAST0_DIM r 7
    BCAST0_DIM r 8
.endm

.macro BCAST_DIM r s d  ; machines whose bit d differs from s's take r across d     14
.link @n = r @ d
    NAND ZERO
    NAND ID#d           ; !id
    NAND s#d            ; p = id | !s
    STO  @p
    NAND s#d            ; !(id & s)
    NAND ID#d           ; !id | s
    NAND @p             ; far = id ^ s
    STO  @far
    NAND @n             ; t = !(far & n)
    STO  @t
    NAND @far           ; !far | n
    NAND r              ; !((!far | n) & r)
    NAND @t             ; far ? n : r
    STO  r
.endm

.macro BCAST x r s      ; r = x of machine s (s0..s8, the same in every machine)  130
    MOV  r x
    BCAST_DIM r s 0
    BCAST_DIM r s 1
    BCAST_DIM r s 2
    BCAST_DIM r s 3
    BCAST_DIM r s 4
    BCAST_DIM r s 5
    BCAST_DIM r s 6
    BCAST_DIM r s 7
    BCAST_DIM r s 8
.endm

; ---- prefixes: machine k combines machines 0..k ---------------------------------------------
; t holds the total of the machine's subcube so far; a machine with bit d set also adds the
; lower half's total, which is the neighbour's t across d.

.macro PXOR_DIM p t d   ; one step of PXOR                                         20
.link @n = t @ d
    NAND ZERO           ; p ^= id & n, as in MOBIUS_DIM
    NAND p
    NAND ID#d
    NAND ONE
    NAND @n
    STO  @q
    NAND ID#d
    NAND ONE
    NAND @n
    NAND p
    NAND @q
    STO  p
    NAND ZERO           ; t ^= n, as in XOR_DIM
    NAND t
    NAND @n
    STO  @u
    NAND @n
    NAND t
    NAND @u
    STO  t
.endm

.macro PXOR x p t       ; p = XOR of x over machines 0..k, t = XOR over all       188
    MOV  p x
    MOV  t x
    PXOR_DIM p t 0
    PXOR_DIM p t 1
    PXOR_DIM p t 2
    PXOR_DIM p t 3
    PXOR_DIM p t 4
    PXOR_DIM p t 5
    PXOR_DIM p t 6
    PXOR_DIM p t 7
    PXOR_DIM p t 8
.endm

.macro PCOUNT_DIM p t d ; one step of PCOUNT                                        446
.link @n0 = t#0 @ d
.link @n1 = t#1 @ d
.link @n2 = t#2 @ d
.link @n3 = t#3 @ d
.link @n4 = t#4 @ d
.link @n5 = t#5 @ d
.link @n6 = t#6 @ d
.link @n7 = t#7 @ d
.link @n8 = t#8 @ d
.link @n9 = t#9 @ d
    LD   ID#d
    AND  @n0
    STO  @m0
    LD   ID#d
    AND  @n1
    STO  @m1
    LD   ID#d
    AND  @n2
    STO  @m2
    LD   ID#d
    AND  @n3
    STO  @m3
    LD   ID#d
    AND  @n4
    STO  @m4
    LD   ID#d
    AND  @n5
    STO  @m5
    LD   ID#d
    AND  @n6
    STO  @m6
    LD   ID#d
    AND  @n7
    STO  @m7
    LD   ID#d
    AND  @n8
    STO  @m8
    LD   ID#d
    AND  @n9
    STO  @m9
    ADD10 p @m          ; p += the lower half's total, if bit d is set
    ADD10 t @n          ; t += the neighbour's total
.endm

; p = how many machines below k have x = 1 (p0..p9), t = how many have it in all (t0..t9).
; With p, the machines that found something know their place in a packed list.      5,659
.macro PCOUNT x p t
    MOV  t#0 x
    CLR
    STO  t#1
    STO  t#2
    STO  t#3
    STO  t#4
    STO  t#5
    STO  t#6
    STO  t#7
    STO  t#8
    STO  t#9
    STO  p#0
    STO  p#1
    STO  p#2
    STO  p#3
    STO  p#4
    STO  p#5
    STO  p#6
    STO  p#7
    STO  p#8
    STO  p#9
    PCOUNT_DIM p t 0
    PCOUNT_DIM p t 1
    PCOUNT_DIM p t 2
    PCOUNT_DIM p t 3
    PCOUNT_DIM p t 4
    PCOUNT_DIM p t 5
    PCOUNT_DIM p t 6
    PCOUNT_DIM p t 7
    PCOUNT_DIM p t 8
.endm

; ---- minimum / maximum of 8-bit numbers ---------------------------------------------------

; g = (a >= b) for 8-bit a, b: the carry out of a + !b + 1                         250
.macro GE_BIT g a b     ; one bit up: g = a > b, or a = b and g                     9
    NAND ZERO           ; the carry of a + !b + g, i.e. a & !b | (a | !b) & g
    NAND a
    NAND b
    NAND g
    STO  @t
    NAND b
    NAND a
    NAND @t
    STO  g
.endm

.macro GE8 g a b        ; g = a >= b, 8 bits                                         74
    SET
    STO  g
    GE_BIT g a#0 b#0
    GE_BIT g a#1 b#1
    GE_BIT g a#2 b#2
    GE_BIT g a#3 b#3
    GE_BIT g a#4 b#4
    GE_BIT g a#5 b#5
    GE_BIT g a#6 b#6
    GE_BIT g a#7 b#7
.endm

.macro PICK8 r s a b    ; r = s ? a : b, 8 bits                                      72
    MUX  r#0 s a#0 b#0
    MUX  r#1 s a#1 b#1
    MUX  r#2 s a#2 b#2
    MUX  r#3 s a#3 b#3
    MUX  r#4 s a#4 b#4
    MUX  r#5 s a#5 b#5
    MUX  r#6 s a#6 b#6
    MUX  r#7 s a#7 b#7
.endm

.macro MOV8 r a         ; r = a, 8 bits                                             32
    MOV  r#0 a#0
    MOV  r#1 a#1
    MOV  r#2 a#2
    MOV  r#3 a#3
    MOV  r#4 a#4
    MOV  r#5 a#5
    MOV  r#6 a#6
    MOV  r#7 a#7
.endm

.macro MAX8_DIM m d     ; m = max(m, m across d)                                    146
.link @n0 = m#0 @ d
.link @n1 = m#1 @ d
.link @n2 = m#2 @ d
.link @n3 = m#3 @ d
.link @n4 = m#4 @ d
.link @n5 = m#5 @ d
.link @n6 = m#6 @ d
.link @n7 = m#7 @ d
    GE8  @g m @n
    PICK8 m @g m @n
.endm

.macro MIN8_DIM m d     ; m = min(m, m across d)                                    146
.link @n0 = m#0 @ d
.link @n1 = m#1 @ d
.link @n2 = m#2 @ d
.link @n3 = m#3 @ d
.link @n4 = m#4 @ d
.link @n5 = m#5 @ d
.link @n6 = m#6 @ d
.link @n7 = m#7 @ d
    GE8  @g m @n
    PICK8 m @g @n m
.endm

.macro MAX8 v m         ; m = the largest v (v0..v7) of all machines              1,346
    MOV8 m v
    MAX8_DIM m 0
    MAX8_DIM m 1
    MAX8_DIM m 2
    MAX8_DIM m 3
    MAX8_DIM m 4
    MAX8_DIM m 5
    MAX8_DIM m 6
    MAX8_DIM m 7
    MAX8_DIM m 8
.endm

.macro MIN8 v m         ; m = the smallest v of all machines                      1,346
    MOV8 m v
    MIN8_DIM m 0
    MIN8_DIM m 1
    MIN8_DIM m 2
    MIN8_DIM m 3
    MIN8_DIM m 4
    MIN8_DIM m 5
    MIN8_DIM m 6
    MIN8_DIM m 7
    MIN8_DIM m 8
.endm

; ---- sorting and moving data (step 7c) ----------------------------------------------------

; Compare-exchange with machine k XOR 2^j: the machine with bit j = 0 keeps the smaller value,
; the other the larger, or the other way round if dir = 1.
.macro CMPX8 v j dir
.link @n0 = v#0 @ j
.link @n1 = v#1 @ j
.link @n2 = v#2 @ j
.link @n3 = v#3 @ j
.link @n4 = v#4 @ j
.link @n5 = v#5 @ j
.link @n6 = v#6 @ j
.link @n7 = v#7 @ j
    GE8   @g v @n          ; g = v >= neighbour's v
    LD    ID#j
    XOR   dir              ; this machine wants the larger value
    XNOR  @g
    STO   @keep            ; ... and already has it, or wants the smaller and has that
    PICK8 v @keep v @n
.endm

; The same for a dimension 9 and up of a bigger cube (step 7f), where machines in other groups
; are read: the neighbour's value is copied first, so the compiled pass needs only two cuts for
; this round (one before v is written, one before it is read across again), not one per bit.
.macro CMPX8_FAR v j dir
.link @n0 = v#0 @ j
.link @n1 = v#1 @ j
.link @n2 = v#2 @ j
.link @n3 = v#3 @ j
.link @n4 = v#4 @ j
.link @n5 = v#5 @ j
.link @n6 = v#6 @ j
.link @n7 = v#7 @ j
    MOV8  @c @n
    GE8   @g v @c
    LD    ID#j
    XOR   dir
    XNOR  @g
    STO   @keep
    PICK8 v @keep v @c
.endm

; Bitonic sort: afterwards machine k holds the k-th smallest of the 512 values. 45 rounds, each
; across one dimension: for blocks of 2, 4, .. 512 machines, sort every block up or down (by the
; block's next bit), then merge.
.macro SORT8 v
    CMPX8 v 0 ID1
    CMPX8 v 1 ID2
    CMPX8 v 0 ID2
    CMPX8 v 2 ID3
    CMPX8 v 1 ID3
    CMPX8 v 0 ID3
    CMPX8 v 3 ID4
    CMPX8 v 2 ID4
    CMPX8 v 1 ID4
    CMPX8 v 0 ID4
    CMPX8 v 4 ID5
    CMPX8 v 3 ID5
    CMPX8 v 2 ID5
    CMPX8 v 1 ID5
    CMPX8 v 0 ID5
    CMPX8 v 5 ID6
    CMPX8 v 4 ID6
    CMPX8 v 3 ID6
    CMPX8 v 2 ID6
    CMPX8 v 1 ID6
    CMPX8 v 0 ID6
    CMPX8 v 6 ID7
    CMPX8 v 5 ID7
    CMPX8 v 4 ID7
    CMPX8 v 3 ID7
    CMPX8 v 2 ID7
    CMPX8 v 1 ID7
    CMPX8 v 0 ID7
    CMPX8 v 7 ID8
    CMPX8 v 6 ID8
    CMPX8 v 5 ID8
    CMPX8 v 4 ID8
    CMPX8 v 3 ID8
    CMPX8 v 2 ID8
    CMPX8 v 1 ID8
    CMPX8 v 0 ID8
    CMPX8 v 8 ZERO
    CMPX8 v 7 ZERO
    CMPX8 v 6 ZERO
    CMPX8 v 5 ZERO
    CMPX8 v 4 ZERO
    CMPX8 v 3 ZERO
    CMPX8 v 2 ZERO
    CMPX8 v 1 ZERO
    CMPX8 v 0 ZERO
.endm

; One step of PACK8: a value moves across dimension d if bit d of its place differs from bit d
; of its target; packed values never meet, so nothing is lost.
.macro PACK8_DIM v h r d
.link @nmv = @mv @ d
.link @nv0 = v#0 @ d
.link @nv1 = v#1 @ d
.link @nv2 = v#2 @ d
.link @nv3 = v#3 @ d
.link @nv4 = v#4 @ d
.link @nv5 = v#5 @ d
.link @nv6 = v#6 @ d
.link @nv7 = v#7 @ d
.link @nr0 = r#0 @ d
.link @nr1 = r#1 @ d
.link @nr2 = r#2 @ d
.link @nr3 = r#3 @ d
.link @nr4 = r#4 @ d
.link @nr5 = r#5 @ d
.link @nr6 = r#6 @ d
.link @nr7 = r#7 @ d
.link @nr8 = r#8 @ d
    LD    ID#d
    XOR   r#d
    AND   h
    STO   @mv              ; this machine's value leaves across d
    LD    h
    ANDC  @mv
    STO   @stay            ; it has a value that stays
    OR    @nmv
    STO   @full            ; it has a value after this step
    PICK8 v @stay v @nv
    MUX   r#0 @stay r#0 @nr0
    MUX   r#1 @stay r#1 @nr1
    MUX   r#2 @stay r#2 @nr2
    MUX   r#3 @stay r#3 @nr3
    MUX   r#4 @stay r#4 @nr4
    MUX   r#5 @stay r#5 @nr5
    MUX   r#6 @stay r#6 @nr6
    MUX   r#7 @stay r#7 @nr7
    MUX   r#8 @stay r#8 @nr8
    MOV   h @full
.endm

; Packs the values v of the machines with h = 1 to the front: machine j ends up with the value of
; the j-th such machine and h = 1, the machines after them with h = 0. r is each machine's place,
; from PCOUNT h r t.
.macro PACK8 v h r
    PACK8_DIM v h r 0
    PACK8_DIM v h r 1
    PACK8_DIM v h r 2
    PACK8_DIM v h r 3
    PACK8_DIM v h r 4
    PACK8_DIM v h r 5
    PACK8_DIM v h r 6
    PACK8_DIM v h r 7
    PACK8_DIM v h r 8
.endm

.macro SWAP8_DIM v c d  ; v = c ? v across d : v                                      72
.link @n0 = v#0 @ d
.link @n1 = v#1 @ d
.link @n2 = v#2 @ d
.link @n3 = v#3 @ d
.link @n4 = v#4 @ d
.link @n5 = v#5 @ d
.link @n6 = v#6 @ d
.link @n7 = v#7 @ d
    PICK8 v c @n v
.endm

; Any permutation of the 512 values, through a Beneš network: 17 rounds across dimensions
; 0 1 .. 8 .. 1 0. c0..c16 are the switch settings of each machine for each round; alpha computes
; them for a given permutation (src/benes.c). Both machines of a pair get the same setting.
.macro PERM8 v c
    SWAP8_DIM v c#0 0
    SWAP8_DIM v c#1 1
    SWAP8_DIM v c#2 2
    SWAP8_DIM v c#3 3
    SWAP8_DIM v c#4 4
    SWAP8_DIM v c#5 5
    SWAP8_DIM v c#6 6
    SWAP8_DIM v c#7 7
    SWAP8_DIM v c#8 8
    SWAP8_DIM v c#9 7
    SWAP8_DIM v c#10 6
    SWAP8_DIM v c#11 5
    SWAP8_DIM v c#12 4
    SWAP8_DIM v c#13 3
    SWAP8_DIM v c#14 2
    SWAP8_DIM v c#15 1
    SWAP8_DIM v c#16 0
.endm

; ---- transforms (step 7d) -----------------------------------------------------------------
;
; The transforms over integers (Walsh-Hadamard, XOR convolution, the NTT) need wide adders and
; multipliers and are generated instead: alpha gen wht | xorconv | polymul (src/gen.c).

.macro MOBIUS_DIM f d   ; machines with bit d set XOR in f of the machine across d        12
.link @n = f @ d
    NAND ZERO           ; f ^= id & n
    NAND f              ; !f
    NAND ID#d           ; f | !id
    NAND ONE            ; !f & id
    NAND @n             ; q = !(!f & id & n)
    STO  @q
    NAND ID#d
    NAND ONE
    NAND @n
    NAND f
    NAND @q
    STO  f
.endm

; Möbius transform over GF(2): with f as a truth table (machine k holds f(k)), f becomes the
; algebraic normal form, machine k holding the coefficient of the monomial whose variables are
; the bits of k: the XOR of f(j) over every j inside k. Doing it twice gives f back.        144
.macro MOBIUS f
    MOBIUS_DIM f 0
    MOBIUS_DIM f 1
    MOBIUS_DIM f 2
    MOBIUS_DIM f 3
    MOBIUS_DIM f 4
    MOBIUS_DIM f 5
    MOBIUS_DIM f 6
    MOBIUS_DIM f 7
    MOBIUS_DIM f 8
.endm

; ---- grids: the machines as a ring (step 7e) ----------------------------------------------
;
; Ring position i is machine i ^ (i >> 1), the Gray code of i. Neighbouring positions differ in
; one bit, so every machine's two ring neighbours are one link away, but across a dimension that
; depends on the machine. A position is odd when its machine number has odd parity. From an even
; position the next one is across dimension 0 and the previous one across the dimension above
; the lowest 1 bit; from an odd position the other way round. Position 511 (machine 256) and
; position 0 (machine 0) meet across dimension 8.
;
; RING_MASKS works this out once: afterwards RN<d> = 1 only for the dimension d towards the next
; position, RP<d> only for the one towards the previous position. RING_NEXT / RING_PREV then
; read a cell of the neighbour: all 9 links, keeping the one the mask picks.

.macro RING_MASK_DIM j k ; lowest 1 bit is j: one neighbour is across dimension k = j + 1    24
    LDC  RO             ; no 1 bit below j
    AND  ID#j
    STO  @l
    AND  RPAR
    STO  RN#k           ; odd position: the next one is across k
    LD   @l
    ANDC RPAR
    STO  RP#k           ; even position: the previous one
    LD   RO
    OR   ID#j
    STO  RO
.endm

.macro RING_MASKS       ; RN0..RN8, RP0..RP8 of this machine                              281
    LD   ID0
    XOR  ID1
    XOR  ID2
    XOR  ID3
    XOR  ID4
    XOR  ID5
    XOR  ID6
    XOR  ID7
    XOR  ID8
    STO  RPAR           ; the parity of the machine number: is the position odd?
    STOC RN0            ; even: the next position is across dimension 0
    STO  RP0            ; odd: the previous one is
    CLR
    STO  RO             ; RO: some bit below the one looked at is 1
    RING_MASK_DIM 0 1
    RING_MASK_DIM 1 2
    RING_MASK_DIM 2 3
    RING_MASK_DIM 3 4
    RING_MASK_DIM 4 5
    RING_MASK_DIM 5 6
    RING_MASK_DIM 6 7
    RING_MASK_DIM 7 8
    LDC  RO             ; machine 256 = position 511: the next one is position 0
    AND  ID8
    OR   RN8
    STO  RN8
    LD   RO             ; machine 0 = position 0: the previous one is position 511
    OR   ID8
    NOT
    OR   RP8
    STO  RP8
.endm

.macro RING_SEL x r m   ; r = x of the machine that masks m0..m8 point to; r may be x        60
.link @n0 = x @ 0
.link @n1 = x @ 1
.link @n2 = x @ 2
.link @n3 = x @ 3
.link @n4 = x @ 4
.link @n5 = x @ 5
.link @n6 = x @ 6
.link @n7 = x @ 7
.link @n8 = x @ 8
    LD   m#0
    NAND @n0
    STO  @t             ; t = AND over d so far of !(m_d & x across d)
    LD   m#1
    NAND @n1
    AND  @t
    STO  @t
    LD   m#2
    NAND @n2
    AND  @t
    STO  @t
    LD   m#3
    NAND @n3
    AND  @t
    STO  @t
    LD   m#4
    NAND @n4
    AND  @t
    STO  @t
    LD   m#5
    NAND @n5
    AND  @t
    STO  @t
    LD   m#6
    NAND @n6
    AND  @t
    STO  @t
    LD   m#7
    NAND @n7
    AND  @t
    STO  @t
    LD   m#8
    NAND @n8
    NAND @t             ; the OR of m_d & x across d: just the one m picks
    STO  r
.endm

.macro RING_NEXT x r    ; r = x of the next machine on the ring (after RING_MASKS)          60
    RING_SEL x r RN
.endm

.macro RING_PREV x r    ; r = x of the previous machine on the ring                           60
    RING_SEL x r RP
.endm
