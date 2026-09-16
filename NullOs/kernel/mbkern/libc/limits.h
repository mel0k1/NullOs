/*
 * Freestanding shim <limits.h> — visible ONLY when compiling the vendored
 * mbedtls/TF-PSA-Crypto (Makefile MB_CFLAGS, -nostdinc).
 *
 * GCC's own limits.h does #include_next <limits.h> expecting the system
 * (glibc) header to exist; with -nostdinc there is no "next". This shim
 * resolves FIRST (-I mbkern/libc before -isystem) and self-contains the
 * standard fixed set, so GCC's chained header is never reached.
 */
#ifndef _MBKERN_SHIM_LIMITS_H
#define _MBKERN_SHIM_LIMITS_H

#define CHAR_BIT    8

#define SCHAR_MIN (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255

#define CHAR_MIN  SCHAR_MIN
#define CHAR_MAX  SCHAR_MAX

#define SHRT_MIN  (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535

#define INT_MIN   (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295u

#define LONG_MIN  (-9223372036854775807L - 1L)
#define LONG_MAX   9223372036854775807L
#define ULONG_MAX  18446744073709551615UL

#define LLONG_MIN  (-9223372036854775807LL - 1LL)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#endif /* _MBKERN_SHIM_LIMITS_H */
