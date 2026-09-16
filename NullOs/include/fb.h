#ifndef FB_H
#define FB_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Framebuffer info
#define FB_WIDTH   1024
#define FB_HEIGHT  768
#define FB_BPP     32

// Pixel format (ARGB)
typedef struct {
    u8  b;
    u8  g;
    u8  r;
    u8  a;
} fb_pixel_t;

static inline fb_pixel_t fb_make_pixel(u8 r, u8 g, u8 b) {
    fb_pixel_t p;
    p.r = r; p.g = g; p.b = b; p.a = 255;
    return p;
}

static inline u32 fb_pack(fb_pixel_t p) {
    return (u32)p.a << 24 | (u32)p.b << 16 | (u32)p.g << 8 | p.r;
}

// Framebuffer state
typedef struct {
    bool    active;        // True if in graphical mode
    u32*    framebuffer;   // Pointer to FB memory
    u32     pitch;         // Bytes per scanline
    u32     width;
    u32     height;
    u32     bpp;           // Bits per pixel
    u32     size;          // Total framebuffer size in bytes
    // Back buffer (allocated in kernel heap)
    u32*    backbuffer;
} fb_info_t;

// Try to set VBE mode via Bochs VBE extensions (works in QEMU/Bochs)
// Returns true on success
bool fb_init(u32 width, u32 height, u32 bpp);

// Switch back to text mode
void fb_shutdown(void);

// Pixel operations (draw to back buffer)
void fb_put_pixel(int x, int y, u32 color);
u32 fb_get_pixel(int x, int y);
void fb_fill_rect(int x, int y, int w, int h, u32 color);
void fb_draw_line(int x0, int y0, int x1, int y1, u32 color);
void fb_draw_char(int x, int y, char c, u32 fg, u32 bg);

// Copy back buffer to framebuffer
void fb_flip(void);

// Clear back buffer
void fb_clear(u32 color);

// Get framebuffer info
const fb_info_t* fb_get_info(void);

// Print to framebuffer (simple bitmap font)
void fb_print(int x, int y, const char* str, u32 fg);

// Shell command: fb test
void cmd_fb(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // FB_H
