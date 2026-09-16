#ifndef EVDEV_H
#define EVDEV_H

/*
 * evdev — synthetic /dev/input/event* nodes (libinput substrate).
 *
 * The kernel keyboard (IRQ1) and PS/2 mouse (IRQ12) drivers feed
 * Linux-shaped `struct input_event` rings; ring-3 readers get the
 * exact ABI libinput expects (EV_KEY/EV_REL/EV_SYN + EVIOCGETINFO
 * ioctls). Producer = IRQ context, consumer = syscall context.
 *
 * fd encoding: FD_EVDEV_BASE - slot (see syscall.c), slots 0 = kbd
 * (/dev/input/event0), 1 = mouse (/dev/input/event1).
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux input_event (x86_64): timeval(16) + type/code (4) + value(4) */
typedef struct {
    u64 sec, usec;
    u16 type, code;
    s32 value;
} input_event_k;

#define EV_SYN       0x00
#define EV_KEY       0x01
#define EV_REL       0x02

#define SYN_REPORT   0
#define REL_X        0
#define REL_Y        1
#define REL_WHEEL    8
#define BTN_LEFT     0x110
#define BTN_RIGHT    0x111
#define BTN_MIDDLE   0x112

/* ioctl commands (libdrm-free, plain _IOC values used by libevdev) */
#define EV_IOC_VERSION  0x80044502   /* _IOR('E', 0x02, int)          */
#define EV_IOC_ID       0x80084502   /* _IOR('E', 0x02, input_id)     */
#define EV_IOC_NAME(b)  (0x80000000u | ((u32)(b) << 16) | 0x4506u)

void evdev_init(void);
/* IRQ-side producers (cheap, lock-free SPSC). */
void evdev_kbd_set1(u8 set1_raw_scancode);            /* make|0x80=break */
void evdev_mouse_packet(s32 dx, s32 dy, s32 dz,
                        u32 buttons, u32 prev_buttons);

/* syscall-side API (slot 0=kbd, 1=mouse) */
s32  evdev_open(s32 slot, u32 open_flags);   /* returns slot or -1     */
void evdev_ref_inc(s32 slot);
void evdev_ref_dec(s32 slot);
bool evdev_slot_used(s32 slot);
bool evdev_ready(s32 slot);
s64  evdev_read(s32 slot, u8* buf, u64 count, bool nonblock);
s64  evdev_ioctl(s32 slot, u64 cmd, u64 arg);

#ifdef __cplusplus
}
#endif

#endif /* EVDEV_H */
