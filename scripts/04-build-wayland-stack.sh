#!/bin/bash
# 04-build-wayland-stack.sh — expat + wayland(client+server+scanner) + wayland-protocols
# -> stage/ (musl-static sysroot). Сессия Layer-0 (wayland-server+libffi = первая
# musl-static userland-либа).
set -eu
ROOT=/home/z/my-project; SRC=$ROOT/src; BUILD=$ROOT/build; STAGE=$ROOT/stage
. $ROOT/scripts/env-build.sh

# ---------- 1) expat (autotools) ----------
cd "$BUILD"
[ -d expat-2.7.1 ] || tar xf "$SRC/expat.tar.gz"
mkdir -p expat-build && cd expat-build
if [ ! -f Makefile ]; then
  ../expat-2.7.1/configure --prefix="$STAGE" --disable-shared --enable-static \
    --without-examples --without-tests > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
fi
make -j2 > build.log 2>&1 || { tail -30 build.log; exit 1; }
make install > install.log 2>&1
[ -f "$STAGE/lib/libexpat.a" ] && echo "[OK] expat -> stage"

# ---------- 2) wayland (meson) ----------
cd "$BUILD"
[ -d wayland-1.23.1 ] || tar xf "$SRC/wayland.tar.gz"
rm -rf wayland-build
meson setup wayland-build wayland-1.23.1 --prefix="$STAGE" --libdir=lib \
  --default-library=static -Dtests=false -Ddocumentation=false \
  -Ddtd_validation=false > wayland-setup.log 2>&1 \
  || { tail -30 wayland-setup.log; exit 1; }
ninja -C wayland-build > wayland-build.log 2>&1 || { tail -30 wayland-build.log; exit 1; }
meson install -C wayland-build > wayland-install.log 2>&1
file "$STAGE/bin/wayland-scanner" | grep -q "statically linked" && echo "[OK] wayland-scanner static"
[ -f "$STAGE/lib/libwayland-server.a" ] && [ -f "$STAGE/lib/libwayland-client.a" ] && echo "[OK] wayland libs -> stage"
"$STAGE/bin/wayland-scanner" --version

# ---------- 3) wayland-protocols (data) ----------
cd "$BUILD"
[ -d wayland-protocols-1.47 ] || tar xJf "$SRC/wayland-protocols.tar.xz"
rm -rf wp-build
meson setup wp-build wayland-protocols-1.47 --prefix="$STAGE" --libdir=lib -Dtests=false > wp-setup.log 2>&1 \
  || { tail -30 wp-setup.log; exit 1; }
ninja -C wp-build > wp-build.log 2>&1 || { tail -20 wp-build.log; exit 1; }
meson install -C wp-build > wp-install.log 2>&1
[ -f "$STAGE/share/wayland-protocols/unstable/xdg-shell/xdg-shell-unstable-v6.xml" ] || true
ls "$STAGE/share/wayland-protocols/stable/" && echo "[OK] wayland-protocols -> stage"
