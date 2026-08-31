/*
 * wltest — NullOs #wl-substrate E2E prober (dwl/libwayland readiness).
 *
 * Pure Linux-x86_64 ABI, freestanding. Stages:
 *   1. eventfd2: write->counter, read->counter, EOF-less drain,
 *      poll readiness via ppoll.
 *   2. timerfd: one-shot fires within ~120 ms; periodic counts.
 *   3. readv/writev: gather/scatter through a pipe (libwayland shape).
 *   4. memfd + ftruncate + MAP_SHARED fd-backed mmap; fork shares
 *      the frames (wl_shm semantics: child writes, parent reads).
 *   5. getrandom: returns the requested byte count, bytes change.
 *   6. futex: WAIT on mismatched value -> -EAGAIN (busy path).
 *   7. epoll over the REAL numbers (232/233) on eventfd+timerfd.
 *
 * exit(0) == all stages passed.
 */

#define SYS_read      0
#define SYS_write     1
#define SYS_close     3
#define SYS_fork      57
#define SYS_wait4     61
#define SYS_exit      60
#define SYS_readv     19
#define SYS_writev    20
#define SYS_ftruncate 77
#define SYS_mmap      9
#define SYS_munmap    11
#define SYS_pipe      22
#define SYS_poll      7
#define SYS_epoll_ctl     233
#define SYS_epoll_wait    232
#define SYS_epoll_create1 291
#define SYS_eventfd2      290
#define SYS_timerfd_create  253
#define SYS_timerfd_settime 254
#define SYS_getrandom   318
#define SYS_futex       202
#define SYS_ppoll       271
#define SYS_memfd_create 319

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define POLLIN     0x0001
#define EPOLLIN    0x1
#define EPOLL_CTL_ADD 1
#define EFD_NONBLOCK  0x800

struct iovec_k { void* base; unsigned long len; };
struct itimerspec_k {
    struct { long sec, nsec; } interval;
    struct { long sec, nsec; } value;
};

static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}
static long sc3(long nr, long a, long b, long c) {
    return sc6(nr, a, b, c, 0, 0, 0);
}
static long sc2(long nr, long a, long b) { return sc3(nr, a, b, 0); }
static long sc1(long nr, long a) { return sc3(nr, a, 0, 0); }

static void wr(const char* s, long n) { sc3(SYS_write, 1, (long)s, n); }
static void say(const char* s) { long n = 0; while (s[n]) n++; wr(s, n); }
static void sayu(const char* s, long v) {
    char b[32]; long n = 0;
    while (s[n]) n++;
    for (long i = 0; i < n; i++) b[i] = s[i];
    /* unsigned decimal */
    char d[24]; long di = 0;
    if (v == 0) d[di++] = '0';
    while (v > 0) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
    for (long i = di - 1; i >= 0; i--) b[n++] = d[i];
    b[n++] = '\n';
    wr(b, n);
}

int main(void) {
    say("[WLTEST] start\n");

    /* ---- 1. eventfd2 ------------------------------------------------ */
    {
        long e = sc3(SYS_eventfd2, 0, EFD_NONBLOCK, 0);
        if (e < 0) { say("[WLTEST] t1 FAIL eventfd\n"); return 1; }
        unsigned long v = 42;
        if (sc3(SYS_write, e, (long)&v, 8) != 8) {
            say("[WLTEST] t1 FAIL write\n"); return 1;
        }
        unsigned long r = 0;
        if (sc3(SYS_read, e, (long)&r, 8) != 8 || r != 42) {
            say("[WLTEST] t1 FAIL read\n"); return 1;
        }
        /* empty + nonblock -> -EAGAIN (-11) */
        long n = sc3(SYS_read, e, (long)&r, 8);
        if (n != -11) { say("[WLTEST] t1 FAIL eagain\n"); return 1; }
        /* poll readiness through ppoll (zero timespec = probe) */
        struct { int fd; short events, revents; } pfd =
            { (int)e, POLLIN, 0 };
        struct { long sec, nsec; } ts0 = { 0, 0 };
        long pr = sc3(SYS_ppoll, (long)&pfd, 1, (long)&ts0);
        if (pr != 0) { say("[WLTEST] t1 FAIL ppoll-idle\n"); return 1; }
        v = 7;
        sc3(SYS_write, e, (long)&v, 8);
        pfd.revents = 0;
        pr = sc3(SYS_ppoll, (long)&pfd, 1, (long)&ts0);
        if (pr != 1 || !(pfd.revents & POLLIN)) {
            say("[WLTEST] t1 FAIL ppoll-ready\n"); return 1;
        }
        sc1(SYS_close, e);
        say("[WLTEST] t1 eventfd+ppoll PASS\n");
    }

    /* ---- 2. timerfd -------------------------------------------------- */
    {
        long t = sc3(SYS_timerfd_create, 1 /*MONOTONIC*/, EFD_NONBLOCK, 0);
        if (t < 0) { say("[WLTEST] t2 FAIL create\n"); return 1; }
        struct itimerspec_k its;
        its.interval.sec = 0; its.interval.nsec = 0;          /* one-shot */
        its.value.sec    = 0; its.value.nsec    = 120000000;  /* 120 ms   */
        if (sc6(SYS_timerfd_settime, t, 0, (long)&its, 0, 0, 0) != 0) {
            say("[WLTEST] t2 FAIL settime\n"); return 1;
        }
        unsigned long exp = 0;
        /* nonblocking read right away: no expiration yet */
        if (sc3(SYS_read, t, (long)&exp, 8) != -11) {
            say("[WLTEST] t2 FAIL not-early\n"); return 1;
        }
        /* blocking wait: poll until ready (cap ~2 s) */
        int ready = 0;
        for (int i = 0; i < 200; i++) {
            struct { int fd; short events, revents; } pfd =
                { (int)t, POLLIN, 0 };
            if (sc3(SYS_poll, (long)&pfd, 1, 20) == 1) { ready = 1; break; }
        }
        if (!ready) { say("[WLTEST] t2 FAIL no-fire\n"); return 1; }
        if (sc3(SYS_read, t, (long)&exp, 8) != 8 || exp != 1) {
            say("[WLTEST] t2 FAIL count\n"); return 1;
        }
        sc1(SYS_close, t);
        say("[WLTEST] t2 timerfd PASS\n");
    }

    /* ---- 3. readv/writev through a pipe ------------------------------ */
    {
        int pf[2];
        if (sc3(SYS_pipe, (long)pf, 0, 0)) {
            say("[WLTEST] t3 FAIL pipe\n"); return 1;
        }
        char a[5] = "AB", b[6] = "CDEF";
        struct iovec_k iov[2] = {
            { a, 2 }, { b, 4 },
        };
        if (sc6(SYS_writev, pf[1], (long)iov, 2, 0, 0, 0) != 6) {
            say("[WLTEST] t3 FAIL writev\n"); return 1;
        }
        char ra[8]; struct iovec_k rio[2] = {
            { ra, 3 }, { ra + 3, 5 },
        };
        long n = sc6(SYS_readv, pf[0], (long)rio, 2, 0, 0, 0);
        if (n != 6 || ra[0] != 'A' || ra[5] != 'F') {
            say("[WLTEST] t3 FAIL readv\n"); return 1;
        }
        sc1(SYS_close, pf[0]); sc1(SYS_close, pf[1]);
        say("[WLTEST] t3 readv/writev PASS\n");
    }

    /* ---- 4. memfd + shared mmap across fork (wl_shm shape) ----------- */
    {
        long m = sc3(SYS_memfd_create, (long)"wlshm", 0, 0);
        if (m < 0) { say("[WLTEST] t4 FAIL memfd\n"); return 1; }
        if (sc6(SYS_ftruncate, m, 8192, 0, 0, 0, 0) != 0) {
            say("[WLTEST] t4 FAIL ftruncate\n"); return 1;
        }
        unsigned char* p = (unsigned char*)
            sc6(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                m, 0);
        if ((long)p < 0 && (long)p > -4096) {
            say("[WLTEST] t4 FAIL mmap\n"); return 1;
        }
        long pid = sc3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            /* child maps the SAME pool and writes the pattern */
            unsigned char* c = (unsigned char*)
                sc6(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                    m, 0);
            if ((long)c < 0 && (long)c > -4096) sc3(SYS_exit, 1, 0, 0);
            for (int i = 0; i < 256; i++) c[i] = (unsigned char)(i * 3);
            sc3(SYS_exit, 7, 0, 0);
        }
        long st = 0;
        sc3(SYS_wait4, pid, (long)&st, 0);
        if ((st >> 8) != 7) { say("[WLTEST] t4 FAIL child\n"); return 1; }
        int ok = 1;
        for (int i = 0; i < 256; i++)
            if (p[i] != (unsigned char)(i * 3)) { ok = 0; break; }
        if (!ok) { say("[WLTEST] t4 FAIL share\n"); return 1; }
        sc3(SYS_munmap, (long)p, 8192, 0);
        sc1(SYS_close, m);
        say("[WLTEST] t4 memfd-shared-mmap PASS\n");
    }

    /* ---- 5. getrandom ------------------------------------------------- */
    {
        unsigned char r1[32], r2[32];
        long n1 = sc3(SYS_getrandom, (long)r1, 32, 0);
        long n2 = sc3(SYS_getrandom, (long)r2, 32, 0);
        if (n1 != 32 || n2 != 32) {
            say("[WLTEST] t5 FAIL count\n"); return 1;
        }
        int diff = 0;
        for (int i = 0; i < 32; i++) if (r1[i] != r2[i]) diff++;
        if (diff < 8) { say("[WLTEST] t5 FAIL entropy\n"); return 1; }
        say("[WLTEST] t5 getrandom PASS\n");
    }

    /* ---- 6. futex WAIT on mismatch -> EAGAIN -------------------------- */
    {
        volatile unsigned u = 123;
        long r = sc6(SYS_futex, (long)&u, 0 /*WAIT*/, 456, 0, 0, 0);
        if (r != -11) { say("[WLTEST] t6 FAIL\n"); return 1; }
        say("[WLTEST] t6 futex PASS\n");
    }

    /* ---- 7. epoll over REAL numbers on eventfd + timerfd -------------- */
    {
        long ep = sc3(SYS_epoll_create1, 0, 0, 0);
        if (ep < 0) { say("[WLTEST] t7 FAIL create1\n"); return 1; }
        long e = sc3(SYS_eventfd2, 0, EFD_NONBLOCK, 0);
        long t = sc3(SYS_timerfd_create, 1, EFD_NONBLOCK, 0);
        struct { unsigned events; unsigned long data; } ev;
        ev.events = EPOLLIN; ev.data = 0xE1;
        if (sc6(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, e, (long)&ev, 0, 0)) {
            say("[WLTEST] t7 FAIL ctl-efd\n"); return 1;
        }
        ev.events = EPOLLIN; ev.data = 0xE2;
        if (sc6(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, t, (long)&ev, 0, 0)) {
            say("[WLTEST] t7 FAIL ctl-tfd\n"); return 1;
        }
        struct { unsigned events; unsigned long data; } out[4];
        /* idle: nothing ready, timeout 0 -> 0 events */
        long n = sc6(SYS_epoll_wait, ep, (long)out, 4, 0, 0, 0);
        if (n != 0) { say("[WLTEST] t7 FAIL spurious\n"); return 1; }
        /* arm both sources; epoll returns readiness AS IT APPEARS
         * (eventfd is ready immediately, timerfd 10 ms later — two
         * wakeups, exactly how libwayland consumes them). */
        unsigned long v = 5;
        sc3(SYS_write, e, (long)&v, 8);
        struct itimerspec_k its;
        its.interval.sec = 0; its.interval.nsec = 0;
        its.value.sec = 0; its.value.nsec = 10000000;   /* 10 ms */
        sc6(SYS_timerfd_settime, t, 0, (long)&its, 0, 0, 0);
        n = sc6(SYS_epoll_wait, ep, (long)out, 4, 1000, 0, 0);
        if (n != 1 || out[0].data != 0xE1) {
            sayu("[WLTEST] t7 FAIL wait-efd", n); return 1;
        }
        /* drain the eventfd, then wait for the timerfd wakeup */
        unsigned long d = 0;
        sc3(SYS_read, e, (long)&d, 8);
        n = sc6(SYS_epoll_wait, ep, (long)out, 4, 1000, 0, 0);
        if (n != 1 || out[0].data != 0xE2) {
            sayu("[WLTEST] t7 FAIL wait-tfd", n); return 1;
        }
        unsigned long ec = 0;
        sc3(SYS_read, t, (long)&ec, 8);
        if (ec != 1) { say("[WLTEST] t7 FAIL tfd-count\n"); return 1; }
        sc1(SYS_close, ep); sc1(SYS_close, e); sc1(SYS_close, t);
        say("[WLTEST] t7 epoll-real-abi PASS\n");
    }

    say("[WLTEST] ALL-PASS\n");
    return 0;
}
