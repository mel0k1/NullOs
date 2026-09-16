/*
 * unixtest — NullOs AF_UNIX transport prober (#wayland-transport).
 *
 * Pure Linux-x86_64 ABI, freestanding. Four independent stages:
 *   1. socketpair(): bidirectional data + close-side EOF
 *   2. SCM_RIGHTS: fd passing through sendmsg/recvmsg across fork()
 *   3. epoll (create1/ctl/wait): readiness on a channel end
 *   4. path sockets: bind/listen/accept/connect echo via fork()
 *
 * exit(0) == every stage passed. One bad stage prints FAIL and exits
 * non-zero — the harness greps the serial/VGA markers.
 */

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_fork    57
#define SYS_wait4   61
#define SYS_exit    60
#define SYS_socket  41
#define SYS_connect 42
#define SYS_accept  43
#define SYS_sendmsg 46
#define SYS_recvmsg 47
#define SYS_bind    49
#define SYS_listen  50
#define SYS_socketpair 53
#define SYS_epoll_create1 291
#define SYS_epoll_ctl     233    /* real x86_64 ABI (was probe-only 290) */
#define SYS_epoll_wait    232    /* real x86_64 ABI */

#define AF_UNIX     1
#define SOCK_STREAM 1
#define EPOLL_CTL_ADD 1
#define EPOLLIN     0x1

#define O_WRONLY    1
#define O_CREAT     0x40
#define O_TRUNC     0x200

struct iovec_k   { void* base; unsigned long len; };
struct msghdr_k  {
    void* name; unsigned namelen;
    struct iovec_k* iov; unsigned long iovlen;
    void* control; unsigned long controllen;
    int flags;
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

static void wr(const char* s, long n) { sc3(SYS_write, 1, (long)s, n); }
static void say(const char* s) { long n = 0; while (s[n]) n++; wr(s, n); }

static int streq(const char* a, const char* b, long n) {
    for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ---- cmsg builders (x86_64: cmsghdr = {size_t len; int level; int type}) */
static void put_cmsg_fd(void* ctl, unsigned long cap, int fd,
                        unsigned long* out_len) {
    (void)cap;
    unsigned long* plen  = (unsigned long*)ctl;
    int*           plvl  = (int*)((char*)ctl + 8);
    int*           ptyp  = (int*)((char*)ctl + 12);
    int*           pdata = (int*)((char*)ctl + 16);
    *plen = 20; *plvl = 1 /*SOL_SOCKET*/; *ptyp = 1 /*SCM_RIGHTS*/;
    *pdata = fd;
    *out_len = 24;
}

int main(void) {
    say("[UXTEST] start\n");
    long status = 0;

    /* ---- TEST 1: socketpair data + EOF ------------------------------ */
    {
        int sv[2];   /* POSIX: int[2] — kernel writes 32-bit slots    */
        if (sc6(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0,
                (long)sv, 0, 0) != 0) {
            say("[UXTEST] t1 FAIL socketpair\n"); return 1;
        }
        if (sc3(SYS_write, sv[0], (long)"ping", 4) != 4) {
            say("[UXTEST] t1 FAIL write\n"); return 1;
        }
        char b[16];
        long n = sc3(SYS_read, sv[1], (long)b, 16);
        if (n != 4 || !streq(b, "ping", 4)) {
            say("[UXTEST] t1 FAIL read\n"); return 1;
        }
        if (sc3(SYS_write, sv[1], (long)"pong", 4) != 4 ||
            sc3(SYS_read, sv[0], (long)b, 16) != 4 || !streq(b, "pong", 4)) {
            say("[UXTEST] t1 FAIL reverse\n"); return 1;
        }
        sc3(SYS_close, sv[1], 0, 0);
        n = sc3(SYS_read, sv[0], (long)b, 16);
        if (n != 0) {
            say("[UXTEST] t1 FAIL EOF\n"); return 1;
        }
        sc3(SYS_close, sv[0], 0, 0);
        say("[UXTEST] t1 socketpair+EOF PASS\n");
    }

    /* ---- TEST 2: SCM_RIGHTS fd passing across fork ------------------ */
    {
        long fd = sc3(SYS_open,
                      (long)"/tmp/uxfd.txt",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { say("[UXTEST] t2 FAIL open\n"); return 1; }
        int p[2];
        if (sc6(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, (long)p, 0, 0)) {
            say("[UXTEST] t2 FAIL socketpair\n"); return 1;
        }
        long pid = sc3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            /* child: receive the fd, write through it, exit(7)      */
            char ctl[64]; char buf[32];
            for (int i = 0; i < 64; i++) ctl[i] = 0;
            struct iovec_k iov  = { buf, 16 };
            struct msghdr_k mh;
            mh.name = 0; mh.namelen = 0;
            mh.iov = &iov; mh.iovlen = 1;
            mh.control = ctl; mh.controllen = 64; mh.flags = 0;
            long n = sc6(SYS_recvmsg, p[1], (long)&mh, 0, 0, 0, 0);
            if (n < 2 || mh.controllen < 20) sc3(SYS_exit, 1, 0, 0);
            int nfd = *(int*)(ctl + 16);
            sc3(SYS_write, nfd, (long)"fd-pass-child", 13);
            sc3(SYS_close, nfd, 0, 0);
            sc3(SYS_exit, 7, 0, 0);
        }
        /* parent: send the fd, close our copy, reap, verify file     */
        char ctl[32];
        unsigned long clen = 0;
        put_cmsg_fd(ctl, 32, (int)fd, &clen);
        struct iovec_k iov = { (void*)"FD", 2 };
        struct msghdr_k mh;
        mh.name = 0; mh.namelen = 0;
        mh.iov = &iov; mh.iovlen = 1;
        mh.control = ctl; mh.controllen = clen; mh.flags = 0;
        if (sc6(SYS_sendmsg, p[0], (long)&mh, 0, 0, 0, 0) != 2) {
            say("[UXTEST] t2 FAIL sendmsg\n"); return 1;
        }
        sc3(SYS_close, fd, 0, 0);
        sc3(SYS_wait4, pid, (long)&status, 0);
        if ((status >> 8) != 7) {
            say("[UXTEST] t2 FAIL child exit\n"); return 1;
        }
        long rfd = sc3(SYS_open, (long)"/tmp/uxfd.txt", O_WRONLY * 0, 0);
        char rb[32];
        long n = sc3(SYS_read, rfd, (long)rb, 32);
        sc3(SYS_close, rfd, 0, 0);
        if (n != 13 || !streq(rb, "fd-pass-child", 13)) {
            say("[UXTEST] t2 FAIL verify\n"); return 1;
        }
        sc3(SYS_close, p[0], 0, 0);
        sc3(SYS_close, p[1], 0, 0);
        say("[UXTEST] t2 scm_rights PASS\n");
    }

    /* ---- TEST 3: epoll readiness on a channel end ------------------- */
    {
        int sv[2];
        if (sc6(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, (long)sv, 0, 0)) {
            say("[UXTEST] t3 FAIL socketpair\n"); return 1;
        }
        long ep = sc3(SYS_epoll_create1, 0, 0, 0);
        if (ep < 0) { say("[UXTEST] t3 FAIL create1\n"); return 1; }
        char ev[16];                       /* {u32 events; u64 data}   */
        *(unsigned*)ev = EPOLLIN;
        *(long*)(ev + 8) = 0xA5A5;
        if (sc6(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, sv[0], (long)ev, 0, 0)) {
            say("[UXTEST] t3 FAIL ctl\n"); return 1;
        }
        char out[32];
        long n = sc6(SYS_epoll_wait, ep, (long)out, 4, 0, 0, 0);
        if (n != 0) { say("[UXTEST] t3 FAIL spurious\n"); return 1; }
        sc3(SYS_write, sv[1], (long)"ep", 2);
        n = sc6(SYS_epoll_wait, ep, (long)out, 4, 1000, 0, 0);
        if (n != 1 || *(long*)(out + 8) != 0xA5A5) {
            say("[UXTEST] t3 FAIL wait\n"); return 1;
        }
        sc3(SYS_close, ep, 0, 0);
        sc3(SYS_close, sv[0], 0, 0);
        sc3(SYS_close, sv[1], 0, 0);
        say("[UXTEST] t3 epoll PASS\n");
    }

    /* ---- TEST 4: path socket bind/listen/accept/connect ------------- */
    {
        long s = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
        if (s < 0) { say("[UXTEST] t4 FAIL socket\n"); return 1; }
        char sa[112];
        *(unsigned short*)sa = AF_UNIX;
        for (int i = 0; i < 108; i++) sa[2 + i] = 0;
        const char* path = "/tmp/wl-0";
        long i2 = 0;
        for (; path[i2]; i2++) sa[2 + i2] = path[i2];
        if (sc6(SYS_bind, s, (long)sa, 110, 0, 0, 0)) {
            say("[UXTEST] t4 FAIL bind\n"); return 1;
        }
        if (sc6(SYS_listen, s, 4, 0, 0, 0, 0)) {
            say("[UXTEST] t4 FAIL listen\n"); return 1;
        }
        long pid = sc3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            long c = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
            if (c < 0) sc3(SYS_exit, 1, 0, 0);
            if (sc6(SYS_connect, c, (long)sa, 110, 0, 0, 0)) sc3(SYS_exit, 2, 0, 0);
            if (sc3(SYS_write, c, (long)"path-ok", 7) != 7) sc3(SYS_exit, 3, 0, 0);
            char b[16];
            long n = sc3(SYS_read, c, (long)b, 16);
            if (n != 9 || !streq(b, "PATH-ECHO", 9)) sc3(SYS_exit, 4, 0, 0);
            sc3(SYS_exit, 9, 0, 0);
        }
        long a = sc6(SYS_accept, s, 0, 0, 0, 0, 0);
        if (a < 0) { say("[UXTEST] t4 FAIL accept\n"); return 1; }
        char b[16];
        long n = sc3(SYS_read, a, (long)b, 16);
        if (n != 7 || !streq(b, "path-ok", 7)) {
            say("[UXTEST] t4 FAIL data\n"); return 1;
        }
        sc3(SYS_write, a, (long)"PATH-ECHO", 9);
        sc3(SYS_close, a, 0, 0);
        sc3(SYS_wait4, pid, (long)&status, 0);
        if ((status >> 8) != 9) {
            say("[UXTEST] t4 FAIL child exit\n"); return 1;
        }
        sc3(SYS_close, s, 0, 0);
        say("[UXTEST] t4 path-socket PASS\n");
    }

    say("[UXTEST] ALL-PASS\n");
    return 0;
}
