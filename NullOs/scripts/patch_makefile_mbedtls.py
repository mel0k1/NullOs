#!/usr/bin/env python3
# patch_makefile_mbedtls.py — apply mbedtls -nostdinc changes to Makefile
# WITHOUT touching whitespace (the Edit tool expands TABs file-wide —
# see worklog Task 10 lesson). Run from the repo root (NullOs/).
import subprocess, sys

MK = "Makefile"

# 1) restore pristine Makefile from HEAD (undo Edit-tool tab expansion)
subprocess.run(["git", "checkout", "--", MK], check=True)

src = open(MK, "r", encoding="utf-8").read()

# 2) MB_CFLAGS: -nostdinc + gcc freestanding -isystem + -U unix macros
#    + kernel shim include dir (mbkern/libc)
old_cflags = (
    "MB_CFLAGS = -ffreestanding -fno-stack-protector -fno-pie -fno-pic \\\n"
    "\t-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O1 -g -w -std=c11 \\\n"
    "\t-I$(KERNEL_DIR)/mb/include \\\n"
)
new_cflags = (
    "# -nostdinc: HOST glibc headers must NOT leak into the kernel build\n"
    "# (features.h defines __GLIBC__ -> explicit_bzero/unistd/SYS_getrandom\n"
    "# paths light up and reference libc symbols that cannot exist here).\n"
    "# GCC's own freestanding headers (stddef/stdint/stdarg/limits/float)\n"
    "# are re-added via -isystem. -U: gcc target macros __linux__/__unix__\n"
    "# make TF-PSA think it runs on a Unix-like OS\n"
    "# (MBEDTLS_PLATFORM_IS_UNIXLIKE -> getpid(), <unistd.h>).\n"
    "# -I mbkern/libc: kernel shim <string.h>/<stdio.h>/<time.h>.\n"
    "GCC_INC := $(shell $(CC) -print-file-name=include)\n"
    "MB_CFLAGS = -ffreestanding -fno-stack-protector -fno-pie -fno-pic \\\n"
    "\t-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O1 -g -w -std=c11 \\\n"
    "\t-nostdinc -isystem $(GCC_INC) \\\n"
    "\t-U__linux__ -U__linux -U__unix__ -U__unix -Uunix -U__gnu_linux__ \\\n"
    "\t-I$(KERNEL_DIR)/mbkern/libc \\\n"
    "\t-I$(KERNEL_DIR)/mb/include \\\n"
)
if old_cflags not in src:
    sys.exit("FAIL: MB_CFLAGS anchor not found")
src = src.replace(old_cflags, new_cflags, 1)

# 3) tlsglue rule: shim dir FIRST so <time.h> is the shim (same struct tm
#    layout the mb objects see), not the host glibc header
old_glue = (
    "# tlsglue.c: kernel file, but needs the mbedtls include roots and\n"
    "# the same config defines as the vendored library objects.\n"
    "$(BUILD_DIR)/kernel/tlsglue.o: $(KERNEL_DIR)/tlsglue.c | $(BUILD_DIR)\n"
    "\t$(CC) $(CFLAGS) -I$(KERNEL_DIR)/mb/include \\\n"
)
new_glue = (
    "# tlsglue.c: kernel file, but needs the mbedtls include roots and\n"
    "# the same config defines as the vendored library objects. mbkern/libc\n"
    "# shim path comes FIRST so <time.h> resolves to the shim (same struct\n"
    "# tm layout the mb objects see), not to the host glibc header.\n"
    "$(BUILD_DIR)/kernel/tlsglue.o: $(KERNEL_DIR)/tlsglue.c | $(BUILD_DIR)\n"
    "\t$(CC) $(CFLAGS) -I$(KERNEL_DIR)/mbkern/libc -I$(KERNEL_DIR)/mb/include \\\n"
)
if old_glue not in src:
    sys.exit("FAIL: tlsglue anchor not found")
src = src.replace(old_glue, new_glue, 1)

open(MK, "w", encoding="utf-8").write(src)
print("OK: Makefile patched, tabs preserved")
