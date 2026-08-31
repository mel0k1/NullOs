#ifndef BUDDY_H
#define BUDDY_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Максимальный порядок (2^MAX_ORDER страниц)
#define MAX_ORDER         10
#define MIN_ORDER         0
#define PAGE_SIZE         4096

// Структура зоны памяти для buddy allocator
typedef struct {
    u64 base_addr;        // Базовый адрес
    u64 total_pages;      // Всего страниц
    u64 free_pages;       // Свободные страницы
    u32 max_order;        // Максимальный порядок
    // Списки свободных блоков для каждого порядка
    void* free_lists[MAX_ORDER + 1];
    u32 free_count[MAX_ORDER + 1];
} buddy_zone_t;

// Глобальная зона памяти
extern buddy_zone_t buddy_zone;

// Инициализация buddy allocator
void buddy_init(u64 base_addr, u64 size_bytes);

// Выделение памяти
void* buddy_alloc(u32 order);
void* buddy_alloc_pages(u32 num_pages);

// Освобождение памяти
void buddy_free(void* ptr, u32 order);
void buddy_free_pages(void* ptr, u32 num_pages);

// Утилиты
u32 buddy_get_order(u64 size_bytes);
u64 buddy_get_free_pages();
u64 buddy_get_total_pages();
void buddy_print_stats();

#ifdef __cplusplus
}
#endif

#endif // BUDDY_H
