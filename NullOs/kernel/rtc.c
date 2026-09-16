#include "../include/rtc.h"
#include "../include/pic.h"
#include "../include/string.h"
#include "../include/vga.h"

// RTC driver — periodic interrupt on IRQ8

// Check if RTC update is in progress
static bool rtc_is_updating(void) {
    outb(RTC_PORT, RTC_STATUS_A);
    return (inb(RTC_DATA) & 0x80) != 0;
}

// Read a single RTC register
static u8 rtc_read_reg(u8 reg) {
    outb(RTC_PORT, reg);
    return inb(RTC_DATA);
}

// Write a single RTC register
static void rtc_write_reg(u8 reg, u8 val) {
    outb(RTC_PORT, reg);
    outb(RTC_DATA, val);
}

void rtc_init(void) {
    // Disable NMI during RTC setup (save old NMI state)
    u8 prev_nmi = inb(RTC_PORT) & 0x80;

    // Read Status B to check current settings
    outb(RTC_PORT, RTC_STATUS_B);
    u8 status_b = inb(RTC_DATA);

    // Enable periodic interrupt (rate: 2 Hz = bit pattern 0x06 in Status A)
    // Status A: set rate select bits (bits 0-3) = 0x06 -> 1024 Hz, but
    // with dividor = 0x15 -> 2 Hz (simpler, fewer interrupts)
    // Actually let's use 2 Hz for manageable overhead:
    // Rate select = 0x0F -> 2 Hz
    // Read-modify-write Status A to set rate without changing DV
    outb(RTC_PORT, RTC_STATUS_A);
    u8 status_a = inb(RTC_DATA);
    status_a = (status_a & 0xF0) | 0x0F;  // Set rate to 2 Hz
    outb(RTC_PORT, RTC_STATUS_A);
    outb(RTC_DATA, status_a);

    // Enable periodic interrupt in Status B, also set 24h mode
    status_b |= 0x40;  // Enable Periodic Interrupt (PIE)
    status_b &= ~0x02; // 24-hour format
    status_b |= 0x04;  // Binary mode (not BCD) — if supported
    // Actually, many RTCs use BCD by default, let's handle conversion
    // in read. Don't force binary mode.
    status_b &= ~0x04; // Keep BCD mode (safer for real hardware)
    outb(RTC_PORT, RTC_STATUS_B);
    outb(RTC_DATA, status_b);

    // Read Status C to clear any pending IRQ
    outb(RTC_PORT, RTC_STATUS_C);
    inb(RTC_DATA);

    // Restore NMI state.
    // Bit 7 of RTC_PORT (0x70) controls NMI: 0=enabled, 1=disabled.
    // We need to set it to prev_nmi without changing the index.
    // Since we last wrote to 0x70 in the Status C read above,
    // just restore the NMI bit.
    outb(RTC_PORT, prev_nmi);  // select register 0 with NMI state restored

    // Unmask IRQ8 (RTC) on PIC2 — bit 0 of PIC2 data port
    pic_unset_mask(8);

    vga_print("[RTC] Real-time clock initialized (2 Hz, IRQ8)\n");
}

void rtc_handler(void) {
    // Read Status C to acknowledge the interrupt (must read to clear)
    outb(RTC_PORT, RTC_STATUS_C);
    inb(RTC_DATA);  // Discard
}

static u8 bcd_to_bin(u8 val) {
    return (val & 0x0F) + ((val >> 4) & 0x0F) * 10;
}

void rtc_read_time(rtc_time_t* t) {
    if (!t) return;

    // Wait until update-in-progress flag is clear
    // to get consistent readings
    int timeout = 10000;
    while (rtc_is_updating() && timeout-- > 0);

    t->second  = rtc_read_reg(RTC_SECONDS);
    t->minute  = rtc_read_reg(RTC_MINUTES);
    t->hour    = rtc_read_reg(RTC_HOURS);
    t->day     = rtc_read_reg(RTC_DAY_MONTH);
    t->month   = rtc_read_reg(RTC_MONTH);
    t->weekday = rtc_read_reg(RTC_DAY_WEEK);

    // Year: two bytes — low byte from 0x09, century from 0x32
    u8 year_low = rtc_read_reg(RTC_YEAR);
    u8 century  = rtc_read_reg(RTC_CENTURY);

    // Convert BCD to binary if in BCD mode
    // Check Status B bit 2 (DM = Data Mode: 0=BCD, 1=Binary)
    outb(RTC_PORT, RTC_STATUS_B);
    u8 dm = inb(RTC_DATA) & 0x04;

    if (!dm) {
        // BCD mode
        t->second  = bcd_to_bin(t->second);
        t->minute  = bcd_to_bin(t->minute);
        t->hour    = bcd_to_bin(t->hour);
        t->day     = bcd_to_bin(t->day);
        t->month   = bcd_to_bin(t->month);
        t->weekday = bcd_to_bin(t->weekday);
        year_low   = bcd_to_bin(year_low);
        century   = bcd_to_bin(century);
    }

    t->year = (u16)(century * 100 + year_low);

    // Convert 12-hour to 24-hour if needed (bit 7 of hours = PM in 12h mode)
    // We set 24h mode in init, but handle it anyway
    outb(RTC_PORT, RTC_STATUS_B);
    u8 h24 = inb(RTC_DATA) & 0x02;  // bit 1 = 1 means 24h
    if (!h24 && t->hour & 0x80) {
        // 12-hour PM mode
        t->hour = ((t->hour & 0x7F) + 12) % 24;
    }
}

const char* rtc_weekday_name(u8 weekday) {
    static const char* names[] = {
        "??", "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    if (weekday >= 1 && weekday <= 7) return names[weekday];
    return names[0];
}

const char* rtc_month_name(u8 month) {
    static const char* names[] = {
        "??", "January", "February", "March", "April",
        "May", "June", "July", "August", "September",
        "October", "November", "December"
    };
    if (month >= 1 && month <= 12) return names[month];
    return names[0];
}

void rtc_format_time(char* buf, size_t bufsize) {
    rtc_time_t t;
    rtc_read_time(&t);

    // YYYY-MM-DD HH:MM:SS
    ksnprintf(buf, bufsize, "%04u-%02u-%02u %02u:%02u:%02u",
              (u32)t.year, (u32)t.month, (u32)t.day,
              (u32)t.hour, (u32)t.minute, (u32)t.second);
}
