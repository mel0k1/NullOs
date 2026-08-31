#include "../include/klog.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/string.h"
#include "../include/timer.h"
#include "../include/mm.h"
#include "../include/serial.h"

// Log level name strings
const char* klog_level_names[] = {
    "EMERG", "ALERT", "CRIT", "ERR",
    "WARN", "NOTICE", "INFO", "DEBUG"
};

// Ring buffer for kernel log messages
#define KLOG_MAX_MESSAGES  512
#define KLOG_MSG_SIZE      256

typedef struct {
    u64  tick;       // Tick when message was logged
    u8   level;      // Log level
    char text[KLOG_MSG_SIZE];
} klog_entry_t;

static klog_entry_t klog_buf[KLOG_MAX_MESSAGES];
static u32 klog_head = 0;  // Next write position
static u32 klog_count = 0; // Total messages ever written (for sequence numbers)
static bool klog_initialized = false;

// VGA color for each log level (must use existing vga_color_t enum values)
static vga_color_t klog_colors[] = {
    VGA_WHITE,        // EMERG  (on red bg would be ideal, but vga_print_color uses fg)
    VGA_LIGHT_RED,    // ALERT
    VGA_RED,          // CRIT
    VGA_LIGHT_RED,    // ERR
    VGA_YELLOW,       // WARN
    VGA_WHITE,        // NOTICE
    VGA_LIGHT_GREY,   // INFO
    VGA_DARK_GREY,    // DEBUG
};

void klog_init(void) {
    kmemset(klog_buf, 0, sizeof(klog_buf));
    klog_head = 0;
    klog_count = 0;
    klog_initialized = true;
}

// Internal: write a raw string into the ring buffer
static void klog_write_entry(u8 level, const char* text) {
    if (!klog_initialized) return;

    klog_entry_t* entry = &klog_buf[klog_head];
    entry->tick = timer_get_ticks();
    entry->level = level;

    // Truncate to fit
    u32 len = (u32)kstrlen(text);
    if (len >= KLOG_MSG_SIZE) len = KLOG_MSG_SIZE - 1;
    kmemcpy(entry->text, text, len);
    entry->text[len] = '\0';

    klog_head = (klog_head + 1) % KLOG_MAX_MESSAGES;
    klog_count++;
}

void klog(u8 level, const char* fmt, ...) {
    char buf[KLOG_MSG_SIZE];

    // Format the message
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, args);
    __builtin_va_end(args);

    // Output to VGA with color
    if (level < 8) {
        vga_print_color(buf, klog_colors[level], VGA_BLACK);
    } else {
        vga_print(buf);
    }

    // Output to serial with level prefix
    serial_print("[");
    if (level < 8) {
        serial_print(klog_level_names[level]);
    } else {
        serial_print("??? ");
    }
    serial_print("] ");
    serial_print(buf);

    // Store in ring buffer
    klog_write_entry(level, buf);

    /* TEMP leak tracer */
    {
        extern u64 pmm_get_available_memory(void);
    }
}

void klog_print_buffer(u8 level_filter) {
    if (!klog_initialized) {
        vga_print("[KLOG] Not initialized.\n");
        return;
    }

    // Determine where to start reading.
    // If buffer is full, start at klog_head (oldest entry).
    // Otherwise start at 0.
    u32 total = klog_count;
    u32 start;
    u32 count;

    if (total >= KLOG_MAX_MESSAGES) {
        start = klog_head; // oldest entry in the ring
        count = KLOG_MAX_MESSAGES;
    } else {
        start = 0;
        count = total;
    }

    if (count == 0) {
        vga_print("(no log messages)\n");
        return;
    }

    for (u32 i = 0; i < count; i++) {
        u32 idx = (start + i) % KLOG_MAX_MESSAGES;
        klog_entry_t* e = &klog_buf[idx];

        // Filter by level
        if (level_filter > 0 && e->level < level_filter) continue;

        // Format: [  5.234] [INFO ] message...
        u64 secs = e->tick / TIMER_FREQ;
        u64 rem  = e->tick % TIMER_FREQ;
        u64 ms   = (rem * 1000) / TIMER_FREQ;

        vga_printf("[%3lu.%03lu] [%-6s] %s",
                   (unsigned long)secs, (unsigned long)ms,
                   (e->level < 8) ? klog_level_names[e->level] : "??? ",
                   e->text);
    }

    vga_printf("--- %u messages shown ---\n", count);
}

u32 klog_get_count(void) {
    return klog_count;
}
