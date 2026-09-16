#ifndef KLOG_H
#define KLOG_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Kernel log levels
#define KLOG_LEVEL_EMERG   0  // System is unusable
#define KLOG_LEVEL_ALERT   1  // Action must be taken immediately
#define KLOG_LEVEL_CRIT    2  // Critical conditions
#define KLOG_LEVEL_ERR     3  // Error conditions
#define KLOG_LEVEL_WARN    4  // Warning conditions
#define KLOG_LEVEL_NOTICE  5  // Normal but significant
#define KLOG_LEVEL_INFO    6  // Informational
#define KLOG_LEVEL_DEBUG   7  // Debug-level messages

// Log level names
extern const char* klog_level_names[];

// Initialize kernel log buffer
void klog_init(void);

// Log a message (printf-style, with level)
// Also outputs to VGA and serial
void klog(u8 level, const char* fmt, ...);

// Shorthand macros
#define klog_emerg(fmt, ...)  klog(KLOG_LEVEL_EMERG, fmt, ##__VA_ARGS__)
#define klog_alert(fmt, ...)  klog(KLOG_LEVEL_ALERT, fmt, ##__VA_ARGS__)
#define klog_crit(fmt, ...)   klog(KLOG_LEVEL_CRIT,  fmt, ##__VA_ARGS__)
#define klog_err(fmt, ...)    klog(KLOG_LEVEL_ERR,   fmt, ##__VA_ARGS__)
#define klog_warn(fmt, ...)   klog(KLOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define klog_notice(fmt, ...) klog(KLOG_LEVEL_NOTICE,fmt, ##__VA_ARGS__)
#define klog_info(fmt, ...)   klog(KLOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define klog_debug(fmt, ...)  klog(KLOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

// Print the log buffer (for dmesg command)
// level_filter: 0 = all, otherwise only messages >= this level
void klog_print_buffer(u8 level_filter);

// Get number of messages in the log
u32 klog_get_count(void);

#ifdef __cplusplus
}
#endif

#endif // KLOG_H
