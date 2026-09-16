/*
 * evdev.c — synthetic /dev/input/event* for NullOs (libinput substrate).
 *
 * Layer-2 dwl-readiness (see README "dwl / Wayland compositor
 * readiness", step 4): libinput wants evdev nodes. NullOs owns the
 * real PS/2 drivers, so we SYNTHESIZE the evdev ABI on top of them:
 *
 *   /dev/input/event0  keyboard: EV_KEY make/break from the set-1
 *                      scancode stream + EV_SYN report per event;
 *   /dev/input/event1  mouse: EV_REL deltas straight from the raw
 *                      PS/2 packet (NOT the 80x25-clamped position),
 *                      EV_KEY button transitions, EV_SYN per packet.
 *
 * Rings are strict SPSC: producer = IRQ (keyboard_handler /
 * mouse_irq_handler), consumer = syscall read. Ordering and
 * make/break pairing are exactly what libinput's state machines
 * require. Timestamps come from the PIT uptime clock.
 */

#include "../include/evdev.h"
#include "../include/timer.h"
#include "../include/string.h"

#define EVDEV_RING  256
#define EVDEV_SLOTS 2

typedef struct {
    volatile u32 head, tail;             /* SPSC: IRQ produces, read() drains */
    input_event_k ev[EVDEV_RING];
} evdata_t;

static evdata_t g_evdata[EVDEV_SLOTS];
static s32      g_refs[EVDEV_SLOTS];
static bool     g_nonblock[EVDEV_SLOTS];
static char     g_name[EVDEV_SLOTS][8] = { "event0", "event1" };

/* EV_KEY bitmask actually emitted by the devices (96 bytes = full
 * KEY_MAX window; mouse buttons live at 0x110..0x112, beyond the
 * old 32-byte keyboard-only mask). Served via EVIOCGBIT — the ioctl
 * libinput/libevdev use to classify a device. */
static u16 g_keybits[48];
static u16 g_keybits_mouse[48];
/* EV_REL bitmask: keyboard has none, mouse has X/Y/WHEEL. */
static const u16 g_relbits_mouse[2] = {
    (1u << REL_X) | (1u << REL_Y),   /* byte 0: bits 0,1         */
    (1u << (REL_WHEEL - 8))          /* byte 1: bit 0 (REL_WHEEL) */
};

/* EVIOCGBIT handler: copy `nwords` u16 mask words into the user
 * buffer, zero-extend to the requested `len` (Linux fills the whole
 * requested window). Returns len, or negative errno. */
/* FIX(#ioctl-arg-hole): every write through the raw ioctl `arg`
 * pointer is now validated with the syscall layer's user-range check
 * (static bounds + kernel-window exclusion + USER-page walk). The old
 * code only NULL-checked — a ring-3 caller could aim kmemset/bitmap
 * writes at ARBITRARY kernel memory (evdev ioctl = kmem corruption
 * primitive). Returns 0 on success, -EFAULT on rejection. */
static int evdev_arg_ok(u64 arg, u64 len)
{
    extern bool syscall_user_range_ok(u64, u64);
    if (!arg || !len) return -14;                 /* EFAULT/EINVAL */
    return syscall_user_range_ok(arg, len) ? 0 : -14;
}

static s64 evdev_bits_get(u64 arg, u32 len, const u16* words,
                          u32 nwords) {
    if (!arg) return -22;
    if (len == 0 || len > 256) return -22;   /* sane window cap     */
    if (evdev_arg_ok(arg, len) != 0) return -14;   /* FIX(#ioctl-arg-hole) */
    u8* out = (u8*)(u64)arg;
    u32 bytes = len;
    kmemset(out, 0, bytes);
    for (u32 i = 0; i < nwords; i++) {
        if (i * 2 >= bytes) break;
        out[i * 2] = (u8)(words[i] & 0xFF);
        if (i * 2 + 1 >= bytes) break;      /* odd window: low only */
        out[i * 2 + 1] = (u8)(words[i] >> 8);
    }
    return (s64)len;
}

static inline void ev_push(int slot, u16 type, u16 code, s32 value)
{
    evdata_t* d = &g_evdata[slot];
    u32 h = d->head;
    if (h - d->tail >= EVDEV_RING) return;   /* ring full: drop new (IRQ) */
    input_event_k* e = &d->ev[h & (EVDEV_RING - 1)];
    u64 ms = timer_get_uptime_ms();
    e->sec   = ms / 1000u;
    e->usec  = (ms % 1000u) * 1000u;
    e->type  = type;
    e->code  = code;
    e->value = value;
    d->head  = h + 1;
}

static inline void ev_syn(int slot)
{
    ev_push(slot, EV_SYN, 0 /*SYN_REPORT*/, 0);
}

/* Set-1 make code -> Linux keycode (US layout). 0 = unmapped. */
u16 kbd_set1_keycode(u8 code)
{
    static const u16 tbl[0x54] = {
        /* 0x00-0x0F */
        0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
        /* 0x10-0x1F: Q W E R T Y U I O P [ ] ENTER LCTRL A S D */
        16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
        /* 0x20-0x2F: F G H J K L ; ' ` LSHIFT \ Z X C V B N M */
        32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
        /* 0x30-0x3F: , . / RSHIFT KP* LALT SPACE CAPS F1..F10 */
        48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
        /* 0x40-0x4F: F6..F10 NUMLOCK SCROLL KP7 KP8 KP9 KP- KP4 KP5 KP6 KP+ KP1 KP2 KP3 KP0 */
        64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
        /* 0x50-0x53: KP. KP0 KP1 KP2 (legacy) */
        83,  84,  85,  86
    };
    if (code >= 0x54) return 0;
    return tbl[code];
}

void evdev_init(void)
{
    kmemset(g_evdata, 0, sizeof(g_evdata));
    kmemset(g_refs, 0, sizeof(g_refs));
    kmemset(g_nonblock, 0, sizeof(g_nonblock));
    kmemset(g_keybits, 0, sizeof(g_keybits));
    kmemset(g_keybits_mouse, 0, sizeof(g_keybits_mouse));

    for (u16 c = 1; c < 0x54; c++) {
        u16 kc = kbd_set1_keycode((u8)c);
        if (kc) g_keybits[kc / 16] |= (u16)(1u << (kc % 16));
    }
    g_keybits_mouse[BTN_LEFT / 16]   |= (u16)(1u << (BTN_LEFT % 16));
    g_keybits_mouse[BTN_RIGHT / 16]  |= (u16)(1u << (BTN_RIGHT % 16));
    g_keybits_mouse[BTN_MIDDLE / 16] |= (u16)(1u << (BTN_MIDDLE % 16));
}

/* ---- keyboard producer: set-1 raw byte (make | 0x80 break) --------- */
void evdev_kbd_set1(u8 raw)
{
    u8 code = raw & 0x7F;
    if (code == 0x60 || code == 0x70) return;   /* E0/F0 prefix bytes    */
    if (code == 0) return;
    u16 kc = kbd_set1_keycode(code);
    if (!kc) return;
    ev_push(0, 0x01 /*EV_KEY*/, kc, (raw & 0x80) ? 0 : 1);
    ev_syn(0);
}

/* ---- mouse producer: raw packet deltas (pre-clamp) ------------------ */
void evdev_mouse_packet(s32 dx, s32 dy, s32 dz,
                        u32 buttons, u32 prev_buttons)
{
    if (dx) ev_push(1, 0x02 /*EV_REL*/, 0 /*REL_X*/, dx);
    if (dy) ev_push(1, 0x02 /*EV_REL*/, 1 /*REL_Y*/, dy);
    if (dz) ev_push(1, 0x02 /*EV_REL*/, 8 /*REL_WHEEL*/, dz);
    u32 chg = buttons ^ prev_buttons;
    if (chg & 0x01) ev_push(1, 0x01, 0x110 /*BTN_LEFT*/,   (buttons & 0x01) ? 1 : 0);
    if (chg & 0x02) ev_push(1, 0x01, 0x111 /*BTN_RIGHT*/,  (buttons & 0x02) ? 1 : 0);
    if (chg & 0x04) ev_push(1, 0x01, 0x112 /*BTN_MIDDLE*/, (buttons & 0x04) ? 1 : 0);
    ev_syn(1);
}

/* ---- syscall side ---------------------------------------------------- */
s32 evdev_open(s32 slot, u32 open_flags)
{
    if (slot < 0 || slot >= EVDEV_SLOTS) return -1;
    if (g_refs[slot] == 0) {
        g_evdata[slot].head = g_evdata[slot].tail;   /* fresh reader: drain stale */
    }
    g_refs[slot]++;
    g_nonblock[slot] = (open_flags & 0x800) != 0;    /* O_NONBLOCK */
    return slot;
}

void evdev_ref_inc(s32 slot)
{
    if (slot >= 0 && slot < EVDEV_SLOTS && g_refs[slot] > 0) g_refs[slot]++;
}

void evdev_ref_dec(s32 slot)
{
    if (slot >= 0 && slot < EVDEV_SLOTS && g_refs[slot] > 0) g_refs[slot]--;
}

bool evdev_slot_used(s32 slot)
{
    return slot >= 0 && slot < EVDEV_SLOTS && g_refs[slot] > 0;
}

bool evdev_ready(s32 slot)
{
    return slot >= 0 && slot < EVDEV_SLOTS &&
           (g_evdata[slot].head - g_evdata[slot].tail) > 0;
}

s64 evdev_read(s32 slot, u8* buf, u64 count, bool nonblock)
{
    if (slot < 0 || slot >= EVDEV_SLOTS || !evdev_slot_used(slot)) return -9;
    if (count < sizeof(input_event_k)) return -22;
    u64 want = count / sizeof(input_event_k);
    for (;;) {
        u32 avail = g_evdata[slot].head - g_evdata[slot].tail;
        if (avail > 0) {
            if (want > avail) want = avail;
            if (want > 32) want = 32;                /* per-read sanity cap */
            evdata_t* d = &g_evdata[slot];
            input_event_k* out = (input_event_k*)(void*)buf;
            for (u32 i = 0; i < want; i++) {
                out[i] = d->ev[d->tail & (EVDEV_RING - 1)];
                d->tail++;
            }
            return (s64)(want * sizeof(input_event_k));
        }
        if (nonblock || g_nonblock[slot]) return -11;   /* EAGAIN */
        extern void task_yield(void);
        task_yield();
    }
}

s64 evdev_ioctl(s32 slot, u64 cmd, u64 arg)
{
    if (slot < 0 || slot >= EVDEV_SLOTS || !evdev_slot_used(slot)) return -9;
    switch (cmd) {
    case 0x80044502u: {                              /* EVIOCGVERSION */
        /* FIX(#ioctl-arg-hole): validate the 4-byte window. */
        if (evdev_arg_ok(arg, 4) != 0) return -14;
        *(s32*)(u64)arg = 0x00010001;                /* EV_VERSION */
        return 0;
    }
    case 0x80084502u: {                              /* EVIOCGID */
        /* FIX(#ioctl-arg-hole): validate the 8-byte window. */
        if (evdev_arg_ok(arg, 8) != 0) return -14;
        u16* id = (u16*)(u64)arg;   /* bustype, vendor, product, version */
        id[0] = 0x0011;                              /* BUS_I8042 */
        id[1] = 1;
        id[2] = (u16)(1 + slot);
        id[3] = 1;
        return 0;
    }
    default: {
        /* EVIOCGBIT(type, len) / EVIOCGKEY(len): _IOC-read, 'E',
         * low-16 cmd = 0x4520..0x4523 (bit type+0x20) / 0x4518.
         * libinput-classification surface: caps come from here.  */
        u32 lo16 = (u32)(cmd & 0xFFFFu);
        if ((cmd & 0x80000000u) &&
            (lo16 == 0x4518u || (lo16 >= 0x4520u && lo16 <= 0x4523u))) {
            u32 len = (u32)(cmd >> 16) & 0x3FFFu;
            switch (lo16) {
            case 0x4518u:               /* EVIOCGKEY: nothing held  */
                return evdev_bits_get(arg, len, 0, 0);
            case 0x4520u: {             /* EVIOCGBIT(EV_SYN): EV_SYN */
                u16 syn = 1;
                return evdev_bits_get(arg, len, &syn, 1);
            }
            case 0x4521u:               /* EVIOCGBIT(EV_KEY)        */
                return evdev_bits_get(arg, len,
                                      slot == 0 ? g_keybits
                                                : g_keybits_mouse,
                                      48);
            case 0x4522u:               /* EVIOCGBIT(EV_REL)        */
                if (slot == 0)
                    return evdev_bits_get(arg, len, 0, 0);
                return evdev_bits_get(arg, len, g_relbits_mouse, 2);
            default:                    /* EVIOCGBIT(EV_ABS): none  */
                return evdev_bits_get(arg, len, 0, 0);
            }
        }
        /* EVIOCGNAME(n): 0x8000 | (n << 16) | 0x4500 | 0x06 */
        if ((cmd & 0x80004500u) == 0x80004500u && (cmd & 0xFFu) == 0x06u) {
            u32 n = (u32)(cmd >> 16) & 0x3FFFu;
            const char* nm = g_name[slot];
            u8* out = (u8*)(u64)arg;
            /* FIX(#ioctl-arg-hole): validate the name window. */
            if (!out || !n) return -22;
            if (evdev_arg_ok(arg, n) != 0) return -14;
            u32 i = 0;
            for (; i + 1 < n && nm[i]; i++) out[i] = (u8)nm[i];
            out[i] = 0;
            return (s64)(i + 1);
        }
        return -25;                                   /* ENOTTY */
    }
    }
}
