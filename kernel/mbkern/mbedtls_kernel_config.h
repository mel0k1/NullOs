/* ============================================================
 * mbedtls_kernel_config.h — MBEDTLS_CONFIG_FILE for NullOs
 * (mbedtls 4.2.0 + TF-PSA-Crypto 1.2.0, freestanding kernel)
 *
 * Scope: TLS 1.2 CLIENT with X509 server-certificate verification
 * (dev mode VERIFY_NONE / production VERIFY_REQUIRED against the
 * compiled-in CA bundle). All crypto primitives come from PSA
 * (TF-PSA-Crypto) — mbedtls 4 has no legacy crypto modules left.
 *
 * NO filesystem, NO sockets (net_sockets.c) — the glue in
 * kernel/tlsglue.c provides memory/time/entropy/transport.
 * ============================================================ */
#ifndef MBKERNEL_CONFIG_H
#define MBKERNEL_CONFIG_H

/* ---- platform (macro-based; no MBEDTLS_PLATFORM_C) -------------- */
/* forward declarations: the macros below expand to these kernel-side
 * implementations (tlsglue.c); the config is read before any header
 * that could declare them. */
#include <stddef.h>
void* mb_kcalloc(size_t n, size_t sz);
void  mb_kfree(void* p);
int   mb_printf(const char* fmt, ...);
int   mb_snprintf(char* s, size_t n, const char* fmt, ...);
long long mb_time(long long* t);   /* MBEDTLS_PLATFORM_TIME_TYPE_MACRO */

#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO long long
#define MBEDTLS_PLATFORM_TIME_MACRO      mb_time
#define MBEDTLS_PLATFORM_CALLOC_MACRO    mb_kcalloc
#define MBEDTLS_PLATFORM_FREE_MACRO      mb_kfree
#define MBEDTLS_PLATFORM_PRINTF_MACRO    mb_printf
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO  mb_snprintf

/* X509 NotBefore/NotAfter: kernel civil-time conversion in tlsglue.c
 * (no libc gmtime_r; the shim <time.h> in mbkern/libc provides the
 * glibc-compatible struct tm both sides agree on). */
#define MBEDTLS_PLATFORM_GMTIME_R_ALT

/* Handshake timing: kernel monotonic ms clock in tlsglue.c. */
#define MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO long long
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* entropy: the PSA platform-entropy driver is configured in the TF-PSA
 * config (MBEDTLS_PSA_DRIVER_GET_ENTROPY -> tlsglue.c implementation).
 * The classic MBEDTLS_NO_PLATFORM_ENTROPY / MBEDTLS_ENTROPY_HARDWARE_ALT
 * were REMOVED in mbedtls 4.0 / TF-PSA-Crypto 1.0 (config checks #error
 * on them) and have no consumers — do not resurrect them here. */

/* ---- TLS -------------------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
/* retain the parsed peer chain after handshake failures — #tls-verify-cn
 * forensics (and future pinning/TOFU features need it too) */
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
/* client-side SNI in ClientHello (#tls-verify-cn ROOT): without this
 * mbedtls_ssl_set_hostname() only stores the name for verification and
 * the ClientHello carries NO server_name extension — shared CDNs
 * (Fastly) then serve their DEFAULT fallback cert
 * (j.sni-*-default.ssl.fastly.net) and VERIFY_REQUIRED dies with
 * CN_MISMATCH even though the bundle and hostname are perfect. */
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_SSL_CIPHERSUITES                  \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,   \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,   \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256

/* server certificate chains arrive in the handshake flight */
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN   4096
/* x509 chain verification budget: Alpine dl-cdn chains are 2-3 certs */
#define MBEDTLS_X509_MAX_INTERMEDIATE_CA  4

/* ---- X509 / PK / encodings -------------------------------------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_OID_C

/* ---- diagnostics (serial; harmless in production) --------------- */
#define MBEDTLS_DEBUG_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_VERSION_C


#endif /* MBKERNEL_CONFIG_H */
