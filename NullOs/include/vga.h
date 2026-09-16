#ifndef VGA_H
#define VGA_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// VGA text mode constants
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000  // Identity-mapped VGA framebuffer

// VGA colors
typedef enum {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15
} vga_color_t;

// Initialize VGA driver with double buffering
void vga_init();

// Clear back buffer only (doesn't update screen)
void vga_clear_buffer(vga_color_t fg, vga_color_t bg);

// Clear screen with specified color (both buffer and framebuffer)
void vga_clear(vga_color_t fg, vga_color_t bg);

// Copy back buffer to framebuffer (page flip)
void vga_flip();

// Set cursor position
void vga_set_cursor(int x, int y);

// Get cursor position
void vga_get_cursor(int* x, int* y);

// Print a character at current position
void vga_putchar(char c);

// Print a string
void vga_print(const char* str);

// Print a string with color
void vga_print_color(const char* str, vga_color_t fg, vga_color_t bg);

// Print decimal number
void vga_print_decimal(int value);

// Print hexadecimal number
void vga_print_hex(u64 value);

// Print unsigned decimal
void vga_print_unsigned(u64 value);

// Print signed integer
void vga_print_int(s64 value);

// Move cursor to next line
void vga_newline();

// Output capture mode (shell redirection): while active, everything
// written through vga_putchar lands in the caller's buffer instead of
// the screen. vga_capture_start(buf, cap) arms it; vga_capture_stop()
// disarms and returns the number of captured bytes.
void vga_capture_start(char* buf, u32 cap);
u32  vga_capture_stop(void);
bool vga_capture_active(void);

// VGA snapshot for virtual terminal save/restore
typedef struct {
    u16 buffer[VGA_WIDTH * VGA_HEIGHT];
    int cursor_x;
    int cursor_y;
    vga_color_t fg;
    vga_color_t bg;
} vga_snapshot_t;

// Save/restore VGA state (used by virtual terminals)
void vga_save_snapshot(vga_snapshot_t* snap);
void vga_restore_snapshot(const vga_snapshot_t* snap);

// Scroll screen up by one line
void vga_scroll();

// Scrollback buffer
#define VGA_SCROLLBACK_LINES 8192

// Initialize scrollback (called by vga_init)
void vga_scrollback_init(void);

// Save current screen to scrollback before clearing/scrolling
void vga_scrollback_save(void);

// Scroll up in scrollback (show older content)
void vga_scrollback_up(int lines);

// Scroll down in scrollback (show newer content)
void vga_scrollback_down(int lines);

// Check if we are in scrollback view mode
bool vga_scrollback_active(void);

// Reset scrollback view (show live screen)
void vga_scrollback_reset(void);

#ifdef __cplusplus
}
#endif

#endif // VGA_H
