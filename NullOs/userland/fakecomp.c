/*
 * fakecomp — minimal wayland server for substrate validation (musl-static).
 *
 * Creates a display socket, exposes one wl_shm global, then serves the
 * event loop until a client comes and goes (or 5 s timeout). Used on the
 * host together with wlprobe to prove the wayland-server+libffi+expat
 * chain end-to-end over a real unix socket.
 *
 * Exit codes: 0 = served >=1 client, 4 = timeout, 5 = socket error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>

static void shm_bind(struct wl_client *client, void *data,
                     uint32_t version, uint32_t id)
{
    struct wl_resource *res =
        wl_resource_create(client, &wl_shm_interface, 1, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, NULL, NULL, NULL);
    wl_shm_send_format(res, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(res, WL_SHM_FORMAT_XRGB8888);
}

int main(void)
{
    if (!getenv("XDG_RUNTIME_DIR"))
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    struct wl_display *d = wl_display_create();
    if (!d) {
        puts("[fakecomp] FAIL wl_display_create");
        return 5;
    }
    if (!wl_global_create(d, &wl_shm_interface, 1, NULL, shm_bind)) {
        puts("[fakecomp] FAIL wl_global_create");
        return 5;
    }
    if (wl_display_add_socket(d, "wayland-0") != 0) {
        puts("[fakecomp] FAIL add_socket");
        return 5;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(d);
    printf("[fakecomp] listening on %s/wayland-0\n", getenv("XDG_RUNTIME_DIR"));
    fflush(stdout);

    int had_client = 0;
    for (int i = 0; i < 500; i++) {            /* <= 5 s @100 ms */
        wl_display_flush_clients(d);           /* wl_display_run() semantics */
        wl_event_loop_dispatch(loop, 100);
        int clients = 0;
        struct wl_client *c;
        wl_client_for_each(c, wl_display_get_client_list(d))
            clients++;
        if (clients > 0)
            had_client = 1;
        else if (had_client) {
            printf("[fakecomp] served client(s), clean exit\n");
            wl_display_destroy(d);
            return 0;
        }
    }
    printf("[fakecomp] timeout had_client=%d\n", had_client);
    wl_display_destroy(d);
    return had_client ? 0 : 4;
}
