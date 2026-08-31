/*
 * Freestanding shim <stdio.h> — visible ONLY when compiling the vendored
 * mbedtls/TF-PSA-Crypto (Makefile MB_CFLAGS, -nostdinc).
 *
 * mbedtls 4.x debug/error/x509 sources include <stdio.h> for declarations
 * while routing all actual output through MBEDTLS_PLATFORM_*_MACRO
 * (mb_printf / mb_snprintf -> serial console) and the tlsglue.c
 * vsnprintf bridge. Declarations only — nothing here links against a
 * libc. <time.h> is pulled in (like the hosted headers do transitively)
 * so x509.c's `struct tm` is complete under MBEDTLS_PLATFORM_GMTIME_R_ALT.
 */
#ifndef _MBKERN_SHIM_STDIO_H
#define _MBKERN_SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <time.h>

typedef struct _mbkern_shim_FILE FILE;

extern FILE* stdout;
extern FILE* stderr;

int ferror(FILE* stream);

int vsnprintf(char* s, size_t n, const char* fmt, va_list ap);
int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);

#endif /* _MBKERN_SHIM_STDIO_H */
