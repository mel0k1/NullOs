#!/bin/bash
# 05-wayland-smoke.sh — wlprobe vs fakecomp E2E через реальный unix-сокет
# (Layer-0 milestone: wayland-server+libffi первая musl-static либа).
set -eu
ROOT=/home/z/my-project; STAGE=$ROOT/stage; BUILD=$ROOT/build
. $ROOT/scripts/env-build.sh

mkdir -p "$BUILD/userland-musl"
$CC -static -Os -o "$BUILD/userland-musl/wlprobe" \
  "$ROOT/nulos/NullOs/userland/wlprobe.c" \
  $(pkg-config --cflags wayland-client) \
  $(pkg-config --libs wayland-client) -lffi

$CC -static -Os -o "$BUILD/userland-musl/fakecomp" \
  "$ROOT/nulos/NullOs/userland/fakecomp.c" \
  $(pkg-config --cflags wayland-server) \
  $(pkg-config --libs wayland-server) -lffi

file "$BUILD/userland-musl/wlprobe" "$BUILD/userland-musl/fakecomp" | grep -c "statically linked"

RUN=/tmp/xdg-run
rm -rf "$RUN" && mkdir -p "$RUN"
export XDG_RUNTIME_DIR=$RUN

"$BUILD/userland-musl/fakecomp" > "$BUILD/userland-musl/fakecomp.log" 2>&1 &
FCPID=$!
sleep 0.4
set +e
"$BUILD/userland-musl/wlprobe"
WLEX=$?
set -e
wait $FCPID || true

echo "--- fakecomp.log ---"; cat "$BUILD/userland-musl/fakecomp.log"
echo "wlprobe exit=$WLEX"
[ "$WLEX" = "0" ] && grep -q "served client" "$BUILD/userland-musl/fakecomp.log" \
  && echo "[MILESTONE] wayland Layer-0 E2E PASS" || { echo "[FAIL]"; exit 1; }
