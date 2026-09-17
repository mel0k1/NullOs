#!/bin/bash
# 06-build-xkb-stack.sh — xkeyboard-config (data) + libxkbcommon (static musl) -> stage/
# Рецепт Task 25: ничего не тянем из X11/Wayland — data-only keymaps + чистый
# libxkbcommon. Смоук: musl-static программа компилирует keymap "us" из
# установленных данных (XKB_DEFAULT_ROOT) — E2E по всей цепочке.
set -eu
ROOT=/home/z/my-project; SRC=$ROOT/src; BUILD=$ROOT/build; STAGE=$ROOT/stage
. $ROOT/scripts/env-build.sh

# ---------- 1) xkeyboard-config (data-only, meson) ----------
cd "$BUILD"
[ -d xkeyboard-config-2.48 ] || tar xf "$SRC/xkeyboard-config.tar.xz"
rm -rf xkbd-build
meson setup xkbd-build xkeyboard-config-2.48 --prefix="$STAGE" \
  -Dcompat-rules=true > xkbd-setup.log 2>&1 \
  || { tail -30 xkbd-setup.log; exit 1; }
ninja -C xkbd-build > xkbd-build.log 2>&1 || { tail -30 xkbd-build.log; exit 1; }
meson install -C xkbd-build > xkbd-install.log 2>&1
[ -f "$STAGE/share/X11/xkb/symbols/us" ] && echo "[OK] xkeyboard-config -> stage"

# ---------- 2) libxkbcommon (meson, static musl) ----------
# ЛОВНЯ: GitHub-releases у libxkbcommon БЕЗ тарболлов (assets=[]), а Debian
# orig — git-снапшот с чужим именем каталога. Meson-проекту autotools не
# нужны — просто приводим имя каталога к ожидаемому.
cd "$BUILD"
[ -d libxkbcommon-1.13.1 ] || {
  tar xf "$SRC/libxkbcommon.tar.gz"
  mv xkbcommon-libxkbcommon-* libxkbcommon-1.13.1
}
rm -rf xkbcommon-build
meson setup xkbcommon-build libxkbcommon-1.13.1 --prefix="$STAGE" --libdir=lib \
  --default-library=static \
  -Denable-x11=false -Denable-wayland=false -Denable-xkbregistry=false -Denable-bash-completion=false \
  -Denable-docs=false -Denable-tools=false > xkb-setup.log 2>&1 \
  || { tail -30 xkb-setup.log; exit 1; }
ninja -C xkbcommon-build > xkb-build.log 2>&1 || { tail -30 xkb-build.log; exit 1; }
meson install -C xkbcommon-build > xkb-install.log 2>&1
[ -f "$STAGE/lib/libxkbcommon.a" ] && [ -f "$STAGE/include/xkbcommon/xkbcommon.h" ] \
  && echo "[OK] libxkbcommon -> stage"

# ---------- 3) smoke: musl-static keymap compile E2E ----------
mkdir -p "$BUILD/userland-musl"
cat > /tmp/xkbsmoke.c <<'EOF'
/* xkbsmoke — compile the "us" keymap from the staged xkeyboard-config
 * data with static libxkbcommon (Layer-0.5 milestone probe). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

int main(void) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) { puts("[xkbsmoke] FAIL context"); return 1; }

    /* data root: the staged xkeyboard-config (RMLVO lookup) */
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105",
                                    .layout = "us",   .variant = "",
                                    .options = "" };
    struct xkb_keymap *km = xkb_keymap_new_from_names(
        ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) { puts("[xkbsmoke] FAIL keymap"); return 2; }

    xkb_layout_index_t nl = xkb_keymap_num_layouts(km);
    xkb_keycode_t min = xkb_keymap_min_keycode(km);
    xkb_keycode_t max = xkb_keymap_max_keycode(km);

    /* keysym check: the A key (evdev code 38) must yield keysym 'a' */
    const xkb_keysym_t *syms = NULL;
    int nsyms = xkb_keymap_key_get_syms_by_level(km, 38, 0, 0, &syms);
    char buf[64] = {0};
    if (nsyms > 0) xkb_keysym_get_name(syms[0], buf, sizeof(buf));

    const char *dump = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t dlen = dump ? strlen(dump) : 0;

    printf("[xkbsmoke] layouts=%u keycodes=[%u..%u] key38='%s' dump=%zu bytes\n",
           (unsigned)nl, (unsigned)min, (unsigned)max, buf, dlen);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);

    int ok = (nl >= 1) && (max > min) && (strcmp(buf, "a") == 0) && dlen > 10000;
    puts(ok ? "[xkbsmoke] PASS" : "[xkbsmoke] FAIL");
    return ok ? 0 : 3;
}
EOF
$CC -static -Os -o "$BUILD/userland-musl/xkbsmoke" /tmp/xkbsmoke.c \
  $(pkg-config --cflags xkbcommon) \
  $(pkg-config --libs xkbcommon) -lm

file "$BUILD/userland-musl/xkbsmoke" | grep -q "statically linked"
export XKB_DEFAULT_ROOT="$STAGE/share/X11/xkb"
"$BUILD/userland-musl/xkbsmoke" \
  && echo "[MILESTONE] xkb Layer-0.5 E2E PASS" || { echo "[FAIL]"; exit 1; }
