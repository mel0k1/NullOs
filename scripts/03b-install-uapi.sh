#!/bin/bash
# 03b-install-uapi.sh — kernel UAPI headers (linux/, asm/, asm-generic/, misc/)
# из хостового linux-libc-dev в musl-sysroot. Рецепт Task 25: точечные UAPI-шимы
# из 03-build-libffi.sh (linux/limits.h, linux/types.h) хватает libffi, но
# libdrm требует полноценные <asm/ioctl.h> и т.д. Запускать ПОСЛЕ 03 (реальные
# UAPI перезаписывают шимы — так и задумано, они строго полнее).
set -eu
ROOT=/home/z/my-project
MUSL_INC=$ROOT/tools/musl/include

[ -d /usr/include/linux ] || { echo "ERROR: /usr/include/linux (linux-libc-dev) not found"; exit 1; }
mkdir -p "$MUSL_INC"
cp -r /usr/include/asm-generic "$MUSL_INC/"
cp -r /usr/include/x86_64-linux-gnu/asm "$MUSL_INC/"
cp -r /usr/include/linux "$MUSL_INC/"
cp -r /usr/include/misc "$MUSL_INC/" 2>/dev/null || true
ls "$MUSL_INC/asm/ioctl.h" >/dev/null && echo "[OK] UAPI headers in musl sysroot"
