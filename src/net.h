/* Logic netlists in the ISCAS .bench format:

       # comment
       INPUT(a)
       OUTPUT(z)
       z = NAND(a, b)        AND NAND OR NOR XOR XNOR NOT BUF(F), any number of inputs
       q = DFF(d)            a flip-flop: q holds d's value from the previous pass

   Gates may come in any order; they are sorted so every gate runs after the gates it reads. */
#ifndef ALPHA_NET_H
#define ALPHA_NET_H

#include "isa.h"

int net_parse(const char *text, const char *name, Program *out, char *err, size_t errlen);
int net_load(const char *path, Program *out, char *err, size_t errlen);

/* A .bench netlist or an assembly file, by the file name's extension. */
int load_program(const char *path, Program *out, char *err, size_t errlen);

#endif
