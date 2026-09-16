/*
 * hello — NullOs Linux-ABI test program.
 *
 * Standard Linux x86_64 syscalls only:
 *   write(1,...)=1  read(0,...)=0  open(2)  close(3)  brk(12)
 */

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_brk    12

#define O_RDONLY   0
#define O_WRONLY   1
#define O_CREAT    0100

static long syscall3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long write1(const char* s, unsigned long len) {
    return syscall3(SYS_write, 1, (long)s, (long)len);
}

static void print(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    write1(s, n);
}

static void print_num(long v) {
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;

    if (u == 0) buf[--i] = '0';
    while (u > 0) { buf[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[--i] = '-';

    write1(buf + i, sizeof(buf) - 1 - (unsigned long)i);
}

int main(int argc, char** argv) {
    print("=== NullOs Linux-ABI test ===\n");

    print("argc = ");
    print_num(argc);
    print("\n");

    for (int i = 0; i < argc; i++) {
        print("argv[");
        print_num(i);
        print("] = \"");
        print(argv[i]);
        print("\"\n");
    }

    /* ---- heap via brk(12): grow, poke, verify ------------------- */
    long cur = syscall3(SYS_brk, 0, 0, 0);
    print("\n[brk] current break = 0x");
    print_num(cur);

    long want = cur + 4096;
    long nb = syscall3(SYS_brk, want, 0, 0);
    if (nb == cur) {
        print("\n[brk] FAILED to grow\n");
    } else {
        print("\n[brk] grown to 0x");
        print_num(nb);

        volatile unsigned char* hp = (volatile unsigned char*)cur;
        hp[0] = 'N'; hp[1] = 'X'; hp[4095] = '!';
        if (hp[0] == 'N' && hp[1] == 'X' && hp[4095] == '!') {
            print("\n[brk] memory r/w verified OK");
        } else {
            print("\n[brk] VERIFY MISMATCH");
        }
    }

    /* ---- file WRITE via open(O_CREAT|O_WRONLY)/write/close -------- */
    long wfd = syscall3(SYS_open, (long)"/tmp/ring3.txt",
                        (long)(O_WRONLY | O_CREAT), 0644);
    print("\n[file] open(/tmp/ring3.txt, O_CREAT|O_WRONLY) -> fd ");
    print_num(wfd);

    if (wfd >= 0) {
        long wn = syscall3(SYS_write, wfd, (long)"written from ring 3!\n", 21);
        print("\n[file] write() -> ");
        print_num(wn);
        print(" bytes");
        syscall3(SYS_close, wfd, 0, 0);

        /* Read back and verify */
        long rfd = syscall3(SYS_open, (long)"/tmp/ring3.txt", O_RDONLY, 0);
        if (rfd >= 0) {
            char vbuf[32];
            long rn = syscall3(SYS_read, rfd, (long)vbuf, 31);
            vbuf[rn > 0 ? rn : 0] = '\0';
            print("\n[file] verify: \"");
            if (rn > 0) write1(vbuf, (unsigned long)rn);
            print("\"");
            syscall3(SYS_close, rfd, 0, 0);
        }
    }

    /* ---- file I/O via open(2)/read/close(3) --------------------- */
    long fd = syscall3(SYS_open, (long)"/hello.txt", O_RDONLY, 0);
    print("\n\n[file] open(/hello.txt) -> fd ");
    print_num(fd);

    if (fd >= 0) {
        char fbuf[65];
        long n = syscall3(SYS_read, fd, (long)fbuf, 64);
        print("\n[file] read() -> ");
        print_num(n);
        print(" bytes: \"");
        if (n > 0) write1(fbuf, (unsigned long)n);
        print("\"\n");

        long cr = syscall3(SYS_close, fd, 0, 0);
        print("[file] close() -> ");
        print_num(cr);
        print("\n");
    }

    print("\nBye from ring 3!\n");
    return 42;
}
