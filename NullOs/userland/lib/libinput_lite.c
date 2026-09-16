/*
 * libinput_lite.c — libinput-compatible event engine over NullOs
 * synthetic evdev (see libinput_lite.h for the design).
 *
 * Freestanding: own syscall shims, static pools, no libc. The ABI
 * that must survive is the FUNCTION SURFACE (wlroots links these
 * names), the event enum values, and the fd semantics.
 */

#include "libinput_lite.h"

/* ---- x86_64 syscall shims ------------------------------------------- */
#define SYS_read          0
#define SYS_open          2
#define SYS_close         3
#define SYS_ioctl         16
#define SYS_epoll_ctl     233
#define SYS_epoll_wait    232
#define SYS_epoll_create1 291

#define O_RDONLY   0
#define O_RDWR     2
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000

#define EPOLLIN      0x001
#define EPOLL_CTL_ADD 1

/* linux/input.h constants (matches kernel/evdev.c synthesis)          */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define REL_X     0
#define REL_Y     1
#define REL_WHEEL 8
#define KEY_MAX       0x2ff
#define KEY_ESC       1
#define BTN_LEFT      0x110
#define BTN_RIGHT     0x111
#define BTN_MIDDLE    0x112
#define SYN_REPORT    0

#define EV_IOC_VERSION 0x80044502u
#define EV_IOC_ID      0x80084502u
#define EV_IOC_NAME(b) (0x80000000u | ((unsigned)(b) << 16) | 0x4506u)
#define EVIOCGBIT(t, l) (0x80000000u | ((unsigned)(l) << 16) | 0x4500u \
                         | (0x20u + (unsigned)(t)))
#define EVIOCGKEY(l)   (0x80000000u | ((unsigned)(l) << 16) | 0x4518u)

static long isc6(long nr, long a, long b, long c, long d, long e, long f) {
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
static long isc3(long nr, long a, long b, long c) {
    return isc6(nr, a, b, c, 0, 0, 0);
}
static long isc1(long nr, long a) { return isc3(nr, a, 0, 0); }

static void li_zero(void *p, unsigned long n) {
    char *c = (char *)p;
    while (n--) *c++ = 0;
}
static int li_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static unsigned long li_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}
static void li_strcpy(char *d, const char *s, unsigned long cap) {
    unsigned long i = 0;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* linux struct input_event (x86_64) */
struct li_iev {
    unsigned long sec, usec;
    unsigned short type, code;
    int value;
};

/* ---- internal objects ----------------------------------------------- */
#define LI_MAX_DEVICES 8
#define LI_MAX_EVENTS  64
#define LI_NAME_CAP    64

struct libinput_device {
    struct libinput *ctx;
    int   fd;
    int   slot;                       /* kernel evdev slot            */
    int   refs;
    char  name[LI_NAME_CAP];          /* EVIOCGNAME                   */
    char  sysname[16];                /* "event0"/"event1"            */
    unsigned short id[4];             /* bustype/vendor/product/ver   */
    int   cap_key, cap_pointer;
    unsigned char keybits[(KEY_MAX + 7) / 8 + 1];
    unsigned char relbits[16];
    /* pointer state machine */
    double pend_dx, pend_dy, pend_wheel;
    unsigned int prev_buttons;
};

struct libinput_event {
    enum libinput_event_type type;
    struct libinput_device *dev;
    unsigned long  time_ms;
    unsigned long long time_usec;
    /* keyboard */
    unsigned int key;
    int          key_state;
    /* pointer */
    double dx, dy, axis_v, axis_h;
    unsigned int button;
    int          button_state;
    enum libinput_pointer_axis_source axis_src;
    int used;                          /* slot occupied flag           */
};

struct libinput {
    const struct libinput_interface *iface;
    void *user_data;
    int   epoll_fd;
    struct libinput_device devs[LI_MAX_DEVICES];
    int   ndevs;
    struct libinput_event queue[LI_MAX_EVENTS];
    unsigned int q_head, q_tail;
    int   refs;
};

/* ---- event queue (SPSC: dispatch produces, get_event consumes) ------ */
static void li_queue_push(struct libinput *li,
                          const struct libinput_event *ev) {
    unsigned int next = (li->q_tail + 1) % LI_MAX_EVENTS;
    if (next == li->q_head) return;               /* overflow: drop   */
    li->queue[li->q_tail] = *ev;
    li->queue[li->q_tail].used = 1;
    li->q_tail = next;
}

/* ---- context creation ----------------------------------------------- */
struct libinput *libinput_udev_create_context(
        const struct libinput_interface *iface, void *user_data,
        void *udev) {
    (void)udev;                    /* no udev on NullOs — direct scan */
    if (!iface) return 0;
    static struct libinput pool[2];
    static unsigned int pool_used = 0;
    if (pool_used >= sizeof(pool) / sizeof(pool[0])) return 0;
    struct libinput *li = &pool[pool_used++];
    li_zero(li, sizeof(*li));
    li->iface = iface;
    li->user_data = user_data;
    li->epoll_fd = (int)isc3(SYS_epoll_create1, 0, 0, 0);
    li->refs = 1;
    if (li->epoll_fd < 0) { pool_used--; return 0; }
    return li;
}

struct libinput *libinput_unref(struct libinput *li) {
    if (!li) return 0;
    if (--li->refs > 0) return li;
    for (int i = 0; i < li->ndevs; i++) {
        struct libinput_device *d = &li->devs[i];
        if (d->fd >= 0 && li->iface && li->iface->close_restricted)
            li->iface->close_restricted(d->fd, li->user_data);
        d->fd = -1;
    }
    isc1(SYS_close, li->epoll_fd);
    li->epoll_fd = -1;
    return 0;
}

/* ---- seat assignment = device enumeration --------------------------- */
static void li_probe_device(struct libinput_device *d) {
    /* id */
    unsigned short id[4] = {0, 0, 0, 0};
    isc3(SYS_ioctl, d->fd, EV_IOC_ID, (long)id);
    for (int i = 0; i < 4; i++) d->id[i] = id[i];
    /* name */
    char nm[LI_NAME_CAP];
    li_zero(nm, sizeof(nm));
    long n = isc3(SYS_ioctl, d->fd, EV_IOC_NAME(LI_NAME_CAP),
                  (long)nm);
    if (n >= 2) li_strcpy(d->name, nm, LI_NAME_CAP);
    else li_strcpy(d->name, d->sysname, LI_NAME_CAP);
    /* caps via EVIOCGBIT */
    li_zero(d->keybits, sizeof(d->keybits));
    li_zero(d->relbits, sizeof(d->relbits));
    isc3(SYS_ioctl, d->fd, EVIOCGBIT(EV_KEY, sizeof(d->keybits)),
         (long)d->keybits);
    isc3(SYS_ioctl, d->fd, EVIOCGBIT(EV_REL, sizeof(d->relbits)),
         (long)d->relbits);
    /* classification, libinput-style */
    d->cap_key = (d->keybits[0] & (1u << (KEY_ESC & 7))) != 0;
    d->cap_pointer = (d->relbits[0] & (1u << REL_X)) != 0;
    /* initial key state */
    unsigned char keystate[32];
    li_zero(keystate, sizeof(keystate));
    isc3(SYS_ioctl, d->fd, EVIOCGKEY(sizeof(keystate)),
         (long)keystate);
}

int libinput_udev_assign_seat(struct libinput *li, const char *seat) {
    if (!li || !li->iface || !li->iface->open_restricted) return -1;
    (void)seat;                    /* single seat0 on NullOs         */
    static const char *paths[2] = { "/dev/input/event0",
                                    "/dev/input/event1" };
    for (int slot = 0; slot < 2; slot++) {
        if (li->ndevs >= LI_MAX_DEVICES) break;
        int fd = li->iface->open_restricted(paths[slot],
                                            O_RDWR | O_CLOEXEC |
                                            O_NONBLOCK,
                                            li->user_data);
        if (fd < 0) continue;      /* absent node: not a failure    */
        struct libinput_device *d = &li->devs[li->ndevs];
        li_zero(d, sizeof(*d));
        d->ctx = li;
        d->fd = fd;
        d->slot = slot;
        d->refs = 1;
        d->prev_buttons = 0;
        li_strcpy(d->sysname, "eventX", sizeof(d->sysname));
        d->sysname[5] = (char)('0' + slot);
        d->sysname[6] = 0;
        li_probe_device(d);
        li->ndevs++;
        /* epoll owns the readiness story */
        struct { unsigned int events; unsigned long long data; } ev;
        ev.events = EPOLLIN;
        ev.data = (unsigned long long)slot;
        isc6(SYS_epoll_ctl, li->epoll_fd, EPOLL_CTL_ADD, fd,
             (long)&ev, 0, 0);
        /* DEVICE_ADDED event, exactly what wlroots waits for */
        struct libinput_event evq;
        li_zero(&evq, sizeof(evq));
        evq.type = LIBINPUT_EVENT_DEVICE_ADDED;
        evq.dev = d;
        evq.used = 1;
        li_queue_push(li, &evq);
    }
    return 0;
}

/* ---- dispatch: drain readable devices into the event queue ---------- */
static void li_feed_device(struct libinput *li,
                           struct libinput_device *d) {
    struct li_iev evs[32];
    long n = isc3(SYS_read, d->fd, (long)evs, sizeof(evs));
    if (n <= 0) return;
    long cnt = n / (long)sizeof(struct li_iev);
    for (long i = 0; i < cnt; i++) {
        unsigned long long usec =
            evs[i].sec * 1000000ull + evs[i].usec;
        unsigned long ms = (unsigned long)(usec / 1000ull);
        if (evs[i].type == EV_KEY) {
            unsigned int code = evs[i].code;
            int pressed = evs[i].value != 0;
            if (d->cap_pointer && code >= BTN_LEFT &&
                code <= BTN_MIDDLE) {
                unsigned int bit = 1u << (code - BTN_LEFT);
                unsigned int want = pressed ? bit : 0;
                if ((d->prev_buttons & bit) != want) {
                    struct libinput_event q;
                    li_zero(&q, sizeof(q));
                    q.type = LIBINPUT_EVENT_POINTER_BUTTON;
                    q.dev = d;
                    q.time_ms = ms; q.time_usec = usec;
                    q.button = code;
                    q.button_state = pressed;
                    q.used = 1;
                    li_queue_push(li, &q);
                }
            } else {
                struct libinput_event q;
                li_zero(&q, sizeof(q));
                q.type = LIBINPUT_EVENT_KEYBOARD_KEY;
                q.dev = d;
                q.time_ms = ms; q.time_usec = usec;
                q.key = code;
                q.key_state = pressed;
                q.used = 1;
                li_queue_push(li, &q);
            }
        } else if (evs[i].type == EV_REL) {
            if (evs[i].code == REL_X)
                d->pend_dx += (double)evs[i].value;
            else if (evs[i].code == REL_Y)
                d->pend_dy += (double)evs[i].value;
            else if (evs[i].code == REL_WHEEL)
                d->pend_wheel += (double)evs[i].value;
        } else if (evs[i].type == EV_SYN &&
                   evs[i].code == SYN_REPORT) {
            if (d->pend_dx != 0.0 || d->pend_dy != 0.0) {
                struct libinput_event q;
                li_zero(&q, sizeof(q));
                q.type = LIBINPUT_EVENT_POINTER_MOTION;
                q.dev = d;
                q.time_ms = ms; q.time_usec = usec;
                q.dx = d->pend_dx; q.dy = d->pend_dy;
                q.used = 1;
                li_queue_push(li, &q);
                d->pend_dx = d->pend_dy = 0.0;
            }
            if (d->pend_wheel != 0.0) {
                struct libinput_event q;
                li_zero(&q, sizeof(q));
                q.type = LIBINPUT_EVENT_POINTER_AXIS;
                q.dev = d;
                q.time_ms = ms; q.time_usec = usec;
                q.axis_src = LIBINPUT_POINTER_AXIS_SOURCE_WHEEL;
                if (d->pend_wheel > 0) q.axis_v = d->pend_wheel;
                else q.axis_h = d->pend_wheel;
                q.used = 1;
                li_queue_push(li, &q);
                d->pend_wheel = 0.0;
            }
        }
    }
}

int libinput_dispatch(struct libinput *li) {
    if (!li || li->epoll_fd < 0) return -1;
    struct { unsigned int events, pad; unsigned long long data; }
        out[LI_MAX_DEVICES];
    li_zero(out, sizeof(out));
    long n = isc6(SYS_epoll_wait, li->epoll_fd, (long)out,
                  LI_MAX_DEVICES, 0, 0, 0);
    if (n < 0) return -1;
    for (long i = 0; i < n; i++) {
        long slot = (long)out[i].data;
        if (slot >= 0 && slot < li->ndevs)
            li_feed_device(li, &li->devs[slot]);
    }
    return 0;
}

int libinput_get_fd(struct libinput *li) {
    if (!li) return -1;
    return li->epoll_fd;
}

/* ---- event handout --------------------------------------------------- */
struct libinput_event *libinput_get_event(struct libinput *li) {
    if (!li || li->q_head == li->q_tail) return 0;
    struct libinput_event *ev = &li->queue[li->q_head];
    li->q_head = (li->q_head + 1) % LI_MAX_EVENTS;
    return ev;                      /* single-consumer: pool slot    */
}

void libinput_event_destroy(struct libinput_event *ev) {
    if (ev) ev->used = 0;           /* returned to the static pool   */
}

/* ---- accessors -------------------------------------------------------- */
enum libinput_event_type libinput_event_get_type(
        struct libinput_event *ev) {
    return ev ? ev->type : LIBINPUT_EVENT_NONE;
}

struct libinput_device *libinput_event_get_device(
        struct libinput_event *ev) {
    return ev ? ev->dev : 0;
}

unsigned long libinput_event_get_time(struct libinput_event *ev) {
    return ev ? ev->time_ms : 0;
}

unsigned long long libinput_event_get_time_usec(
        struct libinput_event *ev) {
    return ev ? ev->time_usec : 0;
}

unsigned int libinput_event_keyboard_get_key(
        struct libinput_event *ev) {
    return ev ? ev->key : 0;
}

enum libinput_key_state libinput_event_keyboard_get_key_state(
        struct libinput_event *ev) {
    return ev ? (ev->key_state ? LIBINPUT_KEY_STATE_PRESSED
                               : LIBINPUT_KEY_STATE_RELEASED)
              : LIBINPUT_KEY_STATE_RELEASED;
}

double libinput_event_pointer_get_dx(struct libinput_event *ev) {
    return ev ? ev->dx : 0.0;
}

double libinput_event_pointer_get_dy(struct libinput_event *ev) {
    return ev ? ev->dy : 0.0;
}

unsigned int libinput_event_pointer_get_button(
        struct libinput_event *ev) {
    return ev ? ev->button : 0;
}

enum libinput_button_state libinput_event_pointer_get_button_state(
        struct libinput_event *ev) {
    return ev ? (ev->button_state ? LIBINPUT_BUTTON_STATE_PRESSED
                                  : LIBINPUT_BUTTON_STATE_RELEASED)
              : LIBINPUT_BUTTON_STATE_RELEASED;
}

int libinput_event_pointer_has_axis(struct libinput_event *ev,
        enum libinput_pointer_axis axis) {
    if (!ev) return 0;
    return axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL
        ? ev->axis_v != 0.0 : ev->axis_h != 0.0;
}

double libinput_event_pointer_get_axis_value(
        struct libinput_event *ev, enum libinput_pointer_axis axis) {
    if (!ev) return 0.0;
    return axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL
        ? ev->axis_v : ev->axis_h;
}

enum libinput_pointer_axis_source
libinput_event_pointer_get_axis_source(struct libinput_event *ev) {
    return ev ? ev->axis_src : LIBINPUT_POINTER_AXIS_SOURCE_WHEEL;
}

struct libinput_device *libinput_device_ref(
        struct libinput_device *d) {
    if (d) d->refs++;
    return d;
}

struct libinput_device *libinput_device_unref(
        struct libinput_device *d) {
    if (d && d->refs > 0) d->refs--;
    return d;
}

const char *libinput_device_get_name(struct libinput_device *d) {
    return d ? d->name : 0;
}

const char *libinput_device_get_sysname(struct libinput_device *d) {
    return d ? d->sysname : 0;
}

unsigned int libinput_device_get_id_vendor(struct libinput_device *d) {
    return d ? d->id[1] : 0;
}

unsigned int libinput_device_get_id_product(struct libinput_device *d) {
    return d ? d->id[2] : 0;
}

int libinput_device_has_capability(struct libinput_device *d,
        enum libinput_device_capability cap) {
    if (!d) return 0;
    switch (cap) {
    case LIBINPUT_DEVICE_CAP_KEYBOARD: return d->cap_key;
    case LIBINPUT_DEVICE_CAP_POINTER:  return d->cap_pointer;
    default: return 0;
    }
}

struct libinput *libinput_device_get_context(
        struct libinput_device *d) {
    return d ? d->ctx : 0;
}
