/*
 * Freestanding shim <inttypes.h> — visible ONLY when compiling the
 * vendored mbedtls/TF-PSA-Crypto (Makefile MB_CFLAGS, -nostdinc);
 * copy of the lwext4 stub (PRI* format macros).
 *
 * The PRI* format macros are used exclusively inside ext4_dbg() blocks,
 * which this build compiles out (-DCONFIG_DEBUG_PRINTF=0), but the
 * #include itself must resolve. Provide the common fixed-width set.
 */
#ifndef _MBKERN_SHIM_INTTYPES_H
#define _MBKERN_SHIM_INTTYPES_H

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"

#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "lli"

#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"

#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "llx"

#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "llX"

#endif /* _MBKERN_SHIM_INTTYPES_H */
