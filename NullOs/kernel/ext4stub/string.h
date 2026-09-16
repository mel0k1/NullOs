/*
 * Freestanding stub <string.h> — visible ONLY when compiling the
 * vendored lwext4 (kernel/ext4/*.c, see the Makefile rule). The real
 * implementations are thin wrappers over the kernel's k* string
 * functions, provided in kernel/ext4glue.c.
 */
#ifndef _EXT4_STUB_STRING_H
#define _EXT4_STUB_STRING_H

#include <stddef.h>

void*  memset(void*, int, size_t);
void*  memcpy(void*, const void*, size_t);
void*  memmove(void*, const void*, size_t);
int    memcmp(const void*, const void*, size_t);
size_t strlen(const char*);
int    strcmp(const char*, const char*);
int    strncmp(const char*, const char*, size_t);
char*  strcpy(char*, const char*);
char*  strncpy(char*, const char*, size_t);

#endif /* _EXT4_STUB_STRING_H */
