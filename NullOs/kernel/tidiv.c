/* ============================================================
 * tidiv.c — 128-bit integer division helpers for the kernel
 * ============================================================
 * mbedtls 4.2's bignum core (MBEDTLS_HAVE_UDBL path, bignum.c
 * divmod helpers) divides `unsigned __int128` values. x86_64 has no
 * native 128/128 division, so GCC emits calls to the libgcc helpers
 * __udivti3/__umodti3 — and we link with `ld` directly (no libgcc in
 * a freestanding kernel). This file provides them.
 *
 * Fast path: divisor fits in 64 bits (the ONLY shape bignum's
 * single-limb divmod produces) — 32-bit-chunked long division feeding
 * hardware 64/64 div's. General path: 128-step shift-subtract.
 * ============================================================ */

typedef unsigned __int128 u128;
typedef unsigned long long u64;

u128 __udivmodti4(u128 num, u128 den, u128* rem_out) {
    if (den == 0) {
        /* Division by zero: libgcc would trap; the kernel's #DE handler
         * catches the ud2 we raise, keeping diagnostics consistent. */
        __asm__ __volatile__ ("ud2");
        return 0;
    }
    if (den > num) {
        if (rem_out) *rem_out = num;
        return 0;
    }
    /* Fast path: 64-bit divisor.
     * FIX #tidiv-rem-overflow: the old 32-bit-chunked loop computed
     * `dividend = (rem << 32) | chunk` — with a NORMALIZED divisor
     * (top bit set, exactly what div_mpi's HAC normalization produces)
     * the intermediate remainder rem can reach d-1 >= 2^63, and
     * `rem << 32` silently overflows the u64 -> garbage quotient digits
     * -> under-reduced mod_mpi result -> infinite subtract loop
     * (#tls-ske-stall: RSA-2048 ServerKeyExchange verify hang).
     * Replaced by the hardware DIVQ: 128/64 -> 64-bit quotient.
     * Fault-free by construction:
     *   hi <  d : (hi:lo) < d*2^64  => quotient < 2^64;
     *   hi >= d : high word from native 64/64, then (r0:lo)/d with
     *             r0 = hi % d < d  => low word also < 2^64. */
    if ((den >> 64) == 0) {
        u64 d   = (u64)den;
        u64 hi  = (u64)(num >> 64);
        u64 lo  = (u64)num;
        u64 q, r;
        if (hi < d) {
            __asm__ __volatile__ ("divq %2"
                                  : "=a"(q), "=d"(r)
                                  : "rm"(d), "a"(lo), "d"(hi));
            if (rem_out) *rem_out = (u128)r;
            return (u128)q;
        }
        u64 q_hi = hi / d;                  /* native 64/64 */
        u64 r0   = hi % d;
        u64 q_lo;
        __asm__ __volatile__ ("divq %2"
                              : "=a"(q_lo), "=d"(r)
                              : "rm"(d), "a"(lo), "d"(r0));
        if (rem_out) *rem_out = (u128)r;
        return ((u128)q_hi << 64) | (u128)q_lo;
    }
    /* General 128/128: shift-subtract long division. */
    u128 q = 0, r = 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((num >> i) & 1u);
        if (r >= den) {
            r -= den;
            q |= (u128)1 << i;
        }
    }
    if (rem_out) *rem_out = r;
    return q;
}

u128 __udivti3(u128 a, u128 b) {
    return __udivmodti4(a, b, (u128*)0);
}

u128 __umodti3(u128 a, u128 b) {
    u128 r;
    __udivmodti4(a, b, &r);
    return r;
}

/* Signed variants (bignum is unsigned-only today; kept for link safety
 * if a future -O level emits them). */
typedef __int128 i128;

static i128 neg128(i128 v) { return (i128)(0 - (u128)v); }

i128 __divti3(i128 a, i128 b) {
    int neg = (a < 0) ^ (b < 0);
    u128 ua = a < 0 ? neg128(a) : (u128)a;
    u128 ub = b < 0 ? neg128(b) : (u128)b;
    u128 q = __udivti3(ua, ub);
    return neg ? neg128((i128)q) : (i128)q;
}

i128 __modti3(i128 a, i128 b) {
    int neg = a < 0;
    u128 ua = a < 0 ? neg128(a) : (u128)a;
    u128 ub = b < 0 ? neg128(b) : (u128)b;
    u128 r;
    __udivmodti4(ua, ub, &r);
    return neg ? neg128((i128)r) : (i128)r;
}
