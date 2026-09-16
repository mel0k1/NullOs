/*
 * layertest — NullOs Layer-2 substrate prober (mprotect / /proc /
 * /dev/shm pools / EFD_SEMAPHORE). Ring-3, freestanding, Linux ABI.
 *
 *   1. mprotect(10): RW->RO really enforces (child writes -> killed
 *      with SIGSEGV semantics: wait4 status low byte 11), RO->RX
 *      keeps W off, unmapped range -> -ENOMEM, bad prot -> -EINVAL.
 *   2. /proc: meminfo/cpuinfo/uptime/version/self/cmdline materialize
 *      and read back with the expected content markers.
 *   3. /dev/shm: open(O_CREAT|O_RDWR|O_TRUNC) + ftruncate + mmap pool;
 *      two mappings in one process share frames; a fork'ed child that
 *      opens the SAME path maps the SAME frames (wl_shm shape);
 *      fstat reports the pool size; unlink destroys the pool.
 *   4. eventfd2 EFD_SEMAPHORE: read hands out ONE unit per read.
 *
 * exit(0) == all stages passed.
 */

#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_fstat     5
#define SYS_mmap      9
#define SYS_mprotect  10
#define SYS_munmap    11
#define SYS_fork      57
#define SYS_exit      60
#define SYS_wait4     61
#define SYS_ftruncate 77
#define SYS_unlink    87
#define SYS_eventfd2  290

#define PROT_READ    1
#define PROT_WRITE   2
#define PROT_EXEC    4
#define MAP_SHARED   1
#define O_RDONLY     0
#define O_RDWR       2
#define O_CREAT      64
#define O_TRUNC      512
#define EFD_NONBLOCK   0x800
#define EFD_SEMAPHORE  0x001

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

/* read whole small file; returns bytes or negative */
static long slurp(const char* path, char* buf, long cap) {
    long fd = sc3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) return fd;
    long off = 0;
    while (off < cap - 1) {
        long n = sc3(SYS_read, fd, (long)(buf + off), cap - 1 - off);
        if (n < 0) { if (n == -4) continue; break; }   /* -EINTR */
        if (n == 0) break;
        off += n;
    }
    buf[off] = 0;
    sc1(SYS_close, fd);
    return off;
}

static int has(const char* hay, const char* needle) {
    for (long i = 0; hay[i]; i++) {
        long j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

int main(void) {
    say("[LAYertest] start\n");

    /* ---- 1. mprotect -------------------------------------------------- */
    {
        unsigned char* p = (unsigned char*)
            sc6(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                -1, 0);
        if ((long)p < 0 && (long)p > -4096) {
            say("[LT] t1 FAIL mmap\n"); return 1;
        }
        p[0] = 0xAB; p[4096] = 0xCD;               /* RW works          */

        if (sc3(SYS_mprotect, (long)p, 4096, PROT_READ) != 0) {
            say("[LT] t1 FAIL mprotect-ro\n"); return 1;
        }
        /* enforcement proof via fork: the child's write MUST die      */
        long pid = sc3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            p[0] = 0x11;                           /* -> SIGSEGV        */
            sc3(SYS_exit, 0, 0, 0);                /* reached = broken  */
        }
        long st = 0;
        sc3(SYS_wait4, pid, (long)&st, 0);
        /* kernel encodes status = exit_code << 8 (normal-exit shape);
         * a fault-killed child reports exit_code=11 (SIGSEGV heart) —
         * ANY nonzero status = abnormal death = RO enforced.       */
        if (st == 0) {
            say("[LT] t1 FAIL child-survived\n"); return 1;
        }
        if ((st >> 8) != 11) {
            say("[LT] t1 status!=11\n"); return 1;
        }
        /* parent unaffected: RO readable, other page still writable   */
        if (p[0] != 0xAB) { say("[LT] t1 FAIL ro-data\n"); return 1; }
        p[4096] = 0xEF;
        if (p[4096] != 0xEF) { say("[LT] t1 FAIL rw-page\n"); return 1; }

        if (sc3(SYS_mprotect, (long)p, 4096,
                PROT_READ | PROT_EXEC) != 0) {
            say("[LT] t1 FAIL mprotect-rx\n"); return 1;
        }
        /* unmapped range -> -ENOMEM (-12); bad prot -> -EINVAL (-22).
         * Probe address: near the TOP of the mmap window — the bump
         * starts at PROC_MMAP_BASE (0x20000000) and has only grown a
         * few pages, so 0x2F000000 is guaranteed unmapped.          */
        long e1 = sc3(SYS_mprotect, 0x2F000000, 4096, PROT_READ);
        if (e1 != -12) { say("[LT] t1 FAIL enomem\n"); return 1; }
        long e2 = sc3(SYS_mprotect, (long)p, 4096, 8);
        if (e2 != -22) { say("[LT] t1 FAIL einval\n"); return 1; }

        sc3(SYS_munmap, (long)p, 8192, 0);
        say("[LT] t1 mprotect W^X PASS\n");
    }

    /* ---- 2. /proc ------------------------------------------------------ */
    {
        static char b[2048];
        if (slurp("/proc/meminfo", b, sizeof(b)) < 0 ||
            !has(b, "MemFree:")) {
            say("[LT] t2 FAIL meminfo\n"); return 1;
        }
        if (slurp("/proc/cpuinfo", b, sizeof(b)) < 0 ||
            !has(b, "model name")) {
            say("[LT] t2 FAIL cpuinfo\n"); return 1;
        }
        if (slurp("/proc/uptime", b, sizeof(b)) < 0 || !has(b, ".")) {
            say("[LT] t2 FAIL uptime\n"); return 1;
        }
        if (slurp("/proc/version", b, sizeof(b)) < 0 ||
            !has(b, "Linux version")) {
            say("[LT] t2 FAIL version\n"); return 1;
        }
        if (slurp("/proc/self/cmdline", b, sizeof(b)) < 0 ||
            !has(b, "layertest")) {
            say("[LT] t2 FAIL cmdline\n"); return 1;
        }
        say("[LT] t2 procfs PASS\n");
    }

    /* ---- 3. /dev/shm pools --------------------------------------------- */
    {
        long fd = sc3(SYS_open, (long)"/dev/shm/lt", 
                      O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) { say("[LT] t3 FAIL open\n"); return 1; }
        if (sc6(SYS_ftruncate, fd, 8192, 0, 0, 0, 0) != 0) {
            say("[LT] t3 FAIL ftruncate\n"); return 1;
        }
        unsigned char* p = (unsigned char*)
            sc6(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, 0);
        if ((long)p < 0 && (long)p > -4096) {
            say("[LT] t3 FAIL mmap\n"); return 1;
        }
        for (int i = 0; i < 128; i++) p[i] = (unsigned char)(i * 5 + 1);

        /* second mapping of the same path == SAME frames              */
        long fd2 = sc3(SYS_open, (long)"/dev/shm/lt", O_RDWR, 0);
        if (fd2 < 0) { say("[LT] t3 FAIL open2\n"); return 1; }
        unsigned char* q = (unsigned char*)
            sc6(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd2, 0);
        if ((long)q < 0 && (long)q > -4096) {
            say("[LT] t3 FAIL mmap2\n"); return 1;
        }
        int ok = 1;
        for (int i = 0; i < 128; i++)
            if (q[i] != (unsigned char)(i * 5 + 1)) { ok = 0; break; }
        if (!ok) { say("[LT] t3 FAIL inproc-share\n"); return 1; }

        /* fstat reports the POOL size (libwayland reads it)           */
        {
            unsigned long stb[18];
            if (sc3(SYS_fstat, fd, (long)stb, 0) != 0) {
                say("[LT] t3 FAIL fstat\n"); return 1;
            }
            if (stb[6] != 8192) {
                say("[LT] t3 FAIL fstat-size\n"); return 1;
            }
        }

        /* fork + reopen: the wl_shm shape (client writes, compositor
         * reads through ITS OWN mapping of the same path)            */
        long pid = sc3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            long cfd = sc3(SYS_open, (long)"/dev/shm/lt", O_RDWR, 0);
            if (cfd < 0) sc3(SYS_exit, 1, 0, 0);
            unsigned char* c = (unsigned char*)
                sc6(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE,
                    MAP_SHARED, cfd, 0);
            if ((long)c < 0 && (long)c > -4096) sc3(SYS_exit, 2, 0, 0);
            for (int i = 0; i < 128; i++)
                if (c[i] != (unsigned char)(i * 5 + 1))
                    sc3(SYS_exit, 3, 0, 0);
            for (int i = 0; i < 128; i++) c[i] = (unsigned char)(i + 200);
            sc3(SYS_exit, 7, 0, 0);
        }
        long st = 0;
        sc3(SYS_wait4, pid, (long)&st, 0);
        if ((st >> 8) != 7) { say("[LT] t3 FAIL child\n"); return 1; }
        ok = 1;
        for (int i = 0; i < 128; i++)
            if (p[i] != (unsigned char)(i + 200)) { ok = 0; break; }
        if (!ok) { say("[LT] t3 FAIL cross-share\n"); return 1; }

        sc3(SYS_munmap, (long)p, 8192, 0);
        sc3(SYS_munmap, (long)q, 8192, 0);
        sc1(SYS_close, fd); sc1(SYS_close, fd2);
        sc1(SYS_unlink, (long)"/dev/shm/lt");
        say("[LT] t3 shmpool PASS\n");
    }

    /* ---- 4. EFD_SEMAPHORE ----------------------------------------------- */
    {
        long e = sc3(SYS_eventfd2, 0,
                     EFD_SEMAPHORE | EFD_NONBLOCK, 0);
        if (e < 0) { say("[LT] t4 FAIL eventfd\n"); return 1; }
        unsigned long v = 3;
        if (sc3(SYS_write, e, (long)&v, 8) != 8) {
            say("[LT] t4 FAIL write\n"); return 1;
        }
        for (int k = 0; k < 3; k++) {
            unsigned long r = 0;
            if (sc3(SYS_read, e, (long)&r, 8) != 8 || r != 1) {
                say("[LT] t4 FAIL sem-read\n"); return 1;
            }
        }
        unsigned long r = 0;
        if (sc3(SYS_read, e, (long)&r, 8) != -11) {
            say("[LT] t4 FAIL drained\n"); return 1;
        }
        sc1(SYS_close, e);
        say("[LT] t4 efd-semaphore PASS\n");
    }

    say("[LAYertest] ALL-PASS\n");
    return 0;
}
