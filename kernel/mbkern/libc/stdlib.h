/*
 * Freestanding shim <stdlib.h> — visible ONLY when compiling the vendored
 * mbedtls/TF-PSA-Crypto (Makefile MB_CFLAGS, -nostdinc).
 *
 * Some upstream headers (crypto/core/alignment.h) include <stdlib.h>
 * without using anything from it; with -nostdinc the include itself must
 * resolve. Declarations only — memory comes from the kernel heap via the
 * MBEDTLS_PLATFORM_CALLOC/FREE_MACRO (mb_kcalloc/mb_kfree) and there is
 * no process exit in ring 0.
 */
#ifndef _MBKERN_SHIM_STDLIB_H
#define _MBKERN_SHIM_STDLIB_H

#include <stddef.h>

void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));

#endif /* _MBKERN_SHIM_STDLIB_H */
