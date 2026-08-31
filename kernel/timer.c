#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/types.h"

// PIT (8253/8254) registers
#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43

// PIT command: channel 0, lobyte/hibyte, mode 2 (rate generator)
#define PIT_CMD_INIT   0x36

// Base frequency of PIT: 1193182 Hz
#define PIT_BASE_FREQ  1193182ULL

static volatile u64 tick_count = 0;
static timer_callback_t timer_cb = NULL;

// I/O delay for PIT programming
static void io_wait(void) {
    outb(0x80, 0);
}

void timer_init() {
    // Calculate divisor for desired frequency
    u16 divisor = (u16)(PIT_BASE_FREQ / TIMER_FREQ);

    // Disable interrupts during PIT programming
    cli();

    // Channel 0, lobyte/hibyte access, mode 2 (rate generator)
    outb(PIT_COMMAND, PIT_CMD_INIT);
    io_wait();
    outb(PIT_CHANNEL0, divisor & 0xFF);
    io_wait();
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    sti();

    tick_count = 0;

    vga_print("[TIMER] PIT initialized at ");
    vga_print_unsigned(TIMER_FREQ);
    vga_print(" Hz (divisor ");
    vga_print_unsigned(divisor);
    vga_print(")\n");
}

void timer_handler() {
    tick_count++;
    if (timer_cb) {
        timer_cb();
    }
}

u64 timer_get_ticks() {
    return tick_count;
}

u64 timer_get_uptime_ms() {
    // Avoid integer overflow: multiply ticks * 1000 first (u64), then divide
    return (tick_count * 1000ULL) / TIMER_FREQ;
}

void timer_sleep_ms(u64 ms) {
    if (ms == 0) return;
    // Avoid overflow: distribute the multiplication
    u64 target = tick_count + (ms / 1000ULL) * TIMER_FREQ + ((ms % 1000ULL) * TIMER_FREQ + 999) / 1000ULL;
    while (tick_count < target) {
        hlt();
    }
}

void timer_set_callback(timer_callback_t cb) {
    timer_cb = cb;
}
