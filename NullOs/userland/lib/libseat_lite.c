/*
 * libseat_lite.c — builtin-backend implementation of libseat_lite.h.
 *
 * Freestanding: talks to NullOs syscalls directly (open/close/
 * eventfd2/poll). The whole point of the lite layer is to give the
 * future dwl/wlroots port the exact call shape it already uses:
 * open seat -> callbacks -> mediated device opens -> dispatch loop.
 */

#include "libseat_lite.h"

/* ---- x86_64 syscall shims (matching kernel LINUX_NR_*) ------------- */
#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_poll        7
#define SYS_ioctl       16
#define SYS_eventfd2    290

#define O_RDONLY        0
#define O_RDWR          2
#define O_NONBLOCK      0x800
#define O_CLOEXEC       0x80000

#define EFD_NONBLOCK    0x800
#define EFD_CLOEXEC     0x80000

#define POLLIN          0x0001

static long lsc6(long nr, long a, long b, long c, long d, long e, long f) {
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
static long lsc3(long nr, long a, long b, long c) {
    return lsc6(nr, a, b, c, 0, 0, 0);
}
static long lsc1(long nr, long a) { return lsc3(nr, a, 0, 0); }

struct libseat {
    const struct libseat_seat_listener *listener;
    void        *data;
    int          wake_fd;      /* eventfd: session-event wakeup source */
    int          active;
    unsigned int ndevs;        /* devices currently opened through us  */
    const char  *name;
};

#define LIBSEAT_MAX_DEVS 16

int libseat_open_seat(const struct libseat_seat_listener *listener,
                      void *data, struct libseat **out) {
    if (!out) return -1;
    /* ring 3 without a libc: static seat pool (a compositor needs 1) */
    static struct libseat pool[2];
    static unsigned int used = 0;
    if (used >= sizeof(pool) / sizeof(pool[0])) return -1;
    struct libseat *seat = &pool[used++];
    seat->listener = listener;
    seat->data     = data;
    seat->wake_fd  = (int)lsc3(SYS_eventfd2, 0,
                               EFD_NONBLOCK | EFD_CLOEXEC, 0);
    seat->active   = 1;
    seat->ndevs    = 0;
    seat->name     = "seat0";
    if (seat->wake_fd < 0) { used--; return -1; }
    if (listener && listener->enable_session)
        listener->enable_session(seat, data);
    *out = seat;
    return 0;
}

int libseat_close_seat(struct libseat *seat) {
    if (!seat) return -1;
    lsc1(SYS_close, seat->wake_fd);
    seat->wake_fd = -1;
    seat->active  = 0;
    return 0;
}

int libseat_dispatch(struct libseat *seat, int timeout) {
    if (!seat || seat->wake_fd < 0) return -1;
    struct { int fd; short events, revents; } p;
    p.fd = seat->wake_fd; p.events = POLLIN; p.revents = 0;
    long r = lsc3(SYS_poll, (long)&p, 1, (long)timeout);
    if (r < 0) return -1;
    /* builtin backend: the wake fd never reports data (no session
     * events exist without a VT switcher). Drain paranoia aside,
     * just report "nothing happened". */
    return 0;
}

int libseat_get_fd(struct libseat *seat) {
    if (!seat) return -1;
    return seat->wake_fd;
}

int libseat_open_device(struct libseat *seat, const char *path,
                        int *fd) {
    if (!seat || !path || !fd) return -1;
    if (seat->ndevs >= LIBSEAT_MAX_DEVS) return -1;
    /* O_RDWR|O_CLOEXEC: exactly what real libinput asks through its
     * open_restricted callback (and what wlroots then hands over). */
    long f = lsc3(SYS_open, (long)path,
                  O_RDWR | O_CLOEXEC | O_NONBLOCK, 0);
    if (f < 0)
        f = lsc3(SYS_open, (long)path, O_RDONLY | O_NONBLOCK, 0);
    if (f < 0) return -1;
    *fd = (int)f;
    seat->ndevs++;
    return 0;
}

int libseat_close_device(struct libseat *seat, int fd) {
    if (!seat || fd < 0) return -1;
    if (seat->ndevs > 0) seat->ndevs--;
    return lsc1(SYS_close, fd) == 0 ? 0 : -1;
}

int libseat_disable_input(struct libseat *seat) {
    (void)seat;
    return 0;                    /* builtin: input is not mediated */
}

int libseat_switch_session(struct libseat *seat, int session) {
    (void)seat; (void)session;
    return -1;                   /* no VT switching on NullOs */
}

int libseat_get_session(struct libseat *seat) {
    if (!seat || !seat->active) return -1;
    return 1;                    /* session id 1, always active */
}

const char *libseat_seat_name(struct libseat *seat) {
    return seat ? seat->name : "seat0";
}
