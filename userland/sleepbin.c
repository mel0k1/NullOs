/*
 * sleep — minimal NullOs native applet (job-control test companion).
 * usage: sleep SECONDS
 */

#define SYS_write     1
#define SYS_nanosleep 35
#define SYS_exit      60

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

int main(int argc, char** argv) {
    if (argc < 2) {
        print("usage: sleep SECONDS\n");
        return 1;
    }
    /* tiny integer parse */
    long sec = 0;
    for (const char* p = argv[1]; *p >= '0' && *p <= '9'; p++)
        sec = sec * 10 + (*p - '0');
    struct { long tv_sec; long tv_nsec; } ts;
    while (sec > 0) {
        ts.tv_sec  = (sec > 60) ? 60 : sec;   /* chunk max 1 min    */
        ts.tv_nsec = 0;
        sec -= ts.tv_sec;
        syscall3(SYS_nanosleep, (long)&ts, 0, 0);
    }
    return 0;
}
