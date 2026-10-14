/* Assembler: source text -> Program. The language is described in docs/ISA.md. */
#ifndef ALPHA_ASM_H
#define ALPHA_ASM_H

#include "isa.h"

/* Both return 1 on success. On failure they return 0, leave *out empty and write
   "file:line: message" to err. */
int asm_file(const char *path, Program *out, char *err, size_t errlen);

/* Text read from a file, as the assembler wants it: UTF-16 (Windows PowerShell 5's > writes it)
   becomes plain 8-bit text, a UTF-8 byte order mark goes. buf[len] must be 0; returns the length. */
size_t text_normalize(char *buf, size_t len);

/* .include paths are resolved relative to base_dir; name is used in error messages. */
int asm_string(const char *src, const char *name, const char *base_dir, Program *out, char *err,
               size_t errlen);

#endif
