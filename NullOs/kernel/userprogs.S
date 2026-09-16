/*
 * Embedded user programs, linked into the kernel via .incbin.
 *
 * The build system compiles everything in userland/ to ELF binaries
 * in build/userland/ BEFORE building the kernel; this file then pulls
 * each binary into the kernel image so fs_init() can register it as a
 * file (e.g. /bin/hello). No host-side codegen needed — GAS .incbin
 * works identically on Linux and Windows toolchains.
 */

    .section .rodata
    .global hello_elf_start
    .balign 8
hello_elf_start:
    .incbin "build/userland/hello.elf"
    .global hello_elf_end
hello_elf_end:
    .balign 8

    .global forktest_elf_start
    .balign 8
forktest_elf_start:
    .incbin "build/userland/forktest.elf"
    .global forktest_elf_end
forktest_elf_end:
    .balign 8

    .global ttyprobe_elf_start
ttyprobe_elf_start:
    .incbin "build/userland/ttyprobe.elf"
    .global ttyprobe_elf_end
ttyprobe_elf_end:

/* Real Alpine Linux busybox.static (~1MB, musl-linked) */
    .global busybox_elf_start
    .balign 8
busybox_elf_start:
    .incbin "userland/busybox.static"
    .global busybox_elf_end
busybox_elf_end:
    .balign 8

/* fork-v2 concurrency proof (parent + 2 children interleaving) */
    .global parfork_elf_start
parfork_elf_start:
    .incbin "build/userland/parfork.elf"
    .global parfork_elf_end
parfork_elf_end:
    .balign 8

/* tiny native sleep for ash job-control tests */
    .global sleepbin_elf_start
sleepbin_elf_start:
    .incbin "build/userland/sleepbin.elf"
    .global sleepbin_elf_end
sleepbin_elf_end:
    .balign 8

/* NullOs native multi-call utility */
    .global nsh_elf_start
    .balign 8
nsh_elf_start:
    .incbin "build/userland/nsh.elf"
    .global nsh_elf_end
nsh_elf_end:
    .balign 8

/* native kernel-net-chain probe (socket/bind/connect/read echo) */
    .global nettest_elf_start
nettest_elf_start:
    .incbin "build/userland/nettest.elf"
    .global nettest_elf_end
nettest_elf_end:
    .balign 8

/* native minimal HTTP client over the kernel TCP stack */
    .global httpget_elf_start
httpget_elf_start:
    .incbin "build/userland/httpget.elf"
    .global httpget_elf_end
httpget_elf_end:
    .balign 8

/* native TCP echo-server: exercises listen()/accept() end-to-end */
    .global srvtest_elf_start
srvtest_elf_start:
    .incbin "build/userland/srvtest.elf"
    .global srvtest_elf_end
srvtest_elf_end:
    .balign 8

/* AF_UNIX transport prober (#wayland-transport): socketpair /
 * SCM_RIGHTS fd passing / epoll / path sockets */
    .global unixtest_elf_start
unixtest_elf_start:
    .incbin "build/userland/unixtest.elf"
    .global unixtest_elf_end
unixtest_elf_end:

    /* #wl-substrate E2E prober: eventfd/timerfd/memfd/readv/futex/
     * getrandom/epoll-real-abi — the dwl/libwayland readiness gate. */
    .align 4
    .global wltest_elf_start
wltest_elf_start:
    .incbin "build/userland/wltest.elf"
    .global wltest_elf_end
wltest_elf_end:
    .balign 8

    /* Layer-2 probes: DRM-lite (dumb buffer + modeset + pageflip)
     * and evdev (libinput substrate) E2E verifiers.                */
    .align 4
    .global drmtest_elf_start
drmtest_elf_start:
    .incbin "build/userland/drmtest.elf"
    .global drmtest_elf_end
drmtest_elf_end:
    .balign 8

    .align 4
    .global evtest_elf_start
evtest_elf_start:
    .incbin "build/userland/evtest.elf"
    .global evtest_elf_end
evtest_elf_end:
    .balign 8

    /* Layer-2 substrate prober: mprotect W^X enforcement, /proc
     * materialization, /dev/shm pools (wl_shm by-path shape),
     * EFD_SEMAPHORE.                                              */
    .align 4
    .global layertest_elf_start
layertest_elf_start:
    .incbin "build/userland/layertest.elf"
    .global layertest_elf_end
layertest_elf_end:
    .balign 8

    /* Layer-3 chain prober: libseat-lite seat lifecycle + mediated
     * device opens, libinput-lite context (open_restricted -> libseat),
     * DEVICE_ADDED caps/names, KEYBOARD_KEY + pointer events E2E.  */
    .align 4
    .global seatprobe_elf_start
seatprobe_elf_start:
    .incbin "build/userland/seatprobe.elf"
    .global seatprobe_elf_end
seatprobe_elf_end:
    .balign 8

/* ---- apk.static two-stage import (STAGE A build only) --------------
 * The 4.3MB Alpine apk-tools-static binary is embedded here ONLY for
 * the one-shot import boot: kernel_main copies it into the ramfs as
 * /bin/apk, storage_sync()s it to disk and powers off. Normal builds
 * keep this section OUT because a >4MB kernel image collides with
 * user VAs (0x400000+) and used to collide with the heap
 * (#heap-overlap). /bin/apk is restored from disk by storage_load()
 * on every regular boot.
 * Stage A is selected by the build system: make APK_DEFS="-DAPK_STAGE_A"
 * (plain `make` = blobless, safe).                                    */
#ifdef APK_STAGE_A
    .global apk_elf_start
    .balign 8
apk_elf_start:
    .incbin "userland/apk.static"
    .global apk_elf_end
apk_elf_end:
    .balign 8
#endif
