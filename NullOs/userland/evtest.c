/*
 * evtest — NullOs evdev substrate E2E prober (Layer-2 dwl readiness).
 *
 * Opens /dev/input/event0 (keyboard) and /dev/input/event1 (mouse),
 * verifies the libinput-facing ABI (EVIOCGVERSION/EVIOCGID/EVIOCGNAME),
 * then polls both fds for 8 seconds printing Linux-shaped
 * struct input_event records:
 *
 *   [EV] d=0 t=1 c=35 v=1     EV_KEY KEY_H press
 *   [EV] d=1 t=2 c=0 v=12     EV_REL REL_X +12
 *   [EV] d=1 t=1 c=272 v=1    EV_KEY BTN_LEFT press
 *
 * The host harness pairs this with HMP sendkey / mouse_move /
 * mouse_button commands and decodes the VGA text output.
 * exit(0) == ABI checks passed (event counts are informational).
 */

#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_ioctl     16
#define SYS_poll      7
#define SYS_exit      60

#define O_RDONLY      0
#define O_NONBLOCK    0x800

#define POLLIN        0x0001

#define EV_IOC_VERSION 0x80044502u
#define EV_IOC_ID      0x80084502u

/* input_event: timeval(16) + u16 type, u16 code, s32 value */
struct iev { unsigned long sec, usec; unsigned short type, code; int value; };

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
    char b[64]; long n = 0;
    while (s[n]) n++;
    for (long i = 0; i < n; i++) b[i] = s[i];
    char d[24]; long di = 0;
    if (v == 0) d[di++] = '0';
    while (v > 0) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
    for (long i = di - 1; i >= 0; i--) b[n++] = d[i];
    b[n++] = '\n';
    wr(b, n);
}
static void catu(char* b, long* n, long v) {
    char d[24]; long di = 0;
    if (v == 0) d[di++] = '0';
    while (v > 0) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
    for (long i = di - 1; i >= 0; i--) b[(*n)++] = d[i];
}
static void say_ev(long dev, long t, long c, long v) {
    char b[96]; long n = 0;
    const char* p = "[EV] d=";
    while (*p) b[n++] = *p++;
    catu(b, &n, dev);
    p = " t=";  while (*p) b[n++] = *p++;
    catu(b, &n, t);
    p = " c=";  while (*p) b[n++] = *p++;
    catu(b, &n, c);
    p = " v=";  while (*p) b[n++] = *p++;
    catu(b, &n, v);
    b[n++] = '\n';
    wr(b, n);
}

int main(void) {
    say("[EVTEST] start\n");

    long kfd = sc3(SYS_open, (long)"/dev/input/event0",
                   O_RDONLY | O_NONBLOCK, 0);
    if (kfd < 0) { say("[EVTEST] FAIL open event0\n"); return 1; }
    long mfd = sc3(SYS_open, (long)"/dev/input/event1",
                   O_RDONLY | O_NONBLOCK, 0);
    if (mfd < 0) { say("[EVTEST] FAIL open event1\n"); return 1; }

    /* ABI checks on the keyboard node */
    {
        int ver = 0;
        if (sc3(SYS_ioctl, kfd, EV_IOC_VERSION, (long)&ver) != 0 ||
            ver != 0x00010001) {
            say("[EVTEST] FAIL version\n"); return 1;
        }
        unsigned short id[4];
        if (sc3(SYS_ioctl, kfd, EV_IOC_ID, (long)id) != 0 ||
            id[0] != 0x11 /* BUS_I8042 */) {
            say("[EVTEST] FAIL id\n"); return 1;
        }
        sayu("[EVTEST] bus ", id[0]);
        char name[16];
        for (int i = 0; i < 16; i++) name[i] = 0;
        /* EVIOCGNAME(16) = 0x80104506 */
        long n = sc3(SYS_ioctl, kfd, 0x80104506u, (long)name);
        if (n < 2 || name[0] != 'e' || name[5] != '0') {
            say("[EVTEST] FAIL name\n"); return 1;
        }
        say("[EVTEST] name event0 OK\n");
    }

    /* drain: 8 s poll loop, print everything that shows up */
    long nev = 0;
    for (int iter = 0; iter < 400; iter++) {          /* 400 x 20 ms */
        struct { int fd; short events, revents; } pfds[2];
        pfds[0].fd = (int)kfd; pfds[0].events = POLLIN;
        pfds[1].fd = (int)mfd; pfds[1].events = POLLIN;
        long r = sc3(SYS_poll, (long)pfds, 2, 20);
        if (r <= 0) continue;
        for (int d = 0; d < 2; d++) {
            if (!(pfds[d].revents & POLLIN)) continue;
            struct iev evs[16];
            long n = sc3(SYS_read, pfds[d].fd, (long)evs, sizeof(evs));
            if (n <= 0) continue;
            long cnt = n / (long)sizeof(struct iev);
            for (long i = 0; i < cnt; i++) {
                nev++;
                say_ev(d, evs[i].type, evs[i].code, evs[i].value);
            }
        }
    }

    sc1(SYS_close, kfd);
    sc1(SYS_close, mfd);
    sayu("[EVTEST] events seen ", nev);
    say("[EVTEST] DONE-ABI-PASS\n");
    return 0;
}
