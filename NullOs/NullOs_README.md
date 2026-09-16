# NullOs

> **A small 64-bit hobby operating system that accidentally stopped being small.**

NullOs started as a from-scratch experimental x86_64 kernel.  
The original project was a minimal educational OS: boot, VGA, memory management, interrupts, and eventually user mode.

That description is now badly outdated.

Today NullOs has its own kernel, physical/virtual memory managers, processes with asynchronous scheduling, ELF user programs, a ramfs/VFS layer, persistent storage, device drivers, a Linux-like syscall subset, `fork()`/COW/`wait4()` with a genuinely parallel fork (v2), a signals/job-control core that really stops and resumes foreground jobs, and an RTL8139 networking path with live ICMP/UDP/TCP traffic flowing to the host.

A real Alpine BusyBox binary is embedded into the kernel image. Its interactive shell (`busybox sh`) runs in ring 3 with job control, and its `nc` applet speaks UDP and TCP to the outside world.

On top of that sits a **Wayland substrate**: AF_UNIX stream sockets with `SCM_RIGHTS` fd passing, epoll over the real x86_64 ABI numbers, eventfd, timerfd, signalfd, `memfd_create` with fd-backed shared `mmap`, and `readv`/`writev` — the syscall surface `libwayland` requires, prepared so that a real compositor (now **labwc**, see the readiness section) has something real to land on.

It is **not Linux** and it is **not a Linux distribution**.

Instead, NullOs implements its own kernel and uses a deliberately Linux-like userspace ABI where compatibility is useful.

---

## What is NullOs?

NullOs is a **from-scratch 64-bit x86_64 operating system** written primarily in C, with C++ and assembly where required.

Its current architecture includes:

- a freestanding kernel
- x86_64 long mode
- Multiboot2/GRUB boot
- physical memory management
- heap, slab and buddy allocators
- virtual memory and page tables
- page-fault handling
- ring-3 user processes
- ELF64 loading
- preemptive scheduling
- asynchronous process creation (`fork` v2: parent and children run in parallel)
- copy-on-write memory
- signals and job control (SIGSTOP/SIGTSTP/SIGCONT, process groups)
- safe address-space teardown and page recycling
- a small VFS/ramfs
- persistent filesystem storage
- ATA/IDE support
- FAT32 support
- RTL8139 networking
- framebuffer/VGA/virtual terminals
- PS/2 keyboard and mouse
- PCI, ACPI, APIC and SMP support
- Linux-compatible system calls for selected userspace software
- native userspace utilities
- musl-oriented compatibility work
- Alpine BusyBox integration

The goal is not to reproduce Linux internally.

The goal is much simpler:

> **If userspace needs a real mechanism, implement the mechanism instead of faking the result.**

---

# Current status

NullOs has moved well beyond its original "educational kernel" stage.

### Kernel

- [x] x86_64 long mode
- [x] Multiboot2 boot
- [x] GDT / IDT
- [x] IRQ handling
- [x] PIT timer
- [x] serial logging
- [x] VGA text output
- [x] virtual terminals / scrollback
- [x] framebuffer support
- [x] kernel logging
- [x] kernel panic / exception diagnostics
- [x] FPU/SSE initialization

### Memory management

- [x] physical memory bitmap
- [x] heap allocator
- [x] slab allocator
- [x] buddy allocator
- [x] virtual memory manager
- [x] x86_64 PML4/PDPT/PD/PT handling
- [x] page faults
- [x] demand paging infrastructure
- [x] page-table pool
- [x] protected carve area for VM/refcount metadata
- [x] physical page reference counting
- [x] copy-on-write infrastructure
- [x] self-map-aware PML4 destruction (recursive `[511]` entry guarded)
- [x] full lifecycle prove-out: spawn -> exit -> reap -> destroy -> reuse

### Processes and userspace

- [x] ring-3 execution
- [x] ELF64 loading
- [x] user stacks
- [x] process/task structures
- [x] process file-descriptor tables
- [x] `fork()` with v2 semantics — children scheduled asynchronously beside the parent
- [x] COW fork isolation
- [x] `wait4()` including `WUNTRACED` stop reports
- [x] zombie/reap lifecycle backed by real address-space destruction
- [x] `execve()` infrastructure
- [x] preemptive scheduling
- [x] kernel/user context switching
- [x] process groups and sessions (`setpgid`/`getpgid`/`setsid`)
- [x] `nanosleep()` and console `poll()` wakes

### Signals and job control

- [x] SIGSTOP/SIGTSTP actually stop tasks; SIGCONT resumes them
- [x] Ctrl-Z routed at IRQ time to the foreground process group
- [x] stopped children reported once via `wait4(WUNTRACED)` as `(sig << 8) | 0x7F`
- [x] `kill()` with negative-pid / own-process-group semantics
- [x] keyboard ISIG handling in both Set-1/Set-2 scan-code modes
- [x] end-to-end proof in Alpine ash: `sleep 5`, Ctrl-Z, `[1]+ Stopped`, `jobs`

### Filesystems

- [x] in-memory ramfs
- [x] file descriptors
- [x] directories
- [x] `open`/`read`/`write`/`close` paths
- [x] `getdents64`
- [x] VFS-style backend attachment
- [x] correct `stat`/`lstat`/`fstat` node typing (`S_IFDIR`, `S_IFREG`, `S_IFCHR`)
- [x] POSIX fd lifecycle: lowest-free-slot allocation, real `close(0..2)`, console sentinel fds
- [x] shell redirections (`cmd < file`, `cmd > file`) via ordinary open/dup/close
- [x] virtual device nodes `/dev/tty` and `/dev/console`
- [x] persistent ramfs storage
- [x] `sync` / restore on reboot
- [x] FAT32 mounting and file access
- [x] `/disk`-style FAT32 VFS exposure

### Networking

- [x] PCI device enumeration
- [x] RTL8139 driver rewritten around proper BAR handling (I/O ports vs MMIO)
- [x] correct ISR bit semantics and inline TX-completion waiting
- [x] software RX ring with CAPR tracking and wrap-safe parsing
- [x] Ethernet frame handling
- [x] IPv4 handling with wire-correct (big-endian) checksums
- [x] ARP resolution with kick/retry
- [x] ICMP echo: native `ping` truly observes replies (not inferred from side effects)
- [x] UDP datagram sockets with blocking and polled reads
- [x] TCP client: handshake, in-order data receive with ACKs, FIN/RST/EOF lifecycle
- [x] socket syscall infrastructure: bind/connect/sendto/recvfrom/setsockopt/getsockopt

This is still far from a complete general-purpose TCP/IP stack (no LISTEN/accept yet, no data-segment retransmit timer, no DNS), but packets genuinely leave the machine and come back.

### Devices / platform

- [x] ATA/IDE
- [x] PCI
- [x] ACPI
- [x] APIC
- [x] SMP/CPU detection
- [x] PS/2 keyboard
- [x] PS/2 mouse
- [x] AC97 audio support
- [x] RTL8139 Ethernet

---

# Userspace

NullOs can execute statically linked ELF64 programs in ring 3.

The project contains several native test programs:

```text
/bin/hello
/bin/forktest
/bin/nsh
/bin/ls
/bin/cat
/bin/echo
/bin/uname
/bin/sleep      # native, nanosleep-backed
/bin/nettest    # UDP echo round-trip, exit 42
/bin/httpget    # real HTTP GET into ramfs
/bin/busybox    # the embedded Alpine BusyBox itself
```

`nsh` is a small native multi-call userspace utility written without libc.  
It exists both as a practical userspace test and as a tiny BusyBox-style tool.

For example:

```text
elf /bin/echo hello from NullOs
hello from NullOs

elf /bin/uname
NullOs nullos 6.1.0-nullos x86_64

elf /bin/ls /
bin/ dev/ tmp/ ...

elf /bin/cat /hello.txt
Hello from NullOs!
```

The important part is that these are not kernel-side command handlers pretending to be processes.

They execute as **ring-3 ELF programs** and communicate with the kernel through system calls.

The newest additions are network-aware natives that served as stepping stones toward real software: `/bin/nettest` drives a complete UDP echo round-trip (socket -> bind -> connect -> write -> read) and exits with code 42 on success, while `/bin/httpget` performs a genuine HTTP GET over the TCP stack — connect, send the request, stream the answer, store the body in ramfs — making it the first program in NullOs history to download a file off the wire.

---

# Linux ABI compatibility

NullOs is **not Linux**.

There is no Linux kernel underneath the project.

Instead, NullOs implements a subset of the Linux x86_64 userspace ABI because it is a convenient compatibility target.

The current syscall layer includes, among others:

```text
# Files and I/O
read write open openat close
stat fstat lstat newfstatat
lseek getdents64 access faccessat
dup dup2 fcntl                     # real F_DUPFD / F_DUPFD_CLOEXEC / flag ops
ioctl                              # TCGETS TCSETS* TIOCGWINSZ TIOCSWINSZ
                                   # TIOCGPGRP TIOCSPGRP TIOCSCTTY FIONREAD
poll                               # console-backed
getcwd

# Processes
fork execve wait4                  # wait4 handles WUNTRACED stop reports
exit exit_group
getpid getppid
setpgid getpgid getpgrp setsid
kill                               # stop/continue subset, negative-pid aware
nanosleep

# Misc ABI surface
uname arch_prctl                   # ARCH_SET_FS for musl TLS
set_tid_address readlink
clock_gettime getrusage sysinfo
getuid geteuid
mmap brk                           # bump-allocation style
mprotect madvise
futex (single-thread) getrandom (limited)

# Networking
socket connect sendto recvfrom
sendmsg recvmsg                    # first-iovec dialect (musl resolver!)
bind listen accept accept4         # LISTEN/server side WORKS (tcp_srv)
setsockopt getsockopt              # AF_INET; DGRAM+STREAM+RAW(ICMP)
                                   # SOCK_RAW/SOCK_DGRAM+IPPROTO_ICMP:
                                   # user ping incl kernel paced retransmit
socketpair                         # AF_UNIX SOCK_STREAM + SCM_RIGHTS
                                   # fd passing across sendmsg/recvmsg/fork
                                   # (the libwayland wire + wl_shm transport)

# Wayland substrate (#wl-substrate)
epoll_create1 epoll_ctl epoll_wait
epoll_pwait                        # REAL x86_64 numbers: 291/233/232/281
eventfd2                           # counters, ppoll/epoll readiness
timerfd_create timerfd_settime
timerfd_gettime                    # wl_event_loop timers
signalfd4                          # honest no-signal-yet semantics
memfd_create ftruncate             # pool pages, fd-backed shared mmap
readv writev                       # libwayland wire gather/scatter
ppoll dup3                         # musl poll lowering / CLOEXEC dup
getrandom                          # real bytes (musl loops on 0)
futex                              # single-thread honest semantics
```

A few of these are honest stubs because the current userspace does not yet require their full Linux semantics.

This is a compatibility layer, not an attempt to claim Linux conformance.

---

# musl and BusyBox

One of the project's current experiments is running software built against **musl libc**.

The repository contains a real statically linked Alpine BusyBox binary:

```text
userland/busybox.static
```

It is an x86_64 static ELF executable of roughly 1 MB.

The kernel embeds it into the final image and exposes it as:

```text
/bin/busybox
```

The project therefore has to provide enough of the surrounding ABI for real musl-linked software to start.

This is why NullOs contains compatibility work such as:

- `arch_prctl()` for FS-base/TLS setup
- a populated auxiliary vector
- `openat()`, `newfstatat()`, `lseek()`, `getdents64()`
- fd-lifecycle semantics musl can rely on (lowest free slot, honest close)
- `ioctl()` termios/win/tty-job requests behind `/dev/tty` and `/dev/console`
- console-backed `poll()` for ash's line editor
- `uname()`, `getppid()`, `exit_group()`, `getcwd()`
- `nanosleep()` and real `fcntl()` duplication semantics
- limited `mprotect()`, `madvise()` and single-threaded `futex()` behavior

The original aspiration was to reach:

```text
/bin/busybox sh
```

rather than merely implementing a fake shell inside the kernel. That aspiration has since become reality:

```text
/ # uname -a
NullOs nullos 6.1.0-nullos #1 SMP NullOs x86_64 Linux
/ # sleep 5
^Z[1]+  Stopped                 sleep 5
/ # jobs
[1]+  Stopped                   sleep 5
```

Alpine ash starts with interactive job control enabled (its sigaction trio, `setpgid`, `TIOCSPGRP` and terminal-attribute dance all succeed), renders its prompt, runs builtins directly and spawns external applets through real `fork()` + `execve()` + `wait4()` cycles.

Applets exercised so far include `sh`, `uname`, `echo`, `ls` (with directory colouring, thanks to correct `S_IFDIR` stats), `cat`, `sleep`, `jobs` — and the network pair `nc -u` (UDP) plus `nc` (TCP), both achieving genuine round-trips against host-side test servers.

---

# labwc / Wayland compositor readiness

> **Pivot (2026-09-16):** the compositor target moved from **dwl** to **[labwc](https://github.com/labwc/labwc)** (0.20.2, vendored at `../labwc` for reference; `../dwl` stays for comparison). Reason: labwc's codebase is **modular and readable** (~19.6 KLOC across 40+ focused files: `config/`, `input/`, `ssd/`, `menu/`, `img/`, `seat.c`, `output.c`, `view.c`…) where dwl is a single dense 3.2 KLOC `dwl.c` — far easier to study, stub and adapt piece by piece. labwc also brings a stacking-window desktop (Openbox-style decorations, menus, workspaces) instead of dwm tiling, which is a better showcase on a single-output QEMU std-VGA machine.

The price is a heavier dependency tree — and the honest plan accounts for every gram of it (see Layer 4 below). Both compositors share the same monster anyway: **wlroots**; and everything done in Layers 0–3 below is compositor-agnostic — it is exactly what wlroots+libwayland+libinput+libseat need, whoever sits on top.

This is a multi-layer port, and NullOs works through it bottom-up.

## Layer 0 — done: the libwayland syscall substrate

Everything libwayland's wire protocol and event loop touch:

- **AF_UNIX SOCK_STREAM** with `socketpair`, `bind/listen/accept/connect` on display paths (`/tmp/wayland-0`), full duplex rings, EOF semantics, fork-ref inheritance;
- **SCM_RIGHTS fd passing** through `sendmsg`/`recvmsg`, with per-direction pending queues, in-flight ref transfer, and drop-on-teardown — the mechanism wl_shm pools travel by;
- **epoll** (`epoll_create1`/`epoll_ctl`/`epoll_wait`/`epoll_pwait`) on the **real x86_64 numbers** (291/233/232/281). An earlier draft used probe-only numbers (290/289) that musl never issues — fixed;
- **eventfd2**, **timerfd** (one-shot + periodic), **signalfd4**, all integrated into `poll`/`epoll` readiness;
- **memfd_create** + **ftruncate** + **fd-backed shared mmap**: pool pages are real physical frames mapped into every sharer's address space — the exact wl_shm model (verified E2E: a fork child writes a pattern into the pool, the parent reads it through its own mapping);
- **readv/writev** for the wire's gather/scatter output buffering;
- **getrandom** (real bytes), **futex** (honest single-thread semantics), **ppoll**, **dup3**.

Proof: `/bin/wltest` (7 stages, embedded in the image) and `/bin/unixtest` (4 stages) both pass **ALL-PASS** in ring 3.

## Layer 1 — done: the scripting substrate for building/porting

Shell infrastructure that `make`/`configure`/`tar` style flows need:

- full ash pipelines with EOF (`cat | wc -l`), two-stage pipelines, pipes inside `sh -c`, and **command substitution `$( )`** — the last one exposed `#dup2-oldslot-leak` (dup2 over a pipe fd silently dropped the old slot's kernel-object reference; the substitution pipe's writer count never reached zero and every `$( )` hung the shell forever) — fixed along with `#stdio-redirect-fd` (real files on fd 0/1/2 after redirection were rejected by a stale `fd < 3` guard) and blocking-wait loops that hlt'ed without yielding (preemption is off by default, so `task_yield()` is mandatory wherever a syscall waits on *another task*);
- **54 busybox applet symlinks** seeded into `/bin` (`sh`, `wc`, `grep`, `tr`, `tar`, `gzip`, `find`, `sed`, `awk`, ...) so scripts can use bare names; `cat`/`ls`/`echo`/`uname` stay owned by the native shell's multi-call binary;
- `apk add` already works end-to-end against the real Alpine CDN (index verify, resolve, download, install), which is the delivery path for musl-linked build tools.

## Layer 2 — done: the device substrate (evdev + DRM-lite + mem APIs)

The items the old roadmap called "real kernel work" are in and verified:

- **evdev synthesis** — `/dev/input/event0` (keyboard) and `/dev/input/event1` (mouse): the PS/2 IRQ producers push `input_event` streams (set-1 make/break → Linux keycodes, raw REL deltas, EV_SYN reports) into strict SPSC rings; `read()`/`ioctl(EVIOCG*)`/nonblock/epoll-readiness behave like libinput expects. Proof: `/bin/evtest` ABI-PASS; the keyboard fix below also means the mirror stream carries shift make/break pairs now;
- **DRM-lite on `/dev/dri/card0`** — QEMU std-VGA's Bochs DISPI interface drives a real modeset (1024x768x32 linear framebuffer through the PCI BAR), dumb-buffer create/map, CRTC set and vblank-ordered pageflip with completion events on `read()`. Proof: `/bin/drmtest` exit 0 (modeset + two dumb buffers + flip + clean text-mode restore);
- **`mprotect(10)`** — real PTE flag updates (W/NX), no longer a stub. The pin survives fork: `#mprotect-cow-mask` adds a `VMM_STICKY_RO` PTE bit that fork copies verbatim (instead of converting to COW) and that the COW handler refuses to resolve, so a write to a pinned page really faults — and `#pf-kill-task` turns unresolvable ring-3 faults into a SIGSEGV-style task kill (`exit_code=11`, `wait4`-visible) instead of halting the whole machine. `PROT_WRITE` eagerly unshares COW frames (with W set the CPU would never fault, so both processes would scribble one frame). Proof: `/bin/layertest` stage 1 — a fork child that writes an RO page dies, the parent keeps reading and writing neighbouring pages;
- **`/proc`** — minimal procfs by materialize-on-open (`/proc/meminfo`, `cpuinfo`, `uptime`, `version`, `loadavg`, `osrelease`, `self/cmdline`, `self/maps`): generators run at `open`/`stat` time and in the shell's file builtins (`procfs_touch`), so busybox `cat`/`grep`/`head` pipeline them. `#procfs-cmdline`: task names are exec paths (fork children inherit), not `proc<N>`;
- **POSIX shm by path (`#shm-open-path`)** — musl `shm_open` = `open("/dev/shm/<name>")`; pools are keyed by path, `ftruncate` sizes them, `MAP_SHARED` maps the same physical frames into every opener (fork-safe, devmap-registered), `O_TRUNC` resets, `unlink` destroys, `fstat` reports the honest size. Proof: `/bin/layertest` stage 3 — two mappings in one process plus a fork child all share frames through the same path;
- **`EFD_SEMAPHORE`** — eventfd reads hand out one unit at a time. Proof: `/bin/layertest` stage 4;
- **`#mmap-fd-oob`** — mmap's `fd` argument is bounds-checked before fd-class dispatch (anonymous mmaps pass fd=-1 and used to index far past the fd table);
- **`#kbd-set1-shift`** — QEMU 10 rejects the PS/2 Set-2 switch, and raw Set-1 mode never tracked shift, so capitals and shifted punctuation were untypeable. Shift make/break now flow through the keyboard ring (consumer tracks in order, like Ctrl already did), Caps toggles, and the evdev mirror sees the events.

Proof binary: `/bin/layertest` (4 stages) — **ALL-PASS** in ring 3, alongside the still-green `/bin/wltest` + `/bin/unixtest`.

## Layer 3 — porting the libraries (libseat-lite + libinput-lite DONE)

**labwc 0.20.2 links against wlroots-0.20** (`dependency('wlroots-0.20', >=0.20.1 <0.21)`), wayland-server >=1.22, wayland-protocols >=1.39, xkbcommon, libdrm, **libxml2** (rc.xml Openbox-style config), **glib-2.0** (small surface: `g_shell_parse_argv`, `g_ascii_strcasecmp`, dir helpers), **cairo** (image-surface drawing for SSD/menus), **pangocairo** (all text goes through the single `src/common/font.c` wrapper), **libpng** (+zlib, already proven in-tree), libinput >=1.26, pixman. Optional and to be disabled at configure time: `xwayland`, `svg` (librsvg), `icon` (libsfdo), `labnag`, `nls`.

The realistic porting order (wlroots/libseat/libinput/libdrm steps are compositor-agnostic — identical for dwl or labwc):

1. **xkbcommon** — pure computation + keymap file parsing; needs only the existing file ABI. Closest to free.
2. **pixman** — pure computation; compiles freestanding. Close to free.
3. **libseat** — **DONE (lite)**, see below.
4. **libinput** — **DONE (lite)**, see below.
0. **wayland-server + libffi** — the wire protocol core; pure C over the Layer-0 substrate (AF_UNIX+SCM_RIGHTS+epoll are done). libffi closures need W^X pages — `mprotect` is real since Layer 2.
1. **xkbcommon** — pure computation + keymap file parsing; needs only the existing file ABI. Closest to free.
2. **pixman** — pure computation; compiles freestanding. Closest to free.
3. **libseat** — **DONE (lite)**, see below.
4. **libinput** — **DONE (lite)**, see below.
5. **libdrm-lite** — only the card0 ioctl wrappers wlroots' DRM backend calls; the kernel side already speaks dumb buffers/modeset/pageflip (Layer 2), so this is glue, not kernel work.
6. **wlroots-0.20 minimal build** — `backends=drm,libinput,headless`, `renderers=pixman`, no GLES2/Vulkan/X11: `wlr_backend_autocreate` then resolves to DRM(+headless), `wlr_renderer_autocreate` to the pixman software renderer. `wlr_renderer_init_wl_shm` rides the /dev/shm pool model. EGL/GLES is the long pole we explicitly skip — the pixman renderer is the software path.
7. **glib-lite shim** — ~200 lines covering labwc's ~8 call sites (shell-parse, strcasecmp, home-dir helpers).
8. **libxml2 (reader-mode build)** + **libpng** — plain musl-static compiles, no exotic deps.
9. **cairo (image-surface-only build)** — `-Dfontconfig=disabled -Dfreetype=disabled`: labwc draws ALL text through pango, never cairo's own text API, so the font backends compile out.
10. **pango-lite** — the decisive shim: labwc's entire pango surface is the `src/common/font.c` wrapper (`PangoFontDescription` set/get, `pango_cairo_create_layout`, `set_text/set_markup/set_ellipsize/get_extents`, `pango_extents_to_pixels`, `g_object_unref`). Back it with an embedded bitmap font (the kernel's 8x16 VGA glyph table is already in-tree) rendering into cairo image surfaces — exactly the libseat-lite/libinput-lite playbook: upstream API shape, lite implementation.
11. **labwc itself** — first bring-up borderless (DRM output, no SSD, compiled-in default config), then decorations/menus as pango-lite grows. Once wlroots runs, the compositor is configuration.

### The wlroots libinput question

wlroots' libinput backend enumerates devices through **udev**; NullOs has no uevents. Two viable paths (decide at the wlroots port): patch the backend to iterate `/dev/input/event*` and feed libinput path-mode (`libinput_path_add_device`), or extend libinput-lite until it satisfies the wlroots call surface directly — the interface callbacks already match (Layer 3 proof).

### Layer 3 substrate: libseat-lite + libinput-lite (verified by /bin/seatprobe)

Two freestanding libraries in `userland/lib/` implement the REAL libseat/libinput call surface (function names, enum values and fd semantics match upstream 1.x) so the future dwl/wlroots port links its existing code shape:

- **libseat-lite** (`libseat_lite.{h,c}`) — the "builtin" seat backend: `libseat_open_seat` fires `enable_session` immediately (single always-active session, no VT switcher on NullOs), `libseat_get_fd` returns an eventfd wake source (never signaled — pollers just see no session events), `libseat_dispatch` is a poll with timeout, `libseat_open_device`/`close_device` open device nodes with the exact flags libinput passes (`O_RDWR|O_CLOEXEC|O_NONBLOCK`, RDONLY fallback), and the honest error path rejects nonexistent nodes. The mediated-over-socket seatd backend stays a documented follow-up (SCM_RIGHTS fd passing already exists in the substrate).
- **libinput-lite** (`libinput_lite.{h,c}`) — `libinput_udev_create_context` (udev arg accepted, ignored: enumeration is a direct `/dev/input/event*` scan), `libinput_udev_assign_seat` opens nodes through the **interface callbacks — i.e. through libseat, the exact dwl call chain** — and classifies each device from the kernel's `EVIOCGBIT` answers (keyboard = `KEY_ESC` bit set, pointer = `REL_X` bit set). The context owns an internal **epoll over the device fds** (real syscall numbers, so `libinput_get_fd` is epoll-correct for wlroots' event loop). Event state machines keep libinput semantics: `DEVICE_ADDED` per node; `EV_KEY` → `KEYBOARD_KEY` (codes 300/400/402/403 match upstream); `EV_REL` deltas accumulate between `SYN_REPORT` pairs into one `POINTER_MOTION` per frame; button transitions → `POINTER_BUTTON`; `REL_WHEEL` → `POINTER_AXIS` (source = wheel). Events hand out from a static SPSC queue via `libinput_get_event`, consumed by `libinput_event_destroy`.
- **kernel: `EVIOCGBIT`/`EVIOCGKEY`** added to `kernel/evdev.c` — the classification ioctl pair real libinput/libevdev depend on: `EVIOCGBIT(EV_SYN/KEY/REL/ABS)` + `EVIOCGKEY` (nothing held initially), with per-slot masks and a full 96-byte `EV_KEY` window (mouse buttons live at `0x110..0x112`, beyond the old 32-byte keyboard-only mask).

**Proof binary: `/bin/seatprobe`** (4 stages, ring 3):

1. **t1 SEAT PASS** — seat lifecycle: `open_seat` → `enable_session` fired, session identity, wake fd, `dispatch(50 ms)`, mediated opens of `/dev/dri/card0` + both evdev nodes, bad path (`/dev/input/nope9`) rejected, `close_device` ×3, `close_seat`;
2. **t2 CTX PASS** — libinput-lite context over a fresh seat via the wlroots call shape (`open_restricted` → `libseat_open_device`): 2 × `DEVICE_ADDED`, `event0 name=event0 caps=KEY`, `event1 name=event1 caps=+POINTER` (caps straight from the new `EVIOCGBIT`), vendor/product ids;
3. **t3 KBD PASS** — poll on the libinput fd → dispatch → `KEYBOARD_KEY` presses for keys injected by the host harness via HMP `sendkey` (`a`=30, `b`=48), make/break states correct;
4. **t4 PTR PARTIAL** — pointer device enumerated with `POINTER` cap, but this sandbox's QEMU (`-display none`) has no PS/2 aux device, so no motion/button events can be generated (same environment precedent as evtest; the state machines are exercised by construction).

Verification E2E: `scripts/seatprobe_e2e.py` (unix-socket HMP driver, sendkey injection, `xp` VGA snapshots) → **SEATPROBE ALL-PASS**. Regression with the new kernel: `wltest`/`unixtest`/`layertest` ALL-PASS, `drmtest` exit=0 (modeset + dumb buffers + text restore), `evtest` exit=0 (ABI), busybox ash script with a pipeline (`cat /proc/meminfo|head -n 1` → `MemTotal: 524160 kB`) E2E.

## Post-audit boot validation (2026-09-16)

The ~30-fix audit pass (see `BUGFIXES-AUDIT.md`) had only ever been compile-verified. It is now **boot-verified E2E**, and the validation itself caught two regressions of the audit — both closed on the spot:

- **#audit-mmap-regression** — the audit's `#uaccess-supervisor-hole` walker (`vmm_range_is_user`) was also fed the *mapping targets* of `mmap(2)`. Every process PML4 inherits the supervisor identity alias over [4MB,1GB), and the OS's own mmap region starts at 0x20000000 — inside that window — so every fd-backed mmap (memfd pools, /dev/shm pools, DRM dumb buffers) walked as present-without-VMM_USER and returned `-ENOMEM`: wltest t4, layertest t3 and drmtest "mmap A" all failed. Fix: `mmap_range_ok()` (static range rules only) validates mapping targets in all four fd-backed/MAP_FIXED branches, while the strict walker stays in force for every buffer-I/O syscall (read/write/poll/ioctl arg paths). The kernel is not *reading* user memory through a mapping target — `vmm_map()` is about to replace the alias entries with VMM_USER pages, exactly like `sys_brk` always grew over the alias unchecked.
- **#audit-mmap-regression (ownership guard)** — the MAP_FIXED replacement paths used to `pmm_free_page()` whatever frame the old mapping pointed at: a MAP_FIXED over the inherited alias handed *live kernel RAM* back to the PMM (double-alloc corruption; a huge-PDE alias was even unmap-no-op'ed while its PA was freed). New `vmm_leaf_is_user()` — only frames whose old leaf carries VMM_USER were ever process-owned; alias frames are never released.
- **#fs-size-symlink** — `fs_file_size()` used the *no-follow* path resolver while `fs_open()` follows final symlinks, so any exec through an applet symlink (`elf /bin/sh script`) failed as a bare `-1` "Failed to create process" while `elf /bin/busybox` worked. Size is a stat-class operation — it now uses the follow resolver. (This trap predates the audit; the previous harness always spelled busybox out.)

Verified after the fixes (QEMU 10.0.11, std-VGA + slirp, unix-HMP driver): boot 7.6 s; `hello` exit 42; **wltest / unixtest / layertest ALL-PASS (exit 0)**; **drmtest exit 0** (dumb-create x2, modeset, pageflip, `[MMAP] drm-dumb ... ret=20000000`, text-mode restore); **seatprobe ALL-PASS** (t1 SEAT/t2 CTX/t3 KBD with sendkey injection; t4 PTR stays environment-limited: no PS/2 aux under `-display none`); **nettest UDP round-trip exit 42**; **httpget HTTP-over-TCP exit 42** (first fetch also lands the script for ash); **ash pipeline smoke 4/4 markers** (`ALPHA-BETA`, `wc -l` pipe, `/proc/meminfo|head`, `$(tr)` substitution, background jobs + `wait`, `ASH-SMOKE-DONE`) — now through `/bin/sh` itself; `spawn 3` and `multi 3` (isolated) green. NIC bring-up is `drivers init <n>` as before (rtl8139 at index 5 in this QEMU layout).

## ash readiness (closed before Layer 3)

Busybox ash was silently lethal on this kernel — the hunt produced four roots, all closed:

- **#tss-identity-overlap** — the TSS descriptor's base was packed with a 24-bit mask, so the CPU read the TSS through the *identity window* (`linear 0x40A080`). Every busybox-sized image (text at `0x400000..0x4F7450`) overwrites that window's split-PT PTE with an image frame; the next timer tick from ring 3 then switched stacks through a fake TSS read out of the image bytes → `#SS`/`#DF` → silent triple fault at an arbitrary point of the script (this was **#bb-spawn-race**). Fix: encode `base[31:24]` (descriptor byte 7) — the TR now reads through the kernel `.high` mapping (`0x4040A080`), which no user image can shadow.
- **#sigsuspend-spin** — `rt_sigsuspend` was ENOSYS, so ash's `wait` on background jobs spun forever. Fixed by a **minimal SIGCHLD machine**: `rt_sigaction` stores the handler + SA_RESTORER for SIGCHLD, `rt_sigsuspend` blocks until a child event and *delivers* by patching the live trap frame (handler entry, then musl's restorer → `rt_sigreturn` restores from the kernel-built blob on the user stack). SIGCHLD regenerates while unreaped zombies exist (Linux semantics); `execve` resets handlers.
- **#zombie-reap-race** — the scheduler's reclaimer deactivated zombie *processes* before the parent's `wait4` could see them; background children became un-reapable ghosts. Zombies with a living parent are now strictly wait4-owned; orphan zombies are released by the kernel.
- **#frame-ptr-park** — the global `syscall_frame_ptr` was clobbered by any other task's syscalls while a task was parked inside its own syscall; late frame patches (signal delivery) hit dead frames. The pointer is now saved/restored per task at every switch site.

Verification: a full ash stress (12 sequential spawns, command substitutions, nested `ash -c`, redirects, 4 background jobs + `wait`, pipelines) runs to completion with zero faults.

## Honest gaps the compositor will hit

- `futex` is single-threaded — dwl is single-threaded, so this holds.
- Signals exist only for SIGCHLD delivery (enough for ash job control); no general signal delivery — wlroots' crash handler and SIGHUP handling degrade to nothing.
- `PROT_NONE` mprotect degrades to read-only (not-present pages would be re-created by the demand pager); everything else W^X is enforced.
- No VMA list: `mprotect` validates ENOMEM per-page over its range; `/proc/self/maps` reports the honest subset (text window, mmap bump region, stack).
- `/proc` materializes on open/stat — directory listings only show files already generated at least once.


---

# Process model

Processes use separate address-space structures and support copy-on-write cloning.

A simplified `fork()` path is:

```text
parent address space
        |
        v
   clone page tables
        |
        +---- kernel mappings
        |
        +---- user pages
                 |
                 v
          mark shared/COW
                 |
        +--------+--------+
        |                 |
      parent            child
        |                 |
      read              read
        |                 |
        +-------+---------+
                |
          child writes
                |
                v
             #PF
                |
                v
          COW page copy
```

The intended observable semantics are the familiar ones:

```text
parent: fork() -> child_pid
child:  fork() -> 0
```

and:

```text
wait4() -> child_pid
status = exit_code << 8
```

Since fork v2 the parent no longer blocks inside the syscall: the child is parked on the run queue with a preserved trap frame and scheduled independently, so true interleaving is observable. The dedicated `parfork` proof spawns chains of parallel children and verifies nondeterministic-but-bounded interleaving with clean reaping (PASS codes 30/50). Stopped children additionally report once through `wait4(..., WUNTRACED)` with `(sig << 8) | 0x7F`, exactly like Linux.

The project contains dedicated fork/COW regression tests because memory isolation is one of the places where a tiny mistake can corrupt the entire kernel.

---

# Filesystem model

NullOs currently has a small in-memory filesystem with persistent storage support.

The ramfs is used for normal userspace files:

```text
/
├── bin/
│   ├── hello
│   ├── forktest
│   ├── nsh
│   ├── ls
│   ├── cat
│   ├── echo
│   ├── uname
│   └── busybox
├── dev/
├── tmp/
└── ...
```

`sync` serializes the in-memory filesystem to the attached ATA disk.

On the next boot, the saved filesystem can be restored automatically.

Directory nodes now carry honest `S_IFDIR` types, so foreign `ls` implementations enumerate directories properly instead of misreading them as files. Virtual character devices (`/dev/tty`, `/dev/console`) resolve through the same namespace and expose exactly the ioctl surface modern shells demand during startup.

There is also a small FAT32 implementation capable of mounting and accessing a FAT32 volume, with a VFS backend used to expose mounted files.

---

# Networking

Networking earned its keep slowly, and only by making every layer below `write()` true.

Through QEMU's user-mode slirp backend, NullOs participates in real packet exchanges:

```text
guest (RTL8139)  <->  slirp  <->  host-side test servers (python echo/http)
```

Verified end-to-end paths today:

| Path | Program | Result |
|-------|---------|--------|
| ICMP | native `ping 10.0.2.2` | reply genuinely observed |
| UDP | Alpine `busybox nc -u 10.0.2.2 9000` | payload `udp-ok-nulos` echoed back from the host |
| TCP | Alpine `busybox nc 10.0.2.2 9100` | typed string echoed over a stream |
| TCP+HTTP | native `/bin/httpget` | first HTTP GET download landed in ramfs |

Getting here required fixing the unglamorous bottom of the stack: BAR-space confusion (I/O ports vs MMIO) inside the RTL8139 driver, transposed TxOK/RxOK ISR bits, an oversized RBLEN mismatching the software ring size, CAPR preload/wrap arithmetic — and, most infamously, IP checksums stored little-endian, which made slirp silently drop *every* outbound frame, ICMP included. That last one was cornered with QEMU `filter-dump` pcaps plus a byte-exact forensic decoder (`pcap_sum.py`) and then fixed in all three packet builders at once. The lesson is now burned into the codebase: checksum fields are numbers on a wire, not byte arrays.

Honest current bounds: the TCP path is client-side only (SYN retransmits exist, data-segment timers do not), the receive window is fixed-size, `LISTEN`/`accept` return errors, and there is no DNS resolution yet.

---

# Hardware and QEMU

The normal development environment is QEMU on x86_64.

The default QEMU configuration provides:

- 512 MB RAM
- an IDE/ATA disk image
- an RTL8139 NIC with user-mode (slirp) networking
- serial output as the primary debugging channel
- x86_64 CPU features required by the current kernel

During development a few extra channels proved invaluable and are wired into the test harnesses:

- `-d int` exception tracing — the reliable oracle that distinguishes real hangs from silent triple faults;
- `-object filter-dump` pcap captures for wire-level forensics;
- host-side python peers (`udp_echo.py`, `tcp_echo.py`, plain `http.server`) playing the far end of the wire.

The project is primarily intended for experimentation under QEMU rather than production hardware.

---

# Building

### Requirements

A typical Linux/WSL environment needs:

- GCC
- G++
- GNU `ld`
- NASM/GAS-compatible assembly tooling
- `make`
- QEMU
- GRUB tooling (`grub-mkrescue`) or `xorriso`
- `qemu-img`

On Windows, the repository also contains a PowerShell build helper:

```text
build.ps1
```

### Build the kernel

```bash
make
```

### Build the bootable ISO

```bash
make iso
```

### Run in QEMU

```bash
make run
```

### Run with GDB stub

```bash
make debug
```

Then attach GDB to the QEMU stub.

### Clean

```bash
make clean
```

### Full rebuild

```bash
make rebuild
```

### Disassemble

```bash
make disasm
```

---

# Regression and debugging

NullOs contains a growing collection of shell scripts for subsystem and end-to-end testing.

Examples include:

```text
scripts/smoke.sh          # boot + basic sanity
scripts/regress.sh        # multi-scenario regression with per-snapshot verdicts
scripts/fork_test.sh      # fork/COW/wait4 isolation battery
scripts/preempt_test.sh   # preemption interference probe
scripts/preempt2.sh       # second-generation preemption scenario
scripts/double_exec.sh    # repeated exec attempts on recycled slots
scripts/pipe_test.sh      # pipe family checks
scripts/vfs_test.sh       # ramfs/VFS surface
scripts/nsh_test.sh       # native nsh applet behaviour
scripts/bb_test.sh        # embedded BusyBox battery
scripts/e2e.sh            # combined end-to-end chain
scripts/probe_net.sh      # ICMP-level network probe
scripts/gdb_fork.sh       # scripted GDB session against the stub
```

Alongside the tree (one level up in the workspace, shipped together with the project) lives the development-harness collection deliberately kept out of the kernel repository itself:

- environment bootstrap: `setup_env.sh`, `env.sh`, ISO assembly via `build_iso.sh` (grub-mkimage/xorriso composition, adopted after stock grub-mkrescue misbehaved in the sandbox);
- master runner: `full_regress.sh`;
- job-control probes: `jc_test.sh`, `tstp_test.sh`;
- network batteries: `net_test.sh` (UDP nc), `net_tcp_test.sh` (TCP nc), `net_http_test.sh` (HTTP download), `net_native.sh`, `net_pcap.sh` plus `pcap_sum.py` for byte-exact capture analysis;
- host-side peers: `udp_echo.py`, `tcp_echo.py`;
- hang diagnostics: `quickprobe.sh`, `hangprobe.sh`;
- serial-log verification: `vga_chunks.py`.

The BusyBox debugging scripts deliberately run the real embedded Alpine binary and collect serial syscall traces, VGA output, QEMU exception logs, interrupt traces and monitor state.

This is useful because large real-world ELF programs expose VM/loader/ABI bugs that tiny handcrafted test programs may never reach. Two further oracles proved decisive repeatedly: exception traces (`-d int`) and wire-level pcaps, which cleanly separate "packet lost" from "packet never sent".

---

# Current known limitations

NullOs is still experimental.

Some hard areas were genuinely closed recently; what remains open is documented just as honestly.

Recently solved — worth remembering because they shaped the architecture:

- large-ELF ring-3 execution: fixed at its roots (stale huge-PDE reuse during inline splits, architectural shadowing of the PMM bitmap and VGA scalars inside busybox VA range, FS-base clobbering in the syscall entry path, canonical user-stack layout);
- address-space lifecycle: teardown is now enabled and proven across repeated spawn/reap cycles (self-map `[511]` guards, double-free elimination in page-table destruction, USER-alias promotion fix that once turned kernel windows into ring-3-mapped aliases).

Still open:

### Scheduler / fd-table architecture

Global fd-table snapshots are handed over at context-switch sites. The yield-site handover is intentionally disabled until per-task fd tables land, because blind snapshots become poisonous the moment processes run concurrently. This is the main structural debt left in the process layer.

### Signal delivery beyond stop/continue

The stop/continue engine is real (task parking, scheduler skipping, single-shot WUNTRACED reports, Ctrl-Z routing at IRQ time), but delivering custom handlers registered through `rt_sigaction` into ring 3 is still stub territory, as is full sigprocmask semantics.

### Foreground resume over duplicated ttys

An ash-visible quirk: a duplicated `/dev/tty` descriptor is not yet recognized as console-backed in the ioctl path, so `tcsetpgrp` fails with ENOTTY and busybox occasionally skips resuming a CONT'd job. The sentinel-fd machinery already covers the plain stdin/stdout cases.

### Extreme-load userspace faults

A sustained `yes`-style writev flood eventually reaches a musl `vfprintf` prologue with a misaligned user RSP and takes a `#GP`. Rare but deterministic, and reproducible without any fork involvement; the prime suspect is RSP restoration across preemption/cooperative resume paths under heavy IRQ load.

### Networking scope

Full client **and** server TCP (LISTEN/accept proven E2E through slirp hostfwd), per-socket retransmission with RTO/backoff, window updates, DNS resolution (hosts file + real DNS over our UDP) and internet-scale HTTP(S) downloads (mbedTLS with CA verification) all work. What remains: no IP fragmentation, small fixed socket table, ICMP limited to echo (no cap_net_raw on the host sandbox blocks internet-side ICMP forwarding).

### ABI completeness

The Linux ABI subset remains intentionally incomplete. A program that expects a full Linux environment will still not necessarily work merely because it is an x86_64 ELF binary.

### Wayland substrate boundaries

The libwayland surface is real but bounded: `signalfd` never reports readable (no user-defined signal handlers yet), `futex` carries single-thread semantics only, `mprotect` is still a success-stub, memfd pools cap at 4 MiB, and epoll instances cap at 4 with 32 registered fds each. The DRM/KMS, evdev and EGL layers of the dwl port (see the dwl readiness section) are kernel work that has not started.

---

# Project structure

The current repository is roughly organized as:

```text
NullOs/
├── boot/             # boot entry code
├── include/          # kernel headers
├── kernel/           # kernel implementation
│   ├── mm.c / buddy.c    # PMM/VMM/heap/slab/buddy/COW
│   ├── proc.c            # processes, fork-v2/exec/wait/reap
│   ├── scheduler.c       # scheduling + stop/continue task parking
│   ├── syscall.c         # Linux-ABI syscall layer
│   ├── elf.c             # ELF64 loader
│   ├── fs.c              # ramfs/VFS + virtual /dev nodes
│   ├── storage.c         # persistence (sync/restore)
│   ├── fat32.c           # FAT32 volume support
│   ├── net.c             # eth/ARP/IPv4/ICMP/UDP/TCP-core
│   ├── rtl8139.c         # NIC driver (BAR-correct)
│   ├── ata.c             # ATA/IDE
│   ├── keyboard.c / mouse.c
│   ├── vt.c / fb.c / vga.c
│   ├── tests.c           # in-kernel self-tests
│   └── ...
├── userland/
│   ├── crt0.S
│   ├── hello.c / forktest.c / nsh.c
│   ├── parfork.c / sleepbin.c / ttyprobe.c
│   ├── nettest.c / httpget.c
│   ├── link.ld
│   └── busybox.static     # real Alpine BusyBox, embedded as /bin/busybox
├── scripts/               # regression/debug/test scripts
├── linker64.ld
├── Makefile
└── NullOs_README.md
```

The `build/` directory contains generated objects, binaries, disk images and the generated ISO and is normally not part of the source architecture.

---

# Design philosophy

NullOs has no intention of becoming a Linux clone.

Its design philosophy is closer to:

> **Don't fake what can be implemented for real.**

That means:

- A shell command should be able to become a real userspace process.
- A userspace file read should go through the filesystem and syscall layer.
- `fork()` should actually clone an address space.
- COW should actually isolate writes.
- ELF loading should actually load an ELF.
- Compatibility should be tested against real software when possible.
- A syscall added for compatibility should have an actual kernel path behind it.

This is also why the project uses real Alpine BusyBox as a compatibility stress test instead of writing an increasingly large collection of artificial test programs.

NullOs does not try to be "Unix enough" by appearance.

It tries to become **mechanically compatible enough for useful software to survive inside it**.

---

# Why "Null"?

The name is intentionally ambiguous.

`Null` can mean:

- zero;
- absence;
- a system starting from almost nothing;
- zero assumptions about what an OS is supposed to look like;
- zero obligation to belong to Linux, Unix, BSD or Windows;
- a deliberate "null point" from which the system defines its own architecture.

In practice, the last interpretation may be the most accurate:

> **NullOs is not trying to be one existing operating system.**

It borrows interfaces where compatibility is useful, invents mechanisms where the project needs its own, and occasionally ends up looking like several operating systems at once.

---

# Roadmap

The roadmap is deliberately flexible because this is a hobby OS and the architecture is still evolving.

Likely future areas include:

- [ ] **[ACTIVE] labwc compositor bring-up** (the dwl target was replaced by labwc — see the readiness section; first station: wayland-server + libffi as a musl-static userland lib, then xkbcommon/pixman, then wlroots-0.20 minimal + the lite shims). The previously-planned "TCP refinements" item is deferred behind the compositor route by owner's decision.
- [x] run the full Alpine BusyBox shell path (done: interactive `sh` with job control)
- [x] TCP on the wire (done: client streams and the first HTTP download)
- [ ] per-task fd tables (retire global switch-time snapshots)
- [ ] deliver custom signal handlers into ring 3 (`rt_sigaction`/sigreturn)
- [ ] finish foreground resume for duplicated tty fds
- [ ] chase the writev-flood stack-alignment fault
- [ ] TCP server side: `LISTEN`/`accept`
- [ ] retransmission timers for data segments
- [ ] DNS resolution (a hosts-file milestone would be an acceptable first cut)
- [ ] busybox `wget` end-to-end over the TCP/HTTP layer
- [ ] apk-tools static against a real Alpine mirror
- [ ] virtio-net (RTL8139 throughput will always be humble)
- [ ] broaden musl coverage: readv/writev/pread64/pipe2/dup3/epoll*
- [ ] a minimal `/proc`
- [ ] better VFS semantics and a richer `/dev`
- [ ] framebuffer-based userspace
- [ ] SMP-aware scheduling refinements
- [ ] more hardware drivers, better documentation infrastructure

---

# Disclaimer

NullOs is an experimental hobby operating system.

It is not production-ready, POSIX-complete, Linux-compatible in the general sense, or intended to replace an existing operating system.

Breaking the kernel is currently a perfectly normal part of development.

If you find a triple fault, congratulations: you found a bug.

---

## License

See the repository for the current licensing information.

---

> **NullOs — zero assumptions, non-zero page faults.**

---

# Session addendum: raw sockets, paste-harness and the killer ABI fixes

## What landed most recently
- **User-ICMP ping** (`busybox ping`): `SOCK_RAW/SOCK_DGRAM + IPPROTO_ICMP`
  sockets, verbatim echo requests under our IP header, replies queued to the
  raw socket. Because user-space signal delivery does not exist yet,
  `net_poll()` paces a retransmit every second with an advancing ICMP
  sequence (the app sees fresh sequences; statistics still print).
  Gateway ping is a verified E2E PASS.
- **paste-harness** (`scripts/paste_run.sh`): arbitrary shell scripts are
  served over HTTP, fetched in-guest by `/bin/httpget` and executed by
  busybox ash — so ':', '&&', '$(...)' survive (they are READ, not typed;
  QEMU sendkey cannot hold Shift here). First consumer:
  **wget-by-name E2E PASS** (`busybox wget http://host:PORT/f.txt`, hosts-
  resolved via musl getaddrinfo, body verified inside the guest).
- **Killer ABI fixes that unblocked everything above**
  (details and markers in worklog Task 12):
  - `connect_tcp` no longer wipes `sk->connected` mid-handshake
    (SYN|ACK was being shipp'd as "no stream socket");
  - POSIX refcounting for open file descriptions across
    `fcntl(F_DUPFD*)/dup/dup2/fork` — aliases die only on last close;
    sockets got their own `sk_refs`;
  - syscall entry arg7 off-by-one (`#a6-off-by-one`): recvfrom's
    addrlen pointer arrived as pad-garbage since forever;
  - sockaddr family endianness on receive paths (`#gai-drop`).

## Session 2 addendum: default route, real DNS, pipes
- **#gai-aaaa RESOLVED**: musl getaddrinfo now resolves PUBLIC names —
  both A and AAAA queries answered through our UDP stack. Proven by
  `busybox wget http://info.cern.ch/` downloading the REAL page from the
  internet by NAME (INET-PASS-42 marker, net_wget_inet_test.sh).
  Fixes: default-route ARP (#route-fix — off-subnet frames go via the
  gateway MAC), POSIX socket type flags (#gai-socket-flags), ARP-kick
  restored in the sendto warm loop (#gai-arp-kick).
- **Internet ICMP ping**: impossible in THIS sandbox — the host container
  lacks cap_net_raw, so slirp cannot forward ICMP to the internet.
  Gateway ping (10.0.2.2) remains fully E2E.
- **pipe2/pipe**: kernel pipes implemented (ring + POSIX last-alias
  refcounts across close/dup/fork; exit releases all fds). First-stage
  pipelines carry data; full `cat | wc` EOF semantics blocked by a
  PRE-EXISTING fork-child fd-table load ordering issue
  (#pipe-fd-switch, traces in logs/pipe-test*).
- **apk-tools**: next session — needs the pipe/rename foundation settled.
- User-mode signal handlers (SIGALRM/setitimer) still pending; the
  kernel ICMP pacer remains the stand-in for ping.
