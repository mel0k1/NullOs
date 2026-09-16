/* #tls-ske-stall diagnostics: expose private identifiers for the MPI
* API before ANY header can include private_access.h in public mode
* (its include guard would block the private re-inclusion). */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* ============================================================
 * tlsglue.c — mbedtls 4.2.0 HTTPS client for NullOs
 * ============================================================
 * Everything the vendored mbedtls/TF-PSA-Crypto needs from the
 * kernel, plus a minimal HTTPS GET client over the kernel TCP stack:
 *
 *   memory    : mb_kcalloc/mb_kfree -> kzalloc/kfree (dlmalloc heap)
 *   printf    : serial console (debug traces + errors)
 *   time      : RTC -> epoch seconds (X509 validity checks)
 *   entropy   : mbedtls_hardware_poll (RDTSC/PIT/RTC mix)
 *   transport : BIO callbacks over net_socket_* TCP stream
 *   DNS       : mini A-record resolver via UDP (10.0.2.3, slirp)
 *
 * Shell: `https <host|ip> <path> [verify]`
 *   default authmode = VERIFY_NONE (dev mode, #tls-dev-verify-none)
 *   `verify`         = VERIFY_REQUIRED against the compiled-in CA
 *                      bundle (kernel/mbkern/ca_bundle.h, #tls-ca-bundle)
 *
 * Body lands in /tmp/https.bin and is reported on the console.
 * ============================================================ */

#include "../include/types.h"
#include "../include/spinlock.h"
#include "../include/serial.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/net.h"
#include "../include/rtc.h"
#include "../include/fs.h"
#include "../include/mm.h"
#include "../include/timer.h"
#include "../include/kernel.h"

#include "mbkern/mbedtls_kernel_config.h"
#include "mbkern/ca_bundle.h"

#include <mbedtls/version.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/debug.h>
#include <mbedtls/error.h>
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>
#include <psa/crypto_driver_random.h>  /* psa_driver_get_entropy_flags_t */

/* tlssanity diagnostics: MPI API + core bignum entry points (private
 * headers in mbedtls 4.x). ALLOW_PRIVATE_ACCESS + the shim <string.h>
 * (memcpy/memset for alignment.h) must be in scope first. */
#include "mbkern/libc/string.h"  /* shim: memcpy/memset decls for alignment.h */
#include <mbedtls/private_access.h>
#include <mbedtls/private/bignum.h>
#include "bignum_core.h"

/* ============================================================
 * platform callbacks
 * ============================================================ */

void* mb_kcalloc(size_t n, size_t sz) {
    if (n && sz && n > (size_t)-1 / sz) return NULL;
    return kzalloc(n * sz);
}

void mb_kfree(void* p) { kfree(p); }

/* Symbolic mbedtls_calloc/mbedtls_free: most TUs expand these as the
 * MBEDTLS_PLATFORM_{CALLOC,FREE}_MACRO (mb_kcalloc/mb_kfree), but the
 * PSA core (psa_crypto.c) references them as plain identifiers without
 * including mbedtls/platform.h. Providing the symbols costs nothing
 * (macro-only TUs never reference them) and closes the gap. */
void* mbedtls_calloc(size_t n, size_t sz) { return mb_kcalloc(n, sz); }
void  mbedtls_free(void* p) { mb_kfree(p); }

/* MBEDTLS_ENTROPY_HARDWARE_ALT poll: consumed by the tf-psa entropy
 * driver; declared here (the canonical prototype lives in a private
 * header in mbedtls 4.x, so we restate it exactly). */

/* debug self-tests inside md/ctr_drbg/hmac_drbg check ferror(stdout);
 * our "console" (serial via mb_printf) never fails, so this is a
 * constant no-op by construction. */
int ferror(struct _IO_FILE* stream) { (void)stream; return 0; }

/* vendored assert() (see mbkern/libc/assert.h): stop at the first
 * internal inconsistency — serial breadcrumb + kernel panic halt. */
void mb_assert_fail(const char* expr, const char* file, int line) {
    serial_printf("[TLSASSERT] %s:%d: %s\n", file, line, expr);
    kernel_panic("mbedtls assert");
    for (;;) __asm__ volatile ("cli\nhlt" ::: "memory");
}

/* debug.c's print helpers route through vsnprintf (via a format-check
 * shim). Route to the kernel's kvsnprintf. */
int vsnprintf(char* s, size_t n, const char* fmt, __builtin_va_list ap) {
    return kvsnprintf(s, n, fmt, ap);
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}

char* strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char* a = h;
        const char* b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return 0;
}

/* x509 SAN iPAddress parsing: our test hosts resolve via A records,
 * so an ASCII dotted-quad conversion is all that is reachable. */
int inet_pton(int af, const char* src, void* dst) {
    if (af != 2 /*AF_INET*/) return -1;
    u8 out[4];
    u32 v = 0; int part = 0;
    for (; *src; src++) {
        if (*src == '.') {
            if (v > 255 || part == 4) return 0;
            out[part++] = (u8)v; v = 0;
        } else if (*src >= '0' && *src <= '9') {
            v = v * 10 + (u32)(*src - '0');
            if (v > 999) return 0;
        } else return 0;
    }
    if (part != 3 || v > 255) return 0;
    out[3] = (u8)v;
    kmemcpy(dst, out, 4);
    return 1;
}

int mb_printf(const char* fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    serial_printf("[TLS] %s", buf);
    return (int)kstrlen(buf);
}

int mb_snprintf(char* s, size_t n, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = kvsnprintf(s, n, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

/* days-from-civil (Howard Hinnant) — RTC -> Unix epoch seconds */
static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (long long)doe - 719468LL;
}

long long mb_time(long long* t) {
    rtc_time_t r;
    rtc_read_time(&r);
    long long days = days_from_civil(r.year, r.month, r.day);
    long long secs = days * 86400LL
                   + (long long)r.hour * 3600LL
                   + (long long)r.minute * 60LL
                   + (long long)r.second;
    if (t) *t = (mbedtls_time_t)secs;
    return (mbedtls_time_t)secs;
}

/* ---- entropy / time ---------------------------------------------- */

/* PSA platform-entropy driver (MBEDTLS_PSA_DRIVER_GET_ENTROPY, see the
 * TF-PSA config): "true source" seeding psa_generate_random(). Must
 * return FULL entropy per call (estimate_bits = 8 * output_size).
 * Mix: RDTSC (per 8-byte word, with a tiny spin so back-to-back words
 * differ) ^ PIT tick counter spread by the golden-ratio constant. */
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
                                 size_t* estimate_bits,
                                 unsigned char* output, size_t output_size) {
    if (flags != PSA_DRIVER_GET_ENTROPY_FLAGS_NONE) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    u64 tick = timer_get_ticks();
    for (size_t i = 0; i < output_size; i += 8) {
        u64 tsc;
        __asm__ __volatile__ ("rdtsc" : "=A"(tsc));
        u64 mix = tsc ^ (tick * 0x9E3779B97F4A7C15ULL);
        tick = mix;               /* chain: word N feeds word N+1 */
        u8* m = (u8*)&mix;
        size_t n = output_size - i < 8 ? output_size - i : 8;
        for (size_t k = 0; k < n; k++) output[i + k] = m[k];  /* assign */
        for (volatile int s = 0; s < 64; s++) { /* TSC spacing */
            __asm__ __volatile__ ("pause");
        }
    }
    if (estimate_bits) *estimate_bits = 8 * output_size;
    return 0;
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT: monotonic milliseconds (PIT ticks). */
long long mbedtls_ms_time(void) {
    return (long long)timer_get_uptime_ms();
}

/* MBEDTLS_PLATFORM_GMTIME_R_ALT: civil-from-days (Howard Hinnant,
 * inverse of days_from_civil above) — UTC, no libc. struct tm layout is
 * the shim mbkern/libc/time.h one (glibc-compatible field order). */
struct tm* mbedtls_platform_gmtime_r(const long long* tt,
                                     struct tm* tm_buf) {
    if (!tt || !tm_buf) return (void*)0;
    long long secs = *tt;
    long long days = secs / 86400LL;
    long long rem  = secs % 86400LL;
    if (rem < 0) { rem += 86400LL; days -= 1; }
    tm_buf->tm_hour = (int)(rem / 3600LL);
    tm_buf->tm_min  = (int)((rem % 3600LL) / 60LL);
    tm_buf->tm_sec  = (int)(rem % 60LL);

    days += 719468LL;                          /* shift to 0000-03-01 era */
    long long era  = (days >= 0 ? days : days - 146096LL) / 146097LL;
    unsigned doe   = (unsigned)(days - era * 146097LL);
    unsigned yoe   = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long long y    = (long long)yoe + era * 400LL;
    unsigned doy   = doe - (365u*yoe + yoe/4u - yoe/100u);
    unsigned mp    = (5u*doy + 2u) / 153u;
    unsigned d     = doy - (153u*mp + 2u)/5u + 1u;
    unsigned m     = mp < 10u ? mp + 3u : mp - 9u;
    if (m <= 2) y += 1;
    tm_buf->tm_year = (int)(y - 1900LL);
    tm_buf->tm_mon  = (int)m - 1;
    tm_buf->tm_mday = (int)d;
    /* weekday: 1970-01-01 was a Thursday (4) */
    tm_buf->tm_wday = (int)((*tt / 86400LL + 4LL) % 7LL);
    if (tm_buf->tm_wday < 0) tm_buf->tm_wday += 7;
    /* day-of-year: exact, via days_from_civil of Jan 1 of the same year */
    tm_buf->tm_yday = (int)(days_from_civil(y, 1, 1) * -1
                            + (days - 719468LL));
    tm_buf->tm_isdst  = 0;
    tm_buf->tm_gmtoff = 0;
    tm_buf->tm_zone   = "UTC";
    return tm_buf;
}

/* ============================================================
 * transport BIO over kernel TCP
 * ============================================================ */

typedef struct {
    int sock;        /* net socket index (STREAM) */
} tls_bio_t;

/* #tls-if-leak: see hlt_irq_restore() in include/types.h — the old
 * "sti\nhlt\ncli" tail left IF=0 on every loop exit, so the whole
 * https continuation (and the shell hlt() wait) ran with IRQs off ->
 * prompt printed but keyboard/timer dead afterwards. */

static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
    tls_bio_t* b = (tls_bio_t*)ctx;
    if (len == 0) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    s64 n = net_socket_stream_recv(b->sock, buf, (u32)len);
    if (n > 0) return (int)n;
    if (n == 0) return 0;                        /* EOF */
    /* -1: no data yet -> WANT_READ after a bounded hlt wait so the
     * timer keeps IRQs and net_poll() flowing (same pattern as the
     * syscall-side blocking reads) */
    for (int i = 0; i < 60 && !net_socket_has_eof(b->sock); i++) {
        hlt_irq_restore();
        net_poll();
        n = net_socket_stream_recv(b->sock, buf, (u32)len);
        if (n >= 0) return (int)n;
    }
    return MBEDTLS_ERR_SSL_TIMEOUT;
}

static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    tls_bio_t* b = (tls_bio_t*)ctx;
    s64 n = net_socket_stream_send(b->sock, buf, (u32)len);
    if (n > 0) return (int)n;
    if (n == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/* ============================================================
 * mini DNS A-record resolver (slirp DNS 10.0.2.3)
 * ============================================================ */

static const u8 DNS_SERVER[4] = { 10, 0, 2, 3 };

static bool ip_ascii_to_octets(const char* s, u8 out[4]) {
    u32 v = 0; int part = 0, dots = 0;
    for (; *s; s++) {
        if (*s == '.') {
            if (v > 255 || part == 4) return false;
            out[part++] = (u8)v; v = 0; dots++;
        } else if (*s >= '0' && *s <= '9') {
            v = v * 10 + (u32)(*s - '0');
            if (v > 999) return false;
        } else return false;
    }
    if (dots != 3 || part != 3 || v > 255) return false;
    out[3] = (u8)v;
    return true;
}

/* returns true + out[4] on success */
static bool dns_resolve_a(const char* host, u8 out[4]) {
    if (ip_ascii_to_octets(host, out)) return true;

    u8 q[512];
    u16 qlen = 0, txid = (u16)timer_get_ticks();

    /* header */
    q[qlen++] = (u8)(txid >> 8); q[qlen++] = (u8)txid;
    q[qlen++] = 0x01; q[qlen++] = 0x00;          /* RD query */
    q[qlen++] = 0; q[qlen++] = 1;                /* QDCOUNT 1 */
    q[qlen++] = 0; q[qlen++] = 0;
    q[qlen++] = 0; q[qlen++] = 0;
    q[qlen++] = 0; q[qlen++] = 0;
    /* QNAME */
    /* FIX(#dns-label-overflow): the label-length check ran AFTER the
     * write — a >63-octet label (DNS hard limit) or a hostile host
     * string could run up to ~100 bytes past q[512] on the KERNEL
     * stack (console-triggerable). Validate BEFORE writing and bound
     * the total query length up front. */
    const char* p = host;
    while (*p) {
        u8 lbl = 0;
        const char* s = p;
        while (*p && *p != '.') { p++; lbl++; }
        if (lbl == 0 || lbl > 63) return false;   /* RFC 1035 limit  */
        if ((u32)qlen + 1u + lbl + 16u > sizeof(q)) return false;
        q[qlen++] = lbl;
        kmemcpy(q + qlen, s, lbl); qlen += lbl;
        if (*p == '.') p++;
    }
    if ((u32)qlen + 5u > sizeof(q)) return false;
    q[qlen++] = 0;
    q[qlen++] = 0; q[qlen++] = 1;                /* type A    */
    q[qlen++] = 0; q[qlen++] = 1;                /* class IN  */

    int sock = net_socket_alloc();
    if (sock < 0) return false;
    net_socket_set_type(sock, 2 /*SOCK_DGRAM*/);

    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        if (net_socket_sendto(sock, DNS_SERVER, 53, q, qlen) < 0) break;
        for (int wait = 0; wait < 40 && !ok; wait++) {
            hlt_irq_restore();      /* #tls-if-leak: was sti\nhlt\ncli */
            net_poll();
            u8 resp[1500];
            u8 sip[4]; u16 sport;
            s64 n = net_socket_recvfrom(sock, resp, sizeof(resp), sip, &sport);
            if (n < 12 || sport != 53) continue;
            if (resp[0] != q[0] || resp[1] != q[1]) continue;   /* id   */
            if (!(resp[2] & 0x80)) continue;                    /* QR=1 */
            u16 qd = (u16)((resp[4] << 8) | resp[5]);
            u16 an = (u16)((resp[6] << 8) | resp[7]);
            if (qd != 1 || an == 0) continue;
            /* skip question section */
            u64 off = 12;
            while (off < (u64)n && resp[off]) {
                if (resp[off] & 0xC0) { off += 2; goto answers; }
                off += resp[off] + 1;
            }
            off += 5;                            /* root + type + class */
answers:
            for (u16 a = 0; a < an && off + 12 <= (u64)n; a++) {
                if (resp[off] & 0xC0) off += 2; else off += resp[off] + 1;
                off += 2;                        /* type   */
                u16 cls = (u16)((resp[off] << 8) | resp[off+1]); off += 2;
                off += 4;                        /* ttl    */
                u16 rdlen = (u16)((resp[off] << 8) | resp[off+1]); off += 2;
                if (cls == 1 && rdlen == 4 && off + 4 <= (u64)n) {
                    kmemcpy(out, resp + off, 4);
                    ok = true;
                }
                off += rdlen;
            }
        }
    }
    net_socket_free(sock);
    return ok;
}

/* ============================================================
 * tlssanity (#tls-ske-stall hunt): run the EXACT failing bignum
 * sequence with a HARDCODED modulus (captured from the hung kernel),
 * no network, no cert parse — isolates kernel-env vs network-env.
 * Then a deterministic kmalloc/kfree storm with pattern canaries.
 * ============================================================ */
void cmd_tlssanity(int argc, char** argv) {
    (void)argc; (void)argv;
    static const unsigned char km_mod[256] = {
        0xcf,0xa3,0x85,0x59,0x5d,0x89,0xc6,0xec,0x99,0x0d,0x92,0x60,
        0x25,0x7c,0x3e,0xb5,0x27,0x51,0x0b,0xe8,0xc8,0x26,0x8f,0x47,
        0xce,0xd4,0xb4,0x53,0xb0,0x97,0xec,0xe7,0xa5,0x55,0x0e,0xfd,
        0x55,0x75,0xd2,0x90,0x0b,0xc3,0x78,0x56,0x89,0x41,0xea,0x54,
        0xbf,0x17,0xdd,0xfa,0xb2,0x2a,0x1a,0x92,0x5f,0x12,0xb1,0x22,
        0x7d,0xab,0xae,0xd2,0xc5,0x34,0x93,0x92,0xd5,0xdd,0xab,0x75,
        0xb5,0x36,0x5b,0x38,0x9f,0x95,0x72,0x71,0xa5,0x2a,0x21,0xd4,
        0x95,0x45,0x35,0x43,0x97,0xad,0xb3,0x66,0x44,0x3b,0x7f,0x7a,
        0xf9,0x00,0x46,0x88,0x08,0x22,0xb0,0x73,0xfb,0xfe,0x5a,0xc0,
        0x88,0xbe,0xf1,0xac,0x0f,0xb4,0x31,0xc2,0xd8,0x2a,0x4f,0x6c,
        0xdb,0x2c,0x3a,0x88,0x89,0x33,0x11,0xa6,0xb1,0x31,0x30,0x20,
        0x36,0xef,0xd2,0x61,0xc5,0x10,0x49,0xc8,0x25,0xfb,0xa5,0x89,
        0x20,0x0f,0xec,0x3e,0x92,0xd9,0x14,0x9d,0x98,0x22,0xa3,0xf6,
        0x4f,0x96,0xb4,0xa3,0xcb,0x4c,0xa2,0xa2,0xc0,0x63,0xd7,0xdb,
        0xb9,0x22,0xfe,0x5f,0xbe,0x27,0xe1,0xc3,0x1e,0xb9,0x01,0x8a,
        0x98,0x0f,0x18,0xd4,0x21,0xaa,0x1a,0x64,0xa4,0x1d,0x19,0x64,
        0xd3,0x27,0x95,0x27,0xdf,0x29,0xa3,0xbe,0xff,0x54,0x8e,0x87,
        0x25,0xe3,0x5c,0x3f,0xf1,0xfb,0x9e,0x33,0x5d,0x6f,0x86,0xf2,
        0x2b,0xda,0x3c,0xf5,0x5f,0xe6,0xed,0x85,0xa4,0x26,0x21,0x78,
        0x32,0x95,0x4b,0x42,0xf5,0xd7,0x1d,0xcc,0xc7,0xfc,0xd2,0xb9,
        0xeb,0x5d,0x6b,0x43,0x16,0x45,0xf8,0xef,0x29,0x50,0xa5,0x79,
        0xdc,0x28,0x4c,0x6f
    };
    serial_printf("[SAN] start\n");
    mbedtls_mpi N, X, E, T;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&X);
    mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&T);

    int rc = mbedtls_mpi_read_binary(&N, km_mod, sizeof(km_mod));
    serial_printf("[SAN] read rc=%d N.n=%u\n", rc,
                  (unsigned)N.MBEDTLS_PRIVATE(n));

    serial_printf("[SAN] r2 enter\n");
    rc = mbedtls_mpi_core_get_mont_r2_unsafe(&X, &N);
    serial_printf("[SAN] r2 rc=%d X.n=%u\n", rc,
                  (unsigned)X.MBEDTLS_PRIVATE(n));

    rc = mbedtls_mpi_lset(&E, 65537);
    serial_printf("[SAN] lset rc=%d\n", rc);
    serial_printf("[SAN] expmod enter\n");
    rc = mbedtls_mpi_exp_mod(&T, &E, &N, &N, NULL);
    serial_printf("[SAN] expmod rc=%d\n", rc);

    mbedtls_mpi_free(&T);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&N);

    /* deterministic heap storm with canaries */
    serial_printf("[SAN] heap storm\n");
    {
        #define STORM_LIVE 64
        #define STORM_ITERS 20000
        static struct { unsigned char* p; size_t sz; u32 magic; } live[STORM_LIVE];
        u32 rng = 0x12345678;
        int errs = 0;
        kmemset(live, 0, sizeof(live));
        for (int it = 0; it < STORM_ITERS; it++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            int slot = (int)(rng % STORM_LIVE);
            if (live[slot].p) {
                /* verify canary before freeing */
                unsigned char* p = live[slot].p;
                size_t sz = live[slot].sz;
                for (size_t i = 0; i < sz; i++) {
                    if (p[i] != (unsigned char)((live[slot].magic + i) & 0xFF)) {
                        serial_printf("[SAN] CANARY FAIL it=%d slot=%d "
                                      "pa=%lx sz=%lu off=%lu\n", it, slot,
                                      (unsigned long)(u64)p,
                                      (unsigned long)sz, (unsigned long)i);
                        if (++errs > 8) return;
                        break;
                    }
                }
                kfree(p);
                live[slot].p = NULL;
            }
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            size_t sz = (size_t)(rng % 4096) + 1;
            unsigned char* p = kmalloc(sz);
            if (!p) { serial_printf("[SAN] OOM it=%d\n", it); return; }
            u32 magic = rng;
            for (size_t i = 0; i < sz; i++) p[i] = (unsigned char)((magic + i) & 0xFF);
            live[slot].p = p;
            live[slot].sz = sz;
            live[slot].magic = magic;
        }
        for (int s = 0; s < STORM_LIVE; s++)
            if (live[s].p) kfree(live[s].p);
        serial_printf("[SAN] storm done errs=%d\n", errs);
    }
    serial_printf("[SAN] DONE\n");
}

static mbedtls_x509_crt g_ca;
static bool g_ca_loaded = false;
static spinlock_t tls_lock = SPINLOCK_INIT;

/* #tls-verify-cn: per-cert forensics DURING chain verification —
 * on a verify failure the 4.x parse path frees the chain at exit, so
 * the post-mortem dump can never see the peer cert. This hook runs
 * while the chain is alive. */
static int tls_vrfy_cb(void* ctx, mbedtls_x509_crt* crt,
                       int depth, uint32_t* flags) {
    mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)ctx;
    char dn[96];
    mbedtls_x509_dn_gets(dn, sizeof(dn), &crt->subject);
    serial_printf("[VRFY] depth=%d flags=0x%08x subj='%s' hasSAN=%d\n",
                  depth, *flags, dn,
                  (crt->ext_types & MBEDTLS_X509_EXT_SUBJECT_ALT_NAME)
                      ? 1 : 0);
    if (depth == 0) {
        for (const mbedtls_x509_sequence* s = &crt->subject_alt_names;
             s; s = s->next) {
            char sb[64];
            u32 n = s->buf.len < sizeof(sb) - 1
                      ? s->buf.len : sizeof(sb) - 1;
            kmemcpy(sb, s->buf.p, n);
            sb[n] = 0;
            serial_printf("[VRFY] san tag=%d len=%u val='%s'\n",
                          (int)(s->buf.tag & MBEDTLS_ASN1_TAG_VALUE_MASK),
                          (unsigned)s->buf.len, sb);
        }
        if (ssl) {
            const char* hn = mbedtls_ssl_get_hostname(ssl);
            serial_printf("[VRFY] hostname='%s' hlen=%u\n",
                          hn ? hn : "(NULL)",
                          (unsigned)(hn ? kstrlen(hn) : 0));
        }
    }
    return 0;
}

static void tls_debug_cb(void* ctx, int level, const char* file, int line,
                         const char* str) {
    (void)ctx; (void)level;
    char clean[256];
    size_t i = 0;
    for (; str[i] && i < sizeof(clean) - 1; i++)
        clean[i] = (str[i] == '\n') ? ' ' : str[i];
    clean[i] = 0;
    serial_printf("[mbedtls] %s:%04d: %s\n", file, line, clean);
}

/* runs the GET; returns 0 and bytes written on success */
static s64 https_get_impl(const char* host, const char* path,
                          bool verify, const char* outfile) {
    u8 ip[4];
    if (!dns_resolve_a(host, ip)) {
        vga_print("https: cannot resolve host\n");
        return -1;
    }
    serial_printf("[TLS] resolved %s -> %u.%u.%u.%u\n", host,
                  ip[0], ip[1], ip[2], ip[3]);

    int rc = psa_crypto_init();
    s64 ret = -1;   /* declared BEFORE every goto done: jumping over an
                     * initialized local leaves it indeterminate — the
                     * first verify-run returned stack garbage and cmd_https
                     * printed a bogus "DONE (3112493 body bytes)" */
    if (rc != PSA_SUCCESS) {
        serial_printf("[TLS] psa_crypto_init rc=%d\n", (int)rc);
        vga_print("https: psa init failed\n");
        return -1;
    }

    /* CA chain (parsed once) */
    u64 irq = spin_lock_irqsave(&tls_lock);
    if (!g_ca_loaded) {
        mbedtls_x509_crt_init(&g_ca);
        rc = mbedtls_x509_crt_parse(&g_ca,
                                    (const unsigned char*)ca_bundle_pem,
                                    ca_bundle_pem_len + 1);
        if (rc < 0) {
            serial_printf("[TLS] CA parse rc=%d\n", rc);
        } else {
            g_ca_loaded = true;
            serial_printf("[TLS] CA bundle: %d parsed, %d failed\n",
                          rc >> 16, rc & 0xFFFF);
        }
    }
    spin_unlock_irqrestore(&tls_lock, irq);
    if (verify && !g_ca_loaded) {
        vga_print("https: CA bundle unavailable\n");
        return -1;
    }

    /* TCP connect */
    int sock = net_socket_alloc();
    if (sock < 0) return -1;
    net_socket_set_type(sock, 1 /*SOCK_STREAM*/);
    if (net_socket_connect(sock, ip, 443) != 0 ||
        net_socket_connect_tcp(sock) != 0) {
        vga_print("https: tcp connect failed\n");
        net_socket_free(sock);
        return -1;
    }

    static tls_bio_t bio;
    bio.sock = sock;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                            : MBEDTLS_SSL_VERIFY_NONE);
    if (g_ca_loaded) mbedtls_ssl_conf_ca_chain(&conf, &g_ca, NULL);
    mbedtls_ssl_conf_verify(&conf, tls_vrfy_cb, &ssl);   /* #tls-verify-cn */
    mbedtls_ssl_conf_dbg(&conf, tls_debug_cb, NULL);
    mbedtls_debug_set_threshold(4);
    mbedtls_ssl_setup(&ssl, &conf);
    mbedtls_ssl_set_hostname(&ssl, host);
    mbedtls_ssl_set_bio(&ssl, &bio, bio_send, bio_recv, NULL);

    /* step-timing probe (#tls-ske-stall): each loop = one state advance */
    {
        int steps = 0;
        while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
                rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char err[160];
                mbedtls_strerror(rc, err, sizeof(err));
                serial_printf("[TLS] handshake rc=%d (%s)\n", rc, err);
                u32 fflags = mbedtls_ssl_get_verify_result(&ssl);
                serial_printf("[TLS] fail-flags=0x%08x now=%lld\n",
                              fflags, mb_time(NULL));
                /* name-check forensics (#tls-verify-cn): what does the
                 * guest actually compare? */
                {
                    const char* hn = mbedtls_ssl_get_hostname(&ssl);
                    serial_printf("[TLS] hostname='%s'\n",
                                  hn ? hn : "(NULL)");
                    /* session->peer_cert appears only after a COMPLETED
                     * handshake; on failure the chain lives in
                     * session_negotiate — read it there directly. */
                    const mbedtls_x509_crt* fc = NULL;
                    {
                        mbedtls_ssl_session* sn =
                            ssl.MBEDTLS_PRIVATE(session_negotiate);
                        if (sn) fc = sn->peer_cert;
                    }
                    if (fc) {
                        char dn[96];
                        mbedtls_x509_dn_gets(dn, sizeof(dn), &fc->subject);
                        serial_printf("[TLS] peer subj='%s' hasSAN=%d\n",
                                      dn,
                                      (fc->ext_types &
                                       MBEDTLS_X509_EXT_SUBJECT_ALT_NAME)
                                          ? 1 : 0);
                        for (const mbedtls_x509_sequence* s =
                                 &fc->subject_alt_names; s; s = s->next) {
                            char sb[64];
                            u32 n = s->buf.len < sizeof(sb) - 1
                                      ? s->buf.len : sizeof(sb) - 1;
                            kmemcpy(sb, s->buf.p, n);
                            sb[n] = 0;
                            serial_printf("[TLS] san tag=%d len=%u val='%s'\n",
                                          (int)(s->buf.tag &
                                                MBEDTLS_ASN1_TAG_VALUE_MASK),
                                          (unsigned)s->buf.len, sb);
                        }
                    } else {
                        serial_printf("[TLS] no peer cert retained\n");
                    }
                }
                vga_printf("https: handshake failed rc=%d\n", rc);
                goto done;
            }
            if ((++steps & 1) == 0)
                serial_printf("[TLS] hs-iter %d t=%lldms\n",
                              steps, (long long)timer_get_uptime_ms());
        }
    }

    {
        const mbedtls_x509_crt* crt = mbedtls_ssl_get_peer_cert(&ssl);
        u32 vres = mbedtls_ssl_get_verify_result(&ssl);
        vga_printf("https: TLS up, cipher=%s\n",
                   mbedtls_ssl_get_ciphersuite(&ssl));
        serial_printf("[TLS] up cipher=%s\n",
                      mbedtls_ssl_get_ciphersuite(&ssl));
        if (crt) {
            char subj[120];
            mbedtls_x509_dn_gets(subj, sizeof(subj), &crt->subject);
            vga_printf("https: server CN: %s\n", subj);
        }
        vga_printf("https: verify result: 0x%08x (%s)\n", vres,
                   verify ? "REQUIRED" : "NONE-dev");
        serial_printf("[TLS] verify result=0x%08x mode=%s\n", vres,
                      verify ? "REQUIRED" : "NONE-dev");
    }

    /* request */
    {
        char req[512];
        int l = mb_snprintf(req, sizeof(req),
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "User-Agent: NullOs-tls/1.0\r\n"
                            "Connection: close\r\n"
                            "\r\n", path, host);
        /* FIX(#https-req-overread): kvsnprintf returns the WOULD-BE
         * length (C99 semantics) — with a long path/host, l > 512 and
         * the write loop below read req[512..l-1]: uninitialized kernel
         * stack transmitted to the remote server. Clamp to the real
         * buffer size. */
        if (l < 0) l = 0;
        if (l > (int)sizeof(req)) l = (int)sizeof(req);
        int off = 0;
        while (off < l) {
            rc = mbedtls_ssl_write(&ssl,
                                   (const unsigned char*)req + off,
                                   (size_t)(l - off));
            if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc <= 0) { vga_print("https: write failed\n"); goto done; }
            off += rc;
        }
    }

    /* response: split headers/body on first \r\n\r\n */
    {
        static u8 rbuf[24576];
        u64 have = 0, body_start = 0;
        bool headers_done = false;
        for (;;) {
            rc = mbedtls_ssl_read(&ssl, rbuf + have,
                                  sizeof(rbuf) - 1 - have);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (rc <= 0) break;                       /* EOF / closed */
            have += (u64)rc;
            if (!headers_done) {
                for (u64 i = have - (u64)rc; i + 3 < have; i++) {
                    if (rbuf[i] == '\r' && rbuf[i+1] == '\n' &&
                        rbuf[i+2] == '\r' && rbuf[i+3] == '\n') {
                        body_start = i + 4;
                        headers_done = true;
                        rbuf[body_start - 2] = 0;     /* keep headers printable */
                        vga_print((const char*)rbuf); /* status line etc. */
                        vga_print("\n");
                        break;
                    }
                }
            }
            if (have >= sizeof(rbuf) - 1) break;
        }

        u64 body_len = headers_done ? (have - body_start) : 0;
        vga_printf("https: %lu bytes received, body %lu bytes\n",
                   (unsigned long)have, (unsigned long)body_len);
        if (outfile && outfile[0] && body_len) {
            s32 fd = fs_open(outfile, FS_WRITE | FS_CREATE);
            if (fd >= 0) {
                fs_write(fd, rbuf + body_start, body_len);
                fs_close(fd);
                vga_printf("https: body saved to %s\n", outfile);
            }
        }
        ret = (s64)body_len;   /* fall through to shared cleanup */
    }

done:
    /* BOTH paths must release the TLS state and the TCP socket —
     * the old success path returned early and leaked the socket
     * (stayed half-open in the table forever). */
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    net_socket_close_tcp(sock);
    net_socket_free(sock);
    return ret;
}

void cmd_https(int argc, char** argv) {
    if (argc < 3) {
        vga_print("Usage: https <host|ip> <path> [verify] [outfile]\n");
        vga_print("  dev mode (no verify) is the default\n");
        return;
    }
    const char* host = argv[1];
    const char* path = argv[2];
    bool verify = (argc > 3 && kstrcmp(argv[3], "verify") == 0);
    const char* outfile = (argc > 4) ? argv[4] : "/tmp/https.bin";
    s64 r = https_get_impl(host, path, verify, outfile);
    if (r < 0) vga_print("https: FAILED\n");
    else vga_printf("https: DONE (%lu body bytes)\n", (unsigned long)r);
}
