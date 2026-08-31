# NullOs – 64-bit Hobby Operating System

A minimal x86_64 operating system kernel written in C/C++ and assembly, designed to explore low-level systems programming, modern CPU features, and kernel architecture. NullOs is a fully functional hobby OS that runs user programs, manages memory, handles interrupts, and provides a basic shell environment with networking and filesystem support.

**Current Version:** 0.1.0  
**Language:** 99.3% C, 0.7% Other (Assembly)  
**Architecture:** x86_64 (Long Mode)

---

## Key Features

### Core Kernel
- **IDT & PIC**: 256-entry interrupt descriptor table with 8259A PIC remapping
- **GDT/TSS**: Global descriptor table with kernel/user segments, IST stack switching
- **64-bit VM**: Virtual memory manager with demand paging, 2MB→4KB page splitting
- **Memory Management**:
  - PMM (Physical Memory Manager) with Multiboot2 memory map support
  - Buddy allocator for page-granule allocation
  - Slab allocator for fixed-size object pools
  - Kernel heap with dlmalloc
  - Copy-on-Write (CoW) reference counting for fork()
- **Preemptive Scheduler**: Cooperative task switching with yield; IST1 for FPU context
- **ACPI Shutdown**: RSDT/XSDT/FADT power management

### CPU Features
- **FPU/SSE**: FXSAVE/FXRSTOR in ISRs and context switches
- **SMP Detection**: MADT/CPUID CPU enumeration (INIT+SIPI startup)
- **APIC**: Local APIC and IO APIC support
- **PCI Enumeration**: Bus/device/function scanning

### I/O & Devices
- **Keyboard**: PS/2 set 1/2 dual-mode, scancode translation, command history
- **Mouse**: PS/2 IRQ12, scroll wheel, cursor tracking
- **Serial**: COM1 UART at 115200 baud for logging
- **ATA/IDE**: LBA28/LBA48 PIO driver for disk access
- **RTC**: CMOS real-time clock (IRQ8)
- **PIT Timer**: Programmable interval timer (configurable Hz)
- **Framebuffer**: Software backbuffer graphics mode
- **AC97 Audio**: PCM output via DMA

### Filesystems & Storage
- **Virtual Filesystems**:
  - In-memory ramfs (default, volatile)
  - FAT32 support for ATA drives (via lwext4 glue)
  - ext4 read/write via embedded lwext4 library
- **File I/O**: open/close/read/write/mkdir/rm/symlink/stat
- **Persistence**: Storage sync to disk (ramfs→ATA), automatic restore on boot

### User-Space Features
- **ELF64 Loader**: Full executable parsing and memory mapping
- **Syscalls**: ~36 syscalls including write, read, open, close, fork, exec, exit, brk, mmap
- **Linux ABI Compatibility**: Standard x86_64 System V calling conventions (via SYSCALL/IRETQ)
- **Shell**: Interactive CLI with ~36 built-in commands
  - File ops: ls, cat, mkdir, touch, rm, cp, mv
  - Process control: spawn, kill, ps, wait, exit, fg, bg
  - System: sysinfo, shutdown, reboot, dmesg, sync
  - Networking: ping, wget, httpget, nslookup, nettest
  - Package mgmt: apk (Alpine Linux pkg manager embedded)
- **Process Management**: fork/exec, job control, signal handling

### Networking (User-Space)
- **Network Stack**: RTL8139 NIC driver, basic TCP/UDP
- **Embedded Binaries**:
  - BusyBox static (alpine minimal distro utilities)
  - Alpine apk-tools for package management
  - nettest, httpget, tcp_srv (demo networking apps)
  - AF_UNIX socket tests (Wayland transport proof-of-concept)

### Virtual Terminals
- **4 VTs** switchable via Alt+F1..F4
- Isolated console contexts, history per session

### Display & Debugging
- **VGA Text**: 80×25 color terminal with scrollback buffer (8K lines)
- **Kernel Logging**: Structured dmesg buffer with levels (info/warn/error/panic)
- **Serial Debug Output**: Parallel logging to COM1

---

## Repository Structure

```
NullOs/
├── kernel/                    Core kernel implementation
│   ├── kernel.cpp             Main entry point, subsystem init
│   ├── *.c, *.S              Low-level drivers & features
│   ├── mb/                    Embedded mbed TLS cryptography library
│   ├── ext4/                  lwext4 filesystem implementation
│   └── dlmalloc.c             Doug Lea malloc implementation
├── include/                   Public headers for kernel subsystems
│   ├── kernel.h               Main kernel interface
│   ├── mm.h, vmm.h            Memory/virtual memory management
│   ├── idt.h, pic.h, apic.h  Interrupt & CPU management
│   ├── fs.h, syscall.h        Filesystem & system call interface
│   └── *.h                   Device/feature headers
├── userland/                  User-space programs (compiled into kernel)
│   ├── hello.c                Linux ABI test program
│   ├── forktest.c             fork() / exec() test
│   ├── nsh.c                  Minimal shell (multi-call applet)
│   ├── nettest.c              Networking test
│   ├── busybox.static         Alpine Linux utilities (binary blob)
│   ├── apk.static             Alpine package manager (binary blob, stage-A only)
│   └── crt0.S, link.ld        User-space runtime & linker script
└── build/                     Compiled output directory
    └── userland/*.elf         Built userland ELF binaries
```

### Key Modules

| Module | Purpose |
|--------|---------|
| `kernel.cpp` | Initialization sequence, demo features, mainloop |
| `mm.c` + `buddy.c` | Physical & buddy allocators |
| `vmm.c` | Paging, demand faulting, CoW refcounts |
| `idt.c` + `isrs.S` | Interrupt handling setup |
| `scheduler.c` | Task switching, cooperative yield |
| `fs.c` | Ramfs in-memory filesystem |
| `ext4glue.c` | lwext4 integration & persistence |
| `syscall.c` | System call dispatch & user-space interface |
| `keyboard.c` + `mouse.c` | PS/2 device handling |
| `ata.c` | ATA/IDE disk I/O (LBA28/48) |
| `net.c` | Basic TCP/UDP stack (user-space apps only) |
| `acpi.c` | ACPI table parsing, shutdown |

---

## Building

### Prerequisites
- x86_64 Linux build environment
- `gcc` (x86_64 cross-compile or native)
- `nasm` (for assembly files)
- `ld` (GNU linker)
- `make` or shell build script

### Quick Start

```bash
git clone https://github.com/mel0k1/NullOs.git
cd NullOs

# Build the kernel (command may vary — see Makefile or build script)
make                        # or ./build.sh

# Run on QEMU
qemu-system-x86_64 -m 256 -kernel build/NullOs.bin [options]
```

**Common QEMU options:**
```bash
# Standard run:
qemu-system-x86_64 -m 256 -kernel kernel.elf

# With network (user-mode):
-net nic,model=rtl8139 -net user

# With serial console:
-serial stdio

# With display:
-display gtk

# Headless (serial only):
-nographic -serial stdio
```

### Build Output
- `kernel.elf` – Bootloader-ready ELF image
- `build/userland/*.elf` – Compiled user programs (linked into kernel)

---

## Running

### Starting the System

1. **Boot**: QEMU loads the kernel ELF, jumps to entry point, Multiboot2 information is passed
2. **Initialization** (`kernel_init`):
   - VGA, GDT, IDT, PIC setup
   - PMM & VMM initialization (128MB identity-mapped)
   - Heap, buddy, slab allocators
   - ATA, keyboard, mouse, FPU, ACPI, RTC
   - Filesystem setup (ramfs + optional ext4 mount)
   - Embedded programs registered to `/bin/*`
3. **Main Loop** (`kernel_mainloop`):
   - Interactive shell prompt
   - Execute commands or launch user programs

### Shell Commands

Once booted, you see:
```
========================================
       NullOs - Version 0.1.0
       64-bit Hobby Operating System
========================================

Welcome to NullOs Shell!
Type 'help' for available commands.
Use ALT+F1..F4 to switch virtual consoles.

> _
```

**Sample commands:**
```bash
> help                 # List all commands
> ls                   # List files (via nsh multi-call)
> cat /hello.txt       # Read file
> mkdir /mydir         # Create directory
> echo Hello           # Print text
> sysinfo              # Kernel info
> ps                   # List processes
> spawn 5              # Run scheduler demo (5 iterations)
> elf /bin/hello run   # Execute user program with args
> nettest              # Run networking test (if NIC present)
> dmesg                # View kernel log
> shutdown             # Power off via ACPI
```

### User Programs

Embedded ELF binaries can be executed:
```bash
> elf /bin/hello run
=== NullOs Linux-ABI test ===
argc = 2
argv[0] = "/bin/hello"
argv[1] = "run"
[brk] current break = 0x...
[brk] grown to 0x...
...
```

---

## Architecture Highlights

### Boot Sequence
1. Bootloader (GRUB2 via Multiboot2) loads kernel ELF + module
2. Assembly entry point establishes minimal GDT, IST stack
3. `kernel_main()` called with Multiboot2 info pointer
4. Subsystems initialized in dependency order
5. Interrupts enabled, shell starts

### Memory Layout (64-bit)
```
0x0000_0000 ─────────────────────
  Kernel identity map (first 128MB / 1GB)
0x1000_0000 ─────────────────────
  User space (0x400000 start)
  Allocated via VMM demand paging
0xFFFF_8000_0000_0000 ─ Kernel high half (if enabled)
```

### Interrupt Flow
1. **Hardware IRQ** (keyboard, timer, etc.) → PIC → CPU interrupt vector
2. **CPU exception** (page fault, div-by-zero) → ISR directly
3. **ISR handler** (in `isrs.S` & `*.c`) → dispatch to subsystem
4. **Scheduler** check on timer tick (pre-emption point)
5. **IRETQ** returns to kernel or user code

### System Call Path (Linux ABI)
```
User program:  syscall (RAX=syscall#, RDI/RSI/RDX=args)
    ↓
CPU:           Triggers interrupt via SYSCALL MSR
    ↓
Kernel:        syscall_entry.S → syscall_handler() → dispatch
    ↓
Subsystem:     Handle (e.g., fs_open, scheduler_fork)
    ↓
Return:        RAX=result, IRETQ to user
```

### Virtual Memory
- **Demand Paging**: Page faults trigger allocation
- **2MB → 4KB**: Large pages split on first write
- **CoW Fork**: Children share parent pages until write (refcount tracking)

### Scheduling
- **Cooperative Yield**: `task_yield()` in user code or on system call
- **Preemptive Timer**: PIT IRQ8 triggers context switch
- **Task Queue**: Global task table (scheduler.c), round-robin

---

## Known Limitations & TODOs

- **Single-threaded Initially**: SMP detection code present, CPU startup WIP
- **Demand Paging**: Works but identity map is pre-allocated (64GB strategy)
- **Networking**: Basic RTL8139 driver, TCP/UDP only user-space apps
- **Persistence**: ext4 mount is read-mostly (writes via fw_ext4 wrapper)
- **No Proper Boot**: Currently expects QEMU; GRUB2 real-hardware boot untested
- **Limited Signal Handling**: fork/exec work, but POSIX signals incomplete

---

## Key Files to Explore

| File | What to Learn |
|------|---------------|
| `kernel/kernel.cpp` | Overall boot flow & subsystem init |
| `kernel/vmm.c` | Demand paging, page table management |
| `kernel/scheduler.c` | Task switching, cooperative multitasking |
| `kernel/fs.c` | In-memory ramfs, file operations |
| `kernel/syscall.c` | System call dispatch & validation |
| `include/kernel.h` | Main kernel API |
| `userland/hello.c` | Example user program, syscall use |

---

## Try This First

After booting:
```bash
> elf /bin/hello run
> dmesg | grep "KERNEL"
> sysinfo
> ls /bin
> mkdir /test
> echo "Hello from NullOs" > /test/msg.txt
> cat /test/msg.txt
```

---

## Debugging

### Serial Console
Connect via:
```bash
qemu-system-x86_64 -m 256 -kernel kernel.elf -serial stdio
```

All kernel log messages (klog_info, klog_warn, etc.) appear here too.

### Kernel Log Buffer
```bash
> dmesg                # Show kernel ring buffer
```

### Inspect Memory
```bash
> hexdump /hello.txt   # Hex dump file
```

---

## Contributing & Extending

To add a feature:
1. Add headers in `include/`
2. Implement in `kernel/`
3. Initialize in `kernel_init()` (kernel.cpp)
4. Add syscalls in `kernel/syscall.c` if user-facing
5. Register embedded programs in `kernel/userprogs.S` (if new binary)
6. Test via shell or ELF runner

---

## References

- **Multiboot2 Spec**: GRUB2 boot protocol
- **System V AMD64 ABI**: x86_64 calling conventions & ELF format
- **Intel x86-64 Manual**: CPU instructions, paging, exceptions
- **ACPI Spec**: Power management (FADT, PM1a)
- **ext4 Format**: Linux filesystem (via lwext4 library)
- **lwext4**: Embedded ext4 library (in `kernel/ext4/`)
- **mbed TLS**: Cryptography (in `kernel/mb/`, for future use)

---

## License

This is a hobby OS project. Use at your own risk.  
Embedded third-party libraries (busybox, apk, lwext4, mbed TLS) retain their original licenses.

---

## Contact & Attribution

Built as a systems programming exploration.  
Original author: seamiq (first commit message "первый коммит")

---

**Happy hacking! 🖥️**

