#ifndef MM_H
#define MM_H

#include "types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Page size and alignment
#define PAGE_SIZE 4096
#define PAGE_ALIGN(addr) ((addr) & ~(PAGE_SIZE - 1))
#define PAGE_ROUND_UP(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

// Memory map entry (usable by both Multiboot2 parser and VMM)
typedef struct {
    u64 addr;
    u64 length;
    u32 type;  // 1=available, 2=reserved, 3=ACPI reclaimable, 4=ACPI NVS, 5=bad
} mmap_entry_t;

// ============================================================
// Physical Memory Manager (PMM)
// ============================================================

void pmm_init(void* mbi_ptr);
phys_addr_t pmm_alloc_page(void);
phys_addr_t pmm_alloc_pages(size_t count);
void pmm_free_page(phys_addr_t addr);
void pmm_free_pages(phys_addr_t addr, size_t count);
u64 pmm_get_total_memory(void);
u64 pmm_get_available_memory(void);

// Physical page refcounts (for COW sharing). Lazy-initialized on first
// use; pages start at refcount 0 (= "not tracked", treated as 1).
void pmm_ref(phys_addr_t addr);            // ++refs
void pmm_mark_shared(phys_addr_t addr);    // untracked -> refs=2 (fork)
void pmm_unref(phys_addr_t addr);          // --refs; free page at 0
u32  pmm_refs(phys_addr_t addr);           // current count
bool pmm_ref_tracked(phys_addr_t addr);    // false = exclusive-untracked
void pmm_refcounts_init(void);             // early boot reservation

// Protected top-of-RAM carve zone (page tables, refcounts, ...).
// Frames here can NEVER be returned by the general allocator.
phys_addr_t pmm_carve_pages(u64 count);
void pmm_carve_init(u64 bytes);

// Mark a physical memory range as used in the bitmap
void pmm_mark_used(phys_addr_t addr, u64 size);

// Get the memory map (filled by pmm_init from Multiboot2)
#define PMM_MAX_MMAP_ENTRIES 32
extern mmap_entry_t pmm_mmap[PMM_MAX_MMAP_ENTRIES];
extern u32 pmm_mmap_count;

// ============================================================
// Virtual Memory Manager (VMM)
// ============================================================

// High-half kernel (#kernel-halffix): VA = PA + 1 GB.
// The kernel (text/data/bss) and the whole RAM window live at VAs
// 0x40000000..0x7FFFFFFF (PML4[0]->PDPT[1]); user images are pre-linked
// at 0x400000 and can NEVER shadow kernel memory anymore — the child
// CR3 clones the window. Identity mapping survives only where devices
// need it: PML4[0]->PDPT[0] (0-1GB: VGA/MBI/trampoline) and PDPT[2..3]
// (2-4GB: PCI/LAPIC MMIO). PA 1-2GB has no identity alias (RAM-only).
// All RAM access goes through PHYS_TO_VIRT; DMA addresses are always
// VIRT_TO_PHYS(kernel VA). Small code model stays valid (<2GB VAs).
#define HH_OFFSET 0x40000000ULL
#define VIRT_TO_PHYS(virt) ((virt) - HH_OFFSET)
#define PHYS_TO_VIRT(phys) ((phys) + HH_OFFSET)

void vmm_init(void);
void vmm_map(virt_addr_t virt, phys_addr_t phys, u64 flags);
void vmm_map_range(virt_addr_t virt, phys_addr_t phys, u64 size, u64 flags);
void vmm_unmap(virt_addr_t virt);
phys_addr_t vmm_get_phys(virt_addr_t virt);
void vmm_switch_pml4(phys_addr_t pml4_phys);

// FIX(#uaccess-supervisor-hole): vet a user buffer on the ACTIVE
// address space — every PRESENT page must carry VMM_USER (rejects the
// supervisor identity/heap alias below 1GB and the 2-4GB MMIO alias).
// Not-present pages pass: the demand pager satisfies ring-3 faults.
bool vmm_range_is_user(virt_addr_t addr, u64 len);
bool vmm_leaf_is_user(virt_addr_t virt);   /* FIX(#audit-mmap-regression) */

// Split a 2MB huge page at `virt` into 512 x 4KB pages.
// Returns true on success, false if not a huge page or out of memory.
// After splitting, the 2MB PD entry is replaced by a PT with 512 4KB entries.
bool vmm_split_huge_page(virt_addr_t virt);

// Get kernel PML4 physical address (for future use)
phys_addr_t vmm_get_kernel_pml4(void);

// ============================================================
// Address spaces & COW (process support)
// ============================================================

#define VMM_COW (1ULL << 9)   // PTE avail bit: copy-on-write mapping
#define VMM_STICKY_RO (1ULL << 10) // PTE avail bit: mprotect(RO) pin.
// #mprotect-cow-mask: without this bit, fork's COW conversion makes
// every user page RO+COW, and the first write COW-resolves it back
// to RW — an mprotect(PROT_READ) pin would silently evaporate in the
// child (and in the parent after fork). STICKY_RO pages skip the COW
// conversion at fork, and vmm_handle_cow refuses them, so a write
// really faults and the task dies SIGSEGV-style (#pf-kill-task).

// vmm_map/vmm_unmap/vmm_get_phys operate on the ACTIVE address space
// (kernel PML4 by default). The scheduler activates a process's PML4
// with vmm_switch_pml4(), so syscalls and the demand pager naturally
// target the current process.
void     vmm_set_active(phys_addr_t pml4_phys);   // variable only, no cr3
phys_addr_t vmm_get_active_pml4(void);
void vmm_pml4_guard(int site);   /* [PML4-GUARD] temporary forensics */

// Resolve a write fault on a COW page in the active space: copies the
// page if shared, or just re-enables writes if sole owner.
// Returns true when the fault was handled (retry the instruction).
bool vmm_handle_cow(virt_addr_t virt);
u64* vmm_walk_leaf(phys_addr_t pml4_phys, u64 virt, bool create_pt, bool user);

// Clone the given address space for fork(): kernel pages are shared
// as-is; every USER page becomes COW in BOTH parent and child.
// Returns child PML4 phys, or 0 on failure. `src` must not be active.
phys_addr_t vmm_fork_pml4(phys_addr_t src, bool cow_user);

// Tear down a process address space: frees all USER pages (unref) and
// every table page of the clone. Must NOT be the currently active one.
void vmm_destroy_pml4(phys_addr_t pml4_phys);

// ============================================================
// Kernel Heap (free-list allocator)
// ============================================================

// Fixed heap address: moved to 16 MB (#heap-overlap FIX).
//
// HISTORY: the old 0x500000 (5 MB) start sat below the user-image
// ceiling (0x400000+) on purpose: back then __kernel_end was ~0x2A0000
// (1.7 MB image) and nothing collided. Two facts broke that layout:
//   * any kernel image larger than 4 MB (e.g. an embedded 4.3 MB
//     apk.static blob ending at __kernel_end == 0x6FA000) makes the
//     free-list heap CARVE ITS HEADERS straight into kernel .rodata —
//     heap_init() writes the first block header at heap start, and
//     every kmalloc split writes more; kmemset(zero) blocks turned
//     .rodata into zeros. That was the "zero-writer" at 0x5b2ff0.
//   * user images are loaded at fixed low VAs (0x400000+); a kernel
//     above 0x400000 shadowed its own identity mapping in user CR3s.
// 16 MB keeps the heap above ANY realistic kernel image growth while
// staying far below the carve zone (128 MB) and buddy (heap end).
// heap_init() now hard-fails if the kernel image ever grows past it.
#define KERNEL_HEAP_START  0x1000000ULL   // 16 MB — above kernel image
#define KERNEL_HEAP_SIZE   (64 * 1024 * 1024)  // 64 MB

void heap_init(void);
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void  kfree(void* ptr);
void* krealloc(void* ptr, size_t size);   // NEW in dlmalloc era
void  heap_dump_stats(void);              // dlmalloc_stats() over serial
size_t heap_get_used(void);
size_t heap_get_free(void);

// ============================================================
// Slab Allocator
// ============================================================

#define SLAB_CACHE_SIZE 64
#define SLAB_MAX_PAGES 256

typedef struct slab_page {
    struct slab_page* next;
    void* free_list;
    u32 used_count;
    u32 capacity;
} slab_page_t;

typedef struct slab_cache {
    size_t object_size;
    size_t objects_per_page;
    slab_page_t* pages;
    void*  free_list;
    size_t total_objects;
    size_t used_objects;
    size_t page_count;
    const char* name;
} slab_cache_t;

void slab_init(void);
slab_cache_t* slab_cache_create(const char* name, size_t object_size);
void* slab_alloc(slab_cache_t* cache);
void  slab_free(slab_cache_t* cache, void* obj);
void  slab_cache_info(const slab_cache_t* cache);

#ifdef __cplusplus
}
#endif

#endif // MM_H
