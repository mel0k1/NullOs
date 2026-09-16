/*
 * Freestanding shim <assert.h> — visible ONLY when compiling the vendored
 * mbedtls/TF-PSA-Crypto (Makefile MB_CFLAGS, -nostdinc).
 *
 * TF-PSA's common header includes <assert.h> and the mps layer uses
 * assert() for internal consistency. Live asserts route to the kernel:
 * mb_assert_fail() (tlsglue.c) prints on the serial console and halts —
 * a hobby kernel WANTS to stop at the first inconsistency, not limp on.
 */
#ifndef _MBKERN_SHIM_ASSERT_H
#define _MBKERN_SHIM_ASSERT_H

void mb_assert_fail(const char* expr, const char* file, int line)
    __attribute__((noreturn));

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 : mb_assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif /* _MBKERN_SHIM_ASSERT_H */
