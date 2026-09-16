/*
 * wlprobe — NullOs wayland-substrate prober (musl-static).
 *
 * Layer-0 milestone: wl_display_connect skeleton over the real
 * libwayland-client + libffi + expat chain. Against fakecomp it must
 * see >=1 global and survive a sync roundtrip. Exit codes:
 *   0 = connect + roundtrip + >=1 global
 *   1 = connect failed, 2 = roundtrip failed, 3 = no globals
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

static int n_globals;
static int saw_shm;

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *iface, uint32_t ver)
{
    n_globals++;
    if (strcmp(iface, "wl_shm") == 0)
        saw_shm = 1;
    printf("[wlprobe] global: %s v%u\n", iface, ver);
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {}

static const struct wl_registry_listener reg_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

int main(void)
{
    if (!getenv("XDG_RUNTIME_DIR"))
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);

    struct wl_display *d = wl_display_connect(NULL);
    if (!d) {
        puts("[wlprobe] FAIL wl_display_connect");
        return 1;
    }

    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &reg_listener, NULL);

    if (wl_display_roundtrip(d) < 0) {
        puts("[wlprobe] FAIL roundtrip");
        return 2;
    }

    printf("[wlprobe] connect+roundtrip OK globals=%d shm=%d\n",
           n_globals, saw_shm);
    wl_display_disconnect(d);
    return (n_globals > 0) ? 0 : 3;
}
