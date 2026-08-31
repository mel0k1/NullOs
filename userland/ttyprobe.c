/* ttyprobe.c — replicates BusyBox ash setjobctl() probe sequence and
 * prints the raw errno of each step, so a failing ioctl/fcntl can be
 * pinpointed on serial/VGA without guessing. Output via write(1). */

#define SYS_write   1
#define SYS_ioctl  16
#define SYS_fcntl  72
#define SYS_getpgrp 111
#define SYS_setpgid 109

static long syscall3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(nr), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

struct tterm { unsigned int f[4]; unsigned char line; unsigned char cc[32];
               unsigned int sp[2]; };

static void out(const char* s) {
    long n = 0; while (s[n]) n++;
    syscall3(SYS_write, 1, (long)s, n);
}

static void hexl(long v) {
    char b[24]; int i = 0;
    if (v < 0) { v = -v; b[i++] = '-'; }
    b[i++] = '0'; b[i++] = 'x';
    int started = 0;
    for (int d = 15; d >= 0; d--) {
        long nib = (v >> (d * 4)) & 0xF;
        if (!nib && !started && d > 0) continue;
        started = 1;
        b[i++] = nib < 10 ? ('0' + nib) : ('A' + nib - 10);
    }
    if (v == 0) b[i++] = '0';
    syscall3(SYS_write, 1, (long)b, i);
}

static long syscall5(long nr, long a, long b, long c, long d, long e) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                      : "rcx", "r11", "memory");
    return r;
}

int main(void) {
    struct tterm t;

    out("[P0] open(/dev/tty): ");
    hexl(syscall3(2, (long)"/dev/tty", 2, 0));       /* O_RDWR */
    out("\n");

    out("[P0b] getppid/openat-style probe skipped. TIOCGWINSZ(0): ");
    unsigned int ws[4];
    hexl(syscall3(16, 0, 0x5413, (long)&ws));
    out("\n");


    out("[P1] getpgrp: ");      hexl(syscall3(111,0,0,0));          out("\n");
    out("[P2] setpgid(0,0): "); hexl(syscall3(109,0,0,0));          out("\n");
    out("[P3] fcntl0 DUPFD10:");hexl(syscall3(72,0,1030L,10));       out("\n");

    long fd = syscall3(72, 0, 1030L, 10);

    out("[P4] ioctl fd TCGETS: ");
    if (fd < 0) { out("no-fd"); }
    else hexl(syscall3(16, fd, 0x5401, (long)&t));
    out("\n");

    out("[P5] ioctl fd TCGETSW? TIOCGPGRP: ");
    if (fd < 0) { out("no-fd"); }
    else hexl(syscall3(16, fd, 0x540F, (long)&t));
    out("\n");

    out("[P6] ioctl fd TCSETSW: ");
    if (fd < 0) { out("no-fd"); }
    else hexl(syscall3(16, fd, 0x5403, (long)&t));
    out("\n");

    out("[P7] ioctl 0 TIOCSCTTY: ");
    hexl(syscall3(16, 0, 0x540E, 0));
    out("\n");

    out("[P8] ioctl 0 TIOCSPGRP(self): ");
    long me = syscall3(111, 0, 0, 0);
    hexl(syscall3(16, 0, 0x5410, (long)&me));
    out("\n");

    out("[P9] getpgrp again: "); hexl(syscall3(111, 0, 0, 0));        out("\n");

    
    syscall3(231 + 60 - 60, 60, 42, 0);      /* SYS_exit=60 via syscall3 */
    for (;;) { }
}
