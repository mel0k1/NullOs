#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/spinlock.h"
#include "../include/serial.h"

// Provided by linker script
extern u8 __kernel_end[];
extern u8 __kernel_phys_end[];   /* #kernel-halffix: PHYSICAL image end */

// ============================================================
/* Carve zone API (defined in PMM section below) */
phys_addr_t pmm_carve_pages(u64 count);

// Physical address from a PTE: strip low flag bits AND the NX bit
// (bit 63 lives above the address but inside the raw entry).
static inline phys_addr_t pte_pa(u64 pte) {
    return pte & ~(0xFFFULL | VMM_NX);
}

// Physical Memory Manager (PMM)
// ============================================================

static u64 total_memory = 0;
static u64* bitmap = NULL;
static size_t bitmap_size = 0;   // number of u64 words
static u64 total_pages = 0;

// Guards bitmap + free-list state. Taken with IRQ saving because
// pmm_alloc_page() may be reached from interrupt context in future.
static spinlock_t pmm_lock = SPINLOCK_INIT;

// Memory map (filled from Multiboot2)
mmap_entry_t pmm_mmap[PMM_MAX_MMAP_ENTRIES];
u32 pmm_mmap_count = 0;

void pmm_init(void* mbi_ptr) {
    // ---- Step 1: Parse Multiboot2 memory map ----
    u64 detected_total = 0;    // highest address in any entry
    u64 detected_usable = 0;   // sum of type==1 regions

    if (mbi_ptr) {
        struct multiboot2_info* mbi = (struct multiboot2_info*)mbi_ptr;
        u32 info_size = mbi->total_size;

        // Walk tags starting at offset 8 (past the 8-byte header)
        struct multiboot2_tag* tag =
            (struct multiboot2_tag*)((u8*)mbi + 8);

        while ((u8*)tag + sizeof(struct multiboot2_tag) <= (u8*)mbi + info_size) {
            if (tag->type == MBI_TAG_TYPE_END) break;

            if (tag->type == MBI_TAG_TYPE_MMAP && tag->size >= 16) {
                struct multiboot2_tag_mmap* mmap_tag =
                    (struct multiboot2_tag_mmap*)tag;

                u32 entry_size = mmap_tag->entry_size;
                if (entry_size < sizeof(struct multiboot2_mmap_entry))
                    entry_size = sizeof(struct multiboot2_mmap_entry);

                u32 entries_len = tag->size - 16;
                u32 num_entries = entries_len / entry_size;
                if (num_entries > PMM_MAX_MMAP_ENTRIES)
                    num_entries = PMM_MAX_MMAP_ENTRIES;

                pmm_mmap_count = 0;
                for (u32 i = 0; i < num_entries; i++) {
                    struct multiboot2_mmap_entry* e =
                        (struct multiboot2_mmap_entry*)
                        ((u8*)mmap_tag + 16 + i * entry_size);

                    pmm_mmap[pmm_mmap_count].addr   = e->base_addr;
                    pmm_mmap[pmm_mmap_count].length = e->length;
                    serial_printf("[MMAP] base=%llx len=%llx type=%u\n",
                                  (unsigned long long)e->base_addr,
                                  (unsigned long long)e->length,
                                  (unsigned)e->type);
                    pmm_mmap[pmm_mmap_count].type   = e->type;
                    pmm_mmap_count++;

                    /* ONLY track usable RAM — including reserved/MMIO
                     * regions above 4GB in this max caused total_memory
                     * to wrap to terabytes and the bitmap cap to clip
                     * usable RAM down to 128MB.                          */
                    if (e->type == 1) {
                        u64 end = e->base_addr + e->length;
                        if (end > detected_total) detected_total = end;
                        detected_usable += e->length;
                    }
                }
                break;
            }

            // Advance to next tag (8-byte aligned)
            u32 tag_size = (tag->size + 7) & ~7u;
            tag = (struct multiboot2_tag*)((u8*)tag + tag_size);
        }
    }

    serial_printf("[PMM] detected_total=%lx\n", (unsigned long)detected_total);

    // ---- Step 2: Fallback if no MBI or no mmap tag ----
    if (detected_total == 0) {
        detected_total = 512 * 1024 * 1024ULL;  // assume 512 MB
        detected_usable = detected_total - 1 * 1024 * 1024;

        pmm_mmap[0].addr   = 0;
        pmm_mmap[0].length = detected_total;
        pmm_mmap[0].type   = 1;
        pmm_mmap_count = 1;
    }

    total_memory = detected_total;
    total_pages  = total_memory / PAGE_SIZE;

    // ---- Step 3: Set up bitmap ----
    // FIX(#exec-big/X1): the bitmap MUST NOT live at user-reachable VAs.
    // At the old spot (right after __kernel_end, 0x44F000) it sat inside
    // the VA range large user images legitimately map (BusyBox spans
    // 0x400000..0x4FC000). While a process runs, CR3 = process tables and
    // kernel reads of `bitmap[]` resolved to USER code pages -> phantom
    // OOM/garbage scans. Relocate to TOP of detected RAM: no user VA can
    // legally get there (user ceiling << physical top on 512M machines),
    // and identity translation still applies (<4GB).
    bitmap_size = (total_pages + 63) / 64;   // number of u64 words
    u64 bitmap_bytes = bitmap_size * sizeof(u64);

    u64 bitmap_phys_top = (total_memory - bitmap_bytes)
                          & ~(u64)(PAGE_SIZE - 1);
    /* Require the area to be usable RAM per memory map */
    bool top_ok = false;
    for (u32 i = 0; i < pmm_mmap_count; i++) {
        if (pmm_mmap[i].type != 1) continue;
        if (pmm_mmap[i].addr <= bitmap_phys_top &&
            bitmap_phys_top + bitmap_bytes <= pmm_mmap[i].addr + pmm_mmap[i].length) {
            top_ok = true;
            break;
        }
    }

    u64 bitmap_phys;
    if (top_ok && bitmap_phys_top > KERNEL_HEAP_START + KERNEL_HEAP_SIZE) {
        bitmap_phys = bitmap_phys_top;
        /* Reserve the frames below the bitmap too so nothing walks into
         * them before marking happens in Step 5. */
        serial_printf("[PMM] bitmap relocated to top-of-RAM %lx (%lu KB)\n",
                      (unsigned long)bitmap_phys,
                      (unsigned long)(bitmap_bytes >> 10));
    } else {
        /* Legacy fallback: after-kernel placement inside the low gap.
         * __kernel_end is a VA since #kernel-halffix — use the PHYSICAL
         * image end (__kernel_phys_end from linker64.ld). */
        bitmap_phys = PAGE_ROUND_UP((u64)__kernel_phys_end);
        u64 bitmap_max_end = KERNEL_HEAP_START - PAGE_SIZE;
        if (bitmap_phys + bitmap_bytes > bitmap_max_end) {
            u64 max_words = (bitmap_max_end - bitmap_phys) / sizeof(u64);
            bitmap_size = max_words;
            total_pages = bitmap_size * 64;
            total_memory = total_pages * PAGE_SIZE;
            bitmap_bytes = bitmap_size * sizeof(u64);
        }
    }

    bitmap = (u64*)PHYS_TO_VIRT(bitmap_phys);   /* #kernel-halffix: PA->VA */

    // Clear bitmap (all pages free initially)
    kmemset(bitmap, 0, bitmap_bytes);

    // ---- Step 4: Mark non-usable regions as used ----
    // First: mark everything as used, then free usable regions
    kmemset(bitmap, 0xFF, bitmap_bytes);  // all used

    for (u32 i = 0; i < pmm_mmap_count; i++) {
        if (pmm_mmap[i].type != 1) continue;  // only type 1 = usable

        u64 base = pmm_mmap[i].addr;
        u64 len  = pmm_mmap[i].length;
        u64 start_page = base / PAGE_SIZE;
        u64 end_page   = (base + len) / PAGE_SIZE;

        if (start_page >= total_pages) continue;
        if (end_page > total_pages) end_page = total_pages;

        for (u64 p = start_page; p < end_page; p++) {
            bitmap[p / 64] &= ~(1ULL << (p % 64));
        }
    }

    // ---- Step 5: Mark kernel + bitmap + heap as used ----
    // Kernel: 0 to __kernel_phys_end. (#kernel-halffix: __kernel_end is
    // a VA above 1GB now — marking up to it "used" the ENTIRE RAM.)
    u64 kernel_end_page = PAGE_ROUND_UP((u64)__kernel_phys_end) / PAGE_SIZE;
    for (u64 p = 0; p < kernel_end_page && p < total_pages; p++) {
        bitmap[p / 64] |= (1ULL << (p % 64));
    }

    // Bitmap pages
    u64 bitmap_start_page = bitmap_phys / PAGE_SIZE;
    u64 bitmap_end_page   = PAGE_ROUND_UP(bitmap_phys + bitmap_bytes) / PAGE_SIZE;
    for (u64 p = bitmap_start_page; p < bitmap_end_page && p < total_pages; p++) {
        bitmap[p / 64] |= (1ULL << (p % 64));
    }

    // Heap: KERNEL_HEAP_START to KERNEL_HEAP_START + KERNEL_HEAP_SIZE
    u64 heap_start_page = KERNEL_HEAP_START / PAGE_SIZE;
    u64 heap_end_page   = (KERNEL_HEAP_START + KERNEL_HEAP_SIZE) / PAGE_SIZE;
    for (u64 p = heap_start_page; p < heap_end_page && p < total_pages; p++) {
        bitmap[p / 64] |= (1ULL << (p % 64));
    }

    // ---- Print info ----
    serial_printf("[PMM] total=%lx(%lu MB) pages=%lu free=%lu MB\n",
                  (unsigned long)total_memory,
                  (unsigned long)(total_memory >> 20),
                  (unsigned long)total_pages,
                  (unsigned long)(pmm_get_available_memory() >> 20));
    /* DEBUG(#exec-big): pin the bitmap pointer state at boot */
    serial_printf("[PMM] INIT bitmap_va=%lx words=%lu\n",
                  (unsigned long)(u64)bitmap, (unsigned long)bitmap_size);
    vga_print("[PMM] Memory map parsed from Multiboot2\n");
    vga_print("[PMM] Total: ");
    vga_print_unsigned(total_memory / 1024 / 1024);
    vga_print(" MB, Bitmap: ");
    vga_print_unsigned(bitmap_bytes / 1024);
    vga_print(" KB at 0x");
    vga_print_hex(bitmap_phys);
    vga_print("\n");
}

// Protected carve zone state (top of RAM): page tables + refcounts.
// General allocator never hands these frames out.
static phys_addr_t carve_ptr  = 0;   /* next free PA in the zone    */
static phys_addr_t carve_lo   = 0;   /* zone start                  */
static u64  carve_start_page = 0;

/* ── #exec-frame-dup instrumentation ─────────────────────────────
 * The LAST-built user image registers its frame list in proc.c; the
 * PMM checks every alloc/free against it. A [PMMALLOC-IMG] line for
 * a pa with no preceding [PMMFREE-IMG] for the same pa is the direct
 * double-issue proof (live frame handed out twice). */
extern bool proc_img_owns_frame(u64 pa);   /* proc.c */

phys_addr_t pmm_alloc_page(void) {
    u64 flags = spin_lock_irqsave(&pmm_lock);
    /* DEBUG(#exec-big): word0 must stay all-ones forever (pages <4MB
     * are kernel/reserved and never freed). A cleared bit here means
     * somebody corrupted the bitmap (or freed a reserved frame). */
    if (__builtin_expect(bitmap[0] != 0xFFFFFFFFFFFFFFFFULL, 0)) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        serial_printf("[BM-CORRUPT] w0=%lx ra=%lx r2=%lx\n",
                      (unsigned long)bitmap[0],
                      (unsigned long)__builtin_return_address(0),
                      (unsigned long)__builtin_return_address(1));
        return 0;
    }
    for (size_t i = 0; i < bitmap_size; i++) {
        if (bitmap[i] == 0xFFFFFFFFFFFFFFFF) continue;
        for (int j = 0; j < 64; j++) {
            if (bitmap[i] & (1ULL << j)) continue;
            phys_addr_t got = (i * 64 + j) * PAGE_SIZE;
            /* Skip carve zone pages, keep scanning above */
            if (carve_start_page &&
                got >= carve_lo &&
                got < carve_lo + 2 * 1024 * 1024)
                continue;
            bitmap[i] |= (1ULL << j);
            spin_unlock_irqrestore(&pmm_lock, flags);
            if (proc_img_owns_frame(got))
                serial_printf("[PMMALLOC-IMG] pa=%lx ra=%lx\n",
                              (unsigned long)got,
                              (unsigned long)__builtin_return_address(0));
            return got;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    serial_printf("[PMM] ALLOC FAIL words=%zu first_word=%lx\n",
                  bitmap_size,
                  bitmap_size ? (unsigned long)bitmap[0] : 0UL);
    return 0;  // Out of memory
}

phys_addr_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    u64 flags = spin_lock_irqsave(&pmm_lock);
    // Try to find `count` contiguous free pages
    for (size_t i = 0; i < bitmap_size; i++) {
        if (bitmap[i] == 0xFFFFFFFFFFFFFFFF) continue;
        for (int j = 0; j < 64; j++) {
            if (bitmap[i] & (1ULL << j)) continue;

            // Found a free bit at (i*64+j) — check if `count` consecutive bits are free
            size_t needed = count;
            size_t wi = i;
            int    bi  = j;
            int    ok  = 1;

            while (needed > 0) {
                if (wi >= bitmap_size) { ok = 0; break; }
                if (bitmap[wi] & (1ULL << bi)) { ok = 0; break; }
                needed--;
                bi++;
                if (bi >= 64) { bi = 0; wi++; }
            }

            if (ok) {
                u64 start_page = i * 64 + (size_t)j;
                for (size_t k = 0; k < count; k++) {
                    size_t p = start_page + k;
                    bitmap[p / 64] |= (1ULL << (p % 64));
                }
                spin_unlock_irqrestore(&pmm_lock, flags);
                return start_page * PAGE_SIZE;
            }
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;  // No contiguous range found
}

void pmm_free_page(phys_addr_t addr) {
    if (addr == 0) return;
    if (proc_img_owns_frame(addr))
        serial_printf("[PMMFREE-IMG] pa=%lx ra=%lx\n",
                      (unsigned long)addr,
                      (unsigned long)__builtin_return_address(0));
    size_t page_num = addr / PAGE_SIZE;
    size_t word_idx = page_num / 64;
    int bit_idx = page_num % 64;
    if (word_idx < bitmap_size) {
        u64 flags = spin_lock_irqsave(&pmm_lock);
        if (!(carve_start_page && page_num >= carve_start_page))
            bitmap[word_idx] &= ~(1ULL << bit_idx);
        spin_unlock_irqrestore(&pmm_lock, flags);
    }
}

void pmm_free_pages(phys_addr_t addr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_free_page(addr + i * PAGE_SIZE);
    }
}

void pmm_mark_used(phys_addr_t addr, u64 size) {
    u64 start_page = PAGE_ALIGN(addr) / PAGE_SIZE;
    u64 end_page   = PAGE_ROUND_UP(addr + size) / PAGE_SIZE;
    for (u64 p = start_page; p < end_page && p < total_pages; p++) {
        bitmap[p / 64] |= (1ULL << (p % 64));
    }
}

u64 pmm_get_total_memory(void) {
    return total_memory;
}

// ============================================================
// Protected carve zone (top of RAM): page tables + refcounts live
// here. The GENERAL allocator can never hand these frames out, so
// kernel tables and user data can never share a physical frame —
// the identity-map aliasing collision is structurally impossible.
// ============================================================

void pmm_carve_init(u64 bytes) {
    if (carve_ptr) return;
    if (bytes < 2 * 1024 * 1024) bytes = 2 * 1024 * 1024;
    /* Place the carve zone at a FIXED mid-RAM address (128 MB) rather
     * than top-of-RAM: top-of-RAM frames were implicated in the
     * #exec-big corruption. Well within identity map, above heap. */
    u64 npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    carve_lo   = 0x8000000ULL;               /* 128 MB               */
    carve_start_page = carve_lo / PAGE_SIZE;
    carve_ptr  = carve_lo;
    /* reserve in the bitmap so general allocator skips it */
    pmm_mark_used(carve_lo, npages * PAGE_SIZE);
}

phys_addr_t pmm_carve_pages(u64 count) {
    if (!carve_ptr) pmm_carve_init(2 * 1024 * 1024);
    if (!carve_ptr) return 0;
    u64 fl = spin_lock_irqsave(&pmm_lock);
    if (carve_ptr + count * PAGE_SIZE > total_memory) {
        spin_unlock_irqrestore(&pmm_lock, fl);
        return 0;
    }
    phys_addr_t pa = carve_ptr;
    carve_ptr += count * PAGE_SIZE;
    spin_unlock_irqrestore(&pmm_lock, fl);
    return pa;
}

u64 pmm_get_available_memory(void) {
    u64 free_pages = 0;
    for (size_t i = 0; i < bitmap_size; i++) {
        u64 word = ~bitmap[i];
        while (word) {
            free_pages += word & 1;
            word >>= 1;
        }
    }
    return free_pages * PAGE_SIZE;
}

/* TEMP DEBUG(#exec-big): raw bitmap access for fault diagnostics */
u64* pmm_debug_bitmap(void) { return bitmap; }
size_t pmm_debug_bitmap_words(void) { return bitmap_size; }

// ============================================================
// Physical page refcounts (COW sharing)
// ============================================================
// Lazily allocated on first use (post-heap). refcount 0 means "not
// tracked" and is treated as 1 — only COW-shared pages get counted.

static u32* page_refs = NULL;
static u64  page_ref_count_cap = 0;

static bool refs_ensure(void) {
    if (page_refs) return true;
    pmm_refcounts_init();
    return page_refs != NULL;
}

/* Called early from kernel_init: take the refcount table from the
 * protected carve zone (never from the general allocator). */
void pmm_refcounts_init(void) {
    if (page_refs) return;
    page_ref_count_cap = total_pages + 16;
    u64 npages = (page_ref_count_cap * sizeof(u32) + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t pp = pmm_carve_pages(npages + 1);   /* +1: pool init inside */
    if (!pp) { page_ref_count_cap = 0; return; }
    page_refs = (u32*)PHYS_TO_VIRT(pp);
    kmemset(page_refs, 0, page_ref_count_cap * sizeof(u32));
    serial_printf("[PMM] refcount table carved at %lx (%lu pages)\n",
                  (unsigned long)pp, (unsigned long)npages);
}

static inline bool ref_idx_ok(phys_addr_t addr, u64* idx_out) {
    if (!refs_ensure()) return false;     /* lazy-alloc the table */
    u64 idx = addr / PAGE_SIZE;
    if (idx >= page_ref_count_cap) return false;
    *idx_out = idx;
    return true;
}

void pmm_ref(phys_addr_t addr) {
    u64 i;
    addr &= ~(phys_addr_t)(PAGE_SIZE - 1);
    if (!ref_idx_ok(addr, &i)) return;
    page_refs[i]++;
}

/* Mark an UNTRACKED page as shared by exactly two mappings (fork).
 * Untracked pages implicitly have one user; sharing adds the child. */
void pmm_mark_shared(phys_addr_t addr) {
    u64 i;
    addr &= ~(phys_addr_t)(PAGE_SIZE - 1);
    if (!ref_idx_ok(addr, &i)) return;
    page_refs[i] = page_refs[i] ? page_refs[i] + 1 : 2;
}

void pmm_unref(phys_addr_t addr) {
    u64 i;
    addr &= ~(phys_addr_t)(PAGE_SIZE - 1);
    if (!ref_idx_ok(addr, &i)) return;
    if (page_refs[i] == 0) return;          /* untracked: never free */
    if (--page_refs[i] == 0) {
        page_refs[i] = 0;                   /* stays untracked after free */
        serial_printf("[UNREF-FREE] pa=%lx\n", (unsigned long)addr);
        pmm_free_page(addr);
    }
}

u32 pmm_refs(phys_addr_t addr) {
    u64 i;
    addr &= ~(phys_addr_t)(PAGE_SIZE - 1);
    if (!ref_idx_ok(addr, &i)) return 1;
    u32 r = page_refs[i];
    return r ? r : 1;
}

/* True when the page carries an explicit refcount entry (COW-shared).
 * Untracked pages are EXCLUSIVELY owned by whoever mapped them, so a
 * destroying address space may free them outright (#reap reclaim).
 * Shared pages must go through pmm_unref() so they survive while any
 * other address space still maps them. */
bool pmm_ref_tracked(phys_addr_t addr) {
    u64 i;
    addr &= ~(phys_addr_t)(PAGE_SIZE - 1);
    if (!ref_idx_ok(addr, &i)) return false;
    return page_refs[i] != 0;
}

// ============================================================
// Virtual Memory Manager (VMM)
// ============================================================

static u64* kernel_pml4 = NULL;
static u64  kernel_pml4_phys = 0;

/* Address space vmm_map/vmm_unmap/vmm_get_phys operate on. Defaults to
 * the kernel PML4; vmm_switch_pml4()/vmm_set_active() retarget it so
 * syscall paths and the demand pager naturally hit the current process
 * address space. */
static u64* active_pml4 = NULL;

// ============================================================
// PT pool: page tables come EXCLUSIVELY from a boot-time reserved
// range. In the identity-map model a user page and a page table are
// both reachable at their PA as a VA; if the allocator ever hands a
// user page out again as a table (or vice versa) the two views
// corrupt each other — this exact collision produced the fork-child
// #PF. Reserving table frames makes it structurally impossible.
// ============================================================
#define PT_POOL_USABLE 128
/* +2 sentinel pages: the pool's edge pages are NEVER handed out; they
 * stay filled with PT_CANARY magic for the whole uptime. Any stray
 * write into the pool region (wrong DMA address, identity-alias
 * store, linear overflow) hits a sentinel and gets logged on the
 * next table allocation. (#ptcanary tripwire) */
#define PT_POOL_PAGES  (PT_POOL_USABLE + 2)
#define PT_CANARY      0x505443414E415259ULL   /* "PTCANARY" */

static spinlock_t pt_pool_lock = {0};
static phys_addr_t pt_pool[PT_POOL_USABLE];
static int          pt_pool_top  = 0;
static phys_addr_t  pt_pool_base = 0;     /* sentinel_lo            */
static phys_addr_t  pt_pool_hi   = 0;     /* sentinel_hi (last page)*/
static bool         pt_pool_ready = false;

static void pt_canary_poison(phys_addr_t pa) {
    volatile u64* p = (volatile u64*)PHYS_TO_VIRT(pa);
    for (int k = 0; k < 16; k++)        p[k]      = PT_CANARY;
    for (int k = 496; k < 512; k++)     p[k]      = PT_CANARY;
}

/* Returns true if both sentinel edge pages still hold the magic. */
static bool pt_canary_check_locked(const char* site) {
    if (!pt_pool_base) return true;
    phys_addr_t edges[2] = { pt_pool_base, pt_pool_hi };
    for (int i = 0; i < 2; i++) {
        volatile u64* p = (volatile u64*)PHYS_TO_VIRT(edges[i]);
        for (int k = 0; k < 16; k++) {
            if (p[k] != PT_CANARY) {
                serial_printf("[PTCANARY] TRIPPED (%s): page %lx qword[%d] "
                              "= %lx — pool edges corrupted!\n",
                              site, (unsigned long)edges[i], k,
                              (unsigned long)p[k]);
                return false;
            }
        }
        for (int k = 496; k < 512; k++) {
            if (p[k] != PT_CANARY) {
                serial_printf("[PTCANARY] TRIPPED (%s): page %lx qword[%d] "
                              "= %lx — pool edges corrupted!\n",
                              site, (unsigned long)edges[i], k,
                              (unsigned long)p[k]);
                return false;
            }
        }
    }
    return true;
}

static void pt_pool_init_locked(void) {
    if (pt_pool_ready) return;
    phys_addr_t base = pmm_carve_pages(PT_POOL_PAGES);
    if (base) {
        pt_pool_base = base;
        pt_pool_hi   = base + (PT_POOL_PAGES - 1) * PAGE_SIZE;
        pt_canary_poison(pt_pool_base);
        pt_canary_poison(pt_pool_hi);
        /* usable pages live between the sentinels */
        for (int k = PT_POOL_USABLE - 1; k >= 0; k--)
            pt_pool[pt_pool_top++] =
                pt_pool_base + (1 + k) * PAGE_SIZE;
        serial_printf("[PTPOOL] %d pages at %lx..%lx (canary sentinels "
                      "at edges)\n", PT_POOL_USABLE,
                      (unsigned long)(pt_pool_base + PAGE_SIZE),
                      (unsigned long)(pt_pool_hi - PAGE_SIZE));
    }
    pt_pool_ready = true;
}

static bool pt_pool_take(phys_addr_t* out) {
    u64 fl = spin_lock_irqsave(&pt_pool_lock);
    pt_pool_init_locked();
    pt_canary_check_locked("take");
    bool ok = pt_pool_top > 0;
    if (ok) *out = pt_pool[--pt_pool_top];
    spin_unlock_irqrestore(&pt_pool_lock, fl);
    return ok;
}

static bool pt_pool_give(phys_addr_t pa) {
    if (pt_pool_base == 0 || pa < pt_pool_base + PAGE_SIZE ||
        pa >= pt_pool_base + PT_POOL_PAGES * PAGE_SIZE)
        return false;                     /* not one of ours */
    u64 fl = spin_lock_irqsave(&pt_pool_lock);
    pt_canary_check_locked("give");
    bool ok = pt_pool_top < PT_POOL_USABLE;
    if (ok) pt_pool[pt_pool_top++] = pa;
    spin_unlock_irqrestore(&pt_pool_lock, fl);
    return ok;
}

static void vmm_free_page_table(phys_addr_t pa) {
    if (pt_pool_give(pa)) {
        /* NOTE: do NOT scrub here — the page may still be mapped in
         * another address space that hasn't switched CR3 yet. The
         * next pt_pool_take will kmemset it before use. */
        return;
    }
    pmm_free_page(pa);
}

static u64* vmm_alloc_page_table(void) {
    phys_addr_t page = 0;
    if (!pt_pool_take(&page)) {
        vga_print("[VMM] PT pool exhausted, falling back to PMM\n");
        page = pmm_alloc_page();
        if (page == 0) return NULL;
    }
    u64* pt = (u64*)PHYS_TO_VIRT(page);
    kmemset(pt, 0, PAGE_SIZE);
    return pt;
}

void vmm_set_active(phys_addr_t pml4_phys) {
    active_pml4 = pml4_phys ? (u64*)PHYS_TO_VIRT(pml4_phys) : kernel_pml4;
}

phys_addr_t vmm_get_active_pml4(void) {
    if (!active_pml4) return kernel_pml4_phys;
    return VIRT_TO_PHYS((u64)active_pml4);
}

void vmm_init(void) {
    // Enable EFER.NXE *before* any page table can carry the NX bit
    // (bit 63). Without NXE, a PTE with NX set is treated as having
    // reserved bits set and EVERY access through it raises #PF(RSV) —
    // this silently broke all non-executable user pages.
    {
        u64 efer = rdmsr(0xC0000080);           /* MSR_EFER */
        efer |= (1ULL << 11);                   /* NXE      */
        wrmsr(0xC0000080, efer);
    }

    // Allocate PML4
    u64* pml4 = vmm_alloc_page_table();
    if (!pml4) {
        vga_print("[VMM] ERROR: Failed to allocate PML4\n");
        return;
    }
    kernel_pml4 = pml4;
    kernel_pml4_phys = VIRT_TO_PHYS((u64)pml4);

    // Identity-map the first 4 GB using 2MiB pages — EXCEPT the 1-2GB
    // range, which becomes the KERNEL WINDOW (#kernel-halffix):
    //   PDPT[0] = identity 0-1GB   (VGA 0xb8000, MBI, AP trampoline)
    //   PDPT[1] = WINDOW: VA 1GB + i*2MB -> PA i*2MB (RAM + kernel image)
    //   PDPT[2,3] = identity 2-4GB (PCI/LAPIC MMIO)
    // The kernel is LINKED at VA = PA + 1GB (linker64.ld): user images
    // are pre-linked at 0x400000 and can no longer shadow kernel
    // .text/.data/.bss in a child CR3 (the #kernel-bss-shadow spawn
    // regression after the mbedtls/ext4 growth past 4MB identity).
    // Children clone this pml4 (vmm_fork_pml4), so the window is present
    // in every address space and the kernel stays executable/reachable
    // from ring 0 under any CR3.
    // Full 0-4GB coverage kept for devices: subsystems (ACPI/SMP) read
    // tables anywhere below 4GB through the identity alias.
    //
    // Layout: PDPT[0..3] -> 4 PDs, each 512 x 2MiB entries.
    // Uses 16 KB of page-table memory; no 1GiB-page CPU support needed,
    // and vmm_map()/vmm_split_huge_page() logic stays valid as-is.
    u64* pdpt = vmm_alloc_page_table();
    if (!pdpt) {
        vga_print("[VMM] ERROR: Failed to allocate PDPT\n");
        return;
    }
    kernel_pml4[0] = VIRT_TO_PHYS((u64)pdpt) | VMM_PRESENT | VMM_WRITE;

    for (u64 p = 0; p < 4; p++) {
        u64* pd = vmm_alloc_page_table();
        if (!pd) {
            vga_print("[VMM] ERROR: Failed to allocate PD\n");
            return;
        }

        if (p == 1) {
            /* THE WINDOW (#kernel-halffix): VA 1GB + i*2MB -> PA i*2MB.
             * Covers RAM 0-1GB (QEMU -m 512M fits) plus the kernel
             * image linked at VA = PA + 1GB. */
            for (u64 i = 0; i < 512; i++) {
                pd[i] = (i * 2 * 1024 * 1024)
                        | VMM_PRESENT | VMM_WRITE | VMM_PS;
            }
        } else {
            for (u64 i = 0; i < 512; i++) {
                pd[i] = ((p * 512 + i) * 2 * 1024 * 1024)
                        | VMM_PRESENT | VMM_WRITE | VMM_PS;
            }
        }

        pdpt[p] = VIRT_TO_PHYS((u64)pd) | VMM_PRESENT | VMM_WRITE;
    }

    // Also self-map the PML4: map PML4 entry 511 -> PML4 itself
    // Entry 511 maps virtual 0xFFFF800000000000, safely above user space
    // and won't conflict with the window mapping at entry 0's PDPT[1].
    kernel_pml4[511] = kernel_pml4_phys | VMM_PRESENT | VMM_WRITE;

    vga_print("[VMM] PML4 at phys 0x");
    vga_print_hex(kernel_pml4_phys);
    vga_print(", identity 0-4GB + kernel window (VA=PA+1GB)\n");

    // We do NOT switch to our PML4 by default.
    // GRUB's page tables work fine for the identity-mapped kernel.
    // vmm_switch_pml4() can be called when needed.
}

void vmm_map(virt_addr_t virt, phys_addr_t phys, u64 flags) {
    if (!kernel_pml4) return;
    u64* pml4 = active_pml4 ? active_pml4 : kernel_pml4;


    u64 pml4_idx = (virt >> 39) & 0x1FF;
    u64 pdpt_idx = (virt >> 30) & 0x1FF;
    u64 pd_idx   = (virt >> 21) & 0x1FF;
    u64 pt_idx   = (virt >> 12) & 0x1FF;

    // User pages need VMM_USER on EVERY level of the walk: a supervisor-
    // only intermediate entry makes the whole translation inaccessible
    // from ring 3 (fetch/read/write -> #PF with U bit set).
    bool user = (flags & VMM_USER) != 0;

    // PML4 -> PDPT
    u64 pml4e = pml4[pml4_idx];
    u64* pdpt;
    if (!(pml4e & VMM_PRESENT)) {
        pdpt = vmm_alloc_page_table();
        if (!pdpt) return;
        pml4[pml4_idx] = VIRT_TO_PHYS((u64)pdpt)
                                | VMM_PRESENT | VMM_WRITE
                                | (user ? VMM_USER : 0);
    } else {
        pdpt = (u64*)PHYS_TO_VIRT(pml4e & ~0xFFFULL);
        if (user && !(pml4e & VMM_USER)) {
            pml4[pml4_idx] |= VMM_USER;
        }
    }

    // PDPT -> PD
    u64 pdpte = pdpt[pdpt_idx];
    u64* pd;
    if (!(pdpte & VMM_PRESENT)) {
        pd = vmm_alloc_page_table();
        if (!pd) return;
        pdpt[pdpt_idx] = VIRT_TO_PHYS((u64)pd)
                         | VMM_PRESENT | VMM_WRITE
                         | (user ? VMM_USER : 0);
    } else {
        pd = (u64*)PHYS_TO_VIRT(pdpte & ~0xFFFULL);
        if (user && !(pdpte & VMM_USER)) {
            pdpt[pdpt_idx] |= VMM_USER;
        }
    }

    // PD -> PT
    u64 pde = pd[pd_idx];
    u64* pt;
    if (!(pde & VMM_PRESENT)) {
        pt = vmm_alloc_page_table();
        if (!pt) return;
        pd[pd_idx] = VIRT_TO_PHYS((u64)pt)
                     | VMM_PRESENT | VMM_WRITE
                     | (user ? VMM_USER : 0);
    } else if (pde & VMM_PS) {
        // PD entry is a 2MB huge page — auto-split it into 4KB pages
        serial_printf("[VMMSPLIT] splitting pde=%lx\n", pde);
        u64 aligned_virt = virt & ~0x1FFFFF;  // align to 2MB boundary
        if (!vmm_split_huge_page(aligned_virt)) {
            serial_printf("[VMMSPLIT] FAILED at %lx\n", aligned_virt);
            vga_print("[VMM] Failed to split 2MB page at 0x");
            vga_print_hex(aligned_virt);
            vga_print("\n");
            return;
        }
        serial_printf("[VMMSPLIT] ok pd[%lu]=%lx\n",
                      (unsigned long)pd_idx, pd[pd_idx]);
        // After split, PD entry now points to a PT
        pde = pd[pd_idx];
        pt = (u64*)PHYS_TO_VIRT(pde & ~0xFFFULL);
    } else {
        pt = (u64*)PHYS_TO_VIRT(pde & ~0xFFFULL);
    }

    // A pre-existing PT may have been created as kernel-only (e.g. by a
    // split of an identity huge page). Promote it for user access too.
    if (user && !(pde & VMM_USER)) {
        pd[pd_idx] |= VMM_USER;
    }

    // PT -> 4KB page
    pt[pt_idx] = (phys & ~0xFFFULL) | flags;
    invlpg(virt);
}

void vmm_map_range(virt_addr_t virt, phys_addr_t phys, u64 size, u64 flags) {
    u64 v = PAGE_ALIGN(virt);
    u64 end = PAGE_ROUND_UP(virt + size);
    // Keep exact correspondence between virtual and physical even when
    // `virt` is unaligned: page at v maps phys + (v - virt). The old
    // code advanced both independently from their aligned starts, which
    // skewed the mapping when virt % PAGE_SIZE != phys % PAGE_SIZE.
    while (v < end) {
        vmm_map(v, phys + (v - virt), flags);
        v += PAGE_SIZE;
    }
}

void vmm_unmap(virt_addr_t virt) {
    if (!kernel_pml4) return;
    u64* pml4 = active_pml4 ? active_pml4 : kernel_pml4;

    u64 pml4_idx = (virt >> 39) & 0x1FF;
    u64 pdpt_idx = (virt >> 30) & 0x1FF;
    u64 pd_idx   = (virt >> 21) & 0x1FF;
    u64 pt_idx   = (virt >> 12) & 0x1FF;

    u64 pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_PRESENT)) return;
    u64* pdpt = (u64*)PHYS_TO_VIRT(pml4e & ~0xFFFULL);

    u64 pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_PRESENT)) return;
    u64* pd = (u64*)PHYS_TO_VIRT(pdpte & ~0xFFFULL);

    u64 pde = pd[pd_idx];
    if (!(pde & VMM_PRESENT)) return;
    if (pde & VMM_PS) return;  // 2MB page, can't unmap 4KB
    u64* pt = (u64*)PHYS_TO_VIRT(pde & ~0xFFFULL);

    pt[pt_idx] = 0;
    invlpg(virt);
}

phys_addr_t vmm_get_phys(virt_addr_t virt) {
    if (!kernel_pml4) return VIRT_TO_PHYS(virt);
    u64* pml4 = active_pml4 ? active_pml4 : kernel_pml4;

    u64 pml4_idx = (virt >> 39) & 0x1FF;
    u64 pdpt_idx = (virt >> 30) & 0x1FF;
    u64 pd_idx   = (virt >> 21) & 0x1FF;
    u64 pt_idx   = (virt >> 12) & 0x1FF;

    u64 pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_PRESENT)) return 0;
    u64* pdpt = (u64*)PHYS_TO_VIRT(pml4e & ~0xFFFULL);

    u64 pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_PRESENT)) return 0;

    // Check for 1GB huge page in PDPT
    if (pdpte & VMM_PS) {
        // 1GB page: physical address in bits [51:30] of PDPTE
        return (pdpte & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFF);
    }

    u64* pd = (u64*)PHYS_TO_VIRT(pdpte & ~0xFFFULL);
    u64 pde = pd[pd_idx];
    if (!(pde & VMM_PRESENT)) return 0;

    // Check for 2MB page in PD
    if (pde & VMM_PS) {
        return (pde & 0x000FFFFFE00000ULL) | (virt & 0x1FFFFF);
    }

    u64* pt = (u64*)PHYS_TO_VIRT(pde & ~0xFFFULL);
    u64 pte = pt[pt_idx];
    if (!(pte & VMM_PRESENT)) return 0;

    return (pte & ~0xFFFULL) | (virt & 0xFFF);
}

void vmm_switch_pml4(phys_addr_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
    vmm_set_active(pml4_phys);
}

/* ── Reliable physical-read window (#pfdump-alias fix) ──────────
 * Identity reads (PHYS_TO_VIRT(pa) == pa) are only TRUE physical
 * reads while the KERNEL pml4 is active. A user space that split a
 * low identity window (seg VAs 0x400000..) re-points those VAs at
 * frame PTEs, so "identity" reads become USER-VA reads — the
 * forensic artifact that faked the 0x683000 frame-dup evidence.
 * begin() switches to the kernel pml4; end() restores. */
u64 vmm_pa_read_begin(void) {
    u64 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    if (kernel_pml4_phys && cr3 != kernel_pml4_phys) {
        __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_pml4_phys) : "memory");
        return cr3;
    }
    return 0;
}

void vmm_pa_read_end(u64 saved) {
    if (saved)
        __asm__ volatile ("mov %0, %%cr3" :: "r"(saved) : "memory");
}

phys_addr_t vmm_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}

// ============================================================
// Address spaces & COW (process support)
// ============================================================

/* Walk to the leaf PTE of `virt` in `pml4` (physical addr).
 * Returns pointer to the PTE (access via PHYS_TO_VIRT) or NULL.
 * Huge pages are rejected: processes only ever use 4KB leaves.
 * When create_pt is true, missing intermediate tables are allocated
 * (flags applied to new intermediate entries). */
u64* vmm_walk_leaf(phys_addr_t pml4_phys, u64 virt,
                          bool create_pt, bool user) {
    u64* pml4 = (u64*)PHYS_TO_VIRT(pml4_phys);
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;
    u64 i1 = (virt >> 12) & 0x1FF;

    u64 e = pml4[i4];
    if (!(e & VMM_PRESENT)) {
        if (!create_pt) return NULL;
        u64* t = vmm_alloc_page_table();
        if (!t) return NULL;
        e = VIRT_TO_PHYS((u64)t) | VMM_PRESENT | VMM_WRITE
            | (user ? VMM_USER : 0);
        pml4[i4] = e;
    } else if (user && !(e & VMM_USER)) {
        e |= VMM_USER;
        pml4[i4] = e;
    }
    u64* pdpt = (u64*)PHYS_TO_VIRT((e & ~0xFFFULL) & ~(1ULL << 63));

    e = pdpt[i3];
    if (!(e & VMM_PRESENT)) {
        if (!create_pt) return NULL;
        u64* t = vmm_alloc_page_table();
        if (!t) return NULL;
        e = VIRT_TO_PHYS((u64)t) | VMM_PRESENT | VMM_WRITE
            | (user ? VMM_USER : 0);
        pdpt[i3] = e;
    } else {
        if (user && !(e & VMM_USER)) { e |= VMM_USER; pdpt[i3] = e; }
        if (e & VMM_PS) return NULL;
    }
    u64* pd = (u64*)PHYS_TO_VIRT((e & ~0xFFFULL) & ~(1ULL << 63));

    e = pd[i2];
    if (!(e & VMM_PRESENT)) {
        if (!create_pt) return NULL;
        u64* t = vmm_alloc_page_table();
        if (!t) return NULL;
        e = VIRT_TO_PHYS((u64)t) | VMM_PRESENT | VMM_WRITE
            | (user ? VMM_USER : 0);
        pd[i2] = e;
    } else if (e & VMM_PS) {
        /* FIX(#reap/user-alias) ORDER MATTERS: the PS check MUST come
         * before the user-flag promotion below. The old order
         * (promote first, split after) stamped USER onto a 2MB
         * IDENTITY PDE; the subsequent split then inherited it via
         * huge_flags=(e&0xFFF), materializing 512 ring-3-accessible
         * alias leaves over raw physical memory — kernel heap and
         * reserved frames included. A later teardown then freed them
         * as "user data" and the bitmap lost live heap bits. Splits
         * therefore ALWAYS keep the original supervisor flags of the
         * window; the caller's explicit leaf write adds USER to
         * exactly one leaf right afterwards. */
        /* Huge 2MB page: INLINE split in THIS table hierarchy.
         * Cannot use vmm_split_huge_page (operates on active_pml4);
         * we need to split in the CHILD's tables here. */
        if (!create_pt) return NULL;
        u64 huge_phys = e & ~0x1FFFFFULL;
        u64 huge_flags = (e & 0xFFF) & ~VMM_PS;
        u64 huge_nx   = e & VMM_NX;
        u64* newpt = vmm_alloc_page_table();
        if (!newpt) return NULL;
        for (int k = 0; k < 512; k++)
            newpt[k] = (huge_phys + k * PAGE_SIZE)
                       | huge_flags | huge_nx;
        pd[i2] = VIRT_TO_PHYS((u64)newpt)
                 | VMM_PRESENT | VMM_WRITE
                 | (user ? VMM_USER : 0) | huge_nx;
        /* PD-entry carries USER so RING 3 can reach its own mapped
         * leaves inside this window; the alias leaves beneath remain
         * supervisor — ring 3 touches ONLY the leaves it was
         * explicitly granted. */
        serial_printf("[WALKSPLIT] va=%lx huge=%lx newpt=%lx\n",
                      (unsigned long)(virt & ~((1ULL << 21) - 1)),
                      (unsigned long)huge_phys,
                      (unsigned long)VIRT_TO_PHYS((u64)newpt));
    } else {
        /* Plain PT-pointer entry: only NOW is it safe to promote the
         * entry itself for user access (never done for huge windows). */
        if (user && !(e & VMM_USER)) { e |= VMM_USER; pd[i2] = e; }
    }
    e = pd[i2];   /* re-read: may have been split/promoted above */
    u64* pt = (u64*)PHYS_TO_VIRT((e & ~0xFFFULL) & ~(1ULL << 63));

    return &pt[i1];
}

bool vmm_handle_cow(virt_addr_t virt) {
    phys_addr_t active = vmm_get_active_pml4();
    if (!active) return false;

    u64* pte = vmm_walk_leaf(active, virt, false, false);
    if (!pte) return false;

    u64 old = *pte;
    if (!(old & VMM_PRESENT) || !(old & VMM_COW)) return false;

    phys_addr_t oldp = pte_pa(old);

    /* Keep every flag bit (PRESENT, USER, NX, ...) except COW; force
     * WRITE back on for the private copy. */
    u64 low = old & 0xFFF;
    low &= ~VMM_COW;
    low |= VMM_WRITE | VMM_PRESENT;
    u64 nx = old & VMM_NX;

    if (pmm_refs(oldp) <= 1) {
        /* Sole owner: just take the page private again */
        *pte = oldp | low | nx;
    } else {
        phys_addr_t np = pmm_alloc_page();
        if (!np) return false;
        /* #ident-alias GUARD: copy frames via a kernel-CR3 window —
         * identity VAs of low frames can alias user seg pages of the
         * faulting space (its windows are split by definition when a
         * user COW fault can happen). */
        {
            extern u64 vmm_pa_read_begin(void);
            extern void vmm_pa_read_end(u64);
            u64 s3 = vmm_pa_read_begin();
            kmemcpy((void*)PHYS_TO_VIRT(np), (void*)PHYS_TO_VIRT(oldp), PAGE_SIZE);
            vmm_pa_read_end(s3);
        }
        pmm_unref(oldp);                  /* we no longer share it   */
        *pte = np | low | nx;
    }
    invlpg(virt & ~(PAGE_SIZE - 1));
    return true;
}

/* Recursive worker for fork/teardown. level: 3=PML4 .. 0=PT.
 * For fork: copies table structure; user leaf pages get COW in both. */
static bool vmm_fork_level(u64 src_e, int level, u64 virt_base, bool cow_user,
                           u64* out_e);

static bool vmm_fork_table(u64 table_phys, int level, u64 virt_base,
                           bool cow_user, u64* out_table_phys) {
    u64* src = (u64*)PHYS_TO_VIRT(table_phys);
    u64* dst = vmm_alloc_page_table();
    if (!dst) return false;

    u64 stride = (u64)1 << (12 + 9 * level);
    for (int i = 0; i < 512; i++) {
        u64 e = src[i];
        if (!(e & VMM_PRESENT)) continue;

        /* FIX(#reap/self-map): the PML4 self-map entry is REBUILT by
         * vmm_fork_pml4(), not cloned. Cloning it recursed into an ENTIRE
         * alias subtree (~6 extra pool pages per fork), double-applied
         * COW refcounts on every user page (mark_shared seen twice), and
         * made vmm_destroy_pml4() visit every frame through TWO views —
         * the accidental balance behind the old #reap corruptions. */
        if (level == 3 && i == 511) continue;

        u64 vaddr = virt_base + (u64)i * stride;
        if (level == 0) {
            u64 pte = e;
            if (cow_user && (pte & VMM_USER)) {
                phys_addr_t pa = pte_pa(pte);
                pmm_mark_shared(pa);      /* parent + child share it  */
                /* Strip WRITE, add COW — keep PRESENT/USER/NX etc. */
                u64 low = (pte & 0xFFF) & ~VMM_WRITE;
                low |= VMM_COW;
                u64 nx = pte & VMM_NX;
                src[i] = pa | low | nx;   /* parent becomes RO+COW too */
                invlpg(vaddr);            /* parent may have TLB entry */
                dst[i] = src[i];
            } else {
                dst[i] = pte;
            }
        } else {
            if (e & VMM_PS) { dst[i] = e; continue; }   /* huge: share as-is */
            u64 child_child = 0;
            if (!vmm_fork_level(e, level - 1, vaddr, cow_user, &child_child))
                return false;
            dst[i] = child_child;
        }
    }
    *out_table_phys = VIRT_TO_PHYS((u64)dst);
    return true;
}

static bool vmm_fork_level(u64 src_e, int level, u64 virt_base, bool cow_user,
                           u64* out_e) {
    u64 child = 0;
    if (!vmm_fork_table(src_e & ~0xFFFULL, level, virt_base, cow_user, &child))
        return false;
    /* Keep ALL low flag bits including VMM_PS: kernel identity uses
     * 2MB huge pages at PD level and must stay huge in the child. */
    *out_e = child | (src_e & 0xFFF);
    return true;
}

phys_addr_t vmm_fork_pml4(phys_addr_t src, bool cow_user) {
    u64 child = 0;
    if (!vmm_fork_table(src, 3, 0, cow_user, &child)) return 0;
    /* Give the child a TRUE self-map like vmm_init does for the kernel:
     * [511] -> the child PML4 itself. Previously [511] pointed at a junk
     * one-off clone whose lifetime nobody owned. */
    ((u64*)PHYS_TO_VIRT(child))[511] =
        child | VMM_PRESENT | VMM_WRITE;
    return child;
}

static void vmm_destroy_level(u64 table_phys, int level, u64 virt_base) {
    u64* t = (u64*)PHYS_TO_VIRT(table_phys);
    u64 stride = (u64)1 << (12 + 9 * level);

    for (int i = 0; i < 512; i++) {
        u64 e = t[i];
        if (!(e & VMM_PRESENT)) continue;

        /* FIX(#reap/self-map): NEVER descend into an entry that points
         * back at THIS table. The [511] self-map is legitimate, but it
         * must not be walked as data: doing so re-visits every frame of
         * this very tree through an alias view — double unref/free of
         * live structures (the historical #reap corruption). */
        if (pte_pa(e) == table_phys) {
            serial_printf("[DESTROY-SM] L%d idx=%d skipped\n", level, i);
            continue;
        }

        u64 vaddr = virt_base + (u64)i * stride;

        if (level == 0) {
            if (e & VMM_USER) {
                phys_addr_t pa = pte_pa(e);
                if (pmm_ref_tracked(pa)) {
                    pmm_unref(pa);      /* shared: frees on last owner */
                } else {
                    /* Exclusive-untracked page: this space was its only
                     * user (no COW marking ever happened). Free outright,
                     * otherwise teardown leaks every spawned image. */
                    pmm_free_page(pa);
                }
            }
        } else {
            if (e & VMM_PS) continue;     /* shared huge page: skip */
            /* FIX(#reap/double-free): the recursive call frees the child
             * table ITSELF at its end (vmm_free_page_table(table_phys)).
             * Freeing it AGAIN here pushed every page-table page into
             * the PT pool TWICE. Duplicate pool entries handed the same
             * physical frame to two different later tables; the second
             * allocator kmemset()-zeroed the frame while the first
             * role still relied on it -> freshly built address spaces
             * switched to zeroed/garbage PML4s (#PF on own RIP / #DF).
             * This was the true engine behind the historical #reap
             * corruption family — never re-free a node here. */
            vmm_destroy_level(e & ~0xFFFULL, level - 1, vaddr);
        }
    }
    vmm_free_page_table(table_phys);
}

void vmm_destroy_pml4(phys_addr_t pml4_phys) {
    if (!pml4_phys || pml4_phys == kernel_pml4_phys) return;

    /* SELF-CHECK (user request "самопроверка PML4[511]"): every space
     * created since the self-map fix carries a TRUE self-map ([511] ->
     * itself). Anything else at [511] is foreign garbage from before
     * the fix or corruption — descending into it would free LIVE pages
     * owned by other spaces. Detect, log loudly, neutralize, proceed
     * with the strictly private part of the tree only. */
    u64 e511 = ((u64*)PHYS_TO_VIRT(pml4_phys))[511];
    bool sm_ok = (e511 & VMM_PRESENT) &&
                 ((e511 & ~(0xFFFULL | VMM_NX)) == pml4_phys);
    serial_printf("[DESTROY] pml4=%lx [511]=%lx %s\n",
                  (unsigned long)pml4_phys, (unsigned long)pte_pa(e511),
                  sm_ok ? "self-ok" : "FOREIGN!");
    if ((e511 & VMM_PRESENT) && !sm_ok) {
        /* Foreign target: cut the pointer instead of following it.
         * vmm_destroy_level additionally skips genuine self-references,
         * so after this cut nothing outside OUR tree is reachable. */
        ((u64*)PHYS_TO_VIRT(pml4_phys))[511] = 0;
    }

    vmm_destroy_level(pml4_phys, 3, 0);
}

/* TEMP DEBUG: raw leaf PTE of the ACTIVE space */
u64 vmm_debug_pte(u64 virt) {
    phys_addr_t active = vmm_get_active_pml4();
    if (!active) return 0xDEAD0000;
    u64* pte = vmm_walk_leaf(active, virt, false, false);
    if (!pte) return 0xBAD00000;
    return *pte;
}

/* TEMP DEBUG: leaf PTE for a KERNEL text page in the ACTIVE space */
u64 vmm_debug_ktext_pt_pa = 0;
u64 vmm_debug_ktext_leaf(void) {
    phys_addr_t active = vmm_get_active_pml4();
    if (!active) return 0xDEAD0001;
    u64* pml4 = (u64*)PHYS_TO_VIRT(active);
    u64 e4 = pml4[0];
    if (!(e4 & 1)) return 0xBAD4;
    u64* p3 = (u64*)PHYS_TO_VIRT((e4 & ~0xFFFULL) & ~(1ULL << 63));
    u64 e3 = p3[0];
    if (!(e3 & 1)) return 0xBAD3;
    u64* p2 = (u64*)PHYS_TO_VIRT((e3 & ~0xFFFULL) & ~(1ULL << 63));
    u64 e2 = p2[0];                          /* PD for 0..1GB        */
    if (!(e2 & 1)) return 0xBAD2;
    if (e2 & 0x80) return 0xFEED0000 | (e2 & 0xFF);   /* huge, fine */
    u64* p1 = (u64*)PHYS_TO_VIRT((e2 & ~0xFFFULL) & ~(1ULL << 63));
    vmm_debug_ktext_pt_pa = e2 & ~0xFFFULL;  /* the PT page itself   */
    return p1[(0x10f000 >> 12) & 511];
}

/* TEMP DEBUG: translated read of up to len bytes via ACTIVE space */
u64 vmm_debug_read(u64 virt) {
    phys_addr_t pa = vmm_get_phys(virt & ~7ULL);
    if (!pa) return 0;
    return *(u64*)PHYS_TO_VIRT(pa);
}

// ============================================================
// VMM: Split 2MB huge page into 512 x 4KB pages
// ============================================================

bool vmm_split_huge_page(virt_addr_t virt) {
    if (!kernel_pml4) return false;
    u64* pml4 = active_pml4 ? active_pml4 : kernel_pml4;

    u64 pml4_idx = (virt >> 39) & 0x1FF;
    u64 pdpt_idx = (virt >> 30) & 0x1FF;
    u64 pd_idx   = (virt >> 21) & 0x1FF;

    // Walk to PD
    u64 pml4e = pml4[pml4_idx];
    if (!(pml4e & VMM_PRESENT)) return false;
    u64* pdpt = (u64*)PHYS_TO_VIRT(pml4e & ~0xFFFULL);

    u64 pdpte = pdpt[pdpt_idx];
    if (!(pdpte & VMM_PRESENT)) return false;
    // Don't split 1GB pages (PDPT PS bit)
    if (pdpte & VMM_PS) return false;

    u64* pd = (u64*)PHYS_TO_VIRT(pdpte & ~0xFFFULL);
    u64 pde = pd[pd_idx];

    // Must be a 2MB huge page (PS bit set)
    if (!(pde & VMM_PRESENT)) return false;
    if (!(pde & VMM_PS)) return false;  // already 4KB

    // Extract the 2MB page's physical address
    // For 2MB pages: phys = pde & ~0x1FFFFF (clear bits 20..0)
    u64 huge_phys = pde & ~0x1FFFFFULL;
    u64 flags = (pde & 0xFFF) & ~VMM_PS;  // keep low 12 flag bits, remove PS bit
    // Preserve NX bit (bit 63) from original PDE
    flags |= (pde & VMM_NX);

    // Allocate a new page table for 4KB pages
    u64* pt = vmm_alloc_page_table();
    if (!pt) {
        vga_print("[VMM] split_huge_page: failed to allocate PT\n");
        return false;
    }

    // Fill PT with 4KB entries mapping the same physical memory
    for (u64 i = 0; i < 512; i++) {
        pt[i] = (huge_phys + i * PAGE_SIZE) | flags;
    }

    // Replace the 2MB PD entry with a pointer to the new PT
    // Preserve present/write/user flags from the original PDE
    u64 pd_flags = (pde & (VMM_PRESENT | VMM_WRITE | VMM_USER));
    pd[pd_idx] = VIRT_TO_PHYS((u64)pt) | pd_flags;

    // Flush the TLB for the entire 2MB region
    for (u64 i = 0; i < 512; i++) {
        invlpg(virt + i * PAGE_SIZE);
    }

    return true;
}

// ============================================================
// Kernel Heap — dlmalloc 2.7.2 (Doug Lea, public domain)
// ============================================================
// The old implicit free-list allocator is REPLACED by the vendored
// Doug Lea malloc 2.7.2 (kernel/dlmalloc.c — PRISTINE, zero source
// modifications). All adaptation lives in kernel/dlcompat.c, in the
// dedicated Makefile compile rule for dlmalloc.o, and in the thin
// wrappers below. Public API kmalloc/kzalloc/kfree is preserved;
// krealloc is NEW (the free-list era had none).
//
// WHY: every kernel subsystem that allocates (fs nodes, sockets,
// ring buffers, and the upcoming lwext4 / mbedTLS integrations)
// shares this heap. dlmalloc gives real binning (no O(n) walk per
// alloc), honest coalescing, realloc, and mallinfo statistics.
//
// LOCKING: dlmalloc is compiled WITHOUT USE_MALLOC_LOCK — it has no
// internal serialization. heap_lock below is the SINGLE serialization
// point. dlmalloc itself must only be reached through these wrappers
// (and kernel_sbrk, which dlmalloc calls internally). Kernel audit:
// no allocation path runs in IRQ context, so irqsave is sufficient.
//
// ARENA: fixed identity-mapped region [KERNEL_HEAP_START, +64MB),
// reserved in the PMM bitmap at init (see pmm_init). dlmalloc grows
// inside it strictly via kernel_sbrk(), which enforces the bounds;
// there is no trim (MORECORE_CANNOT_TRIM=1) and no mmap path
// (HAVE_MMAP=0) — every byte comes from the arena.
//
// SEMANTICS CHANGE vs free-list allocator: double free is no longer
// "detected and silently ignored" — it is undefined behavior, exactly
// as on any real malloc. -DDL_DEBUG builds catch it via dlmalloc's
// internal assertions (dl_assert_fail halts with a serial report).

// Names in dlmalloc.h resolve to the dl-prefixed public symbols that
// the vendored kernel/dlmalloc.c actually emits (it is compiled with
// -DUSE_DL_PREFIX in its Makefile rule).
#define USE_DL_PREFIX 1
#include "../include/dlmalloc.h"
/* NOTE: the kit's malloc.h 2.7.2 carries a known upstream typo — its
 * USE_DL_PREFIX branch still declares `mallinfo` instead of
 * `dlmallinfo`. The vendored dlmalloc.o really exports dlmallinfo and
 * dlmalloc_stats (see nm output), so declare them here. */
struct mallinfo dlmallinfo(void);
void dlmalloc_stats(void);
// Guards ALL dlmalloc state (bins, top, and the break below).
static spinlock_t heap_lock = SPINLOCK_INIT;

// Current break inside the fixed arena. Managed ONLY by kernel_sbrk.
static u8* dl_brk = (u8*)(0x40000000ULL + KERNEL_HEAP_START);  /* PHYS_TO_VIRT */

/*
 * MORECORE for dlmalloc (Makefile: -DMORECORE=kernel_sbrk).
 * Strict Unix sbrk(2) semantics:
 *   - MORECORE(0) probe returns the current break;
 *   - positive/negative increments move the break and return the OLD
 *     break (dlmalloc's alignment-correction path issues negative
 *     calls — they must succeed inside the arena);
 *   - any move outside [KERNEL_HEAP_START, +KERNEL_HEAP_SIZE) fails
 *     with (void*)-1 == MORECORE_FAILURE.
 *
 * NOT taken under heap_lock: dlmalloc only calls MORECORE from within
 * dlmalloc/dlfree/dlrealloc, which the wrappers already serialize.
 */
void* kernel_sbrk(long inc) {
    if (inc == 0) return (void*)dl_brk;

    u8* old = dl_brk;
    if (inc > 0) {
        if ((u64)inc > KERNEL_HEAP_SIZE) return (void*)-1;
        u8* nb = dl_brk + inc;
        if (nb > (u8*)(0x40000000ULL + KERNEL_HEAP_START + KERNEL_HEAP_SIZE)) return (void*)-1;
        dl_brk = nb;
    } else {
        u8* nb = dl_brk + inc;             // inc < 0
        if (nb < (u8*)(0x40000000ULL + KERNEL_HEAP_START)) return (void*)-1;
        dl_brk = nb;
    }
    return (void*)old;
}

void heap_init(void) {
    /* FATAL GUARD (#heap-overlap): the heap owns [KERNEL_HEAP_START,
     * +64MB) unconditionally. If the kernel image (which ends at
     * __kernel_end and includes .rodata/.data/.bss) reaches that far,
     * the arena would overlay kernel code/data and the first writes
     * into it would clobber the image — the exact mechanism behind
     * the 0x5b2ff0 "zero-writer" seen with a 4.3MB embedded apk blob.
     * Fail LOUDLY instead of corrupting silently. */
    if ((u64)__kernel_end >= PHYS_TO_VIRT(KERNEL_HEAP_START)) {
        vga_clear(VGA_WHITE, VGA_RED);
        vga_set_cursor(0, 0);
        vga_print("\n*** FATAL: KERNEL IMAGE OVERLAPS HEAP ***\n\n");
        vga_print("__kernel_end = 0x");
        vga_print_hex((u64)__kernel_end);
        vga_print("\nKERNEL_HEAP_START = 0x");
        vga_print_hex(KERNEL_HEAP_START);
        vga_print("\nShrink the kernel image or raise KERNEL_HEAP_START.\n");
        serial_printf("[FATAL] heap-overlap: __kernel_end=%lx >= heap=%lx\n",
                      (unsigned long)(u64)__kernel_end,
                      (unsigned long)KERNEL_HEAP_START);
        cli();
        while (1) { hlt(); }
    }

    dl_brk = (u8*)PHYS_TO_VIRT(KERNEL_HEAP_START);

    vga_print("[HEAP] dlmalloc 2.7.2 at VA 0x");
    vga_print_hex(PHYS_TO_VIRT(KERNEL_HEAP_START));
    vga_print(" (PA 0x");
    vga_print_hex(KERNEL_HEAP_START);
    vga_print(", ");
    vga_print_unsigned(KERNEL_HEAP_SIZE / 1024 / 1024);
    vga_print(" MB arena via kernel_sbrk)\n");
}

static void heap_oom_report(size_t size) {
    vga_print("[HEAP] Out of memory (requested ");
    vga_print_unsigned(size);
    vga_print(" bytes)!\n");
    struct mallinfo mi = dlmallinfo();
    serial_printf("[HEAP-OOM] req=%lu uordblks=%d fordblks=%d arena=%d keepcost=%d brk=%lx\n",
                  (unsigned long)size,
                  mi.uordblks, mi.fordblks, mi.arena, mi.keepcost,
                  (unsigned long)(u64)dl_brk);
}

void* kmalloc(size_t size) {
    if (size == 0) size = 1;

    u64 irq = spin_lock_irqsave(&heap_lock);
    void* p = dlmalloc(size);
    spin_unlock_irqrestore(&heap_lock, irq);

    if (!p) heap_oom_report(size);
    return p;
}

void* kzalloc(size_t size) {
    void* ptr = kmalloc(size);
    if (ptr) kmemset(ptr, 0, size);
    return ptr;
}

/* NEW: did not exist in the free-list era — dlrealloc-backed. */
void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }

    u64 irq = spin_lock_irqsave(&heap_lock);
    void* p = dlrealloc(ptr, size);
    spin_unlock_irqrestore(&heap_lock, irq);

    if (!p) heap_oom_report(size);
    return p;
}

void kfree(void* ptr) {
    if (!ptr) return;

    u64 irq = spin_lock_irqsave(&heap_lock);
    dlfree(ptr);
    spin_unlock_irqrestore(&heap_lock, irq);
}

size_t heap_get_used(void) {
    u64 irq = spin_lock_irqsave(&heap_lock);
    size_t used = (size_t)dlmallinfo().uordblks;
    spin_unlock_irqrestore(&heap_lock, irq);
    return used;
}

size_t heap_get_free(void) {
    u64 irq = spin_lock_irqsave(&heap_lock);
    size_t used = (size_t)dlmallinfo().uordblks;
    spin_unlock_irqrestore(&heap_lock, irq);
    return KERNEL_HEAP_SIZE - used;
}

// Diagnostic: dlmalloc_stats() over the serial console (via dlcompat.c
// fprintf shim). Safe to call from shell/panic contexts.
void heap_dump_stats(void) {
    u64 irq = spin_lock_irqsave(&heap_lock);
    dlmalloc_stats();
    spin_unlock_irqrestore(&heap_lock, irq);
}

// ============================================================
// Slab Allocator (improved with page tracking)
// ============================================================

static slab_cache_t slab_caches[SLAB_CACHE_SIZE];
static size_t slab_cache_count = 0;
// Guards per-cache free lists and page chains
static spinlock_t slab_lock = SPINLOCK_INIT;

void slab_init(void) {
    slab_cache_count = 0;
    kmemset(slab_caches, 0, sizeof(slab_caches));
    vga_print("[SLAB] Allocator initialized (page-tracked)\n");
}

slab_cache_t* slab_cache_create(const char* name, size_t object_size) {
    if (slab_cache_count >= SLAB_CACHE_SIZE) return NULL;
    if (object_size < sizeof(void*)) object_size = sizeof(void*);
    // Align object size to 8 bytes
    object_size = (object_size + 7) & ~(size_t)7;

    slab_cache_t* cache = &slab_caches[slab_cache_count++];
    cache->object_size = object_size;
    // Reserve space for slab_page_t header at the start of each page
    cache->objects_per_page = (PAGE_SIZE - sizeof(slab_page_t)) / object_size;
    cache->pages = NULL;
    cache->free_list = NULL;
    cache->total_objects = 0;
    cache->used_objects = 0;
    cache->page_count = 0;
    cache->name = name;

    return cache;
}

void* slab_alloc(slab_cache_t* cache) {
    if (!cache) return NULL;

    u64 irq = spin_lock_irqsave(&slab_lock);

    // Reuse previously freed objects: scan pages for a non-empty
    // per-page free list (kept in sync by slab_free).
    for (slab_page_t* p = cache->pages; p; p = p->next) {
        if (p->free_list) {
            void* obj = p->free_list;
            p->free_list = *(void**)obj;
            p->used_count++;
            cache->used_objects++;
            spin_unlock_irqrestore(&slab_lock, irq);
            return obj;
        }
    }

    // Allocate a new page
    if (cache->page_count >= SLAB_MAX_PAGES) {
        spin_unlock_irqrestore(&slab_lock, irq);
        return NULL;
    }
    // pmm_lock is a different lock — safe to take while holding slab_lock
    phys_addr_t page = pmm_alloc_page();
    if (page == 0) {
        spin_unlock_irqrestore(&slab_lock, irq);
        return NULL;
    }

    void* slab = (void*)PHYS_TO_VIRT(page);
    slab_page_t* sp = (slab_page_t*)slab;
    sp->next = NULL;
    sp->free_list = NULL;
    sp->used_count = 0;
    sp->capacity = cache->objects_per_page;

    // Link page into cache
    sp->next = cache->pages;
    cache->pages = sp;
    cache->page_count++;

    // Objects start after the slab_page_t header
    u8* obj_area = (u8*)slab + sizeof(slab_page_t);
    u32 count = cache->objects_per_page;

    // Build free list for this page
    void** page_free = NULL;
    for (u32 i = count; i > 1; i--) {
        void* obj = obj_area + (i - 1) * cache->object_size;
        *(void**)obj = page_free;
        page_free = obj;
    }
    sp->free_list = page_free;
    cache->total_objects += count;

    // Return first object
    void* first = obj_area;
    cache->used_objects++;
    sp->used_count = 1;
    spin_unlock_irqrestore(&slab_lock, irq);
    return first;
}

void slab_free(slab_cache_t* cache, void* obj) {
    if (!cache || !obj) return;

    u64 irq = spin_lock_irqsave(&slab_lock);

    // Ownership check: the object must lie inside one of this cache's
    // pages. Without this a wrong-cache or garbage free silently
    // corrupts the shared free list.
    slab_page_t* owner = NULL;
    for (slab_page_t* p = cache->pages; p; p = p->next) {
        u8* base = (u8*)p + sizeof(slab_page_t);
        u8* end  = base + (u64)p->capacity * cache->object_size;
        if ((u8*)obj >= base && (u8*)obj < end &&
            ((u8*)obj - base) % cache->object_size == 0) {
            owner = p;
            break;
        }
    }
    if (!owner) {
        spin_unlock_irqrestore(&slab_lock, irq);
        vga_printf("[SLAB] free: object %p does not belong to cache '%s'\n",
                   obj, cache->name ? cache->name : "(anon)");
        return;
    }

    // Double-free check: linear scan of this page's free list.
    // O(objects_per_page), acceptable for a kernel without per-object tags.
    for (void* it = owner->free_list; it; it = *(void**)it) {
        if (it == obj) {
            spin_unlock_irqrestore(&slab_lock, irq);
            vga_printf("[SLAB] double free detected at %p\n", obj);
            return;
        }
    }

    *(void**)obj = owner->free_list;
    owner->free_list = obj;
    if (owner->used_count > 0) owner->used_count--;
    if (cache->used_objects > 0) cache->used_objects--;
    spin_unlock_irqrestore(&slab_lock, irq);
}

void slab_cache_info(const slab_cache_t* cache) {
    if (!cache) return;
    vga_printf("  Cache '%s': obj_size=%lu, pages=%lu, used=%lu/%lu\n",
               cache->name ? cache->name : "(anon)",
               (unsigned long)cache->object_size,
               (unsigned long)cache->page_count,
               (unsigned long)cache->used_objects,
               (unsigned long)cache->total_objects);
}
