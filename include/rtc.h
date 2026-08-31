#ifndef RTC_H
#define RTC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// RTC I/O ports
#define RTC_PORT     0x70
#define RTC_DATA     0x71

// RTC register indices
#define RTC_SECONDS      0x00
#define RTC_MINUTES      0x02
#define RTC_HOURS        0x04
#define RTC_DAY_WEEK     0x06
#define RTC_DAY_MONTH    0x07
#define RTC_MONTH        0x08
#define RTC_YEAR         0x09
#define RTC_CENTURY      0x32
#define RTC_STATUS_A     0x0A
#define RTC_STATUS_B     0x0B
#define RTC_STATUS_C     0x0C

// RTC date/time structure
typedef struct {
    u8 second;
    u8 minute;
    u8 hour;
    u8 day;
    u8 month;
    u16 year;
    u8 weekday;  // 1=Sunday, 2=Monday, ... 7=Saturday
} rtc_time_t;

// Initialize RTC (enable periodic interrupt on IRQ8)
void rtc_init(void);

// IRQ8 handler — call from irq_handler for irq==8
void rtc_handler(void);

// Read current time (converts from BCD if needed)
void rtc_read_time(rtc_time_t* t);

// Get a formatted time string "YYYY-MM-DD HH:MM:SS" into buf (min 20 bytes)
void rtc_format_time(char* buf, size_t bufsize);

// Get day of week name
const char* rtc_weekday_name(u8 weekday);

// Get month name
const char* rtc_month_name(u8 month);

#ifdef __cplusplus
}
#endif

#endif // RTC_H
