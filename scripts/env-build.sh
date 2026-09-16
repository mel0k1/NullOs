#!/bin/bash
# env-build.sh — общее окружение musl-static сборки в stage/
ROOT=/home/z/my-project
STAGE=$ROOT/stage
export CC="$ROOT/tools/musl/bin/musl-gcc"
export CXX=false
export PKG_CONFIG_PATH="$STAGE/lib/pkgconfig:$STAGE/share/pkgconfig"
export PKG_CONFIG="pkg-config --define-variable=prefix=$STAGE"
export LDFLAGS="-static"
export CFLAGS="-O2 -fno-semantic-interposition"
export AR="ar"
export PATH="/home/z/.venv/bin:$PATH"
