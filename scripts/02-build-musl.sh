#!/bin/bash
# 02-build-musl.sh — musl 1.2.5 -> tools/musl (rootless, статический тулчейн).
# Рецепт из сессии Layer-0: prefix=tools/musl; инсталляция падает только на
# ln /lib/ld-musl (нужен root) — при полностью статической линковке это безвредно.
set -eu
ROOT=/home/z/my-project
SRC=$ROOT/src
BUILD=$ROOT/build

cd "$BUILD"
[ -d musl-1.2.5 ] || tar xf "$SRC/musl-1.2.5.tar.gz"
mkdir -p musl-build && cd musl-build

"$BUILD/musl-1.2.5/configure" --prefix="$ROOT/tools/musl" \
  --disable-shared --enable-static > configure.log 2>&1 || { tail -20 configure.log; exit 1; }

make -j2 > build.log 2>&1 || { tail -30 build.log; exit 1; }
make install > install.log 2>&1 || true   # ln /lib/ld-musl — ожидаемо падает

export CC="$ROOT/tools/musl/bin/musl-gcc"
printf '#include <stdio.h>\nint main(){puts("musl-static-ok");return 0;}\n' > /tmp/hello.c
$CC -static -o /tmp/hello-musl /tmp/hello.c && /tmp/hello-musl && file /tmp/hello-musl | grep -o "statically linked"
