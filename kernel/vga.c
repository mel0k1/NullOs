#include "vga.h"

// Back buffer
static u16 vga_back_buffer[VGA_WIDTH * VGA_HEIGHT];
// VGA framebuffer pointer
static u16* vga_framebuffer = (u16*)VGA_MEMORY;

static int cursor_x = 0;
static int cursor_y = 0;
static vga_color_t current_fg = VGA_WHITE;
static vga_color_t current_bg = VGA_BLACK;

// Output capture (shell redirection). When capture_buf is set, all
// vga_putchar output is redirected into it and the screen state is
// left completely untouched.
static char* capture_buf = NULL;
static u32   capture_pos = 0;
static u32   capture_cap = 0;

void vga_capture_start(char* buf, u32 cap) {
    capture_buf = buf;
    capture_pos = 0;
    capture_cap = cap;
}

u32 vga_capture_stop(void) {
    u32 n = capture_pos;
    if (capture_buf && capture_cap > 0) {
        capture_buf[capture_pos < capture_cap ? capture_pos : capture_cap - 1] = '\0';
    }
    capture_buf = NULL;
    capture_cap = 0;
    return n;
}

bool vga_capture_active(void) { return capture_buf != NULL; }

// Create VGA entry with color
static inline u16 vga_entry(char c, vga_color_t fg, vga_color_t bg) {
    return (u16)c | ((u16)(fg | (bg << 4)) << 8);
}

// Ring buffer for scrollback
static u16 scrollback_buffer[VGA_SCROLLBACK_LINES * VGA_WIDTH];
/* FIX(#exec-big/X2): these scalars are touched on every vga/vt write,
 * including from syscall paths while CR3 = user process. They must
 * live below the user VA range, hence the dedicated section. */
static u32 scrollback_head __attribute__((section(".vtlow"))) = 0;
static u32 scrollback_count __attribute__((section(".vtlow"))) = 0;
static bool scrollback_viewing __attribute__((section(".vtlow"))) = false;
static u32 scrollback_offset __attribute__((section(".vtlow"))) = 0;

static inline void scrollback_push_line(const u16* line) {
    u16* dst = &scrollback_buffer[scrollback_head * VGA_WIDTH];
    for (int i = 0; i < VGA_WIDTH; i++) dst[i] = line[i];
    scrollback_head = (scrollback_head + 1) % VGA_SCROLLBACK_LINES;
    if (scrollback_count < VGA_SCROLLBACK_LINES) scrollback_count++;
}

void vga_scrollback_init(void) {
    scrollback_head = 0;
    scrollback_count = 0;
    scrollback_viewing = false;
    scrollback_offset = 0;
}

void vga_scrollback_save(void) {
    scrollback_push_line(&vga_back_buffer[0]);
}

void vga_scrollback_up(int lines) {
    if (scrollback_count == 0) return;
    scrollback_viewing = true;
    scrollback_offset += lines;
    if (scrollback_offset > scrollback_count) scrollback_offset = scrollback_count;
    u32 view_base = (scrollback_head + VGA_SCROLLBACK_LINES - scrollback_offset) % VGA_SCROLLBACK_LINES;
    for (int row = 0; row < VGA_HEIGHT; row++) {
        u32 line_idx = (view_base + row) % VGA_SCROLLBACK_LINES;
        if (line_idx < scrollback_count) {
            for (int col = 0; col < VGA_WIDTH; col++)
                vga_back_buffer[row * VGA_WIDTH + col] = scrollback_buffer[line_idx * VGA_WIDTH + col];
        } else {
            for (int col = 0; col < VGA_WIDTH; col++)
                vga_back_buffer[row * VGA_WIDTH + col] = vga_entry(' ', current_fg, current_bg);
        }
    }
    vga_flip();
}

void vga_scrollback_down(int lines) {
    if (!scrollback_viewing) return;
    if (scrollback_offset <= (u32)lines) {
        scrollback_offset = 0;
        scrollback_viewing = false;
        vga_flip();
        return;
    }
    scrollback_offset -= lines;
    vga_scrollback_up(0);
}

bool vga_scrollback_active(void) { return scrollback_viewing; }
void vga_scrollback_reset(void) { scrollback_viewing = false; scrollback_offset = 0; vga_flip(); }

void vga_init() {
    cursor_x = 0;
    cursor_y = 0;
    current_fg = VGA_LIGHT_GREY;
    current_bg = VGA_BLACK;
    
    // Очищаем back buffer
    vga_clear_buffer(current_fg, current_bg);
    // Копируем в framebuffer
    vga_flip();
    // Init scrollback
    vga_scrollback_init();
}

void vga_clear_buffer(vga_color_t fg, vga_color_t bg) {
    current_fg = fg;
    current_bg = bg;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_back_buffer[i] = vga_entry(' ', fg, bg);
    }
    cursor_x = 0;
    cursor_y = 0;
}

void vga_clear(vga_color_t fg, vga_color_t bg) {
    // Очищаем и буфер, и видеопамять
    vga_clear_buffer(fg, bg);
    vga_flip();
    
    // Обновляем аппаратный курсор
    u16 pos = 0;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

// Копирование back buffer в видеопамять (Page Flip)
void vga_flip() {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_framebuffer[i] = vga_back_buffer[i];
    }
    
    // Обновляем аппаратный курсор
    u16 pos = cursor_y * VGA_WIDTH + cursor_x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

void vga_set_cursor(int x, int y) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        cursor_x = x;
        cursor_y = y;
        
        // Обновляем курсор сразу в буфере и на экране
        u16 pos = y * VGA_WIDTH + x;
        outb(0x3D4, 0x0F);
        outb(0x3D5, (u8)(pos & 0xFF));
        outb(0x3D4, 0x0E);
        outb(0x3D5, (u8)((pos >> 8) & 0xFF));
    }
}

void vga_get_cursor(int* x, int* y) {
    *x = cursor_x;
    *y = cursor_y;
}

void vga_scroll() {
    // Save top line to scrollback before scrolling
    vga_scrollback_save();
    // Exit scrollback view mode
    if (scrollback_viewing) vga_scrollback_reset();
    // Move all lines up by one в back buffer
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        vga_back_buffer[i] = vga_back_buffer[i + VGA_WIDTH];
    }
    
    // Clear last line в back buffer
    for (int i = 0; i < VGA_WIDTH; i++) {
        vga_back_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + i] = vga_entry(' ', current_fg, current_bg);
    }
    
    cursor_y = VGA_HEIGHT - 1;
    
    // Копируем изменения в видеопамять
    vga_flip();
}

void vga_putchar(char c) {
    // Capture mode: divert into the capture buffer, screen untouched
    if (capture_buf) {
        if (c == '\r') return;
        if (capture_pos + 1 < capture_cap) {
            capture_buf[capture_pos++] = c;
        }
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga_back_buffer[cursor_y * VGA_WIDTH + cursor_x] = vga_entry(' ', current_fg, current_bg);
            // Сразу обновляем экран для backspace
            vga_flip();
        }
    } else if (c >= ' ') {
        u16 entry = vga_entry(c, current_fg, current_bg);
        vga_back_buffer[cursor_y * VGA_WIDTH + cursor_x] = entry;
        vga_framebuffer[cursor_y * VGA_WIDTH + cursor_x] = entry;  // direct write
        cursor_x++;
    }
    
    // Handle line wrap
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }
    
    // Handle scroll
    if (cursor_y >= VGA_HEIGHT) {
        vga_scroll();
    }
}

void vga_print(const char* str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

void vga_print_color(const char* str, vga_color_t fg, vga_color_t bg) {
    vga_color_t old_fg = current_fg;
    vga_color_t old_bg = current_bg;
    current_fg = fg;
    current_bg = bg;
    vga_print(str);
    current_fg = old_fg;
    current_bg = old_bg;
}

void vga_print_decimal(int value) {
    if (value == 0) {
        vga_putchar('0');
        return;
    }
    
    char buffer[12];  // Max 10 digits + sign + null
    int i = 0;
    unsigned int uval;
    
    if (value < 0) {
        vga_putchar('-');
        uval = (unsigned int)(0 - (unsigned int)value);  // avoid UB on INT_MIN
    } else {
        uval = (unsigned int)value;
    }
    
    while (uval > 0) {
        buffer[i++] = '0' + (uval % 10);
        uval /= 10;
    }
    
    // Print in reverse order
    while (i > 0) {
        vga_putchar(buffer[--i]);
    }
}

void vga_print_unsigned(u64 value) {
    char buffer[21];  // Max 20 digits + null
    int i = 0;
    
    if (value == 0) {
        vga_putchar('0');
        return;
    }
    
    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    while (i > 0) {
        vga_putchar(buffer[--i]);
    }
}

void vga_print_int(s64 value) {
    if (value == 0) {
        vga_putchar('0');
        return;
    }
    
    if (value < 0) {
        vga_putchar('-');
        vga_print_unsigned((u64)(0 - (u64)value));  // avoid UB on INT64_MIN
    } else {
        vga_print_unsigned((u64)value);
    }
}

void vga_print_hex(u64 value) {
    vga_print("0x");
    
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[17];  // 16 hex digits + null
    int i = 0;
    
    if (value == 0) {
        vga_putchar('0');
        return;
    }
    
    u64 temp = value;
    while (temp > 0) {
        buffer[i++] = hex_chars[temp & 0xF];
        temp >>= 4;
    }
    
    while (i > 0) {
        vga_putchar(buffer[--i]);
    }
}

void vga_newline() {
    vga_putchar('\n');
}

// ============================================================
// VGA snapshot save/restore (for virtual terminal switching)
// ============================================================

void vga_save_snapshot(vga_snapshot_t* snap) {
    snap->cursor_x = cursor_x;
    snap->cursor_y = cursor_y;
    snap->fg = current_fg;
    snap->bg = current_bg;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        snap->buffer[i] = vga_back_buffer[i];
    }
}

void vga_restore_snapshot(const vga_snapshot_t* snap) {
    cursor_x = snap->cursor_x;
    cursor_y = snap->cursor_y;
    current_fg = snap->fg;
    current_bg = snap->bg;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_back_buffer[i] = snap->buffer[i];
    }
    vga_flip();
}
