/*
 * Freestanding shim <time.h> — visible ONLY when compiling the vendored
 * mbedtls/TF-PSA-Crypto (Makefile MB_CFLAGS, -nostdinc) and included by
 * tlsglue.c via the same -I path.
 *
 * Why: MBEDTLS_HAVE_TIME_DATE makes platform_util.h #include <time.h>
 * unconditionally, and x509.c declares a complete `struct tm` for
 * mbedtls_platform_gmtime_r (MBEDTLS_PLATFORM_GMTIME_R_ALT). Without
 * -nostdinc the HOST glibc headers leak into the kernel build (features.h
 * defines __GLIBC__, which silently enables explicit_bzero/unistd paths and
 * pulls SYS_getrandom — all wired to symbols that cannot exist here).
 *
 * struct tm mirrors the glibc x86_64 layout BIT-FOR-BIT (field order,
 * types), so tlsglue.c's mbedtls_platform_gmtime_r (compiled against this
 * shim) and any TU filling/reading struct tm agree on the ABI.
 */
#ifndef _MBKERN_SHIM_TIME_H
#define _MBKERN_SHIM_TIME_H

typedef long long mbkern_time_t;
typedef mbkern_time_t time_t;

struct tm {
    int tm_sec;      /* seconds after the minute [0..60] */
    int tm_min;      /* minutes after the hour   [0..59] */
    int tm_hour;     /* hours since midnight     [0..23] */
    int tm_mday;     /* day of the month         [1..31] */
    int tm_mon;      /* months since January     [0..11] */
    int tm_year;     /* years since 1900                */
    int tm_wday;     /* days since Sunday        [0..6]  */
    int tm_yday;     /* days since January 1     [0..365]*/
    int tm_isdst;    /* DST flag (always 0 here)        */
    long tm_gmtoff;  /* seconds east of UTC (0)         */
    const char* tm_zone; /* timezone abbreviation "UTC" */
};

#endif /* _MBKERN_SHIM_TIME_H */
