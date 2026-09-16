# Makefile for NullOs - 64-bit Hobby OS

# Tools
CC = gcc
CXX = g++
LD = ld
QEMU = qemu-system-x86_64

# Directories
BUILD_DIR = build
BOOT_DIR = boot
KERNEL_DIR = kernel
INCLUDE_DIR = include
USERLAND_DIR = userland

# Flags
CFLAGS = -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O0 -g \
	-I$(INCLUDE_DIR) -std=c11 $(EXTRA_DEFS)

CXXFLAGS = -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	-Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O0 -g \
	-I$(INCLUDE_DIR) -std=c++17 \
	-fno-exceptions -fno-rtti $(EXTRA_DEFS) $(APK_DEFS)

# Pass EXTRA_DEFS="-DFORK_AUTORUN_DEBUG" for GDB debugging sessions
EXTRA_DEFS ?=

ASFLAGS = -c -g

LDFLAGS = -n -T $(LINKER_SCRIPT) -z max-page-size=0x1000

# Source files
BOOT_S = $(BOOT_DIR)/boot64.S
KERNEL_S = $(filter-out $(KERNEL_DIR)/gdt.S, $(wildcard $(KERNEL_DIR)/*.S))
KERNEL_C = $(wildcard $(KERNEL_DIR)/*.c)
KERNEL_CPP = $(wildcard $(KERNEL_DIR)/*.cpp)

# Object files
BOOT_OBJ = $(BUILD_DIR)/boot64.o
KERNEL_S_OBJ = $(patsubst $(KERNEL_DIR)/%.S,$(BUILD_DIR)/kernel/%.o,$(KERNEL_S))
KERNEL_C_OBJ = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_C))
KERNEL_CPP_OBJ = $(patsubst $(KERNEL_DIR)/%.cpp,$(BUILD_DIR)/kernel/%.o,$(KERNEL_CPP))

# Vendored lwext4 (subdirectory — not covered by the kernel wildcard)
EXT4_C      = $(wildcard $(KERNEL_DIR)/ext4/*.c)
EXT4_C_OBJ  = $(patsubst $(KERNEL_DIR)/ext4/%.c,$(BUILD_DIR)/kernel/ext4/%.o,$(EXT4_C))


# Vendored mbedtls 4.2.0 + TF-PSA-Crypto 1.2.0 (subdirectories)
MB_TLS_C     = $(wildcard $(KERNEL_DIR)/mb/tls/*.c)
MB_CORE_C    = $(wildcard $(KERNEL_DIR)/mb/crypto/core/*.c)
MB_PLAT_C    = $(wildcard $(KERNEL_DIR)/mb/crypto/platform/*.c)
MB_EXTRA_C   = $(wildcard $(KERNEL_DIR)/mb/crypto/extras/*.c) $(wildcard $(KERNEL_DIR)/mb/crypto/utilities/*.c)
MB_DRV_C     = $(wildcard $(KERNEL_DIR)/mb/crypto/drivers/builtin/src/*.c)
MB_TLS_OBJ   = $(patsubst $(KERNEL_DIR)/mb/tls/%.c,$(BUILD_DIR)/kernel/mb/tls/%.o,$(MB_TLS_C))
MB_CORE_OBJ  = $(patsubst $(KERNEL_DIR)/mb/crypto/core/%.c,$(BUILD_DIR)/kernel/mb/crypto/core/%.o,$(MB_CORE_C))
MB_EXTRA_OBJ = $(patsubst $(KERNEL_DIR)/mb/crypto/%.c,$(BUILD_DIR)/kernel/mb/crypto/%.o,$(MB_EXTRA_C))
MB_DRV_OBJ   = $(patsubst $(KERNEL_DIR)/mb/crypto/drivers/builtin/src/%.c,$(BUILD_DIR)/kernel/mb/crypto/drivers/builtin/src/%.o,$(MB_DRV_C))
MB_PLAT_OBJ  = $(patsubst $(KERNEL_DIR)/mb/crypto/platform/%.c,$(BUILD_DIR)/kernel/mb/crypto/platform/%.o,$(MB_PLAT_C))
MB_OBJS      = $(MB_TLS_OBJ) $(MB_CORE_OBJ) $(MB_PLAT_OBJ) $(MB_EXTRA_OBJ) $(MB_DRV_OBJ)
KERNEL_OBJS = $(BOOT_OBJ) $(KERNEL_S_OBJ) $(KERNEL_C_OBJ) $(KERNEL_CPP_OBJ) $(EXT4_C_OBJ) $(MB_OBJS)

# Userland (ring-3 programs, embedded into the kernel image)
USERLAND_BUILD = $(BUILD_DIR)/userland
HELLO_ELF = $(USERLAND_BUILD)/hello.elf
FORKTEST_ELF = $(USERLAND_BUILD)/forktest.elf
TTYPROBE_ELF = $(USERLAND_BUILD)/ttyprobe.elf
PARFORK_ELF = $(USERLAND_BUILD)/parfork.elf
SLEEPBIN_ELF = $(USERLAND_BUILD)/sleepbin.elf

# Output files
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
KERNEL_ISO = $(BUILD_DIR)/nullos.iso
GRUB_CFG = $(BUILD_DIR)/grub.cfg
DISK_IMG = $(BUILD_DIR)/disk.img

# Linker script
LINKER_SCRIPT = linker64.ld

# Default target
all: $(KERNEL_BIN)
	@echo "NullOs kernel built successfully!"
	@ls -lh $(KERNEL_BIN)

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/kernel

$(USERLAND_BUILD): | $(BUILD_DIR)
	mkdir -p $(USERLAND_BUILD)

# ============================================================
# Userland: ring-3 ELF programs linked at 0x400000.
# Built BEFORE the kernel: kernel/userprogs.S embeds hello.elf
# via .incbin. Requires a linker with elf_x86_64 emulation
# (native on Linux/WSL; on MSYS2 use build.ps1 which links via WSL).
# ============================================================

$(USERLAND_BUILD)/crt0.o: $(USERLAND_DIR)/crt0.S | $(USERLAND_BUILD)
	$(CC) $(ASFLAGS) $< -o $@

$(USERLAND_BUILD)/hello.o: $(USERLAND_DIR)/hello.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

$(HELLO_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/hello.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/hello.o

$(USERLAND_BUILD)/forktest.o: $(USERLAND_DIR)/forktest.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

$(FORKTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/forktest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/forktest.o

$(USERLAND_BUILD)/ttyprobe.o: $(USERLAND_DIR)/ttyprobe.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@


$(TTYPROBE_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/ttyprobe.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/ttyprobe.o

$(USERLAND_BUILD)/parfork.o: $(USERLAND_DIR)/parfork.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

$(PARFORK_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/parfork.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/parfork.o

$(USERLAND_BUILD)/sleepbin.o: $(USERLAND_DIR)/sleepbin.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

$(SLEEPBIN_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/sleepbin.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/sleepbin.o

$(USERLAND_BUILD)/nettest.o: $(USERLAND_DIR)/nettest.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

$(USERLAND_BUILD)/httpget.o: $(USERLAND_DIR)/httpget.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

HTTPGET_ELF = $(USERLAND_BUILD)/httpget.elf

$(HTTPGET_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/httpget.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/httpget.o

$(USERLAND_BUILD)/srvtest.o: $(USERLAND_DIR)/tcp_srv.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

NETTEST_ELF = $(USERLAND_BUILD)/nettest.elf

$(NETTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/nettest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/nettest.o

$(USERLAND_BUILD)/nsh.o: $(USERLAND_DIR)/nsh.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

NSH_ELF = $(USERLAND_BUILD)/nsh.elf

$(NSH_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/nsh.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/nsh.o

# userprogs.S must be reassembled when the embedded binaries change
SRVTEST_ELF = $(USERLAND_BUILD)/srvtest.elf

$(SRVTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/srvtest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/srvtest.o

$(USERLAND_BUILD)/unixtest.o: $(USERLAND_DIR)/unixtest.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

UNIXTEST_ELF = $(USERLAND_BUILD)/unixtest.elf

$(UNIXTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/unixtest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/unixtest.o

$(USERLAND_BUILD)/wltest.o: $(USERLAND_DIR)/wltest.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

WLTEST_ELF = $(USERLAND_BUILD)/wltest.elf

$(WLTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/wltest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/wltest.o

# ---- Layer-2 dwl-substrate probes (DRM-lite + evdev) ----
$(USERLAND_BUILD)/drmtest.o: $(USERLAND_DIR)/drmtest.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

DRMTEST_ELF = $(USERLAND_BUILD)/drmtest.elf

$(DRMTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/drmtest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/drmtest.o

$(USERLAND_BUILD)/evtest.o: $(USERLAND_DIR)/evtest.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

EVTEST_ELF = $(USERLAND_BUILD)/evtest.elf

$(EVTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/evtest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/evtest.o

# ---- Layer-2 substrate prober: mprotect/procfs/shm-pools/efd-sem ----
$(USERLAND_BUILD)/layertest.o: $(USERLAND_DIR)/layertest.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

LAYERTEST_ELF = $(USERLAND_BUILD)/layertest.elf

$(LAYERTEST_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/layertest.o $(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/layertest.o

# ---- Layer-3 library substrate: libseat-lite + libinput-lite ----
$(USERLAND_BUILD)/libseat_lite.o: $(USERLAND_DIR)/lib/libseat_lite.c $(USERLAND_DIR)/lib/libseat_lite.h | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

# No -mno-sse here: the API returns double (real libinput shape) and
# ring 3 has FPU/SSE enabled (kernel/fpu.c FXSAVE/FXRSTOR).
$(USERLAND_BUILD)/libinput_lite.o: $(USERLAND_DIR)/lib/libinput_lite.c $(USERLAND_DIR)/lib/libinput_lite.h | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

$(USERLAND_BUILD)/seatprobe.o: $(USERLAND_DIR)/seatprobe.c | $(USERLAND_BUILD)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-mmx -mno-sse -mno-sse2 -O2 -std=c11 \
	-Wall -Wextra -c $< -o $@

SEATPROBE_ELF = $(USERLAND_BUILD)/seatprobe.elf

$(SEATPROBE_ELF): $(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/seatprobe.o \
		$(USERLAND_BUILD)/libseat_lite.o $(USERLAND_BUILD)/libinput_lite.o \
		$(USERLAND_DIR)/link.ld
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none \
	-T $(USERLAND_DIR)/link.ld -o $@ \
	$(USERLAND_BUILD)/crt0.o $(USERLAND_BUILD)/seatprobe.o \
		$(USERLAND_BUILD)/libseat_lite.o $(USERLAND_BUILD)/libinput_lite.o


# apk.static two-stage import: stage A embeds the 4.3MB blob and is
# built ONLY via `make APK_DEFS="-DAPK_STAGE_A"`; plain `make` stays
# blobless (kernel must stay under 4MB — see include/mm.h #heap-overlap).
APK_DEFS ?=
$(BUILD_DIR)/kernel/userprogs.o: $(KERNEL_DIR)/userprogs.S $(HELLO_ELF) $(FORKTEST_ELF) $(TTYPROBE_ELF) $(NSH_ELF) $(PARFORK_ELF) $(SLEEPBIN_ELF) $(NETTEST_ELF) $(HTTPGET_ELF) $(SRVTEST_ELF) $(UNIXTEST_ELF) $(WLTEST_ELF) $(DRMTEST_ELF) $(EVTEST_ELF) $(LAYERTEST_ELF) $(SEATPROBE_ELF) $(USERLAND_DIR)/apk.static | $(BUILD_DIR)
	$(CC) $(ASFLAGS) $(APK_DEFS) $< -o $@

# Compile boot assembly (GAS)
$(BUILD_DIR)/boot64.o: $(BOOT_S) | $(BUILD_DIR)
	$(CC) $(ASFLAGS) $< -o $@

# Compile kernel assembly (GAS)
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) $< -o $@

# dlmalloc 2.7.2 (vendored, PRISTINE) — special freestanding flags.
# See kernel/mm.c (wrappers/kernel_sbrk) and kernel/dlcompat.c.
$(BUILD_DIR)/kernel/dlmalloc.o: $(KERNEL_DIR)/dlmalloc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(KERNEL_DIR)/dlstubs \
		-include $(KERNEL_DIR)/dlmorecore.h \
		-DLACKS_UNISTD_H -DLACKS_SYS_PARAM_H \
		-DHAVE_MMAP=0 -DHAVE_MREMAP=0 \
		-DMORECORE=kernel_sbrk -DMORECORE_CANNOT_TRIM=1 -DMORECORE_CLEARS=0 \
		-DUSE_DL_PREFIX -Dmalloc_getpagesize=4096 \
		-c $< -o $@

# ext4glue.c: needs the lwext4 headers (ext4/ext4.h -> <ext4_config.h>)
# plus the stub libc headers, which the generic kernel rule lacks.
$(BUILD_DIR)/kernel/ext4glue.o: $(KERNEL_DIR)/ext4glue.c | $(BUILD_DIR)
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
		-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O0 -g -Wall -Wextra \
		-Wno-unused-parameter -Wno-unused-function -std=c11 -I$(KERNEL_DIR)/ext4stub -I$(KERNEL_DIR)/ext4 \
		-DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_DEBUG_PRINTF=0 -DCONFIG_DEBUG_ASSERT=0 \
		-c $< -o $@

# lwext4 1.0 (vendored, PRISTINE) — freestanding flags.
# Glue (blockdev/alloc/mount) lives in kernel/ext4glue.c.
$(BUILD_DIR)/kernel/ext4/%.o: $(KERNEL_DIR)/ext4/%.c | $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/kernel/ext4
	$(CC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
		-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O0 -g -Wall -Wextra \
		-Wno-unused-parameter -Wno-unused-function -std=c11 -I$(KERNEL_DIR)/ext4stub -I$(KERNEL_DIR)/ext4 \
		-DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_DEBUG_PRINTF=0 -DCONFIG_DEBUG_ASSERT=0 \
		-c $< -o $@

# mbedtls 4.2.0 (vendored PRISTINE) — one rule for all subdirs.
# Config: kernel/mbkern/{mbedtls_kernel_config,tfpsa_kernel_config}.h
# -nostdinc: HOST glibc headers must NOT leak into the kernel build
# (features.h defines __GLIBC__ -> explicit_bzero/unistd/SYS_getrandom
# paths light up and reference libc symbols that cannot exist here).
# GCC's own freestanding headers (stddef/stdint/stdarg/limits/float)
# are re-added via -isystem. -U: gcc target macros __linux__/__unix__
# make TF-PSA think it runs on a Unix-like OS
# (MBEDTLS_PLATFORM_IS_UNIXLIKE -> getpid(), <unistd.h>).
# -I mbkern/libc: kernel shim <string.h>/<stdio.h>/<time.h>.
GCC_INC := $(shell $(CC) -print-file-name=include)
MB_CFLAGS = -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
	-m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O1 -g -w -std=c11 \
	-nostdinc -isystem $(GCC_INC) \
	-U__linux__ -U__linux -U__unix__ -U__unix -Uunix -U__gnu_linux__ \
	-I$(KERNEL_DIR)/mbkern/libc \
	-I$(KERNEL_DIR)/mb/include \
	-I$(KERNEL_DIR)/mb/tls -I$(KERNEL_DIR)/mb/crypto/core -I$(KERNEL_DIR)/mb/crypto/extras -I$(KERNEL_DIR)/mb/crypto/utilities -I$(KERNEL_DIR)/mb/crypto/dispatch -I$(KERNEL_DIR)/mb/crypto/platform \
	-I$(KERNEL_DIR)/mb/crypto/drivers/builtin/src \
	-I$(KERNEL_DIR)/mb/crypto/drivers/builtin/include \
	-DMBEDTLS_CONFIG_FILE='"mbkern/mbedtls_kernel_config.h"' \
	-DTF_PSA_CRYPTO_CONFIG_FILE='"mbkern/tfpsa_kernel_config.h"' \
	-I$(KERNEL_DIR)

$(BUILD_DIR)/kernel/mb/%.o: $(KERNEL_DIR)/mb/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(MB_CFLAGS) -c $< -o $@

# tlsglue.c: kernel file, but needs the mbedtls include roots and
# the same config defines as the vendored library objects. mbkern/libc
# shim path comes FIRST so <time.h> resolves to the shim (same struct
# tm layout the mb objects see), not to the host glibc header.
$(BUILD_DIR)/kernel/tlsglue.o: $(KERNEL_DIR)/tlsglue.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(KERNEL_DIR)/mbkern/libc -I$(KERNEL_DIR)/mb/include \
		-I$(KERNEL_DIR)/mb/tls -I$(KERNEL_DIR)/mb/crypto/core \
		-I$(KERNEL_DIR)/mb/crypto/extras -I$(KERNEL_DIR)/mb/crypto/utilities \
		-I$(KERNEL_DIR)/mb/crypto/drivers/builtin/src \
		-I$(KERNEL_DIR)/mb/crypto/drivers/builtin/include \
		-DMBEDTLS_CONFIG_FILE='"mbkern/mbedtls_kernel_config.h"' \
		-DTF_PSA_CRYPTO_CONFIG_FILE='"mbkern/tfpsa_kernel_config.h"' \
		-I$(KERNEL_DIR) -c $< -o $@

# Compile kernel C files
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile kernel C++ files
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link kernel
$(KERNEL_BIN): $(KERNEL_OBJS) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@
	@echo "Kernel linked at $(KERNEL_BIN)"

# Create GRUB configuration
$(GRUB_CFG): | $(BUILD_DIR)
	echo 'set timeout_style=hidden' > $@
	echo 'set timeout=0' >> $@
	echo 'set default=0' >> $@
	echo '' >> $@
	echo 'menuentry "NullOs" {' >> $@
	echo '  multiboot2 /boot/kernel.bin' >> $@
	echo '  boot' >> $@
	echo '}' >> $@

# Create bootable ISO
iso: $(KERNEL_ISO)

$(KERNEL_ISO): $(KERNEL_BIN) $(GRUB_CFG)
	mkdir -p $(BUILD_DIR)/isodir/boot/grub
	cp $(KERNEL_BIN) $(BUILD_DIR)/isodir/boot/kernel.bin
	cp $(GRUB_CFG) $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(KERNEL_ISO) $(BUILD_DIR)/isodir 2>/dev/null || \
	xorriso -as mkisofs -b '(null)' -no-emul-boot -o $(KERNEL_ISO) $(BUILD_DIR)/isodir 2>/dev/null || \
	echo "Could not create ISO (grub-mkrescue/xorriso not available)"
	@echo "ISO created at $(KERNEL_ISO)"

# Persistent data disk (primary master, 64 MB) for ramfs sync/mount
$(DISK_IMG):
	qemu-img create -f raw $@ 64M || dd if=/dev/zero of=$@ bs=1M count=64

# Run in QEMU (via GRUB ISO: multiboot2 ELF64, entered in long mode)
run: $(KERNEL_ISO) $(DISK_IMG)
	$(QEMU) -cdrom $(KERNEL_ISO) -m 512M \
	-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
	-cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
	-netdev user,id=n0 -device rtl8139,netdev=n0 \
	-serial stdio \
	-no-reboot

# Debug mode with GDB
debug: $(KERNEL_ISO) $(DISK_IMG)
	$(QEMU) -cdrom $(KERNEL_ISO) -m 512M \
	-drive file=$(DISK_IMG),format=raw,if=ide,index=0 \
	-cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
	-netdev user,id=n0 -device rtl8139,netdev=n0 \
	-serial stdio \
	-no-reboot \
	-s -S

# Clean build
clean:
	rm -rf $(BUILD_DIR)
	@echo "Build directory cleaned"

# Rebuild
rebuild: clean all

# Disassemble kernel
disasm: $(KERNEL_BIN)
	objdump -D -M intel $(KERNEL_BIN) | less

# Show help
help:
	@echo "NullOs Build System"
	@echo "===================="
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build the kernel (default)"
	@echo "  run      - Run in QEMU"
	@echo "  iso      - Create bootable ISO"
	@echo "  debug    - Run with GDB stub"
	@echo "  clean    - Remove build files"
	@echo "  rebuild  - Clean and rebuild"
	@echo "  disasm   - Disassemble kernel"
	@echo "  help     - Show this help"

.PHONY: all run iso clean rebuild debug disasm help
