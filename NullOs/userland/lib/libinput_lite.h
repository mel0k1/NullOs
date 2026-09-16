/*
 * libinput_lite.h — minimal libinput-compatible input event API for
 * NullOs (Layer-3 dwl readiness).
 *
 * wlroots' libinput backend (backend/libinput.c) consumes exactly the
 * names below: a context created against an interface whose
 * open_restricted/close_restricted go through libseat, an
 * assign_seat() that enumerates the evdev nodes, then a
 * poll -> dispatch -> get_event loop with KEYBOARD_KEY /
 * POINTER_MOTION / POINTER_BUTTON / POINTER_AXIS events.
 *
 * Lite strategy: no udev exists on NullOs, so the "udev" context
 * enumerates /dev/input/event* directly (fixed kernel slots).
 * Internally the context runs its own epoll over the device fds —
 * the same shape real libinput has (its fd is epoll-correct for
 * wlroots' event loop). Device classification uses the kernel's
 * EVIOCGBIT answer; state machines keep libinput semantics:
 *   - keyboard: 1 event per EV_KEY;
 *   - pointer:  REL deltas accumulate between SYN_REPORT pairs into
 *               one POINTER_MOTION per frame, button transitions
 *               become POINTER_BUTTON, wheel REL_WHEEL becomes
 *               POINTER_AXIS (source = wheel).
 *
 * Event type / state enums carry the REAL libinput numeric values so
 * downstream switch() code ports verbatim.
 */

#ifndef LIBINPUT_LITE_H
#define LIBINPUT_LITE_H

#ifdef __cplusplus
extern "C" {
#endif

struct libinput;
struct libinput_device;
struct libinput_event;

/* ---- real libinput enum values -------------------------------------- */
enum libinput_event_type {
    LIBINPUT_EVENT_NONE = 0,
    LIBINPUT_EVENT_DEVICE_ADDED = 1,
    LIBINPUT_EVENT_DEVICE_REMOVED = 2,
    LIBINPUT_EVENT_KEYBOARD_KEY = 300,
    LIBINPUT_EVENT_POINTER_MOTION = 400,
    LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE = 401,
    LIBINPUT_EVENT_POINTER_BUTTON = 402,
    LIBINPUT_EVENT_POINTER_AXIS = 403
};

enum libinput_key_state {
    LIBINPUT_KEY_STATE_RELEASED = 0,
    LIBINPUT_KEY_STATE_PRESSED = 1
};

enum libinput_button_state {
    LIBINPUT_BUTTON_STATE_RELEASED = 0,
    LIBINPUT_BUTTON_STATE_PRESSED = 1
};

enum libinput_pointer_axis {
    LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL = 0,
    LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL = 1
};

enum libinput_pointer_axis_source {
    LIBINPUT_POINTER_AXIS_SOURCE_WHEEL = 0,
    LIBINPUT_POINTER_AXIS_SOURCE_FINGER = 1,
    LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS = 2
};

enum libinput_device_capability {
    LIBINPUT_DEVICE_CAP_KEYBOARD = 0,
    LIBINPUT_DEVICE_CAP_POINTER = 1,
    LIBINPUT_DEVICE_CAP_TOUCH = 2,
    LIBINPUT_DEVICE_CAP_TABLET_TOOL = 3,
    LIBINPUT_DEVICE_CAP_TABLET_PAD = 4,
    LIBINPUT_DEVICE_CAP_GESTURE = 5,
    LIBINPUT_DEVICE_CAP_SWITCH = 6
};

struct libinput_interface {
    int  (*open_restricted)(const char *path, int flags, void *data);
    void (*close_restricted)(int fd, void *data);
};

/* context creation ("udev" API — udev arg accepted, ignored: NullOs
 * has no udev, enumeration is a direct /dev/input/event* scan)      */
struct libinput *libinput_udev_create_context(
        const struct libinput_interface *iface, void *user_data,
        void *udev);
int  libinput_udev_assign_seat(struct libinput *li,
                               const char *seat_name);

/* event loop plumbing */
int  libinput_dispatch(struct libinput *li);
int  libinput_get_fd(struct libinput *li);
struct libinput_event *libinput_get_event(struct libinput *li);
void libinput_event_destroy(struct libinput_event *ev);
struct libinput *libinput_unref(struct libinput *li);

/* event accessors */
enum libinput_event_type libinput_event_get_type(
        struct libinput_event *ev);
struct libinput_device *libinput_event_get_device(
        struct libinput_event *ev);
unsigned long libinput_event_get_time(
        struct libinput_event *ev);           /* ms */
unsigned long long libinput_event_get_time_usec(
        struct libinput_event *ev);

/* keyboard */
unsigned int libinput_event_keyboard_get_key(
        struct libinput_event *ev);
enum libinput_key_state libinput_event_keyboard_get_key_state(
        struct libinput_event *ev);

/* pointer */
double libinput_event_pointer_get_dx(struct libinput_event *ev);
double libinput_event_pointer_get_dy(struct libinput_event *ev);
unsigned int libinput_event_pointer_get_button(
        struct libinput_event *ev);
enum libinput_button_state libinput_event_pointer_get_button_state(
        struct libinput_event *ev);
int libinput_event_pointer_has_axis(struct libinput_event *ev,
        enum libinput_pointer_axis axis);
double libinput_event_pointer_get_axis_value(struct libinput_event *ev,
        enum libinput_pointer_axis axis);
enum libinput_pointer_axis_source libinput_event_pointer_get_axis_source(
        struct libinput_event *ev);

/* devices */
struct libinput_device *libinput_device_ref(struct libinput_device *d);
struct libinput_device *libinput_device_unref(struct libinput_device *d);
const char *libinput_device_get_name(struct libinput_device *d);
const char *libinput_device_get_sysname(struct libinput_device *d);
unsigned int libinput_device_get_id_vendor(struct libinput_device *d);
unsigned int libinput_device_get_id_product(struct libinput_device *d);
int libinput_device_has_capability(struct libinput_device *d,
        enum libinput_device_capability cap);
struct libinput *libinput_device_get_context(
        struct libinput_device *d);

#ifdef __cplusplus
}
#endif

#endif /* LIBINPUT_LITE_H */
