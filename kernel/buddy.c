#include "../include/buddy.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/spinlock.h"

// Глобальная зона памяти
buddy_zone_t buddy_zone;

// Внутренние списки свободных блоков (typedef для удобства)
typedef struct free_block {
    struct free_block* next;
} free_block_t;

static free_block_t* free_lists[MAX_ORDER + 1] = {0};

// Guards the free lists and counters (SMP-safe alloc/free)
static spinlock_t buddy_lock = SPINLOCK_INIT;

// ============================================================
// Helper: compute buddy address for a block at given order
// Buddy is at addr XOR (size_of_block), where size = (1 << order) * PAGE_SIZE
// ============================================================
static inline u64 buddy_addr(u64 block_addr, u32 order) {
    u64 size = (1ULL << order) * PAGE_SIZE;
    return block_addr ^ size;
}

// ============================================================
// Helper: check if address is within the buddy zone
// ============================================================
static inline bool in_zone(u64 addr) {
    u64 zone_end = buddy_zone.base_addr + (u64)buddy_zone.total_pages * PAGE_SIZE;
    return addr >= buddy_zone.base_addr && addr < zone_end;
}

// ============================================================
// Helper: check if a block at `addr` of `order` is page-aligned
// (buddy blocks are always aligned to their size)
// ============================================================
static inline bool is_aligned(u64 addr, u32 order) {
    u64 size = (1ULL << order) * PAGE_SIZE;
    return (addr & (size - 1)) == 0;
}

// ============================================================
// Helper: find and remove a block from a free list at given order
// Returns true if found and removed, false otherwise.
// ============================================================
static bool remove_from_list(u32 order, u64 addr) {
    free_block_t** pp = &free_lists[order];
    while (*pp) {
        if ((u64)*pp == addr) {
            *pp = (*pp)->next;
            buddy_zone.free_count[order]--;
            // The absorbed buddy's pages were already counted as free;
            // subtract them here so the final add in buddy_free() keeps
            // the total accurate after a merge.
            buddy_zone.free_pages -= (1ULL << order);
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

void buddy_init(u64 base_addr, u64 size_bytes) {
    buddy_zone.base_addr = base_addr;
    buddy_zone.total_pages = size_bytes / PAGE_SIZE;
    buddy_zone.free_pages = 0;
    buddy_zone.max_order = MAX_ORDER;

    // Очищаем списки
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_lists[i] = 0;
        buddy_zone.free_count[i] = 0;
    }

    // Align base to max-order block size
    u64 block_size = (1ULL << MAX_ORDER) * PAGE_SIZE;
    u64 addr = (base_addr + block_size - 1) & ~(block_size - 1);
    u64 zone_end = base_addr + size_bytes;

    // Add all aligned max-order blocks
    while (addr + block_size <= zone_end) {
        free_block_t* block = (free_block_t*)addr;
        block->next = free_lists[MAX_ORDER];
        free_lists[MAX_ORDER] = block;
        buddy_zone.free_count[MAX_ORDER]++;
        buddy_zone.free_pages += (1ULL << MAX_ORDER);
        addr += block_size;
    }

    // Handle remaining tail with smaller orders
    if (addr < zone_end) {
        u64 remaining = zone_end - addr;
        for (s32 o = MAX_ORDER; o >= 0; o--) {
            u64 bs = (1ULL << o) * PAGE_SIZE;
            if (bs <= remaining && is_aligned(addr, (u32)o)) {
                free_block_t* block = (free_block_t*)addr;
                block->next = free_lists[o];
                free_lists[o] = block;
                buddy_zone.free_count[o]++;
                buddy_zone.free_pages += (1ULL << o);
                addr += bs;
                remaining -= bs;
                o++; // will be decremented by loop
            }
        }
    }

    vga_print("[Buddy] Initialized: ");
    vga_print_int(buddy_zone.total_pages);
    vga_print(" pages (");
    vga_print_int(size_bytes / 1024 / 1024);
    vga_print(" MB), Free: ");
    vga_print_int(buddy_zone.free_pages);
    vga_print(" pages\n");
}

u32 buddy_get_order(u64 size_bytes) {
    u32 order = 0;
    u64 pages = (size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    while ((1ULL << order) < pages && order < MAX_ORDER) {
        order++;
    }

    // If pages is exactly a power of two and equal to (1 << MAX_ORDER),
    // the loop above stops at order = MAX_ORDER - 1. Fix:
    if ((1ULL << order) < pages) {
        // pages exceeds max allocatable size — clamp
        order = MAX_ORDER;
    }

    return order;
}

void* buddy_alloc(u32 order) {
    if (order > MAX_ORDER) {
        return 0;
    }

    u64 irq = spin_lock_irqsave(&buddy_lock);

    // Ищем свободный блок нужного порядка
    if (free_lists[order] != 0) {
        free_block_t* block = free_lists[order];
        free_lists[order] = block->next;
        buddy_zone.free_count[order]--;
        buddy_zone.free_pages -= (1ULL << order);
        spin_unlock_irqrestore(&buddy_lock, irq);
        return (void*)block;
    }

    // Пытаемся разделить блок большего порядка
    for (u32 o = order + 1; o <= MAX_ORDER; o++) {
        if (free_lists[o] != 0) {
            free_block_t* block = free_lists[o];
            free_lists[o] = block->next;
            buddy_zone.free_count[o]--;

            // Последовательно делим блок до нужного порядка
            u64 block_addr = (u64)block;
            for (u32 i = o; i > order; i--) {
                // Блок порядка i делится на два блока порядка i-1
                u64 half_size = (1ULL << (i - 1)) * PAGE_SIZE;
                u64 buddy = block_addr + half_size;

                // Добавляем buddy-блок в свободный список
                free_block_t* bb = (free_block_t*)buddy;
                bb->next = free_lists[i - 1];
                free_lists[i - 1] = bb;
                buddy_zone.free_count[i - 1]++;
            }

            buddy_zone.free_pages -= (1ULL << order);
            spin_unlock_irqrestore(&buddy_lock, irq);
            return (void*)block_addr;
        }
    }

    // Нет свободной памяти
    spin_unlock_irqrestore(&buddy_lock, irq);
    return 0;
}

void* buddy_alloc_pages(u32 num_pages) {
    if (num_pages == 0) return NULL;
    u32 order = 0;

    while ((1ULL << order) < num_pages && order < MAX_ORDER) {
        order++;
    }

    // Clamp if num_pages exceeds max order capacity
    if ((1ULL << order) < num_pages) {
        return NULL;  // too large
    }

    return buddy_alloc(order);
}

// ============================================================
// buddy_free — освобождает блок и объединяет с buddy-блоками
// ============================================================
void buddy_free(void* ptr, u32 order) {
    if (ptr == 0 || order > MAX_ORDER) {
        return;
    }

    u64 block_addr = (u64)ptr;

    // Проверяем что адрес выровнен и в зоне
    if (!is_aligned(block_addr, order)) {
        vga_print("[Buddy] WARNING: free of unaligned block at 0x");
        vga_print_hex(block_addr);
        vga_print("\n");
        return;
    }
    if (!in_zone(block_addr)) {
        return;
    }

    u64 irq = spin_lock_irqsave(&buddy_lock);

    // Попытаемся объединить с buddy-блоком
    // Идем снизу вверх по порядкам
    while (order < MAX_ORDER) {
        u64 buddy = buddy_addr(block_addr, order);

        // Buddy должен быть в зоне и выровнен
        if (!in_zone(buddy) || !is_aligned(buddy, order)) {
            break;
        }

        // Пытаемся найти buddy в свободном списке
        if (!remove_from_list(order, buddy)) {
            // Buddy не свободен — нельзя объединить
            break;
        }

        // Merge: both blocks become one block of order+1
        // free_pages: we removed buddy (-(1<<order)), now the merged
        // block replaces both, so net change = +(1<<(order+1)) - (1<<order) = +(1<<order)
        if (buddy < block_addr) {
            block_addr = buddy;
        }

        order++;
    }

    // Add the (possibly coalesced) block to the free list
    free_block_t* block = (free_block_t*)block_addr;
    block->next = free_lists[order];
    free_lists[order] = block;
    buddy_zone.free_count[order]++;
    // The merged block of size (1<<order) replaces the original + buddy
    buddy_zone.free_pages += (1ULL << order);

    spin_unlock_irqrestore(&buddy_lock, irq);
}

void buddy_free_pages(void* ptr, u32 num_pages) {
    u32 order = buddy_get_order(num_pages * PAGE_SIZE);
    buddy_free(ptr, order);
}

u64 buddy_get_free_pages() {
    return buddy_zone.free_pages;
}

u64 buddy_get_total_pages() {
    return buddy_zone.total_pages;
}

void buddy_print_stats() {
    vga_print("\n[Buddy] Memory Statistics:\n");
    vga_print("------------------------\n");
    vga_print("Total pages: ");
    vga_print_int(buddy_zone.total_pages);
    vga_print("\nFree pages: ");
    vga_print_int(buddy_zone.free_pages);
    vga_print("\nUsed pages: ");
    vga_print_int(buddy_zone.total_pages - buddy_zone.free_pages);
    vga_print("\n\nBlocks by order:\n");

    for (int i = 0; i <= MAX_ORDER; i++) {
        vga_print("Order ");
        vga_print_int(i);
        vga_print(": ");
        vga_print_int(buddy_zone.free_count[i]);
        vga_print(" blocks (");
        vga_print_int((1ULL << i) * buddy_zone.free_count[i]);
        vga_print(" pages)\n");
    }
}
