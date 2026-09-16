/* ============================================================
 * proc.c — Unix-lite process layer for NullOs
 * ============================================================
 * A PROCESS is a task with its own address space (pml4_phys) and
 * its own kernel stack for the syscall path. Built on top of:
 *
 *   - vmm_fork_pml4()      COW clone of address spaces
 *   - context_switch_park  sync-run plumbing (runner/orphan wake)
 *   - syscall_frame_ptr    access to the live trap frame
 *
 * Execution model (v2, concurrent):
 *   shell -> nsh / busybox ash live as independent tasks.
 *   fork(): child is created READY with a copy of the parent's live
 *   syscall trap frame (so it "returns" from fork() with RAX=0);
 *   BOTH parent and child are runnable concurrently from that point.
 *   The scheduler interleaves them (cooperative round-robin at
 *   syscall boundaries / task_yield; IRQ preemption optional).
 *   fd-table ownership transfers at every switch site
 *   (sched_fds_on_switch), so a child execve() cannot clobber the
 *   parent's descriptor view any more.
 *   wait4() loops with task_yield(), reaping zombies as they appear.
 * ============================================================ */

#include "../include/proc.h"
#include "../include/scheduler.h"
#include "../include/mm.h"
#include "../include/elf.h"
#include "../include/fs.h"
#include "../include/gdt.h"
#include "../include/timer.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/fpu.h"

// ---- externals ----------------------------------------------------
extern void     iret_to_usermode(u64 rip, u64 rsp, u64 rflags,
                                 u64 cs, u64 ss);
extern void     task_run_to_completion(s32 slot);
extern void     sched_exit_switch_away(void);

extern void     syscall_process_reset(void);
extern void     syscall_fds_save(task_t* t);
extern void     syscall_fds_load(task_t* t);
extern void     syscall_fds_copy(task_t* dst, task_t* src);

#define PROC_KSTACK_SIZE (64 * 1024)

static s32 next_pid = 1;

// ============================================================
// Resource release (#reap milestone)
// ============================================================
// Tears down a process's private address space and kernel stack.
// SAFE NOW: vmm_destroy_pml4 carries a PML4[511] self-check (foreign
// entries are neutralized, not followed) and only unrefs refcount-
// tracked (COW-shared) user pages, freeing exclusive-untracked pages
// directly — live spaces can no longer lose their frames to someone
// else's teardown. Caller must be sure NO ONE still RUNS this space
// (zombie at reap / fresh-failed spawn / post-execve old space).
static void proc_release_mm(task_t* t) {
    if (!t) return;
    if (t->pml4_phys) {
        serial_printf("[REL511] p=%lx [511]=%lx\n",
                      (unsigned long)t->pml4_phys,
                      (unsigned long)((u64*)PHYS_TO_VIRT(t->pml4_phys))[511]);
        vmm_destroy_pml4(t->pml4_phys);
        t->pml4_phys = 0;
    }
    if (t->kstack_base) {
        kfree((void*)t->kstack_base);
        t->kstack_base = 0;
    }
}

/* #zombie-reap-race: orphan zombies (parent dead before wait4) are
 * released by the scheduler's reclaimer; a living parent's zombies
 * are strictly wait4-owned (POSIX) — the reclaimer must skip them. */
void proc_reap_orphan(task_t* t) {
    if (!t || !t->active || !t->zombie) return;
    proc_release_mm(t);
    t->zombie  = false;
    t->active  = false;
    t->state   = 0;
}

// ============================================================
// Slot allocation / lookup
// ============================================================

static task_t* g_tasks;         /* &tasks[0] from scheduler */
void proc_bind_task_table(task_t* table) { g_tasks = table; }

static s32 proc_alloc_slot(void) {
    if (!g_tasks) return -1;
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        task_t* t = &g_tasks[i];
        if (t->active && t->state == TASK_FINISHED) {
            /* #reap: slot force-recycled before parent reaped it. The
             * victim is idle (FINISHED, switched away) — release its
             * address space and kstack instead of leaking them. */
            proc_release_mm(t);
        }
        if (!t->active) {
            kmemset(t, 0, sizeof(task_t));
            return i;
        }
    }
    return -1;
}

static task_t* proc_by_pid(s32 pid) {
    if (!g_tasks || pid <= 0) return NULL;
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        if (g_tasks[i].active && g_tasks[i].pid == pid) return &g_tasks[i];
    }
    return NULL;
}

// ============================================================
// Per-process VM cursors
// ============================================================

u64 proc_get_brk(void) {
    task_t* t = task_get_current();
    return (t && t->pid > 0 && t->brk_cur) ? t->brk_cur : PROC_BRK_BASE;
}
void proc_set_brk(u64 v) {
    task_t* t = task_get_current();
    if (t && t->pid > 0) t->brk_cur = v;
}
u64 proc_get_mmap(void) {
    task_t* t = task_get_current();
    return (t && t->pid > 0 && t->mmap_cur) ? t->mmap_cur : PROC_MMAP_BASE;
}
void proc_set_mmap(u64 v) {
    task_t* t = task_get_current();
    if (t && t->pid > 0) t->mmap_cur = v;
}

void proc_reactivate_current(void) {
    task_t* t = task_get_current();
    if (t && t->pid > 0) sched_activate(task_get_id());
}

// ============================================================
// Process entry (runs on the child's OWN kernel stack)
// ============================================================

static void process_entry(void) {
    task_t* t = task_get_current();
    serial_printf("[PENTRY] pid=%ld rip=%lx rsp=%lx\n",
                  (s64)(t ? t->pid : -1),
                  t ? t->user_rip : 0, t ? t->user_rsp : 0);
    if (!t || t->pid <= 0) { cli(); for(;;) hlt(); }
    t->started = true;

    sched_activate(task_get_id());        /* cr3 + stacks + TSS       */
    syscall_fds_load(t);                  /* this process's fd table  */
    sti();

    serial_printf("[PENTRY] jumping to ring3\n");
    iret_to_usermode(t->user_rip, t->user_rsp, 0x202ULL,
                     GDT_UCODE_SEL, GDT_UDATA_SEL);
    serial_printf("[PENTRY] IRETQ RETURNED (bug!)\n");
    /* never returns */
    cli();
    for (;;) hlt();
}

// ============================================================
// Last-built image frame registry (#exec-frame-dup instrumentation)
// build_user_image records every frame it allocated; the PMM uses
// proc_img_owns_frame() to scream on free/re-issue of a LIVE frame,
// and g_exec_verify() re-validates PTEs + file-backed content at
// critical execve points (post-build / post-destroy).
// ============================================================
#define XV_MAX_SEGS 16
static struct { u64 va, pa_arr, np, foff, fend, flags; } g_xv_seg[XV_MAX_SEGS];
static u32  g_xv_nseg;
static u64  g_xv_pml4;
static u64  g_xv_stk_pa, g_xv_stk_bot, g_xv_stk_np;
static const u8* g_xv_file;
static u64  g_xv_file_size;

bool proc_img_owns_frame(u64 pa) {
    for (u32 s = 0; s < g_xv_nseg; s++) {
        const phys_addr_t* pas = (const phys_addr_t*)g_xv_seg[s].pa_arr;
        for (u64 k = 0; k < g_xv_seg[s].np; k++)
            if (pas[k] == pa) return true;
    }
    if (g_xv_stk_np && pa >= g_xv_stk_pa &&
        pa < g_xv_stk_pa + g_xv_stk_np * PAGE_SIZE) return true;
    return false;
}

/* Verify the last-built image: PTE==pas[k] and file-backed frame
 * content matches the exec buffer. Runs inside a kernel-CR3 read
 * window so identity frame reads are TRUE physical reads even when
 * the active user space has split identity windows (#pfdump-alias). */
static void g_exec_verify(const char* tag) {
    if (!g_xv_nseg || !g_xv_pml4) return;
    extern u64 vmm_pa_read_begin(void);
    extern void vmm_pa_read_end(u64);
    u64 saved = vmm_pa_read_begin();
    u32 total_bad = 0;
    for (u32 s = 0; s < g_xv_nseg; s++) {
        const phys_addr_t* pas = (const phys_addr_t*)g_xv_seg[s].pa_arr;
        u32 bad = 0;
        for (u64 k = 0; k < g_xv_seg[s].np; k++) {
            u64 va = g_xv_seg[s].va + k * PAGE_SIZE;
            u64* pte = vmm_walk_leaf(g_xv_pml4, va, false, false);
            if (!pte || !(*pte & 1) ||
                (*pte & 0x000ffffffffff000ULL) != pas[k]) {
                if (bad < 3)
                    serial_printf("[XV %s] seg%u k=%lu va=%lx PTE-BUG pte=%lx want=%lx\n",
                                  tag, s, (unsigned long)k, (unsigned long)va,
                                  pte ? *pte : 0, (unsigned long)pas[k]);
                bad++;
                continue;
            }
            /* content check: file-backed range + zero tail */
            u64 lo = g_xv_seg[s].foff + k * PAGE_SIZE;
            const u8* frm = (const u8*)PHYS_TO_VIRT(pas[k]);
            if (lo >= g_xv_seg[s].fend) {
                for (u64 q = 0; q < PAGE_SIZE; q += 4096) {
                    if (frm[q]) { if (bad < 3) serial_printf("[XV %s] seg%u k=%lu va=%lx ZERO-BUG\n", tag, s, (unsigned long)k, (unsigned long)va); bad++; break; }
                }
                continue;
            }
            u64 hi = lo + PAGE_SIZE;
            if (hi > g_xv_seg[s].fend) hi = g_xv_seg[s].fend;
            const u8* f = g_xv_file + lo;
            for (u64 o = 0; o < hi - lo; o++) {
                if (frm[o] != f[o]) {
                    if (bad < 3)
                        serial_printf("[XV %s] seg%u k=%lu va=%lx off=%lx exp=%02x got=%02x\n",
                                      tag, s, (unsigned long)k, (unsigned long)va,
                                      (unsigned long)(lo + o), f[o], frm[o]);
                    bad++;
                    break;
                }
            }
            if (hi - lo < PAGE_SIZE) {
                for (u64 o = hi - lo; o < PAGE_SIZE; o++)
                    if (frm[o]) { if (bad < 3) serial_printf("[XV %s] seg%u k=%lu va=%lx TAIL-BUG\n", tag, s, (unsigned long)k, (unsigned long)va); bad++; break; }
            }
        }
        if (bad) serial_printf("[XV %s] seg%u BAD=%u/%lu\n", tag, s,
                               bad, (unsigned long)g_xv_seg[s].np);
        total_bad += bad;
    }
    /* stack PTEs */
    for (u64 k = 0; k < g_xv_stk_np; k++) {
        u64 va = g_xv_stk_bot + k * PAGE_SIZE;
        u64* pte = vmm_walk_leaf(g_xv_pml4, va, false, false);
        if (!pte || !(*pte & 1) ||
            (*pte & 0x000ffffffffff000ULL) != g_xv_stk_pa + k * PAGE_SIZE) {
            serial_printf("[XV %s] stack k=%lu va=%lx PTE-BUG\n", tag,
                          (unsigned long)k, (unsigned long)va);
            total_bad++;
        }
    }
    vmm_pa_read_end(saved);
    if (!total_bad)
        serial_printf("[XV %s] OK nseg=%u\n", tag, g_xv_nseg);
}

// ============================================================
// ELF image builder (segments + SysV argv stack)
// Writes go through identity-mapped user VAs while cr3 is still the
// kernel's — safe because kernel maps all <4GB identically.
// ============================================================

static bool build_user_image(u64 pml4_phys, const void* data, u64 size,
                             char* const argv[], int argc,
                             u64* out_rip, u64* out_rsp) {
    elf_load_info_t info;
    if (!elf_parse(data, size, &info) || !info.valid) return false;

    u64 load_base = ELF_USER_LOAD_BASE;
    if (info.base_address == load_base) load_base = 0;

    /* #argv-probe fix: capture argv strings into a kernel buffer NOW —
     * they live in the OLD space's user stack and are UNREADABLE once
     * the kernel-pml4 identity window below is active (user VAs are
     * not mapped there). Everything after this point must read only
     * kernel memory or frames via identity. */
    /* #exec-sh-c-stack FIX (part 2): argv staging 16x192 -> 32x1024.
     * `sh -c "SCRIPT"` passes the whole SCRIPT as ONE argv element —
     * the old 190-byte cap silently truncated every non-trivial script
     * (427-char probe came out 249 chars: cut at the staging layer).
     * Static footprint: 32KB .bss — zero new leak paths in the exec
     * path. 64KB user stack easily holds the 32KB worst case. */
    static char argv_buf[32][1024];
    static const char* sarg_v[32];
    {
        static const char* def_argv[1] = { "/bin/program" };
        const char** sarg = (argv && argv[0]) ? (const char**)argv : def_argv;
        int n = (argc > 32) ? 32 : argc;
        if (n < 1) n = 1;
        for (int i = 0; i < n; i++) {
            u64 len = kstrlen(sarg[i]);          /* user VA: pre-window */
            if (len > 1022) len = 1022;
            kmemcpy(argv_buf[i], sarg[i], len + 1);
            argv_buf[i][len] = 0;
            sarg_v[i] = argv_buf[i];
        }
        sarg_v[0] = (argv && argv[0]) ? argv_buf[0] : def_argv[0];
    }

    /* ---- Phase 1: allocate + fill PHYSICAL frames via identity ----
     * No CR3 switch needed: writes go through PHYS_TO_VIRT(pa),
     * which is always identity <4GB and always valid in kernel CR3.
     * This eliminates ALL demand faults and cross-CR3 issues.
     * #ident-alias GUARD: the WRITER here may run under the exec'ing
     * process's OLD user CR3 whose low identity windows are SPLIT —
     * identity VAs in [0x400000..0xA00000) alias ITS OWN seg frames.
     * Switch to the kernel pml4 for the whole build so every identity
     * write/read hits the TRUE physical frame. */
    {
        extern u64 vmm_pa_read_begin(void);
        extern void vmm_pa_read_end(u64);
        u64 s3 = vmm_pa_read_begin();

    #define MAX_IMG_SEGS 16
    struct { u64 va, pa; u64 npages; u64 flags; u64 foff, fend; } seg[MAX_IMG_SEGS];
    u32 nseg = 0;

    for (u16 i = 0; i < info.segment_count && nseg < MAX_IMG_SEGS; i++) {
        u64 dest   = info.segments[i].vaddr + load_base;
        u64 memsz  = info.segments[i].memsz;
        u64 filesz = info.segments[i].filesz;
        if (memsz == 0) continue;
        serial_printf("[BUILD] seg %u va=%lx np=%lu\n", (unsigned)i,
                      (unsigned long)dest, (unsigned long)((PAGE_ROUND_UP(dest+memsz)-PAGE_ALIGN(dest))/PAGE_SIZE));

        u64 page_start = PAGE_ALIGN(dest);
        u64 page_end   = PAGE_ROUND_UP(dest + memsz);
        u64 np         = (page_end - page_start) / PAGE_SIZE;
        u32 fl = VMM_PRESENT | VMM_USER;
        if (info.segments[i].flags & 2) fl |= VMM_WRITE;
        if (!(info.segments[i].flags & 1)) fl |= VMM_NX;

        /* Allocate frame PA list from heap (avoids large stack usage) */
        phys_addr_t* frame_pas = (phys_addr_t*)kmalloc(np * sizeof(phys_addr_t));
        bool alloc_ok = (frame_pas != NULL);
        for (u64 k = 0; alloc_ok && k < np; k++) {
            frame_pas[k] = pmm_alloc_page();
            if (!frame_pas[k]) { serial_printf("[BUILD] OOM page %lu\n", (unsigned long)k); alloc_ok = false; }
            if (alloc_ok) kmemset((void*)PHYS_TO_VIRT(frame_pas[k]), 0, PAGE_SIZE);
        }
        serial_printf("[BUILD] seg %u alloc done (%lu pages)\n", (unsigned)i, (unsigned long)np);
        if (!alloc_ok) {
            for (u64 k = 0; k < np; k++)
                if (frame_pas && frame_pas[k]) pmm_free_page(frame_pas[k]);
            if (frame_pas) kfree(frame_pas);
            serial_printf("[BUILD] SEGFAIL seg=%u\n", (unsigned)i);  /* DIAG */
            /* FIX(#build-img-leak): fall through to the common cleanup —
             * the old `return false` leaked every PREVIOUS segment's
             * frames and frame arrays. */
            goto build_fail;
        }

        /* Copy file data into individual frames using REAL file offset */
        u64 src_off = info.segments[i].offset;
        u64 dst_off = dest - page_start;      /* offset within first page */
        u64 remaining = filesz;
        for (u64 k = 0; k < np && remaining > 0; k++) {
            u64 dst_pa = frame_pas[k];
            u64 in_page_off = (k == 0) ? dst_off : 0;
            u64 to_copy = PAGE_SIZE - in_page_off;
            if (to_copy > remaining) to_copy = remaining;
            /* FIX(#elf-offset-wrap): wrap-safe clamp — `src_off > size`
             * used to underflow `size - src_off` into ~2^63 and kmemcpy
             * faulted on a non-canonical source (kernel #PF halt). */
            if (src_off > size) break;
            if (to_copy > size - src_off)
                to_copy = size - src_off;
            if (to_copy == 0) break;
            kmemcpy((void*)PHYS_TO_VIRT(dst_pa + in_page_off),
                    (const u8*)data + src_off,
                    to_copy);
            src_off += to_copy;
            remaining -= to_copy;
        }
        serial_printf("[BUILD] seg %u data copied (%lu bytes left)\n",
                      (unsigned)i, (unsigned long)filesz);
        /* #apk-gc diagnostic: frame-list integrity probes */
        for (u32 pk = 0; pk < 4 && pk < np; pk++)
            serial_printf("[BUILD] seg%u pas[%u]=%lx\n",
                          (unsigned)i, pk, (unsigned long)frame_pas[pk]);
        if (np > 700) {
            serial_printf("[BUILD] seg%u pas[193]=%lx pas[642]=%lx "
                          "pas[700]=%lx pas[761]=%lx\n",
                          (unsigned)i,
                          (unsigned long)frame_pas[193],
                          (unsigned long)frame_pas[642],
                          (unsigned long)frame_pas[700],
                          (unsigned long)frame_pas[761]);
        }

        seg[nseg].va     = page_start;
        seg[nseg].pa     = (u64)frame_pas;  /* store ARRAY pointer */
        seg[nseg].npages = np;
        seg[nseg].flags  = fl;
        seg[nseg].foff   = info.segments[i].offset - (dest - page_start);
        seg[nseg].fend   = info.segments[i].offset + filesz;
        nseg++;
    }

    /* ---- Stack: individual frames ---- */
    serial_printf("[BUILD] stack alloc avail=%lu KB\n",
                  (unsigned long)(pmm_get_available_memory() >> 10));
    u64 stack_top = 0x7FFFFF000ULL;
    u64 stack_bot = stack_top - ELF_USER_STACK_SIZE;
    u64 stack_np  = ELF_USER_STACK_SIZE / PAGE_SIZE;
    phys_addr_t stk_pa = pmm_alloc_pages(stack_np);
    if (!stk_pa) {
        /* fallback comment preserved: individual-page fallback never
         * existed; a failed stack alloc now goes through the common
         * cleanup instead of leaking every segment frame. */
        serial_printf("[BUILD] stack contiguous failed\n");
        goto build_fail;
    }    kmemset((void*)PHYS_TO_VIRT(stk_pa), 0, ELF_USER_STACK_SIZE);
    serial_printf("[BUILD] stack pa=%lx..%lx np=%lu\n",
                  (unsigned long)stk_pa,
                  (unsigned long)(stk_pa + ELF_USER_STACK_SIZE - 1),
                  (unsigned long)stack_np);

    /* Build SysV initial stack DIRECTLY in the stack frames.
     * FIX(#exec-big/A): the old layout placed strings at the very top
     * and built vectors UPWARD, so rsp could land exactly ON stack_top
     * (argv "uname -a" made vec_off == ELF_USER_STACK_SIZE). iret then
     * entered with user_rsp == unmapped boundary page -> immediate #PF.
     * Canonical layout now: strings fill the top going DOWN, vector
     * block sits below them (also 16B aligned), rsp points at the
     * BOTTOM of the vectors — always strictly inside mapped pages.
     * Strings come from the pre-window kernel copy (sarg_v). */
    const char** sarg = sarg_v;
    if (argc < 1) argc = 1;

    u64 auxv_pairs = 11;
    u64 vec_qwords = 1 + (u64)argc + 2 + auxv_pairs * 2 + 2; /* argc+argv+NULL+envp+auxv+AT_RANDOM pad */
    u64 vec_total = vec_qwords * 8 + 32;

    u8* stk = (u8*)PHYS_TO_VIRT(stk_pa);
    /* #exec-sh-c-stack FIX (part 3): stack-side argv slots 16 -> 32,
     * synced with the execve pointer cap and the staging buffer. */
    u64 arg_va[32];
    int n = (argc > 32) ? 32 : argc;

    u64 cursor = ELF_USER_STACK_SIZE;            /* offset of stack_top  */
    for (int i = n - 1; i >= 0; i--) {
        u64 len = kstrlen(sarg[i]) + 1;
        cursor -= len;
        if (cursor < vec_total) goto build_fail;  /* overflow guard      */
        kmemcpy(stk + cursor, sarg[i], len);
        arg_va[i] = stack_bot + cursor;
    }
    cursor &= ~15ULL;

    /* Vector block below the strings */
    cursor -= vec_total;
    cursor &= ~15ULL;
    if (cursor < PAGE_SIZE) goto build_fail;      /* keep 4K for crt push */

    u64 vec_off = cursor;
    u64* uv = (u64*)(stk + vec_off);
    uv[0] = (u64)n;
    for (int i = 0; i < n; i++) uv[1+i] = arg_va[i];
    uv[1+n] = 0; uv[2+n] = 0;

    /* AT_RANDOM bytes at the end of the block */
    u64 rnd_off = vec_off + ((u64)n + 3 + auxv_pairs * 2) * 8;
    u64 t = timer_get_ticks();
    for (int k = 0; k < 16; k++) stk[rnd_off+k] = (u8)(t >> ((k&7)*8));
    u64 rnd_va = stack_bot + rnd_off;

    u64 phdr_va = load_base > 0
        ? load_base + info.phoff
        : info.segments[0].vaddr + info.phoff;

    u64* av = (u64*)(stk + vec_off + ((u64)n + 3) * 8);
    *av++ = 3;  *av++ = phdr_va;
    *av++ = 4;  *av++ = info.phentsize;
    *av++ = 5;  *av++ = info.phnum;
    *av++ = 6;  *av++ = 4096;
    *av++ = 9;  *av++ = info.entry_point + load_base;
    *av++ = 11; *av++ = 0;
    *av++ = 12; *av++ = 0;
    *av++ = 13; *av++ = 0;
    *av++ = 14; *av++ = 0;
    *av++ = 23; *av++ = 0;
    *av++ = 25; *av++ = rnd_va;
    *av++ = 0;  *av++ = 0;

    u64 user_rsp = stack_bot + vec_off;

    serial_printf("[BUILD] phase2 map\n");
    for (u32 s2 = 0; s2 < nseg; s2++) {
        phys_addr_t* pas2 = (phys_addr_t*)seg[s2].pa;
        if (seg[s2].npages > 700)
            serial_printf("[BUILD] pre-phase2 seg%u pas[193]=%lx "
                          "pas[642]=%lx\n", s2,
                          (unsigned long)pas2[193],
                          (unsigned long)pas2[642]);
    }
    for (u32 s = 0; s < nseg; s++) {
        phys_addr_t* pas = (phys_addr_t*)seg[s].pa;
        for (u64 k = 0; k < seg[s].npages; k++) {
            u64 va = seg[s].va + k * PAGE_SIZE;
            u64* pte = vmm_walk_leaf(pml4_phys, va, true,
                                     !!(seg[s].flags & VMM_USER));
            if (!pte) {
                serial_printf("[BUILD] walk FAIL va=%lx\n", va);
                /* FIX(#build-img-leak): rewind the PTEs already written
                 * in this PML4 — the caller destroys it and would
                 * double-free every mapped frame after our cleanup. */
                for (u32 s_r = 0; s_r <= s; s_r++) {
                    u64 kmax = (s_r == s) ? k : seg[s_r].npages;
                    for (u64 k_r = 0; k_r < kmax; k_r++) {
                        u64* pte_r = vmm_walk_leaf(
                            pml4_phys,
                            seg[s_r].va + k_r * PAGE_SIZE,
                            false, false);
                        if (pte_r) *pte_r = 0;
                    }
                }
                goto build_fail;
            }
            *pte = pas[k] | seg[s].flags;
        }
        serial_printf("[BUILD] seg %u mapped\n", (unsigned)s);
    }
    for (u64 k = 0; k < stack_np; k++) {
        u64* pte = vmm_walk_leaf(pml4_phys, stack_bot + k * PAGE_SIZE,
                                 true, true);
        if (!pte) {
            serial_printf("[BUILD] stack walk FAIL k=%lu\n", (unsigned long)k);
            for (u64 k_r = 0; k_r < k; k_r++) {
                u64* pte_r = vmm_walk_leaf(pml4_phys,
                                           stack_bot + k_r * PAGE_SIZE,
                                           false, false);
                if (pte_r) *pte_r = 0;
            }
            for (u32 s_r = 0; s_r < nseg; s_r++) {
                for (u64 k_r = 0; k_r < seg[s_r].npages; k_r++) {
                    u64* pte_r = vmm_walk_leaf(
                        pml4_phys, seg[s_r].va + k_r * PAGE_SIZE,
                        false, false);
                    if (pte_r) *pte_r = 0;
                }
            }
            goto build_fail;
        }
        *pte = (stk_pa + k * PAGE_SIZE)
               | VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX;
    }

    /* publish the frame registry for PMM alarms + g_exec_verify().
     * FIX(#build-img-leak): the previous image's kmalloc'd pa arrays
     * were silently overwritten (never freed) on every exec — a few KB
     * leaked per execve for busybox-sized images. */
    for (u32 s0 = 0; s0 < XV_MAX_SEGS; s0++) {
        if (g_xv_seg[s0].pa_arr) {
            kfree((void*)(u64)g_xv_seg[s0].pa_arr);
            g_xv_seg[s0].pa_arr = 0;
        }
    }
    g_xv_nseg = (nseg < XV_MAX_SEGS) ? nseg : XV_MAX_SEGS;
    for (u32 s3 = 0; s3 < g_xv_nseg; s3++) {
        g_xv_seg[s3].va     = seg[s3].va;
        g_xv_seg[s3].pa_arr = seg[s3].pa;
        g_xv_seg[s3].np     = seg[s3].npages;
        g_xv_seg[s3].foff   = seg[s3].foff;
        g_xv_seg[s3].fend   = seg[s3].fend;
        g_xv_seg[s3].flags  = seg[s3].flags;
    }
    g_xv_pml4    = pml4_phys;
    g_xv_stk_pa  = stk_pa;
    g_xv_stk_bot = stack_bot;
    g_xv_stk_np  = stack_np;
    g_xv_file    = (const u8*)data;
    g_xv_file_size = size;

    *out_rip = info.entry_point + load_base;
    *out_rsp = user_rsp;
    vmm_pa_read_end(s3);                  /* #ident-alias GUARD ends */
    serial_printf("[BUILD] done rip=%lx rsp=%lx\n", *out_rip, *out_rsp);
    return true;

build_fail:
    /* FIX(#build-img-leak): every error path used to `return false`
     * straight out, orphaning ALL already-allocated segment frames,
     * their kmalloc'd frame lists and the stack frames — the caller
     * only destroys the (now rewound) PML4, so nothing ever freed
     * them. The user-triggerable argv-overflow guards made this a
     * several-MB-per-failed-execve leak. */
    for (u32 sf = 0; sf < nseg; sf++) {
        phys_addr_t* pas = (phys_addr_t*)seg[sf].pa;
        if (!pas) continue;
        for (u64 k = 0; k < seg[sf].npages; k++)
            if (pas[k]) pmm_free_page(pas[k]);
        kfree(pas);
        seg[sf].pa = 0;
    }
    if (stk_pa) pmm_free_pages(stk_pa, stack_np);
    vmm_pa_read_end(s3);
    serial_printf("[BUILD] FAILED — frames reclaimed\n");
    return false;
    }
}
// ============================================================
// Spawn
// ============================================================

s32 process_spawn(const char* path, char* const argv[], int argc) {
    serial_printf("[SPAWN] enter path=%s\n", path);
    /* Read the executable into a right-sized buffer.
     * KNOWN ISSUE (#exec-big): images > 512 KB trip a triple-fault in
     * the load path (documented in the dossier below). Small binaries
     * (hello, forktest) run fine. Gate until fixed. */
    s64 fsize = fs_file_size(path);
    if (fsize <= 0 || fsize > 16 * 1024 * 1024) return -1;
    if (0) {
        vga_print("[ELF] image too large for exec (known bug #exec-big)\n");
        serial_printf("[PROC] refused big image %ld\n", (s64)fsize);
        return -7;
    }
    static u8* exe_buf = NULL;
    if (!exe_buf) exe_buf = (u8*)kmalloc(16 * 1024 * 1024);
    if (!exe_buf) return -12;
    (void)0;
    s32 fd = fs_open(path, FS_READ);
    if (fd < 0) return -1;
    s64 total = 0;
    while (total < fsize) {
        s64 r = fs_read((u32)fd, exe_buf + total, (u64)(fsize - total));
        if (r <= 0) break;
        total += r;
    }
    fs_close((u32)fd);
    if (total <= 0) return -1;

    s32 slot = proc_alloc_slot();
    if (slot < 0) return -1;
    task_t* t = &g_tasks[slot];

    t->kstack_base = (u64)kmalloc(PROC_KSTACK_SIZE);
    if (!t->kstack_base) return -1;
    t->kstack_top  = t->kstack_base + PROC_KSTACK_SIZE;

    /* Per-process PML4 via recursive clone. Each process gets PRIVATE
     * intermediate tables so user mappings never corrupt kernel ones.
     * Huge pages split on demand by vmm_walk_leaf.
     * Teardown deferred (bug #reap) — slots leak, frames stay mapped. */
    t->pml4_phys = vmm_fork_pml4(vmm_get_kernel_pml4(), false);
    if (!t->pml4_phys) {
        kfree((void*)t->kstack_base);
        t->active = false;
        return -1;
    }

    t->pid       = next_pid++;
    task_t* par  = task_get_current();
    t->ppid      = (par && par->pid > 0) ? par->pid : 0;
    t->pgrp      = t->pid;                /* self-led process group   */
    t->brk_cur   = PROC_BRK_BASE;
    t->mmap_cur  = PROC_MMAP_BASE;
    t->entry     = NULL;                   /* wrapper-driven           */
    t->state     = TASK_READY;
    t->active    = true;
    t->preempt_remaining = SCHED_PREEMPT_QUANTUM;
    t->park_ksp  = NULL;
    t->started   = false;
    /* #procfs-cmdline: name = exec path (argv[0]), not "proc<N>" —
     * /proc/self/cmdline materializes from this (libwayland logs and
     * ps-style output expect the real binary path).                  */
    {
        u64 i = 0;
        while (path[i] && i < TASK_NAME_MAX - 1) {
            t->name[i] = path[i]; i++;
        }
        t->name[i] = 0;
    }
    sched_active_count_add();

    if (!build_user_image(t->pml4_phys, exe_buf, (u64)total,
                          argv, argc, &t->user_rip, &t->user_rsp)) {
        sched_active_count_sub();
        vmm_destroy_pml4(t->pml4_phys);
        kfree((void*)t->kstack_base);
        t->active = false;
        return -1;
    }
    g_exec_verify("spawn");


    /* fresh fd table snapshot for this process */
    syscall_process_reset();
    serial_printf("[SPAWN] fd reset ok\n");
    syscall_fds_save(t);
    serial_printf("[SPAWN] fd saved\n");

    /* Initial kernel-stack frame so context_switch_park lands the
     * child in its entry wrapper (same layout as task_create):
     *   [rsp+0]=rbx .. [rsp+40]=r15, [rsp+48]=return address */
    {
        u64* sp = (u64*)t->kstack_top;
        sp[-1] = (u64)process_entry;
        sp[-2] = 0; sp[-3] = 0; sp[-4] = 0;
        sp[-5] = 0; sp[-6] = 0; sp[-7] = 0;
        t->rsp = &sp[-7];
    }
    return slot;
}

/* marker for debugging */
void process_spawn_marker(const char* msg) {
    serial_printf("[SPAWN] %s\n", msg);
}

// ============================================================
// Exit
// ============================================================

/* SIGCHLD-state probe for rt_sigsuspend (#sigsuspend-spin): report
 * whether this task's children produced a waitable event.
 *   1 = at least one zombie child (wait4 would reap it now)
 *   0 = children exist, all still running
 *  -1 = no children at all (wait4 would report ECHILD)         */
s32 proc_wait_event(void) {
    task_t* cur = task_get_current();
    if (!cur || cur->pid <= 0) return -1;
    s32 state = -1;
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        task_t* t = &g_tasks[i];
        if (!t->active || t->ppid != cur->pid) continue;
        if (t->zombie) return 1;               /* waitable now          */
        state = 0;                             /* running child         */
    }
    return state;
}

void proc_exit_current(s32 code) {
    task_t* t = task_get_current();
    if (!t || t->pid <= 0) {
        /* legacy single-shot path (shell-era programs) */
        extern volatile u64 sys_exit_pending;
        sys_exit_pending = 1;
        return;
    }
    serial_printf("[PROC] pid=%d exit=%d\n", t->pid, code);
    {
        serial_printf("[EXIT511] p=%lx [511]=%lx\n",
                      (unsigned long)t->pml4_phys,
                      (unsigned long)((u64*)PHYS_TO_VIRT(t->pml4_phys))[511]);
    }
    t->exit_code = code;
    t->zombie    = true;
    t->state     = TASK_FINISHED;
    sched_active_count_sub();

    /* #sigchld-mach: latch SIGCHLD on the parent. The minimal signal
     * machine delivers it when the parent next blocks in
     * rt_sigsuspend (busybox ash dowait). */
    if (t->ppid > 0) {
        for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
            task_t* p = &g_tasks[i];
            if (p->active && p->pid == t->ppid) { p->sig_pending = 1; break; }
        }
    }

    /* Release every descriptor the task holds: pipe end aliases die,
     * ramfs descriptions close, sockets free — readers blocked on a
     * pipe then see EOF instead of hanging forever (#pipe-eof-leak). */
    {
        extern void syscall_release_all_fds(void);
        syscall_release_all_fds();
    }

    sched_exit_switch_away();             /* never returns */
    cli();
    for (;;) hlt();
}

// ============================================================
// fork
// ============================================================

extern void fork_resume_child(void);
extern u64  syscall_frame_ptr;

s32 proc_do_fork(void) {
    task_t* parent = task_get_current();
    if (!parent || parent->pid <= 0 || !syscall_frame_ptr) return -12; /* ENOMEM */

    s32 slot = proc_alloc_slot();
    if (slot < 0) return -12;
    task_t* ch = &g_tasks[slot];

    /* Capture the parent's LIVE fd table first: ash-style shells open
     * /dev/tty and dup descriptors well after their own execve saved
     * the boot-time stdio-only snapshot; without this refresh the
     * child inherits a stale table. */
    syscall_fds_save(parent);

    ch->kstack_base = (u64)kmalloc(PROC_KSTACK_SIZE);
    if (!ch->kstack_base) { ch->active = false; return -12; }
    ch->kstack_top  = ch->kstack_base + PROC_KSTACK_SIZE;
    ch->pml4_phys   = vmm_fork_pml4(parent->pml4_phys, true);
    if (!ch->pml4_phys) {
        kfree((void*)ch->kstack_base);
        ch->active = false;
        return -12;
    }

    ch->pid      = next_pid++;
    ch->ppid     = parent->pid;
    ch->pgrp     = parent->pgrp;          /* POSIX: inherit pgrp      */
    ch->stop_sig = 0;                     /* children start runnable  */
    ch->stop_reported = false;
    ch->brk_cur  = parent->brk_cur;
    ch->mmap_cur = parent->mmap_cur;
    ch->fs_base  = parent->fs_base;       /* TLS base is image-relative and
                                             identical after fork (COW clone);
                                             inherited until child re-sets it */
    ch->state    = TASK_READY;
    ch->active   = true;
    ch->preempt_remaining = SCHED_PREEMPT_QUANTUM;
    /* #procfs-cmdline: fork children keep the parent's argv[0] name
     * (POSIX /proc/<pid>/cmdline semantics), not a synthetic id.    */
    {
        u64 i = 0;
        while (parent->name[i] && i < TASK_NAME_MAX - 1) {
            ch->name[i] = parent->name[i]; i++;
        }
        ch->name[i] = 0;
    }
    sched_active_count_add();
    syscall_fds_copy(ch, parent);

    /* Copy the parent's live syscall frame onto the child stack top
     * so the child "returns" from fork() at the same user RIP/RSP. */
    u64 dst = ch->kstack_top - SF_SIZE;
    kmemcpy((void*)dst, (const void*)syscall_frame_ptr, SF_SIZE);
    *(u64*)(dst + SF_RAX) = 0;            /* child sees fork()==0     */

    /* Park the child as a continuation that jumps into its frame */
    ch->cont[0]=0; ch->cont[1]=0; ch->cont[2]=0; ch->cont[3]=0;
    ch->cont[4]=0; ch->cont[5]=0;
    ch->cont[6] = (u64)&fork_resume_child;
    ch->cont[7] = dst;
    ch->rsp = ch->cont;

    serial_printf("[PROC] fork: parent=%d child=%d\n", parent->pid, ch->pid);
    /* [dossier excerpts kept below for archaeology: the historical
     * "child dies right after IRETQ" mystery was resolved long ago;
     * the resume mechanics are identical to v1 — only WHO switches
     * into the parked continuation changed, plus WHEN.]             */

    /* Fresh fork children must not enter userland relying on an
     * uninitialized TCB FPU image (task_create gives every fresh task
     * an fninit template; fork used to inherit zeros because the v1
     * sync-runner hid the issue). Mirror the template here.          */
    {
        static u8 init_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(64)));
        static bool fpu_template_ready = false;
        if (!fpu_template_ready) {
            __asm__ volatile("fninit");
            fpu_save(init_fpu_state);
            fpu_template_ready = true;
        }
        kmemcpy(ch->fpu_state, init_fpu_state, FPU_STATE_SIZE);
    }

    /* ============================================================
     * FORK V2 — async child.
     * The child sits READY in the task table with a valid parked
     * continuation (fork_resume_child + copied syscall frame).
     * Whoever picks it next (task_yield / preempt / exit-fallback)
     * resumes it exactly like v1 did — just later, and interleaved
     * with the parent instead of instead-of-it.
     *
     * Invariants that make this legal:
     *  1. fd-table ownership transfers at EVERY switch site
     *     (scheduler.c:sched_fds_on_switch), so the child's execve()
     *     wiping the global proc_fds[] can no longer blind the parent.
     *  2. sched_exit_switch_away() gained a direct hand-off fallback:
     *     a child exiting without a sync-runner wakes the next READY
     *     task instead of cli-hlt wedging the CPU forever.
     *  3. All console-blocking syscalls (read/poll/wait4) yield while
     *     they pend, so parked children keep making progress.
     * ============================================================ */
    return ch->pid;
}

static bool copy_user_path(u64 path_u, char* out, u32 cap) {
    if (!path_u) return false;
    /* FIX(#execve-raw-ptr): the path pointer was dereferenced with NO
     * validation at all — execve((char*)0xFFFF800000000000, ...) took
     * a kernel-mode fault outside the demand-paged region and HALTED
     * the machine. Validate page-by-page WHILE consuming the string:
     * each byte is read only after its page passed the syscall
     * user-range check (static bounds + kernel-window exclusion +
     * USER-page walk), so a path that starts in user memory and
     * crosses into a supervisor alias page is rejected too. */
    extern bool syscall_user_range_ok(u64, u64);
    if (!syscall_user_range_ok(path_u, 1)) return false;
    const char* p = (const char*)path_u;
    u64 vetted_page = path_u & ~0xFFFULL;
    u32 i = 0;
    while (i + 1 < cap) {
        u64 a = path_u + i;
        if ((a & ~0xFFFULL) != vetted_page) {
            vetted_page = a & ~0xFFFULL;
            if (!syscall_user_range_ok(a, 1)) return false;
        }
        if (!p[i]) break;
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

/* FIX(#execve-raw-ptr): vet a user argv STRING the same page-by-page
 * way (up to `maxlen` bytes). Returns false when the string leaves
 * genuine user memory. */
static bool user_str_ok(u64 addr, u32 maxlen) {
    extern bool syscall_user_range_ok(u64, u64);
    if (!addr) return false;
    if (!syscall_user_range_ok(addr, 1)) return false;
    const char* p = (const char*)addr;
    u64 vetted_page = addr & ~0xFFFULL;
    for (u32 i = 0; i < maxlen; i++) {
        u64 a = addr + i;
        if ((a & ~0xFFFULL) != vetted_page) {
            vetted_page = a & ~0xFFFULL;
            if (!syscall_user_range_ok(a, 1)) return false;
        }
        if (!p[i]) return true;
    }
    return true;                              /* unterminated: capped */
}

s64 proc_do_execve(const char* path_u, char* const argv_u[]) {
    task_t* cur = task_get_current();
    if (!cur || cur->pid <= 0 || !syscall_frame_ptr) return -8; /* ENOEXEC */

    char path[128];
    if (!copy_user_path((u64)path_u, path, sizeof(path))) return -14;

    /* #exec-sh-c-stack FIX (part 1): argv depth cap 8 -> 32. The old
     * cap silently DROPPED arguments beyond #7 for exec'd images, so
     * `sh -c 'prog a b c d e f g h'` passed a corrupted command line
     * (no error, just wrong argv) — classic "sh -c doesn't work"
     * symptom. 32 pointer slots = 256 bytes of stack — trivial. */
    char* argv[32];
    int argc = 0;
    argv[argc++] = path;
    /* FIX(#execve-raw-ptr): the argv ARRAY pointer itself was only
     * NULL-checked (a wild array pointer faulted the kernel), and the
     * element range test lacked the kernel-window exclusion, so argv
     * strings could be sourced from kernel-window memory (physical
     * memory disclosure into the child's argv / serial log). */
    if (argv_u) {
        extern bool syscall_user_range_ok(u64, u64);
        if (!syscall_user_range_ok((u64)argv_u, 32 * 8)) return -14;
        for (int i = 1; i < 32; i++) {
            char* a = argv_u[i];
            if (!a || (u64)a < 0x400000ULL ||
                (u64)a >= 0x7FFFFF000ULL) break;
            if (!user_str_ok((u64)a, 4096)) break;
            argv[argc++] = a;
        }
    }
    /* #argv-probe: what the exec'ing image will actually see */
    {
        serial_printf("[EXECV] path=%s argc=%d\n", path, argc);
        for (int i = 0; i < argc; i++)
            serial_printf("[EXECV] argv[%d]='%s'\n", i, argv[i]);
    }

    /* Read executable (shared kernel buffer, BusyBox-sized) */
    static u8* xbuf = NULL;
    if (!xbuf) xbuf = (u8*)kmalloc(16 * 1024 * 1024);
    if (!xbuf) return -12;
    s32 fd = fs_open(path, FS_READ);
    if (fd < 0) return -2;
    s64 total = 0;
    while (total < 16 * 1024 * 1024) {
        s64 r = fs_read((u32)fd, xbuf + total, 16 * 1024 * 1024 - (u64)total);
        if (r <= 0) break;
        total += r;
    }
    fs_close((u32)fd);
    if (total <= 0) return -8;

    /* Fresh address space from the kernel template. The template now
     * clones WITHOUT the [511] junk subtree (~6 fewer pool pages per
     * exec/spawn) and receives a true self-map. */
    u64 np = vmm_fork_pml4(vmm_get_kernel_pml4(), false);
    if (!np) return -12;

    u64 rip = 0, rsp = 0;
    if (!build_user_image(np, xbuf, (u64)total, argv, argc, &rip, &rsp)) {
        vmm_destroy_pml4(np);
        return -8;
    }
    g_exec_verify("pre-swap");

    /* Swap: old space torn down AFTER the new one is fully built */
    u64 old = cur->pml4_phys;
    cur->pml4_phys = np;
    cur->user_rip  = rip;                 /* informational            */
    cur->user_rsp  = rsp;
    cur->brk_cur   = PROC_BRK_BASE;
    cur->mmap_cur  = PROC_MMAP_BASE;
    /* #sigchld-mach: POSIX exec resets caught handlers and drops
     * pending signals — a fresh image must never inherit the old
     * one's SIGCHLD trampoline. */
    cur->sig_act = 0; cur->sig_restorer = 0;
    cur->sig_pending = 0; cur->sig_in_handler = 0;
    /* #pipe-fd-switch FIX (fd-inheritance across execve): POSIX says
     * execve KEEPS the caller's open descriptors (CLOEXEC excepted).
     * The old process_reset()+save pair handed every exec'd image a
     * wiped stdio-only table — a pipeline child's dup2(pipe,0) was
     * erased by its own exec, so `echo x | busybox wc` blocked reading
     * the tty forever. The per-task snapshot system already isolates
     * the parent (sched_fds_on_switch at every site), so just capture
     * the caller's CURRENT view (with the dup2'd ends) as the new
     * image's snapshot. Fresh-task spawns (proc_do_spawn) still reset
     * — there a stdio-only table is the desired state. No CLOEXEC
     * tracking yet: busybox ash closes its extra fds explicitly
     * (script fd, pipe ends) before exec, as traced in Task 13. */
    syscall_fds_save(cur);
    vmm_switch_pml4(np);                  /* cr3 + active             */
    if (old) vmm_destroy_pml4(old);
    g_exec_verify("post-destroy");

    /* Patch the LIVE trap frame: this syscall "returns" into the new
     * program instead of back into the old one. */
    u64 f = syscall_frame_ptr;
    *(u64*)(f + SF_RIP)    = rip;
    *(u64*)(f + SF_RSP)    = rsp;
    *(u64*)(f + SF_RAX)    = 0;
    for (int r = SF_R11; r <= SF_R15; r += 8) *(u64*)(f + r) = 0;

    serial_printf("[PROC] execve pid=%d path=%s\n", cur->pid, path);
    return 0;
}

// ============================================================
// wait4
// ============================================================

s32 proc_do_wait4(s32 pid, u64 status_user, bool nohang, bool wuntraced) {
    task_t* cur = task_get_current();
    serial_printf("[W4] enter cur=%ld want=%ld nohang=%ld\n",
                  (s64)(cur ? cur->pid : -1), (s64)pid, (s64)nohang);

    for (;;) {
        s32 found = -1;
        for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
            task_t* t = &g_tasks[i];
            if (!t->active || !t->zombie) continue;
            if (cur && t->ppid != cur->pid) continue;
            if (pid > 0 && t->pid != pid) continue;
            found = i;
            break;
        }
        serial_printf("[W4] scan done found=%d\n", found);
        if (found >= 0) {
            task_t* t = &g_tasks[found];
            /* FIX(#w4-status-window): syscall_user_range_ok instead of the
             * hand-rolled bounds (kernel-window + supervisor-alias VAs now
             * rejected, and a genuinely unmapped user page still passes —
             * the demand pager serves it). */
            if (status_user) {
                extern bool syscall_user_range_ok(u64, u64);
                if (syscall_user_range_ok((u64)status_user, 4))
                    *(u32*)status_user = (u32)(t->exit_code << 8);
            }
            s32 rpid = t->pid;
            serial_printf("[PROC] reap pid=%d code=%d\n", rpid, t->exit_code);
            /* #reap DONE: tear down the zombie's address space + kstack.
             * Safe now that vmm_destroy_pml4(1) never follows foreign/
             * self-map entries, (2) unrefs COW-shared pages so children
             * forked from us keep theirs, and (3) reclaims exclusive
             * untracked frames instead of leaking them. The victim is
             * idle (exited via sched_exit_switch_away; state FINISHED
             * is never scheduled again). */
            proc_release_mm(t);
            t->zombie = false;
            t->active = false;
            return rpid;
        }

        /* Job-control stop report: a STOPPED child whose stop the
         * parent has not seen yet. Reported WITHOUT reaping — the
         * child stays parked until SIGCONT, then runs to a normal
         * exit and gets reaped through the zombie path above. */
        if (wuntraced) {
            for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
                task_t* t = &g_tasks[i];
                if (!t->active || t->zombie || !t->stop_sig ||
                    t->stop_reported) continue;
                if (cur && t->ppid != cur->pid) continue;
                if (pid > 0 && t->pid != pid) continue;
                /* FIX(#w4-status-window): same validation as above. */
                if (status_user) {
                    extern bool syscall_user_range_ok(u64, u64);
                    if (syscall_user_range_ok((u64)status_user, 4))
                        *(u32*)status_user = (u32)((t->stop_sig << 8) | 0x7F);
                }
                t->stop_reported = true;
                serial_printf("[PROC] stopped-report pid=%d sig=%u\n",
                              t->pid, t->stop_sig);
                return t->pid;
            }
        }

        /* FIX(#wait4-echild): a process with ZERO children busy-yielded
         * forever (Linux returns -ECHILD immediately); with WNOHANG it
         * returned 0 ("children exist") instead of -ECHILD. Reuse the
         * #sigsuspend probe: no children at all -> ECHILD. */
        if (nohang) {
            if (proc_wait_event() < 0) return -10;   /* -ECHILD */
            return 0;
        }
        if (proc_wait_event() < 0) return -10;       /* -ECHILD    */
        task_yield();                     /* let children run/finish  */
    }
}
