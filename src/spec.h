/* Specifications for alpha verify and alpha search, e.g.

       c:s = a + b + c

   Left of '=': output vectors, joined (the first name gives the highest bits). Right: input vectors
   and numbers with ~ - * + - << >> & ^ | and parentheses, on 64-bit integers; the result is cut to
   the width of the left side. A vector is either the cell with exactly that name, or the cells
   name0, name1, ... (bit i is cell name<i>).

   Input assignments are numbers x: input k (in .in order) is bit n-1-k of x, n = number of inputs,
   so the first input is the highest bit. */
#ifndef ALPHA_SPEC_H
#define ALPHA_SPEC_H

#include "isa.h"

typedef struct Spec Spec;

/* inputs_allowed = 0: the right side must be a constant (for search). */
Spec    *spec_parse(const char *text, const Program *p, int inputs_allowed, char *err,
                    size_t errlen);
void     spec_free(Spec *s);
int      spec_width(const Spec *s);
uint32_t spec_out_cell(const Spec *s, int bit);  /* output cell of left-side bit (0 = lowest) */
uint64_t spec_value(const Spec *s, uint64_t x);  /* right side for assignment x */

/* Bits of vector `name` among cells list[0..n): idx[i] = position in list of bit i. Returns the
   width, 0 if no such vector, -1 if it has gaps (name0 and name2 but no name1) or over 64 bits. */
int vec_resolve(const Program *p, const uint32_t *list, size_t n, const char *name, int *idx);

/* Writes "a=0x1f b=0x3 c=1" for the vectors made of cells list[0..n), with bit values from val. */
void vec_format(const Program *p, const uint32_t *list, size_t n, const uint8_t *val, char *buf,
                size_t len);

#endif
