/*
 * seatprobe — NullOs Layer-3 prober: libseat-lite -> libinput-lite
 * chain in ring 3, the exact call shape dwl/wlroots will use:
 *
 *   stage 1 (SEAT):  libseat_open_seat -> enable_session callback,
 *                    mediated opens of /dev/dri/card0 and both evdev
 *                    nodes, dispatch/get_fd, error path (bad node),
 *                    close_device/close_seat;
 *   stage 2 (CTX):   libinput_udev_create_context with an interface
 *                    whose open_restricted goes THROUGH libseat
 *                    (the real dwl shape), assign_seat -> DEVICE_ADDED
 *                    per node, EVIOCGBIT-based caps + names;
 *   stage 3 (KBD):   poll(libinput fd) -> dispatch -> KEYBOARD_KEY
 *                    press events for keys the host harness types
 *                    (a=30, b=48) — pointer environment permitting
 *                    the pointer fd rides the same epoll;
 *   stage 4 (PTR):   best-effort POINTER_MOTION/BUTTON events from
 *                    HMP mouse_move/mouse_button (absent PS/2 aux in
 *                    this sandbox = PARTIAL, not a failure — same
 *                    precedent as evtest).
 *
 * exit(0) = ALL-PASS (or kbd-PASS with pointer env missing).
 * exit(1) = FAIL; the harness greps the [SEATPROBE] markers.
 */

#define SYS_write     1
#define SYS_exit      60

#include "lib/libseat_lite.h"
#include "lib/libinput_lite.h"

static long ssc3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

static void wr(const char* s, long n) { ssc3(SYS_write, 1, (long)s, n); }
static void say(const char* s) {
    long n = 0; while (s[n]) n++; wr(s, n);
}
static void sayu(const char* s, long v) {
    char b[96]; long n = 0;
    while (s[n]) n++;
    char d[24]; long di = 0;
    if (v == 0) d[di++] = '0';
    while (v > 0) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
    for (long i = di - 1; i >= 0; i--) b[n++] = d[i];
    b[n++] = '\n';
    wr(b, n);
}

static void msleep(long ms) {
    /* NullOs poll(nfds=0) is EINVAL — use nanosleep(35), which the
     * kernel implements (sys_nanosleep_impl). */
    struct { long sec, nsec; } req = { 0, ms * 1000000 };
    ssc3(35, (long)&req, 0, 0);
}

/* ---- stage 1: seat --------------------------------------------------- */
static int g_enable_fired = 0;
static void on_enable(struct libseat* s, void* d) {
    (void)s; (void)d; g_enable_fired = 1;
}
static void on_disable(struct libseat* s, void* d) {
    (void)s; (void)d;
}

static int stage_seat(void) {
    say("[SEATPROBE] t1 SEAT\n");

    static const struct libseat_seat_listener lst = {
        on_enable, on_disable
    };
    struct libseat* seat = 0;
    if (libseat_open_seat(&lst, 0, &seat) != 0 || !seat) {
        say("[SEATPROBE] FAIL open_seat\n"); return 1;
    }
    if (!g_enable_fired) {
        say("[SEATPROBE] FAIL enable_session not fired\n"); return 1;
    }
    if (libseat_get_session(seat) != 1 ||
        !libseat_seat_name(seat)) {
        say("[SEATPROBE] FAIL session identity\n"); return 1;
    }
    int wfd = libseat_get_fd(seat);
    if (wfd < 0) {
        say("[SEATPROBE] FAIL get_fd\n"); return 1;
    }
    if (libseat_dispatch(seat, 50) != 0) {
        say("[SEATPROBE] FAIL dispatch\n"); return 1;
    }

    int dfd = -1;
    if (libseat_open_device(seat, "/dev/dri/card0", &dfd) != 0 ||
        dfd < 0) {
        say("[SEATPROBE] FAIL open card0\n"); return 1;
    }
    sayu("[SEATPROBE] card0 fd ", dfd);
    int kfd = -1, mfd = -1;
    if (libseat_open_device(seat, "/dev/input/event0", &kfd) != 0 ||
        kfd < 0) {
        say("[SEATPROBE] FAIL open event0\n"); return 1;
    }
    if (libseat_open_device(seat, "/dev/input/event1", &mfd) != 0 ||
        mfd < 0) {
        say("[SEATPROBE] FAIL open event1\n"); return 1;
    }
    /* honest error path: a nonexistent node must fail */
    int bfd = -1;
    if (libseat_open_device(seat, "/dev/input/nope9", &bfd) == 0) {
        say("[SEATPROBE] FAIL bad-path accepted\n"); return 1;
    }
    say("[SEATPROBE] t1 devices opened (card0+event0+event1), "
        "bad path rejected\n");

    if (libseat_close_device(seat, kfd) != 0 ||
        libseat_close_device(seat, mfd) != 0 ||
        libseat_close_device(seat, dfd) != 0) {
        say("[SEATPROBE] FAIL close_device\n"); return 1;
    }
    if (libseat_close_seat(seat) != 0) {
        say("[SEATPROBE] FAIL close_seat\n"); return 1;
    }
    say("[SEATPROBE] t1 SEAT PASS\n");
    return 0;
}

/* ---- stage 2/3/4: libinput over the seat ----------------------------- */
static struct libseat* g_seat = 0;

static int open_restricted(const char* path, int flags, void* d) {
    (void)flags; (void)d;
    int fd = -1;
    if (libseat_open_device(g_seat, path, &fd) != 0) return -1;
    return fd;
}
static void close_restricted(int fd, void* d) {
    (void)d;
    libseat_close_device(g_seat, fd);
}

struct devinfo {
    int present;
    int cap_key, cap_pointer;
    const char* name;
    const char* sysname;
    unsigned int vendor, product;
};
static struct devinfo g_devs[8];
static int g_ndevis;

static int stage_ctx(void) {
    say("[SEATPROBE] t2 CTX\n");

    static const struct libseat_seat_listener lst = {
        on_enable, on_disable
    };
    if (libseat_open_seat(&lst, 0, &g_seat) != 0 || !g_seat) {
        say("[SEATPROBE] FAIL seat for ctx\n"); return 1;
    }

    static const struct libinput_interface iface = {
        open_restricted, close_restricted
    };
    struct libinput* li = libinput_udev_create_context(&iface, 0, 0);
    if (!li) {
        say("[SEATPROBE] FAIL create_context\n"); return 1;
    }
    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        say("[SEATPROBE] FAIL assign_seat\n"); return 1;
    }
    int lfd = libinput_get_fd(li);
    if (lfd < 0) {
        say("[SEATPROBE] FAIL libinput fd\n"); return 1;
    }

    /* DEVICE_ADDED for every enumerated node, with caps/names */
    struct libinput_event* ev;
    int added = 0;
    while ((ev = libinput_get_event(li)) != 0) {
        if (libinput_event_get_type(ev) ==
            LIBINPUT_EVENT_DEVICE_ADDED) {
            struct libinput_device* d =
                libinput_event_get_device(ev);
            struct devinfo* di = &g_devs[g_ndevis];
            di->present = 1;
            di->cap_key = libinput_device_has_capability(
                d, LIBINPUT_DEVICE_CAP_KEYBOARD);
            di->cap_pointer = libinput_device_has_capability(
                d, LIBINPUT_DEVICE_CAP_POINTER);
            di->name = libinput_device_get_name(d);
            di->sysname = libinput_device_get_sysname(d);
            di->vendor = libinput_device_get_id_vendor(d);
            di->product = libinput_device_get_id_product(d);
            g_ndevis++;
            added++;
            say("[SEATPROBE] added ");
            say(di->sysname);
            say(" name=");
            say(di->name);
            say(di->cap_key ? " caps=KEY" : " caps=");
            say(di->cap_pointer ? "+POINTER" : "");
            sayu(" v.", (long)di->vendor);
        }
        libinput_event_destroy(ev);
    }
    if (added < 1) {
        say("[SEATPROBE] FAIL no devices enumerated\n"); return 1;
    }
    /* keyboard must be among them (kbd node always exists) */
    if (!g_devs[0].present || !g_devs[0].cap_key) {
        say("[SEATPROBE] FAIL kbd caps\n"); return 1;
    }
    say("[SEATPROBE] t2 CTX PASS\n");
    return 0;
}

/* The context stays alive across stages via the static libinput pool;
 * we re-fetch it by creating a second one in the pump stages. To keep
 * the probe simple the pump owns its own context. */
static struct libinput* g_li = 0;

static int stage_pump(void) {
    say("[SEATPROBE] t3 KBD: WAIT-KEYS\n");

    int press_a = 0, press_b = 0;
    int motions = 0, buttons = 0, axis = 0;

    /* 25 s window; the harness types keys within the first ~8 s and
     * drives the mouse right after (pointer env permitting). */
    for (int iter = 0; iter < 250; iter++) {
        libinput_dispatch(g_li);
        struct libinput_event* ev;
        while ((ev = libinput_get_event(g_li)) != 0) {
            switch (libinput_event_get_type(ev)) {
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                unsigned int k = libinput_event_keyboard_get_key(ev);
                int st = libinput_event_keyboard_get_key_state(ev);
                if (st == LIBINPUT_KEY_STATE_PRESSED) {
                    if (k == 30) press_a = 1;
                    if (k == 48) press_b = 1;
                    sayu("[SEATPROBE] KEY press c=", (long)k);
                }
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION:
                motions++;
                break;
            case LIBINPUT_EVENT_POINTER_BUTTON:
                buttons++;
                break;
            case LIBINPUT_EVENT_POINTER_AXIS:
                axis++;
                break;
            default:
                break;
            }
            libinput_event_destroy(ev);
        }
        if (iter == 99) {           /* pointer window bookkeeping */
            sayu("[SEATPROBE] t4 PTR after 10s: motions=", motions);
            sayu("[SEATPROBE] t4 buttons=", buttons);
            sayu("[SEATPROBE] t4 axis=", axis);
        }
        msleep(100);
    }

    sayu("[SEATPROBE] kbd a(30)=", press_a);
    sayu("[SEATPROBE] kbd b(48)=", press_b);
    if (!press_a || !press_b) {
        say("[SEATPROBE] FAIL kbd events missing\n"); return 1;
    }
    say("[SEATPROBE] t3 KBD PASS\n");

    struct devinfo* m = 0;
    for (int i = 0; i < g_ndevis; i++)
        if (g_devs[i].cap_pointer) m = &g_devs[i];
    if (!m) {
        say("[SEATPROBE] t4 PTR SKIPPED (no pointer device)\n");
        return 0;
    }
    if (motions > 0 || buttons > 0) {
        sayu("[SEATPROBE] t4 PTR PASS motions=", motions);
        sayu("[SEATPROBE] t4 buttons=", buttons);
    } else {
        say("[SEATPROBE] t4 PTR PARTIAL (no aux mouse in this "
            "environment)\n");
    }
    return 0;
}

int main(void) {
    say("[SEATPROBE] start\n");

    if (stage_seat()) return 1;

    /* context over a fresh seat (the wlroots call shape) */
    if (stage_ctx()) return 1;

    static const struct libinput_interface iface = {
        open_restricted, close_restricted
    };
    g_li = libinput_udev_create_context(&iface, 0, 0);
    if (!g_li) { say("[SEATPROBE] FAIL ctx2\n"); return 1; }
    if (libinput_udev_assign_seat(g_li, "seat0") != 0) {
        say("[SEATPROBE] FAIL assign2\n"); return 1;
    }

    if (stage_pump()) return 1;

    say("[SEATPROBE] ALL-PASS\n");
    return 0;
}
