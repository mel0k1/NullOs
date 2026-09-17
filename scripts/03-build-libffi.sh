#!/bin/bash
# 03-build-libffi.sh — libffi 3.4.8 static musl -> stage/
# Рецепт Layer-0: CC=musl-gcc; LDFLAGS=-static (на хосте нет /lib/ld-musl,
# динамические тест-бинари не запустятся); UAPI-шим linux/limits.h
# (musl не ставит kernel UAPI заголовки).
set -eu
ROOT=/home/z/my-project
SRC=$ROOT/src
BUILD=$ROOT/build
STAGE=$ROOT/stage

# UAPI shim до сборки
mkdir -p "$ROOT/tools/musl/include/linux"
cat > "$ROOT/tools/musl/include/linux/limits.h" <<'EOF'
/* NullOs build shim: kernel UAPI limits absent from musl installs. */
#ifndef _LINUX_LIMITS_SHIM_H
#define _LINUX_LIMITS_SHIM_H
#define PATH_MAX  4096
#define NAME_MAX  255
#define NGROUPS_MAX 65536
#define OPEN_MAX  256
#endif
EOF

cat > "$ROOT/tools/musl/include/linux/types.h" <<'EOF'
/* NullOs build shim: kernel UAPI scalar types (asm-generic style). */
#ifndef _LINUX_TYPES_SHIM_H
#define _LINUX_TYPES_SHIM_H
#include <stdint.h>
typedef uint8_t  __u8;  typedef int8_t  __s8;
typedef uint16_t __u16; typedef int16_t __s16;
typedef uint32_t __u32; typedef int32_t __s32;
typedef uint64_t __u64; typedef int64_t __s64;
typedef __u16 __le16; typedef __u32 __le32; typedef __u64 __le64;
typedef __u16 __be16; typedef __u32 __be32; typedef __u64 __be64;
typedef __u64 __aligned_u64 __attribute__((aligned(8)));
#endif
EOF

cd "$BUILD"
[ -d libffi-3.4.8 ] || tar xf "$SRC/libffi-3.4.8.tar.gz"
mkdir -p libffi-build && cd libffi-build

export CC="$ROOT/tools/musl/bin/musl-gcc"
export LDFLAGS="-static"
../libffi-3.4.8/configure --prefix="$STAGE" --disable-shared --enable-static \
  --disable-docs --disable-multi-os-directory > configure.log 2>&1 \
  || { tail -20 configure.log; exit 1; }

make -j2 > build.log 2>&1 || { tail -30 build.log; exit 1; }
make install > install.log 2>&1

ls -la "$STAGE/lib/libffi.a" "$STAGE/include/ffi.h" >/dev/null && echo "[OK] libffi installed"

# smoke: ffi_call add(19,23) -> 42
cat > /tmp/ffitest.c <<'EOF'
#include <ffi.h>
#include <stdio.h>
int add(int a, int b){ return a+b; }
int main(void){
  ffi_cif cif; ffi_type *args[2]={&ffi_type_sint32,&ffi_type_sint32};
  int a=19,b=23; void *vals[2]={&a,&b}; int rc=0;
  if (ffi_prep_cif(&cif,FFI_DEFAULT_ABI,2,&ffi_type_sint32,args)!=FFI_OK){puts("prep-FAIL");return 1;}
  ffi_call(&cif,FFI_FN(add),&rc,vals);
  printf("ffi_call=%d\n",rc);
  return rc==42?0:1;
}
EOF
$CC -static -o /tmp/ffitest /tmp/ffitest.c -I"$STAGE/include" "$STAGE/lib/libffi.a" && /tmp/ffitest && echo "[OK] ffi smoke 42" && file /tmp/ffitest | grep -o "statically linked"
