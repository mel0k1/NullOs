#include "../include/elf.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/string.h"
#include "../include/mm.h"
#include "../include/fs.h"
#include "../include/gdt.h"
#include "../include/shell.h"

// Assembly stub: void iret_to_usermode(u64 rip, u64 rsp, u64 rflags, u64 cs, u64 ss);
extern void iret_to_usermode(u64 rip, u64 rsp, u64 rflags, u64 cs, u64 ss);

// Global for syscall_entry.S: saved user RSP
extern u64 syscall_user_rsp;

// Global exit flag from syscall.c
extern volatile u64 sys_exit_pending;

// Forward declaration for kernel mainloop (defined in kernel.cpp)
extern void kernel_mainloop(void);

// User-space address range where ELF segments may be mapped.
// Everything below USER_MIN or at/above USER_MAX is kernel territory
// and must never be mapped with VMM_USER by the loader.
#define ELF_USER_ADDR_MIN  0x400000ULL
#define ELF_USER_ADDR_MAX  0x7FFFFF000ULL  // user stack top (canonical user area)

// Kernel exit handler: called from syscall_entry.S when user process calls sys_exit
// Re-enters the shell loop so the user gets back to the command line
void handle_user_exit(void) {
    vga_print("\n[SYS] Returning to NullOs Shell...\n");
    vga_print("(User process terminated)\n\n");

    // Reset shell state and re-enter mainloop
    extern shell_t shell;
    extern void shell_init(void);
    shell.running = false;
    shell.input_pos = 0;
    shell.arg_count = 0;
    shell.history_browse_index = 0;
    shell.current_dir[0] = '/';
    shell.current_dir[1] = '\0';
    shell.cmd_buffer[0] = '\0';

    kernel_mainloop();
}

// ============================================================
// ELF validation
// ============================================================

bool elf_validate(const void* data, u64 size) {
    if (!data || size < sizeof(elf64_header_t)) {
        return false;
    }

    const elf64_header_t* hdr = (const elf64_header_t*)data;

    // Check magic
    if (hdr->e_ident[0] != 0x7F ||
        hdr->e_ident[1] != 'E'  ||
        hdr->e_ident[2] != 'L'  ||
        hdr->e_ident[3] != 'F') {
        return false;
    }

    // Check 64-bit
    if (hdr->e_ident[4] != ELF_CLASS64) return false;

    // Check little-endian
    if (hdr->e_ident[5] != ELF_DATA_LS) return false;

    // Check executable type
    if (hdr->e_type != ELF_TYPE_EXEC) return false;

    // Check x86_64
    if (hdr->e_machine != ELF_MACHINE_X86_64) return false;

    // Sanity check program headers
    /* FIX(#elf-offset-wrap): the old `e_phoff + phnum*phentsize > size`
     * wraps for huge e_phoff (e.g. e_phoff = 2^64 - phnum*phentsize +
     * j) — the check passes and elf_parse computes data + e_phoff, a
     * wild pointer far below the buffer (kernel OOB read / #PF).
     * Subtract without wrapping. Also demand a sane e_phentsize: a
     * phentsize < sizeof(elf64_phdr_t) made every phdr read run up to
     * 48 bytes past the validated region. */
    if (hdr->e_phentsize < sizeof(elf64_phdr_t)) return false;
    if (hdr->e_phoff > size) return false;
    if ((u64)hdr->e_phnum * hdr->e_phentsize > size - hdr->e_phoff) {
        return false;
    }

    return true;
}

// ============================================================
// ELF parsing (extract load info without loading)
// ============================================================

bool elf_parse(const void* data, u64 size, elf_load_info_t* info) {
    if (!elf_validate(data, size)) return false;

    const elf64_header_t* hdr = (const elf64_header_t*)data;
    kmemset(info, 0, sizeof(elf_load_info_t));

    info->valid = true;
    info->entry_point = hdr->e_entry;
    info->base_address = 0xFFFFFFFFFFFFFFFFULL;
    info->top_address = 0;
    info->segment_count = 0;
    info->phoff      = hdr->e_phoff;
    info->phnum      = hdr->e_phnum;
    info->phentsize  = hdr->e_phentsize;

    // Walk program headers
    for (u16 i = 0; i < hdr->e_phnum && info->segment_count < ELF_MAX_SEGMENTS; i++) {
        const elf64_phdr_t* phdr =
            (const elf64_phdr_t*)((const u8*)data + hdr->e_phoff + (u64)i * hdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        // Verify segment is within file
        /* FIX(#elf-offset-wrap): wrap-safe form — `p_offset + p_filesz`
         * overflows for crafted values (p_offset = -N, p_filesz = N
         * passed the old check and underflowed `size - src_off` in
         * build_user_image into a huge kmemcpy length). */
        if (phdr->p_offset > size || phdr->p_filesz > size - phdr->p_offset) {
            vga_printf("[ELF] Segment %u extends beyond file (offset=0x%lx, filesz=0x%lx, filesize=0x%lx)\n",
                       (u32)i, (u64)phdr->p_offset, (u64)phdr->p_filesz, size);
            continue;
        }

        // Verify segment target range stays inside user space.
        // Without this check a crafted p_vaddr would let the loader map
        // USER-writable pages over kernel memory (privilege escalation).
        {
            u64 seg_lo = phdr->p_vaddr;
            u64 seg_hi = phdr->p_vaddr + phdr->p_memsz;
            if (seg_hi < seg_lo ||                 // overflow
                seg_lo < ELF_USER_ADDR_MIN ||      // kernel low memory
                seg_hi > ELF_USER_ADDR_MAX) {      // kernel/canonical hole
                vga_printf("[ELF] Segment %u vaddr out of user range (0x%lx-0x%lx), rejected\n",
                           (u32)i, seg_lo, seg_hi);
                continue;
            }
        }

        u32 idx = info->segment_count;
        info->segments[idx].vaddr  = phdr->p_vaddr;
        info->segments[idx].memsz  = phdr->p_memsz;
        info->segments[idx].filesz = phdr->p_filesz;
        info->segments[info->segment_count].offset = phdr->p_offset;
        info->segments[idx].flags  = phdr->p_flags;

        // Track load range
        if (phdr->p_vaddr < info->base_address) {
            info->base_address = phdr->p_vaddr;
        }
        if (phdr->p_vaddr + phdr->p_memsz > info->top_address) {
            info->top_address = phdr->p_vaddr + phdr->p_memsz;
        }

        info->segment_count++;
    }

    if (info->segment_count == 0) {
        vga_print("[ELF] No loadable segments found.\n");
        return false;
    }

    return true;
}

// ============================================================
// ELF loading — maps pages and copies segment data
// ============================================================

bool elf_load(const void* data, u64 size, const elf_load_info_t* info, u64 load_base) {
    if (!data || !info || !info->valid) return false;

    const elf64_header_t* hdr = (const elf64_header_t*)data;
    const u8* src = (const u8*)data;

    // Walk program headers and load segments
    u32 loaded = 0;
    for (u16 i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t* phdr =
            (const elf64_phdr_t*)(src + hdr->e_phoff + (u64)i * hdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        // Calculate destination virtual address
        u64 dest = phdr->p_vaddr;
        if (load_base != 0) {
            dest = phdr->p_vaddr + load_base;
        }

        // Never map user pages outside the allowed user region
        if (dest < ELF_USER_ADDR_MIN ||
            dest + phdr->p_memsz > ELF_USER_ADDR_MAX ||
            dest + phdr->p_memsz < dest) {
            vga_printf("[ELF] Segment %u target 0x%lx-0x%lx outside user range, skipped\n",
                       i, dest, dest + phdr->p_memsz);
            continue;
        }

        // Map pages for this segment using VMM
        u64 page_start = PAGE_ALIGN(dest);
        u64 page_end   = PAGE_ROUND_UP(dest + phdr->p_memsz);

        for (u64 vaddr = page_start; vaddr < page_end; vaddr += PAGE_SIZE) {
            phys_addr_t phys = pmm_alloc_page();
            if (phys == 0) {
                vga_printf("[ELF] OOM at 0x%lx! pmm_free=%lu MB\n",
                           vaddr,
                           (unsigned long)(pmm_get_available_memory() >> 20));
                return false;
            }

            // Determine page flags from segment flags
            u64 vmm_flags = VMM_PRESENT | VMM_USER;
            if (phdr->p_flags & PF_W) vmm_flags |= VMM_WRITE;
            if (!(phdr->p_flags & PF_X)) vmm_flags |= VMM_NX;

            vmm_map(vaddr, phys, vmm_flags);

            // Zero the physical page (needed for BSS and guard pages)
            kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        }
        serial_printf("[ELFSEG] mapped va=%lx pages=%lu\n",
                      (unsigned long)page_start,
                      (unsigned long)((page_end-page_start)/PAGE_SIZE));

        // Copy file data into the mapped virtual pages
        if (phdr->p_filesz > 0) {
            kmemcpy((void*)(u64)dest, src + phdr->p_offset, (size_t)phdr->p_filesz);
        }

        {
            extern u64 vmm_debug_ktext_leaf(void);
            serial_printf("[KMAP] after seg %u: ktext leaf=%lx\n",
                          (unsigned)info->segment_count,
                          vmm_debug_ktext_leaf());
        }
        vga_printf("[ELF] Loaded segment %u: 0x%lx - 0x%lx (filesz=%lu, memsz=%lu)\n",
                   loaded, (u64)dest, (u64)(dest + phdr->p_memsz),
                   (unsigned long)phdr->p_filesz, (unsigned long)phdr->p_memsz);
        loaded++;
    }

    return loaded > 0;
}

// ============================================================
// Print ELF info
// ============================================================

void elf_print_info(const elf_load_info_t* info) {
    if (!info || !info->valid) {
        vga_print("[ELF] Not a valid ELF.\n");
        return;
    }

    vga_printf("[ELF] Entry point:    0x%lx\n", info->entry_point);
    vga_printf("[ELF] Load range:     0x%lx - 0x%lx\n",
               info->base_address, info->top_address);
    vga_printf("[ELF] Total memory:   %lu bytes (%lu KB)\n",
               (unsigned long)(info->top_address - info->base_address),
               (unsigned long)((info->top_address - info->base_address) / 1024));
    vga_printf("[ELF] Segments:       %u\n", info->segment_count);

    for (u32 i = 0; i < info->segment_count; i++) {
        const char* flags_str = "";
        u32 f = info->segments[i].flags;
        char fbuf[4] = {0};
        int fi = 0;
        if (f & PF_R) fbuf[fi++] = 'R';
        if (f & PF_W) fbuf[fi++] = 'W';
        if (f & PF_X) fbuf[fi++] = 'X';
        flags_str = fbuf;

        vga_printf("  Seg %u: 0x%lx - 0x%lx  filesz=%lu  memsz=%lu  %s\n",
                   i,
                   (u64)info->segments[i].vaddr,
                   (u64)(info->segments[i].vaddr + info->segments[i].memsz),
                   (unsigned long)info->segments[i].filesz,
                   (unsigned long)info->segments[i].memsz,
                   flags_str);
    }
}

// ============================================================
// Load ELF from filesystem (parse + optionally buffer for execution)
// ============================================================

// Internal: reusable file buffer for ELF loading
#define ELF_MAX_FILE_SIZE (8 * 1024 * 1024)  // 8 MB hard cap
static u8* elf_buf = NULL;
static u64 elf_buf_size = 0;      // buffer capacity
static u64 elf_file_size = 0;     // actual bytes read from file

bool elf_load_from_file(const char* path, elf_load_info_t* info) {
    s32 fd = fs_open(path, FS_READ);
    if (fd < 0) {
        vga_printf("[ELF] Cannot open '%s'\n", path);
        return false;
    }

    /* Probe the real file size and size the reusable load buffer
     * accordingly. (#elf-bigfile FIX: the old fixed 4MB buffer
     * silently truncated apk.static (4.36MB) — its third PT_LOAD
     * extends past the 4MB mark, so the binary ran with a zeroed
     * .data tail. Full-fs reads like busybox md5sum saw the whole
     * file; only exec was broken.) */
    u32 fsz = fs_get_size_by_fd((u32)fd);
    if (fsz == 0 || fsz > ELF_MAX_FILE_SIZE) {
        vga_printf("[ELF] File too large for loader (%u bytes, cap %u)\n",
                   (unsigned)fsz, (unsigned)ELF_MAX_FILE_SIZE);
        fs_close(fd);
        return false;
    }
    if (!elf_buf) {
        elf_buf = (u8*)kmalloc(fsz);
        if (elf_buf) elf_buf_size = fsz;
    } else if (fsz > elf_buf_size) {
        u8* nb = (u8*)kmalloc(fsz);
        if (nb) {
            kfree(elf_buf);
            elf_buf = nb;
            elf_buf_size = fsz;
        }
    }
    if (!elf_buf) {
        vga_print("[ELF] Cannot allocate load buffer\n");
        fs_close(fd);
        return false;
    }

    // Read entire file
    u64 total_read = 0;
    s64 bytes_read;
    while (total_read < elf_buf_size &&
           (bytes_read = fs_read(fd, elf_buf + total_read,
                                 (elf_buf_size - total_read) < 4096
                                     ? (u64)(elf_buf_size - total_read)
                                     : 4096)) > 0) {
        total_read += (u64)bytes_read;
    }
    fs_close(fd);
    elf_file_size = total_read;

    if (total_read < sizeof(elf64_header_t)) {
        vga_printf("[ELF] File too small (%lu bytes)\n", (unsigned long)total_read);
        return false;
    }

    vga_printf("[ELF] Read %lu bytes from '%s'\n", (unsigned long)total_read, path);

    if (!elf_validate(elf_buf, total_read)) {
        vga_print("[ELF] Invalid ELF64 executable\n");
        return false;
    }

    if (!elf_parse(elf_buf, total_read, info)) {
        vga_print("[ELF] Parse failed\n");
        return false;
    }

    vga_print("[ELF] Parsed successfully.\n");
    return true;
}

// ============================================================
// Execute ELF in user mode
// ============================================================

bool elf_execute(const void* data, u64 size, elf_load_info_t* info,
                 u64 load_base, int argc, char** argv) {
    if (!data || !info || !info->valid) return false;

    // Default load base for ET_EXEC binaries linked at 0.
    if (load_base == 0) load_base = ELF_USER_LOAD_BASE;

    // If the ELF is already linked at the intended load base (typical for
    // ET_EXEC binaries built for 0x400000), don't relocate it again —
    // double-relocation breaks absolute addresses in non-PIE code.
    if (info->base_address == load_base) {
        load_base = 0;
    }

    vga_printf("[ELF] Executing at base 0x%lx\n", load_base);

    // Step 1: Load segments into user-space virtual pages
    if (!elf_load(data, size, info, load_base)) {
        vga_print("[ELF] Failed to load segments\n");
        return false;
    }

    // Step 2: Allocate and map user stack
    u64 user_stack_top = 0x7FFFFF000ULL;  // Typical user stack top (just below canonical gap)
    u64 user_stack_bot = user_stack_top - ELF_USER_STACK_SIZE;

    // Allocate physical pages for stack
    for (u64 vaddr = user_stack_bot; vaddr < user_stack_top; vaddr += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (phys == 0) {
            vga_print("[ELF] Out of memory allocating user stack\n");
            return false;
        }
        // Stack pages: present, writable, user, no execute
        vmm_map(vaddr, phys, VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX);
        kmemset((void*)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    }

    // Step 3: Build the System V initial stack (same as Linux):
    //
    //   [rsp+0]        argc
    //   [rsp+8]        argv[0] .. argv[argc-1], NULL
    //   ...            envp = NULL (empty environment)
    //   ...            auxv: AT_NULL pair
    //
    // Strings live just below the vector block; user_rsp points at argc.
    u64 entry_rip = info->entry_point + load_base;

    if (argc < 1) { argc = 1; }                 /* guarantee argv[0] */
    if (!argv || !argv[0]) { argv = NULL; }

    const char* default_argv0 = "/bin/program";
    const char** sarg = (const char**)argv;
    if (!sarg) {
        static const char* def[1];
        def[0] = default_argv0;
        sarg = def;
    }

    /* total string bytes (each NUL-terminated) */
    u64 str_bytes = 0;
    for (int i = 0; i < argc; i++) {
        str_bytes += kstrlen(sarg[i]) + 1;
    }
    str_bytes = (str_bytes + 15) & ~15ULL;

    /* vector block: argc + argv[argc] + NULL + envp NULL + AT_NULL pair */
    u64 vec_qwords = 1 + (u64)argc + 1 + 1 + 2;

    u64 usage = str_bytes + vec_qwords * 8;
    if (usage > ELF_USER_STACK_SIZE / 2) {
        vga_print("[ELF] argv too large for stack\n");
        return false;
    }

    u64 sp = user_stack_top - str_bytes;
    char* usp = (char*)sp;

    u64 arg_ptrs[16];
    int copy_n = (argc > 16) ? 16 : argc;
    for (int i = 0; i < copy_n; i++) {
        u64 len = kstrlen(sarg[i]) + 1;
        kmemcpy(usp, sarg[i], len);
        arg_ptrs[i] = (u64)usp;
        usp += len;
    }

    sp -= vec_qwords * 8;
    sp &= ~15ULL;                               /* 16-byte align       */
    u64* uv = (u64*)sp;

    uv[0] = (u64)copy_n;                        /* argc                */
    for (int i = 0; i < copy_n; i++) {
        uv[1 + i] = arg_ptrs[i];                /* argv[i]             */
    }
    uv[1 + copy_n]     = 0;                     /* argv NULL           */
    uv[1 + copy_n + 1] = 0;                     /* envp = NULL         */
    uv[copy_n + 3]     = 0;                     /* auxv: a_type=AT_NULL*/
    uv[copy_n + 4]     = 0;                     /* auxv: a_val=0       */

    u64 user_rsp = sp;

    vga_printf("[ELF] argv: argc=%d, first='%s'\n", copy_n,
               copy_n > 0 ? sarg[0] : "");

    // Step 4: Set TSS.RSP0 so that syscalls/interrupts switch back to kernel stack
    //
    // A single persistent kernel stack is reused across ALL user program
    // executions. The previous code kmalloc'd a fresh 64 KB stack per run
    // and never freed the old one — every `elf <path> run` permanently
    // leaked a stack. Reusing one stack also bounds nesting: each exit
    // path (handle_user_exit -> kernel_mainloop) starts from the same
    // fixed point instead of stacking new frames on old ones.
    static u8 syscall_kstack[64 * 1024] __attribute__((aligned(16)));
    tss_set_kernel_stack((u64)(syscall_kstack + sizeof(syscall_kstack)));
    {
        extern u64 syscall_kstack_top;
        syscall_kstack_top = (u64)(syscall_kstack + sizeof(syscall_kstack));
    }

    // fresh process state: clean fd table for the new program
    extern void syscall_process_reset(void);
    syscall_process_reset();

    vga_printf("[ELF] Entry: 0x%lx, Stack: 0x%lx - 0x%lx\n",
               entry_rip, user_stack_bot, user_stack_top);
    vga_printf("[ELF] Jumping to user mode (Ring 3) via iretq...\n");

    // Step 5: iretq to user mode
    //   CS = 0x20 (GDT_UCODE_SEL, DPL3, L-bit)
    //   SS = 0x18 (GDT_UDATA_SEL, DPL3)
    //   RFLAGS = 0x202 (IF=1, reserved bit 1 set)
    // Also sets syscall_user_rsp so syscall_entry.S can recover user RSP.
    iret_to_usermode(entry_rip, user_rsp, 0x202ULL, GDT_UCODE_SEL, GDT_UDATA_SEL);

    // iret_to_usermode does not return on success.
    // If we get here, something went wrong (e.g. iretq faulted).
    vga_print("[ELF] WARNING: iret_to_usermode returned (should not happen)\n");
    return true;
}

// ============================================================
// Shell command
// ============================================================

void cmd_elf(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage: elf <filepath> [run]\n");
        vga_print("  elf <path>     Parse and display ELF64 info\n");
        vga_print("  elf <path> run Load + execute in user mode\n");
        return;
    }

    elf_load_info_t info;
    if (!elf_load_from_file(argv[1], &info)) {
        vga_printf("[ELF] Failed to load '%s'\n", argv[1]);
        return;
    }

    elf_print_info(&info);
    vga_printf("[ELF] pmm_free before exec: %lu KB\n",
               (unsigned long)(pmm_get_available_memory() >> 10));

    // Parse program arguments:
    //   elf <path> [args...]  — ALWAYS executes now (process mode).
    //   (legacy literal token "run" is tolerated and skipped)
    char* prog_argv[16];
    int   prog_argc = 0;

    prog_argv[prog_argc++] = (char*)argv[1];

    for (int i = 2; i < argc; i++) {
        if (prog_argc < 15 && kstrcmp(argv[i], "run") != 0)
            prog_argv[prog_argc++] = argv[i];
    }

    {
        /* Run as a real PROCESS: own address space + kernel stack.
         * The shell blocks in run_to_completion until it exits. */
        extern s32 process_spawn(const char* path, char* const argv[], int argc);
        extern void task_run_to_completion(s32 slot);
        s32 slot = process_spawn(argv[1], prog_argv, prog_argc);
        if (slot < 0) {
            vga_print("[ELF] Failed to create process.\n");
            return;
        }
        task_run_to_completion(slot);
        vga_print("[ELF] Process finished.\n");
    }
}
