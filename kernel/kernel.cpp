#include "kernel.h"
#include "vga.h"
#include "mm.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "syscall.h"
#include "fs.h"
#include "shell.h"
#include "buddy.h"
#include "timer.h"
#include "string.h"
#include "serial.h"
#include "rtc.h"
#include "scheduler.h"
#include "ata.h"
#include "apic.h"
#include "kbd_scancodes.h"
#include "gdt.h"
#include "pci.h"
#include "fb.h"
#include "fpu.h"
#include "acpi.h"
#include "klog.h"
#include "vt.h"
#include "mouse.h"
#include "fat32.h"
#include "smp.h"
#include "ac97.h"
#include "net.h"
#include "storage.h"

extern u8 __kernel_end[];

// Embedded user program (built from userland/, pulled in via userprogs.S)
extern "C" {
extern const unsigned char hello_elf_start[];
extern const unsigned char hello_elf_end[];
extern const unsigned char forktest_elf_start[];
extern const unsigned char forktest_elf_end[];
extern const unsigned char busybox_elf_start[];
extern const unsigned char busybox_elf_end[];
extern const unsigned char ttyprobe_elf_start[];
extern const unsigned char ttyprobe_elf_end[];
extern const unsigned char nsh_elf_start[];
extern const unsigned char nsh_elf_end[];
extern const unsigned char parfork_elf_start[];
extern const unsigned char parfork_elf_end[];
extern const unsigned char sleepbin_elf_start[];
extern const unsigned char sleepbin_elf_end[];
extern const unsigned char nettest_elf_start[];
extern const unsigned char nettest_elf_end[];
extern const unsigned char httpget_elf_start[];
extern const unsigned char httpget_elf_end[];
extern const unsigned char srvtest_elf_start[];
extern const unsigned char wltest_elf_start[];
extern const unsigned char wltest_elf_end[];
extern const unsigned char unixtest_elf_start[];
/* apk.static import blob — present ONLY in stage-A builds
 * (make APK_DEFS="-DAPK_STAGE_A"); see userprogs.S. */
#ifdef APK_STAGE_A
extern const unsigned char apk_elf_start[];
extern const unsigned char apk_elf_end[];
#endif
extern const unsigned char srvtest_elf_end[];
extern const unsigned char unixtest_elf_end[];
}

extern "C" {

void kernel_init(void* mbi_ptr) {
    vga_init();
    gdt_init();

    vga_print_color("\n========================================\n", VGA_YELLOW, VGA_BLACK);
    vga_print_color("       NullOs - Version ", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print(NULLOS_VERSION);
    vga_print_color("\n       64-bit Hobby Operating System\n", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print_color("========================================\n\n", VGA_YELLOW, VGA_BLACK);

    klog_init();
    klog_info("[KERNEL] Initializing subsystems...\n");

    klog_info("[GDT] Loaded by bootloader\n");

    idt_init();
    klog_info("[IDT] Initialized (256 entries)\n");

    pic_init();
    klog_info("[PIC] Remapped to IRQ 32-47\n");

    // Initialize PMM with real Multiboot2 memory map
    pmm_init(mbi_ptr);
    klog_info("[PMM] Physical memory manager initialized\n");

    // Initialize VMM (creates kernel PML4, identity-maps first 128 MB)
    vmm_init();
    // Switch to our PML4 now that it's set up
    vmm_switch_pml4(vmm_get_kernel_pml4());
    klog_info("[VMM] Virtual memory manager initialized (128MB identity)\n");

    // Initialize heap (free-list allocator)
    heap_init();
    klog_info("[HEAP] Kernel heap initialized\n");

    // Initialize slab allocator
    slab_init();
    klog_info("[SLAB] Slab allocator initialized\n");

    // Initialize virtual terminal system (needs heap for kzalloc)
    vt_init();
    klog_info("[VT] Virtual terminals initialized (%d consoles)\n", VT_MAX_CONSOLES);

    // Initialize buddy allocator AFTER heap
    u64 buddy_base = KERNEL_HEAP_START + KERNEL_HEAP_SIZE + PAGE_SIZE;

    // Early-init COW refcount table (512KB from PMM) BEFORE any user
    // memory exists — avoids allocating inside the fork critical path.
    {
        extern void pmm_refcounts_init(void);
        pmm_refcounts_init();
    }

    u64 buddy_size = 128 * 1024 * 1024ULL - buddy_base;
    if ((s64)buddy_size <= 0) buddy_size = 0;   /* base beyond RAM */
    if (buddy_size > 64 * 1024 * 1024) buddy_size = 64 * 1024 * 1024;
    /* never reserve pages beyond actual physical RAM */
    {
        extern phys_addr_t pmm_get_total_memory(void);
        u64 ram_pages = pmm_get_total_memory() / PAGE_SIZE;
        if (buddy_base >= ram_pages * PAGE_SIZE) {
            buddy_size = 0;
            vga_print("[BUDDY] skipped: buddy region beyond RAM\n");
        } else if (buddy_base + buddy_size > ram_pages * PAGE_SIZE) {
            buddy_size = ram_pages * PAGE_SIZE - buddy_base;
        }
    }
    if (buddy_size == 0) {
        vga_print("[BUDDY] disabled (no RAM for buddy zone)\n");
    } else {
        buddy_init(buddy_base, buddy_size);
    }
    pmm_mark_used(buddy_base, buddy_size);
    klog_info("[BUDDY] Buddy allocator initialized (base=0x%lx, size=%uMB)\n",
               (unsigned long)buddy_base, (u32)(buddy_size / 1024 / 1024));

    // Initialize PIT timer
    timer_init();
    klog_info("[TIMER] PIT initialized at %u Hz\n", TIMER_FREQ);

    // Initialize Serial COM1
    serial_init();
    klog_info("[SERIAL] COM1 initialized (115200 baud)\n");

    // Initialize RTC
    rtc_init();
    klog_info("[RTC] Real-time clock initialized\n");

    // Initialize cooperative scheduler
    scheduler_init();
    {
        extern void proc_bind_task_table(void* table);
        extern void* scheduler_task_table(void);
        proc_bind_task_table(scheduler_task_table());
    }

    // Initialize filesystem and syscalls
    fs_init();
    syscall_init();
    klog_info("[FS] Filesystem initialized\n");

    // Initialize keyboard after PIC is set up
    keyboard_init();
    keyboard_enable_set2();
    klog_info("[KEYBOARD] PS/2 driver initialized%s\n",
              apic_is_supported() ? " (APIC supported)" : "");

    // Initialize PS/2 mouse (IRQ12)
    if (mouse_init()) {
        klog_info("[MOUSE] PS/2 mouse driver initialized\n");
    } else {
        klog_warn("[MOUSE] No PS/2 mouse detected (continuing without mouse)\n");
    }

    // Initialize FPU/SSE
    fpu_init();
    klog_info("[FPU] FPU/SSE initialized (FXSAVE/FXRSTOR)\n");

    // Initialize ATA/IDE driver
    ata_init();
    klog_info("[ATA] ATA/IDE driver initialized\n");

    // Initialize PCI enumeration
    pci_init();
    klog_info("[PCI] PCI enumeration complete\n");

    // ACPI init: safe now — vmm_init() identity-maps the full 4GB
    // *before* any ACPI table access, so RSDT/DSDT reads never hit the
    // demand pager (the old "infinite PF loop" is gone with the 4GB map).
    acpi_init();
    klog_info("[ACPI] FADT/PM1a initialized\n");

    // Initialize network stack (after NIC driver)
    net_init();

    // Initialize SMP (detect CPUs)
    smp_init();
    klog_info("[SMP] CPU detection complete\n");

    // Enable interrupts
    sti();

    // Auto-restore the persisted ramfs (files saved with `sync`).
    // Runs after sti() so the ATA driver's tick-based timeouts work.
    if (storage_has_data()) {
        klog_info("[STORAGE] Saved filesystem found on disk\n");
        storage_load();
    }

    // #ext4-persist: mount the ATA disk as ext4 (lwext4) and attach
    // its files into the ramfs as live backend nodes. Writes through
    // those nodes hit the disk immediately; ramfs-native files reach
    // the volume via `ext4sync`. Silent no-op when the disk holds no
    // ext4 signature (mount fails -> rc != EOK).
    extern int ext4_storage_init(void);
    ext4_storage_init();

    // Register embedded user programs into /bin so they can be run with
    // `elf /bin/hello run`. The binaries are linked into the kernel image
    // from build/userland/*.elf via kernel/userprogs.S (.incbin).
    {
        const u32 hello_len = (u32)(hello_elf_end - hello_elf_start);
        const u32 fork_len  = (u32)(forktest_elf_end - forktest_elf_start);
        const u32 bb_len    = (u32)(busybox_elf_end - busybox_elf_start);
        const u32 nsh_len   = (u32)(nsh_elf_end - nsh_elf_start);
        if (hello_len > 0) {
            fs_write_file("/bin/hello", hello_elf_start, hello_len);
            klog_info("[FS] Registered /bin/hello (%u bytes)\n", hello_len);
        }
        if (fork_len > 0) {
            fs_write_file("/bin/forktest", forktest_elf_start, fork_len);
            klog_info("[FS] Registered /bin/forktest (%u bytes)\n", fork_len);
        }
        if (bb_len > 0) {
            fs_write_file("/bin/busybox", busybox_elf_start, bb_len);
            klog_info("[FS] Registered /bin/busybox (%u bytes) — REAL Alpine!\n",
                      bb_len);
        }
        const u32 tp_len = (u32)(ttyprobe_elf_end - ttyprobe_elf_start);
        if (tp_len > 0) {
            fs_write_file("/bin/ttyprobe", ttyprobe_elf_start, tp_len);
            klog_info("[FS] Registered /bin/ttyprobe (%u bytes)\n", tp_len);
        }
        if (nsh_len > 0) {
            /* BusyBox-style multi-call: same binary under applet names */
            fs_write_file("/bin/nsh", nsh_elf_start, nsh_len);
            fs_write_file("/bin/ls", nsh_elf_start, nsh_len);
            fs_write_file("/bin/cat", nsh_elf_start, nsh_len);
            fs_write_file("/bin/echo", nsh_elf_start, nsh_len);
            fs_write_file("/bin/uname", nsh_elf_start, nsh_len);
            klog_info("[FS] Registered /bin/nsh + applets (%u bytes)\n",
                      nsh_len);
        }
        {
            const u32 pf_len = (u32)(parfork_elf_end - parfork_elf_start);
            if (pf_len > 0) {
                fs_write_file("/bin/parfork", parfork_elf_start, pf_len);
                klog_info("[FS] Registered /bin/parfork (%u bytes)\n", pf_len);
            }
        }
        {
            const u32 nt_len = (u32)(nettest_elf_end - nettest_elf_start);
            if (nt_len > 0) {
                fs_write_file("/bin/nettest", nettest_elf_start, nt_len);
                klog_info("[FS] Registered /bin/nettest (%u bytes)\n", nt_len);
            }
        }
        {
            const u32 hg_len = (u32)(httpget_elf_end - httpget_elf_start);
            if (hg_len > 0) {
                fs_write_file("/bin/httpget", httpget_elf_start, hg_len);
                klog_info("[FS] Registered /bin/httpget (%u bytes)\n", hg_len);
            }
        }
        {
            /* TCP echo-server smoke: exercises listen()/accept() over
             * the kernel TCP stack (host-side client drives it E2E). */
            const u32 sv_len = (u32)(srvtest_elf_end - srvtest_elf_start);
            if (sv_len > 0) {
                fs_write_file("/bin/srvtest", srvtest_elf_start, sv_len);
                klog_info("[FS] Registered /bin/srvtest (%u bytes)\n",
                          sv_len);
            }
        }
        {
            /* AF_UNIX transport prober (#wayland-transport): the
             * libwayland wire foundation E2E in ring3.            */
            const u32 ux_len = (u32)(unixtest_elf_end - unixtest_elf_start);
            if (ux_len > 0) {
                fs_write_file("/bin/unixtest", unixtest_elf_start, ux_len);
                klog_info("[FS] Registered /bin/unixtest (%u bytes)\n",
                          ux_len);
            }
        }
        {
            /* #wl-substrate prober: dwl/libwayland readiness E2E.  */
            const u32 wlt_len = (u32)(wltest_elf_end - wltest_elf_start);
            if (wlt_len > 0) {
                fs_write_file("/bin/wltest", wltest_elf_start, wlt_len);
                klog_info("[FS] Registered /bin/wltest (%u bytes)\n",
                          wlt_len);
            }
        }
        {
            /* Job-control E2E: fg-external on PATH inside BusyBox ash.
             * Tiny native applet — a full busybox copy here (1 MB) was
             * observed to exhaust heap and stall later execs (#jc-heap). */
            const u32 sb_len =
                (u32)(sleepbin_elf_end - sleepbin_elf_start);
            if (sb_len > 0) {
                fs_write_file("/bin/sleep", sleepbin_elf_start, sb_len);
                klog_info("[FS] Registered /bin/sleep (%u bytes)\n", sb_len);
            }
        }
        {
            /* #busybox-applet-links: busybox is a multi-call binary —
             * every applet dispatches on argv[0]'s basename. Without
             * symlinks, bare `cat|wc` inside ash died with "not
             * found" (rc=127), which killed whole classes of scripts
             * (make/configure/tar pipelines) on the path to running
             * downloadable software like dwl. The fs layer resolves
             * symlinks in exec (fs_walk_path follow, 8-hop budget),
             * and the kernel execfs seeds /bin/busybox BEFORE this,
             * so each link below is a 10-byte node. */
            /* NOTE: cat/ls/echo/uname stay owned by the NATIVE nsh
             * multi-call binary (seeded above) — ash scripts calling
             * them still work because nsh dispatches on argv[0]. */
            static const char* const bb_applets[] = {
                "sh", "ash", "wc", "grep", "tr",
                "head", "tail", "cut", "sort", "uniq", "sed", "awk",
                "mkdir", "rmdir", "rm", "cp", "mv", "touch", "ln",
                "tar", "gzip", "gunzip", "zcat", "find", "xargs",
                "chmod", "chown", "stat", "du", "df", "mount", "umount",
                "ps", "top", "kill", "true", "false", "test", "expr",
                "wget", "ping", "nslookup", "hostname", "date",
                "md5sum", "sha256sum", "dd", "sync", "clear", "which",
                "basename", "dirname", "sleep", "printf", "yes",
                0 };
            int seeded = 0;
            for (int ai = 0; bb_applets[ai]; ai++) {
                char lp[40];
                const char* base = "/bin/";
                int bi = 0;
                for (; base[bi]; bi++) lp[bi] = base[bi];
                const char* nm = bb_applets[ai];
                int ni = 0;
                for (; nm[ni]; ni++) lp[bi + ni] = nm[ni];
                lp[bi + ni] = 0;
                if (fs_symlink("/bin/busybox", lp) == 0) seeded++;
            }
            /* /bin/sh matters beyond applets: `sh -c` from other
             * scripts (rc=127 was the #exec-sh-c-stack red herring —
             * the mechanism itself was healthy). */
            klog_info("[FS] Seeded %d busybox applet symlinks in /bin\n",
                      seeded);
        }
    }


    // Seed the name-resolution config files musl getaddrinfo()
    // expects. /etc/hosts lets busybox wget/nslookup resolve at
    // least 'localhost' and 'host' offline; /etc/resolv.conf points
    // musl's DNS stub at slirp's built-in resolver (10.0.2.3), which
    // answers real DNS over our UDP path (auto-bound sockets).
    // Seeded AFTER storage_load() so persisted copies win.
    {
        static const char hosts_txt[] =
            "127.0.0.1\tlocalhost\n"
            "::1\tlocalhost\n"
            "10.0.2.2\thost gateway\n";
        static const char resolv_txt[] =
            "nameserver 10.0.2.3\n";
        fs_mkdir("/etc");                 /* EEXISTS is fine */
        fs_write_file("/etc/hosts",
                      hosts_txt, sizeof(hosts_txt) - 1);
        fs_write_file("/etc/resolv.conf",
                      resolv_txt, sizeof(resolv_txt) - 1);
        klog_info("[FS] Seeded /etc/hosts + /etc/resolv.conf\n");
    }

    // Seed the apk-tools db skeleton (#apk-dirfd): apk add --initdb
    // expects /etc/apk/{world,keys}, /lib/apk/db and a cache dir to
    // exist; the ramfs starts empty apart from the seed files above.
    {
        fs_mkdir("/etc/apk");
        fs_mkdir("/etc/apk/keys");
        fs_mkdir("/lib");
        fs_mkdir("/lib/apk");
        fs_mkdir("/lib/apk/db");
        fs_mkdir("/var");
        fs_mkdir("/var/cache");
        fs_mkdir("/var/cache/apk");
        /* Alpine apk public keys: without them every signed index
         * (dl-cdn) fails with "BAD signature". */
        {
            #include "keys_blob.h"   /* apk_keys[] table              */
            for (unsigned k = 0; k < sizeof(apk_keys) / sizeof(apk_keys[0]);
                 k++) {
                char kpath[128];
                ksnprintf(kpath, sizeof(kpath), "/etc/apk/keys/%s",
                          apk_keys[k].name);
                fs_write_file(kpath, apk_keys[k].data, apk_keys[k].len);
            }
        }
        /* NOTE: do NOT seed /etc/apk/world — it is an adb-format blob;
         * a text placeholder makes apk's adb reader fail with EIO
         * ("Unable to read database state"). apk --initdb writes a
         * fresh one itself. */
        /* #apk-repo-seed: the repository list apk update/add fetches
         * against — dl-cdn v3.19/main, signed with the keys above. */
        {
            static const char repos_txt[] =
                "http://dl-cdn.alpinelinux.org/alpine/v3.19/main\n";
            fs_write_file("/etc/apk/repositories",
                          repos_txt, sizeof(repos_txt) - 1);
        }
        klog_info("[FS] Seeded /etc/apk skeleton (apk db + keys)\n");
    }

    // Seed the deterministic net-test payload (#netcmd-seed). This
    // replaces the fragile paste-harness (host python http.server)
    // for experiments: the guest reads its own instructions from the
    // ramfs, so ':'/'-' never pass through the lossy sendkey pipe.
    {
        static const char netcmd_txt[] =
            "busybox echo APKRUN-UP\n"
            "apk add --no-interactive --initdb zlib\n"
            "busybox echo APKADD-RC=$?\n"
            "busybox ls -la /var/cache/apk/\n"
            "busybox ls -la /lib/apk/db/\n"
            "busybox echo APKRUN-DONE\n";
        fs_mkdir("/tmp");                 /* EEXISTS is fine */
        fs_write_file("/tmp/netcmd.sh",
                      netcmd_txt, sizeof(netcmd_txt) - 1);
        klog_info("[FS] Seeded /tmp/netcmd.sh (%u bytes)\n",
                  (unsigned)(sizeof(netcmd_txt) - 1));
    }

    // ---- apk.static two-stage import (STAGE A) ----------------------
    // In a stage-A build the 4.3MB Alpine apk-tools-static binary is
    // embedded in .rodata. The kernel image is then >4MB, which is
    // UNSAFE for running user programs (user VAs at 0x400000+ would
    // shadow the kernel identity map), so this boot does exactly one
    // thing: copy the blob into the ramfs as /bin/apk, persist it to
    // disk via storage_sync(), and power off. Regular (blobless)
    // builds restore /bin/apk from disk via storage_load().
    #ifdef APK_STAGE_A
    {
        const u32 apk_len = (u32)(apk_elf_end - apk_elf_start);

        /* CRC32 (reflected, poly 0xEDB88320) — import integrity check.
         * Compare against the host-side value printed by the import
         * script; boot B re-checks with busybox md5sum. */
        u32 crc = 0xFFFFFFFFu;
        for (u32 i = 0; i < apk_len; i++) {
            crc ^= apk_elf_start[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
        crc ^= 0xFFFFFFFFu;
        klog_info("[APKIMPORT] blob len=%u crc32=%08x\n", apk_len, crc);

        bool already = false;
        {
            s32 fd = fs_open("/bin/apk", FS_READ);
            if (fd >= 0) {
                u32 sz = fs_get_size_by_fd((u32)fd);
                fs_close(fd);
                already = (sz == apk_len);
                klog_info("[APKIMPORT] /bin/apk exists (size=%u) — %s\n",
                          sz, already ? "skip import" : "re-import");
            }
        }
        if (!already) {
            s32 w = fs_write_file("/bin/apk", apk_elf_start, apk_len);
            klog_info("[APKIMPORT] fs_write_file /bin/apk -> %d\n", w);
            if (w == 0) {
                int r = storage_sync();
                klog_info("[APKIMPORT] storage_sync -> %d (disk persisted; "
                          "powering off)\n", r);
            }
        }
        klog_info("[APKIMPORT] stage-A boot complete — shutting down\n");
        kernel_shutdown();
        return;   /* not reached normally */
    }
    #endif

    klog_info("[KERNEL] Initialization complete!\n");
}

void kernel_mainloop() {
    // Print the full welcome banner only once per boot. This function is
    // re-entered after every user program exit (and after VT switches) —
    // printing the whole banner each time flooded the console.
    static bool first_entry = true;
    if (first_entry) {
        first_entry = false;
        vga_print("\nWelcome to NullOs Shell!\n");
        vga_print("Type 'help' for available commands.\n");
        vga_print("Use ALT+F1..F4 to switch virtual consoles.\n\n");
    }

    // Run shell — it returns on VT switch or user program exit
    shell_run();

    // After each return, re-arm the shell and run it again.
    while (1) {
        if (!shell.running) {
            shell.running = true;
            shell.arg_count = 0;
            shell.input_pos = 0;
            shell.history_browse_index = 0;
            shell.current_dir[0] = '/';
            shell.current_dir[1] = '\0';
            shell.cmd_buffer[0] = '\0';
            // Note: command history is intentionally preserved here —
            // it belongs to the session, not to a single program run.
            vga_print("\n");
        }
        shell_run();
    }
}

void kernel_shutdown() {
    klog_notice("[KERNEL] Shutting down...\n");
    klog_info("[ACPI] Sending shutdown via PM1a_CNT...\n");
    cli();
    acpi_shutdown();
    for (volatile int i = 0; i < 100000; i++) {}
    klog_warn("[ACPI] ACPI shutdown failed, trying QEMU fallback...\n");
    outw(0xB004, 0x2000);
    while(1) { hlt(); }
}

void kernel_reboot() {
    klog_notice("[KERNEL] Rebooting...\n");
    cli();
    while (inb(0x64) & 0x02);
    outb(0x64, 0xFE);
    u64 temp = 0;
    __asm__ volatile ("lidt (%0)" :: "r"(&temp));
    __asm__ volatile ("int $3");
}

void kernel_panic(const char* message) {
    cli();
    klog_emerg("*** KERNEL PANIC: %s ***\n", message);
    vga_clear(VGA_WHITE, VGA_RED);
    vga_set_cursor(0, 0);
    vga_print_color("\n*** KERNEL PANIC ***\n\n", VGA_WHITE, VGA_RED);
    vga_print_color("Error: ", VGA_WHITE, VGA_RED);
    vga_print_color(message, VGA_WHITE, VGA_RED);
    vga_print("\n\nSystem halted.\n");

    serial_print("\n*** KERNEL PANIC ***\n");
    serial_print("Error: ");
    serial_print(message);
    serial_print("\n");

    while (1) { hlt(); }
}

void get_memory_info(u32* total, u32* free, u32* used) {
    u64 t = pmm_get_total_memory();
    u64 f = pmm_get_available_memory();
    *total = (u32)(t / 1024);
    *free  = (u32)(f / 1024);
    *used  = (u32)((t - f) / 1024);
}

void kernel_print_info() {
    vga_print("\n=== Kernel Info ===\n");
    vga_printf("Version:   %s\n", NULLOS_VERSION);
    vga_print("Arch:      x86_64 (Long Mode)\n");
    vga_printf("Kernel end: 0x%lx\n", (u64)__kernel_end);
    vga_printf("Heap:      0x%lx (%u MB)\n",
               KERNEL_HEAP_START, (u32)(KERNEL_HEAP_SIZE / 1024 / 1024));
    vga_printf("PMM Total: %u KB\n", (u32)(pmm_get_total_memory() / 1024));
    vga_printf("PMM Free:  %u KB\n", (u32)(pmm_get_available_memory() / 1024));
    vga_printf("Buddy Free: %u pages\n", buddy_get_free_pages());
    vga_print("==================\n");
}

static void kernel_demo_task(void* arg) {
    u32 count = (u32)(u64)arg;
    for (u32 i = 1; i <= count; i++) {
        vga_printf("  [demo_task] %u/%u\n", i, count);
        task_yield();
    }
    vga_printf("  [demo_task] done!\n");
}

void kernel_main(void* mbi_ptr) {
    kernel_init(mbi_ptr);

    // Demo: test output
    vga_print("\n[DEMO] Testing output functions:\n");
    vga_print("- String: ");
    vga_print_color("Hello, 64-bit World!", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print("\n");
    vga_printf("- Printf: %d, 0x%x, %s\n", -42, (u32)0xDEADBEEF, "works!");
    vga_printf("- Pointer: %p\n", (void*)0x100000);

    // Demo: test snprintf
    vga_print("\n[DEMO] Testing ksnprintf:\n");
    char snbuf[64];
    ksnprintf(snbuf, sizeof(snbuf), "Test: %s, num=%u", "hello", 42);
    vga_printf("  snprintf result: '%s'\n", snbuf);

    // Demo: test memory allocation
    vga_print("\n[DEMO] Testing malloc/free:\n");
    void* p1 = kmalloc(128);
    vga_printf("  Allocated 128B at %p\n", p1);
    void* p2 = kmalloc(256);
    vga_printf("  Allocated 256B at %p\n", p2);
    vga_printf("  Heap free: %u KB\n", heap_get_free() / 1024);
    kfree(p1);
    vga_printf("  Freed 128B, heap free now: %u KB\n", heap_get_free() / 1024);
    void* p3 = kmalloc(64);
    vga_printf("  Allocated 64B at %p (reused freed space)\n", p3);
    kfree(p2);
    kfree(p3);

    // Demo: test string functions
    vga_print("\n[DEMO] Testing string functions:\n");
    vga_printf("  strstr('hello world', 'world') = %s\n",
               kstrstr("hello world", "world") ? "found" : "not found");
    vga_printf("  atoi('12345') = %d\n", katoi("12345"));

    // Demo: test filesystem
    vga_print("\n[DEMO] Testing filesystem:\n");
    fs_write_file("/hello.txt", "Hello from NullOs!\n", 21);
    fs_mkdir("/test_dir");
    fs_write_file("/test_dir/note.txt", "This is a test file.\n", 23);
    vga_print("  Created /hello.txt, /test_dir/note.txt\n");

    // Demo: test serial output
    serial_printf("[SERIAL] Kernel boot complete. PMM free=%lu MB\n",
                  (unsigned long)(pmm_get_available_memory() >> 20));

    // Demo: test RTC
    char timebuf[24];
    rtc_format_time(timebuf, sizeof(timebuf));
    vga_printf("\n[DEMO] RTC time: %s\n", timebuf);

    // Demo: test scheduler
    // NOTE: disabled at boot for now — interactive test via `spawn`.
    vga_print("\n[DEMO] Scheduler ready (try 'spawn 5').\n");
    // s32 demo_id = task_create("counter", kernel_demo_task, (void*)(u64)5, 0);
    // task_run_to_completion(demo_id);
    // vga_print("[DEMO] Scheduler demo done.\n");

    kernel_print_info();

    vga_print("\n[INFO] Features enabled:\n");
    vga_print("  - Interrupts (IDT)\n");
    vga_print("  - PIC (8259)\n");
    vga_printf("  - PIT Timer (%u Hz)\n", TIMER_FREQ);
    vga_print("  - RTC (CMOS, IRQ8)\n");
    vga_print("  - GDT/TSS (kernel + user segments, IST1)\n");
    vga_print("  - syscall/IRETQ (MSR LSTAR, user return via IRETQ)\n");
    vga_print("  - FPU/SSE (FXSAVE/FXRSTOR in ISR + scheduler)\n");
    vga_print("  - ACPI shutdown (RSDP/XSDT/FADT)\n");
    vga_print("  - VMM demand paging + 2MB->4KB split\n");
    vga_print("  - Buddy allocator (coalescing)\n");
    vga_print("  - Preemptive scheduler\n");
    vga_print("  - PS/2 Keyboard (set 1/2, history nav)\n");
    vga_print("  - Serial COM1 (115200 baud)\n");
    vga_print("  - ATA/IDE PIO (LBA28/LBA48)\n");
    vga_print("  - APIC (Local + IO APIC)\n");
    vga_print("  - PCI enumeration\n");
    vga_print("  - ELF64 loader (parse)\n");
    vga_print("  - VGA scrollback (8K lines)\n");
    vga_print("  - Slab allocator (page-tracked)\n");
    vga_print("  - Framebuffer (software backbuffer)\n");
    vga_print("  - Virtual terminals (Alt+F1..F4, 4 consoles)\n");
    vga_print("  - PS/2 Mouse (IRQ12, scroll wheel, cursor)\n");
    vga_print("  - sys_exit returns to shell (no more halt)\n");
    vga_print("  - FAT32 filesystem (mount/ls/cat ATA drives)\n");
    vga_print("  - Formatted printf (%%d %%x %%s %%p ...)\n");
    vga_print("  - ksnprintf\n");
    vga_print("  - Kernel log buffer (dmesg)\n");
    vga_print("  - Environment variables (export/env)\n");
    vga_print("  - hexdump/xxd command\n");
    vga_print("  - PCI driver binding (init from shell)\n");
    vga_print("  - AC97 audio driver (PCM out, DMA)\n");
    vga_print("  - SMP detection (MADT/CPUID, INIT+SIPI)\n");
    vga_printf("  - Interactive Shell (%u commands)\n", 36);

    klog_info("[KERNEL] Starting interactive shell...\n");
    vga_print("(Type 'help' for commands, 'sysinfo' for details)\n");
    vga_print("(Switch consoles with ALT+F1..F4)\n\n");

    serial_printf("[K] entering mainloop\n");
    kernel_mainloop();
}

} // extern "C"
