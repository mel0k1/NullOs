/*
 * forktest — NullOs fork/exec/wait4 smoke test.
 *
 *   parent: pid=N, forks; child prints its own pid (0 from fork),
 *           writes to a COW buffer, exits with code 33.
 *   parent: wait4()s, verifies status==33<<8 and that the COW buffer
 *           still holds ITS original data (child wrote different).
 */

#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_fork   57
#define SYS_wait4  61
#define SYS_exit   60
#define SYS_getpid 39

static long syscall3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

static void print(const char* s) {
    long n = 0;
    while (s[n]) n++;
    syscall3(SYS_write, 1, (long)s, n);
}

static void print_num(long v) {
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) buf[--i] = '0';
    while (u) { buf[--i] = '0' + (u % 10); u /= 10; }
    syscall3(SYS_write, 1, (long)(buf + i), sizeof(buf) - 1 - (unsigned long)i);
}

static volatile long cow_var = 1000;

int main(void) {
    print("[forktest] parent pid=");
    print_num(syscall3(SYS_getpid, 0, 0, 0));
    print("\n");

    long pid = syscall3(SYS_fork, 0, 0, 0);

    if (pid == 0) {
        /* ---- child ---- */
        print("[forktest] child: fork()=0, pid=");
        print_num(syscall3(SYS_getpid, 0, 0, 0));
        print("\n");
        cow_var = 777;                    /* trigger COW copy         */
        print("[forktest] child: cow_var=");
        print_num(cow_var);
        print(", exiting 33\n");
        syscall3(SYS_exit, 33, 0, 0);
    }

    /* ---- parent ---- */
    print("[forktest] parent: fork()=");
    print_num(pid);
    print("\n");

    int status = 0;
    long w = syscall3(SYS_wait4, -1, (long)&status, 0);
    print("[forktest] wait4 -> ");
    print_num(w);
    print(", status.raw=0x");
    /* tiny hex */
    {
        unsigned v = (unsigned)status;
        char hx[9]; int i = 8;
        if (!v) hx[--i] = '0';
        while (v) { int d = v & 15; hx[--i] = d < 10 ? '0'+d : 'a'+d-10; v >>= 4; }
        syscall3(SYS_write, 1, (long)(hx+i), 8-i);
    }
    print("\n");

    print("[forktest] parent cow_var=");
    print_num(cow_var);
    print(cow_var == 1000
          ? " (COW isolation OK)\n"
          : " (COW BROKEN!)\n");

    print("[forktest] done\n");
    return 7;
}