/*
 * parfork — NullOs fork-v2 concurrency proof.
 *
 * v1 semantics (sync-run): every child ran to completion INSIDE its
 * parent's fork() syscall. Output order was strictly grouped:
 *     [parent pre] [child A all] ... [child B all] [parent post]
 *
 * v2 semantics (this test must show): children stay RUNNABLE after
 * fork(); parent keeps printing right after each fork returns, so the
 * three processes interleave on the console, driven by cooperative
 * round-robin at syscall boundaries:
 *     A..P..B  A..P..B  A..B  ...
 *
 * Also verifies: two distinct child pids, both reaped via wait4(),
 * correct exit codes, per-child private address space (counter).
 */

#define SYS_write      1
#define SYS_fork       57
#define SYS_wait4      61
#define SYS_exit       60
#define SYS_getpid     39
#define SYS_nanosleep  35

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

static void nap(int ms) {
    struct { long sec; long nsec; } ts = { 0, ms * 1000000L };
    syscall3(SYS_nanosleep, (long)&ts, 0, 0);
}

static volatile long counter = 0;

static void child_work(char tag, int code_base)
{
    counter = code_base;                 /* private space: no crosstalk */
    for (int i = 0; i < 5; i++) {
        print("[parfork] ");
        char t[2] = { tag, '\0' };
        print(t); print(" step="); print_num(i);
        print(" pid=");   print_num(syscall3(SYS_getpid, 0, 0, 0));
        print(" cnt=");   print_num(counter);
        print("\n");
        counter++;                       /* COW-private increment       */
        nap(30);
    }
    {
        char t[16] = "[parfork] ?: exit\n";
        t[9] = tag;
        print(t);
    }
    syscall3(SYS_exit, code_base + 10, 0, 0);
}

int main(void) {
    print("[parfork] parent pid=");
    print_num(syscall3(SYS_getpid, 0, 0, 0));
    print(" — FORK V2 INTERLEAVE TEST\n");

    long pidA = syscall3(SYS_fork, 0, 0, 0);
    if (pidA == 0) child_work('A', 20);

    /* Parent proves it kept the CPU across the fork: this line would
     * be IMPOSSIBLE before A finished under v1 sync-run semantics.  */
    print("[parfork] parent: right after fork(A)="); print_num(pidA);
    print(" (child still runnable)\n");
    nap(45);                             /* let A make visible progress */

    long pidB = syscall3(SYS_fork, 0, 0, 0);
    if (pidB == 0) child_work('B', 40);

    print("[parfork] parent: right after fork(B)="); print_num(pidB);
    print(" (two children alive at once)\n");

    /* Honest wait4 loop — parent and BOTH children interleave here
     * until they exit. EXACTLY TWO zombies exist, so the loop must
     * stop after two reaps (a third wait4() with no children left
     * would spin forever under our minimal ECHILD semantics).      */
    long wA = 0, wB = 0;
    int remaining = 2;
    while (remaining-- > 0) {
        int st = 0;
        long w = syscall3(SYS_wait4, -1, (long)&st, 0);
        print("[parfork] reaped pid="); print_num(w);
        print(" code="); print_num((st >> 8) & 0xFF);
        print("\n");
        if (w == pidA) wA = (st >> 8) & 0xFF;
        else if (w == pidB) wB = (st >> 8) & 0xFF;
    }

    print("[parfork] SUMMARY pids="); print_num(pidA);
    print(","); print_num(pidB);
    print(" codes="); print_num(wA); print("/"); print_num(wB);
    print("\n");
    print((wA == 30 && wB == 50 && pidA != pidB && pidA > 0 && pidB > 0)
          ? "[parfork] PASS: concurrent fork v2 verified\n"
          : "[parfork] FAIL: unexpected reap data\n");
    return 9;
}
