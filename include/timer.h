#ifndef TIMER_H
#define TIMER_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TIMER_FREQ 100  // 100 Hz (10 ms per tick)

// Initialize PIT timer
typedef void (*timer_callback_t)(void);

void timer_init();
void timer_handler();

// Get tick count since boot
u64 timer_get_ticks();

// Get milliseconds since boot
u64 timer_get_uptime_ms();

// Sleep for at least `ms` milliseconds (busy-wait)
void timer_sleep_ms(u64 ms);

// Register a callback called on every tick (only one supported)
void timer_set_callback(timer_callback_t cb);

#ifdef __cplusplus
}
#endif

#endif // TIMER_H
