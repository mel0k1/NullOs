/*
 * libseat_lite.h — minimal libseat-compatible seat/session API for
 * NullOs (Layer-3 dwl readiness).
 *
 * wlroots' session layer (backend/session/session.c) talks to libseat
 * through exactly this API surface:
 *
 *     libseat_open_seat(&listener, data, &seat);
 *     libseat_get_fd(seat)        -> pollable fd for session events
 *     libseat_dispatch(seat, t)   -> pump enable/disable events
 *     libseat_open_device(...)    -> mediated device open
 *     libseat_close_device(...)
 *     libseat_close_seat(seat);
 *
 * Lite backend = libseat's "builtin" seat: there is no seatd daemon
 * and no VT switching on NullOs, so the session is simply ALWAYS
 * active (enable_session fires inside open_seat, disable_session
 * never does). Device opens go straight to the kernel device nodes;
 * the mediated-over-socket seatd backend stays a documented follow-up.
 *
 * Function names/semantics follow real libseat 1.x so a future dwl
 * port links unmodified.
 */

#ifndef LIBSEAT_LITE_H
#define LIBSEAT_LITE_H

#ifdef __cplusplus
extern "C" {
#endif

struct libseat;

struct libseat_seat_listener {
    /* Session became active (fires immediately on open_seat). */
    void (*enable_session)(struct libseat *seat, void *data);
    /* Session must release devices (never fires on the lite builtin
     * backend — kept for API compatibility). */
    void (*disable_session)(struct libseat *seat, void *data);
};

/* 0 on success, -1 on error. The seat starts active: the listener's
 * enable_session callback fires before return. */
int libseat_open_seat(const struct libseat_seat_listener *listener,
                      void *data, struct libseat **out);
int libseat_close_seat(struct libseat *seat);

/* Pump session events (lite backend: none). Returns 0, never blocks
 * beyond `timeout` ms (-1 = forever). */
int libseat_dispatch(struct libseat *seat, int timeout);

/* Pollable fd used for session-event wakeups (an eventfd that stays
 * empty on the builtin backend — pollers just see no data). */
int libseat_get_fd(struct libseat *seat);

/* Mediated device open. 0 on success with *fd set, -1 on error. */
int libseat_open_device(struct libseat *seat, const char *path,
                        int *fd);
int libseat_close_device(struct libseat *seat, int fd);

/* Session management (builtin backend: ack-only no-ops). */
int libseat_disable_input(struct libseat *seat);
int libseat_switch_session(struct libseat *seat, int session);
int libseat_get_session(struct libseat *seat);
const char *libseat_seat_name(struct libseat *seat);

#ifdef __cplusplus
}
#endif

#endif /* LIBSEAT_LITE_H */
