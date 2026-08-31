/*
 * Freestanding shim <string.h> — visible ONLY when compiling the vendored
 * mbedtls/TF-PSA-Crypto (Makefile MB_CFLAGS, -nostdinc).
 *
 * Declarations for the string/memory primitives the kernel exports
 * (ext4glue.c: memset/memcpy/memmove/memcmp/strlen/strcmp/strncmp/
 * strcpy/strncpy over the k* implementations; tlsglue.c: strchr/strstr).
 * Any symbol actually referenced by the library resolves at kernel link
 * time; this header only satisfies -nostdinc compilation.
 */
#ifndef _MBKERN_SHIM_STRING_H
#define _MBKERN_SHIM_STRING_H

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
char*  strchr(const char*, int);
char*  strstr(const char*, const char*);

#endif /* _MBKERN_SHIM_STRING_H */
