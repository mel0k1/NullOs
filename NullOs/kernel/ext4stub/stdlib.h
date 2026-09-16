/*
 * Freestanding stub <stdlib.h> — visible ONLY when compiling the
 * vendored lwext4 (kernel/ext4/*.c, see the Makefile rule).
 *
 * lwext4 allocates its block cache and bookkeeping through the plain
 * C allocation names; ext4glue.c routes them to the kernel heap
 * (dlmalloc-backed kmalloc/kfree), which is the very reason the
 * allocator migration had to land first.
 */
#ifndef _EXT4_STUB_STDLIB_H
#define _EXT4_STUB_STDLIB_H

#include <stddef.h>

void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void  free(void*);
void  qsort(void*, size_t, size_t, int (*)(const void*, const void*));
void  abort(void) __attribute__((noreturn));

#endif /* _EXT4_STUB_STDLIB_H */
