/*
 * Freestanding stub for <stdio.h> — used ONLY when compiling the
 * vendored dlmalloc (kernel/dlmalloc.c, see Makefile rule).
 *
 * dlmalloc 2.7.2 includes <stdio.h> unconditionally and uses
 * fprintf(stderr, ...) exclusively inside dlmalloc_stats(). The
 * kernel provides a real fprintf in dlcompat.c that routes to the
 * serial console, so stats are readable from the QEMU serial log.
 */
#ifndef _DL_STUB_STDIO_H
#define _DL_STUB_STDIO_H

typedef struct _dl_stub_FILE FILE;
extern FILE* stderr;

int fprintf(FILE* stream, const char* fmt, ...);

#endif /* _DL_STUB_STDIO_H */
