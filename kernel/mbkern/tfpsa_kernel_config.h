/* ============================================================
 * tfpsa_kernel_config.h — TF_PSA_CRYPTO_CONFIG_FILE for NullOs.
 *
 * Base: the pristine default psa/crypto_config.h (all algorithms
 * wanted — simplest correct pairing with the full builtin driver
 * set we compile). Then kernel adjustments:
 *   - no persistent key storage (no ITS files, no storage backend)
 * ============================================================ */
#ifndef TFKERN_CONFIG_H
#define TFKERN_CONFIG_H

#include "psa/crypto_config.h"

#ifdef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#endif

/* kernel builds with -mno-sse: AES-NI intrinsics/asm are impossible.
 * The builtin scalar AES driver covers everything. */
#ifdef MBEDTLS_AESNI_C
#undef MBEDTLS_AESNI_C
#endif
#ifdef MBEDTLS_AES_USE_AESNI
#undef MBEDTLS_AES_USE_AESNI
#endif
#ifdef MBEDTLS_PADLOCK_C
#undef MBEDTLS_PADLOCK_C
#endif
#ifdef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#endif

/* No filesystem: MBEDTLS_FS_IO (a legacy option configured here, in the
 * psa/crypto_config.h layer — see mbedtls_config_check_user.h) pulls the
 * seed-file helpers in entropy.c/hmac_drbg.c/ctr_drbg.c which reference
 * fopen/fread/fwrite/fclose/setbuf — meaningless in a freestanding kernel. */
#ifdef MBEDTLS_FS_IO
#undef MBEDTLS_FS_IO
#endif

/* No self-test checkup functions: *_self_test() print via printf() and
 * rsa.c's myrand() references rand() — stdio/stdlib have no kernel home
 * and the checkup routines are test-only. */
#ifdef MBEDTLS_SELF_TEST
#undef MBEDTLS_SELF_TEST
#endif

/* Kernel entropy source: TF-PSA 1.x removed MBEDTLS_ENTROPY_HARDWARE_ALT
 * (it #errors in tf_psa_crypto_config_check_user.h — the classic macro is
 * dead in 4.x/1.x). The sanctioned 4.x knob is the platform-entropy driver:
 *   BUILTIN_GET_ENTROPY = OS sources (getrandom()/sysctl//dev/urandom via
 *                         fopen) — impossible in the kernel;
 *   DRIVER_GET_ENTROPY  = WE provide mbedtls_platform_get_entropy()
 *                         (RDTSC/PIT/RTC mix in tlsglue.c). */
#ifdef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#endif
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY

/* Memory: MBEDTLS_PLATFORM_MEMORY is commented out in the pristine
 * psa/crypto_config.h — without it platform.h maps mbedtls_calloc/free
 * to the libc calloc/free (impossible here). Enabling it activates the
 * MBEDTLS_PLATFORM_{CALLOC,FREE}_MACRO pair (mb_kcalloc/mb_kfree, see
 * mbedtls_kernel_config.h). This is a psa-layer option: configure it
 * HERE, not in the mbedtls config (the check machinery enforces that). */
#define MBEDTLS_PLATFORM_MEMORY

/* Platform time hooks. IMPORTANT: platform-level TUs (crypto/platform/
 * platform_util.c) never read MBEDTLS_CONFIG_FILE — they only see THIS
 * config (via tf-psa-crypto/build_info.h), while MBEDTLS_HAVE_TIME_DATE
 * comes from the pristine psa/crypto_config.h itself. So these MUST be
 * defined here, or platform_util.c falls back to libc gmtime()/
 * clock_gettime()/time(). */
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO long long

#endif /* TFKERN_CONFIG_H */
