/*
 * drm.c — NullOs DRM-lite shim on /dev/dri/card0 (wlroots substrate).
 *
 * See include/drm.h for the design contract. Everything here is the
 * honest minimal subset wlroots' pixman-renderer DRM path needs:
 * mode info, dumb buffers, CRTC set, page flip with vblank-ordered
 * completion events via read().
 *
 * Display hardware: QEMU std-VGA implements the Bochs DISPI register
 * interface at io ports 0x1CE (index) / 0x1CF (data). Writing
 * VBE_DISPI_INDEX_ENABLE with ENABLED|LFB switches the emulated
 * monitor from text mode to a linear framebuffer at the card's PCI
 * BAR0. The kernel reaches the LFB through the boot-time identity
 * window (VA == PA, 2-4 GB, supervisor-only PTEs — the same window
 * the LAPIC MMIO uses).
 */

#include "../include/drm.h"
#include "../include/timer.h"
#include "../include/string.h"
#include "../include/pci.h"
#include "../include/mm.h"
#include "../include/serial.h"

/* PA-window helpers (mm.c; not part of mm.h yet) */
extern u64 vmm_pa_read_begin(void);
extern void vmm_pa_read_end(u64 saved);

/* ---- DISPI (Bochs VBE) registers — REAL Bochs/QEMU index layout ---- */
#define VBE_DISPI_INDEX_ID          0x00
#define VBE_DISPI_INDEX_XRES        0x01
#define VBE_DISPI_INDEX_YRES        0x02
#define VBE_DISPI_INDEX_BPP         0x03
#define VBE_DISPI_INDEX_ENABLE      0x04
#define VBE_DISPI_INDEX_BANK        0x05
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x06
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x07
#define VBE_DISPI_INDEX_X_OFFSET    0x08
#define VBE_DISPI_INDEX_Y_OFFSET    0x09

#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED       0x40
#define VBE_DISPI_NOCLEARMEM        0x80

static inline void outw_k(u16 port, u16 val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline u16 inw_k(u16 port)
{
    u16 v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void dispi_write(u16 idx, u16 val)
{
    outw_k(0x1CE, idx);
    outw_k(0x1CF, val);
}
static u16 dispi_read(u16 idx)
{
    outw_k(0x1CE, idx);
    return inw_k(0x1CF);
}

/* ---- card state ------------------------------------------------------ */
static bool     g_drm_ok = false;
static u64      g_lfb_pa = 0;         /* PCI BAR0 (framebuffer MMIO)    */
static u32      g_mode_w = 0, g_mode_h = 0, g_mode_bpp = 32;
static bool     g_graph_on = false;
static u32      g_flip_seq = 0;

#define DRM_MAX_FDS   4
#define DRM_MAX_DUMB  8
#define DUMB_MAX_PAGES 1024                  /* 4 MiB cap               */
#define VBLANK_PERIOD_MS 16                  /* ~60 Hz                  */

typedef struct {
    bool used;
    s32  refs;
    bool nonblock;
    bool flip_pending;
    u64  flip_deadline_ms;
    u64  flip_user;
} drm_fd_t;
static drm_fd_t g_drmfds[DRM_MAX_FDS];

typedef struct {
    bool used;
    u32  w, h, bpp, pitch;
    u64  size;
    u32  npages;
    phys_addr_t pages[DUMB_MAX_PAGES];
} dumb_t;
static dumb_t g_dumb[DRM_MAX_DUMB];

/* CRTC binding: the scanout fb (dumb[] index) */
static s32 g_crtc_fb = -1;

/* ---- device-backed region registry (munmap guard) -------------------- */
#define DEVMAP_MAX 64
typedef struct { bool used; u64 start, npages; } devmap_ent;
static devmap_ent g_devmap[DEVMAP_MAX];

void devmap_register(u64 start, u64 npages)
{
    if (!npages) return;
    for (int i = 0; i < DEVMAP_MAX; i++) {
        if (!g_devmap[i].used) {
            g_devmap[i].used = true;
            g_devmap[i].start = start;
            g_devmap[i].npages = npages;
            return;
        }
    }
}

bool devmap_contains(u64 page)
{
    for (int i = 0; i < DEVMAP_MAX; i++) {
        if (!g_devmap[i].used) continue;
        if (page >= g_devmap[i].start &&
            page < g_devmap[i].start + g_devmap[i].npages * 0x1000u)
            return true;
    }
    return false;
}

void devmap_release(u64 start, u64 npages)
{
    for (int i = 0; i < DEVMAP_MAX; i++) {
        if (!g_devmap[i].used) continue;
        if (g_devmap[i].start == start && g_devmap[i].npages == npages) {
            g_devmap[i].used = false;
            return;
        }
    }
}

/* ---- LFB access ------------------------------------------------------ */
static volatile u32* lfb_ptr(void)
{
    /* identity window: VA == PA for the 2-4 GB MMIO range */
    return (volatile u32*)(u64)g_lfb_pa;
}

static void lfb_copy_from(dumb_t* d)
{
    if (!g_graph_on || !g_lfb_pa || !d || !d->used) return;
    u32 lines = d->h;
    if (lines > g_mode_h) lines = g_mode_h;
    u32 src_pitch_px = d->pitch / 4;
    u32 dst_pitch_px = g_mode_w;
    volatile u32* dst = lfb_ptr();
    /* FIX(#lfb-pages-per-scanline): the old blit indexed d->pages[y] —
     * valid ONLY when pitch == 4096 (one page per scanline). Any other
     * width (640x480 -> pitch 2560, 301 pages for 480 lines) read
     * zero/absent page entries -> PHYS_TO_VIRT(0) NULL deref in the
     * copy loop. Walk the buffer the way it was allocated: linear
     * byte offset -> (page, offset) with page-boundary crossing. */
    for (u32 y = 0; y < lines; y++) {
        u64 row_off = (u64)y * d->pitch;
        for (u32 x = 0; x < dst_pitch_px && x < src_pitch_px; ) {
            u32 page = (u32)(row_off >> 12);
            u32 off  = (u32)(row_off & 0xFFFULL);
            if (page >= d->npages) return;   /* out of allocation    */
            u32 chunk_px = (0x1000u - off) >> 2;
            if (chunk_px == 0) chunk_px = 1; /* paranoia: never 0     */
            u32 take = src_pitch_px - x;
            if (take > chunk_px) take = chunk_px;
            if (take > dst_pitch_px - x) take = dst_pitch_px - x;
            const u32* src =
                (const u32*)(u64)(PHYS_TO_VIRT(d->pages[page]) + off);
            volatile u32* dstrow = dst + (u64)y * dst_pitch_px + x;
            for (u32 k = 0; k < take; k++) dstrow[k] = src[k];
            x += take;
            row_off += (u64)take * 4;
        }
    }
}

static void dispi_set_mode(u32 w, u32 h, u32 bpp)
{
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);      /* reset                  */
    dispi_write(VBE_DISPI_INDEX_XRES, (u16)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (u16)h);
    dispi_write(VBE_DISPI_INDEX_BPP, (u16)bpp);
    dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (u16)w);
    dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (u16)h);
    dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    dispi_write(VBE_DISPI_INDEX_ENABLE,
                VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED |
                VBE_DISPI_NOCLEARMEM);
    g_mode_w = w; g_mode_h = h; g_mode_bpp = bpp;
    g_graph_on = true;
}

static void dispi_text_back(void)
{
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
    g_graph_on = false;
    /* The LFB shares VRAM with the 0xB8000 text buffer on std-VGA
     * (legacy window aliases vram 0x18000): a graphics session
     * TRASHES the text screen. Re-init the text console so the
     * shell prompt after the compositor exits is visible again. */
    {
        extern void vga_init(void);
        vga_init();
        serial_printf("[DRM] text mode restored, console re-inited\n");
    }
}

/* ---- boot init -------------------------------------------------------- */
void drm_init(void)
{
    g_drm_ok = false;
    g_lfb_pa = 0;
    g_graph_on = false;
    g_crtc_fb = -1;
    kmemset(g_drmfds, 0, sizeof(g_drmfds));
    kmemset(g_dumb, 0, sizeof(g_dumb));

    u32 n = pci_get_device_count();
    for (u32 i = 0; i < n; i++) {
        const pci_device_t* d = pci_get_device(i);
        if (!d || d->vendor_id == 0xFFFF) continue;
        if (d->class_code != 0x03) continue;     /* DISPLAY                */
        u32 bar0 = pci_read_config(d->bus, d->dev, d->func, 0x10);
        if ((bar0 & 0x1u) == 0 && (bar0 & 0xFFFFFFF0u)) {
            g_lfb_pa = (u64)(bar0 & 0xFFFFFFF0u);
            serial_printf("[DRM] display %04x:%04x bus=%d dev=%d BAR0=%llx\n",
                          d->vendor_id, d->device_id, d->bus, d->dev,
                          (unsigned long long)g_lfb_pa);
            break;
        }
    }
    if (!g_lfb_pa) {
        serial_printf("[DRM] no display BAR0 — card0 disabled\n");
        return;
    }
    u16 id = dispi_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0u) {
        serial_printf("[DRM] DISPI id=%x unsupported — card0 disabled\n",
                      id);
        return;
    }
    g_mode_w = 1024; g_mode_h = 768; g_mode_bpp = 32;
    g_drm_ok = true;
    serial_printf("[DRM] card0 ready DISPI id=%x lfb=%llx\n", id,
                  (unsigned long long)g_lfb_pa);
}

bool drm_available(void) { return g_drm_ok; }

/* ---- fd management ----------------------------------------------------- */
s32 drm_open(u32 open_flags)
{
    if (!g_drm_ok) return -1;
    for (int i = 0; i < DRM_MAX_FDS; i++) {
        if (!g_drmfds[i].used) {
            kmemset(&g_drmfds[i], 0, sizeof(drm_fd_t));
            g_drmfds[i].used = true;
            g_drmfds[i].refs = 1;
            g_drmfds[i].nonblock = (open_flags & 0x800u) != 0;
            return i;
        }
    }
    return -1;
}

void drm_ref_inc(s32 slot)
{
    if (slot >= 0 && slot < DRM_MAX_FDS && g_drmfds[slot].used)
        g_drmfds[slot].refs++;
}

void drm_ref_dec(s32 slot)
{
    if (slot < 0 || slot >= DRM_MAX_FDS || !g_drmfds[slot].used) return;
    if (--g_drmfds[slot].refs > 0) return;
    g_drmfds[slot].used = false;
    /* last card fd closed: compositor gone — restore the text console
       and release all dumb buffers (shim ownership model)              */
    for (int i = 0; i < DRM_MAX_DUMB; i++) {
        if (!g_dumb[i].used) continue;
        for (u32 p = 0; p < g_dumb[i].npages; p++)
            pmm_free_page(g_dumb[i].pages[p]);
        g_dumb[i].used = false;
    }
    g_crtc_fb = -1;
    if (g_graph_on) dispi_text_back();
}

bool drm_slot_used(s32 slot)
{
    return slot >= 0 && slot < DRM_MAX_FDS && g_drmfds[slot].used;
}

/* ---- flip pump: complete due flips (poll/read side) -------------------- */
static bool drm_pump(s32 slot)
{
    drm_fd_t* f = &g_drmfds[slot];
    if (!f->flip_pending) return false;
    if (timer_get_uptime_ms() < f->flip_deadline_ms) return false;
    if (g_crtc_fb >= 0 && g_crtc_fb < DRM_MAX_DUMB && g_dumb[g_crtc_fb].used)
        lfb_copy_from(&g_dumb[g_crtc_fb]);
    f->flip_pending = false;
    return true;
}

bool drm_ready(s32 slot)
{
    if (slot < 0 || slot >= DRM_MAX_FDS || !g_drmfds[slot].used)
        return false;
    if (!g_drmfds[slot].flip_pending) return false;
    return timer_get_uptime_ms() >= g_drmfds[slot].flip_deadline_ms;
}

s64 drm_read(s32 slot, u8* buf, u64 count, bool nonblock)
{
    if (slot < 0 || slot >= DRM_MAX_FDS || !g_drmfds[slot].used) return -9;
    drm_fd_t* f = &g_drmfds[slot];
    if (count < sizeof(drm_event_vblank_k)) return -22;
    for (;;) {
        if (drm_pump(slot)) {
            drm_event_vblank_k ev;
            kmemset(&ev, 0, sizeof(ev));
            ev.base.type   = DRM_EVENT_FLIP_COMPLETE;
            ev.base.length = (u32)sizeof(ev);
            ev.user_data   = f->flip_user;
            u64 ms = timer_get_uptime_ms();
            ev.tv_sec  = (u32)(ms / 1000u);
            ev.tv_usec = (u32)((ms % 1000u) * 1000u);
            ev.sequence = ++g_flip_seq;
            ev.crtc_id  = DRM_NULOS_CRTC_ID;
            kmemcpy(buf, &ev, sizeof(ev));
            return (s64)sizeof(ev);
        }
        if (nonblock || f->nonblock) return -11;   /* EAGAIN */
        extern void task_yield(void);
        task_yield();
    }
}

/* ---- dumb buffers -------------------------------------------------------- */
static s32 dumb_create(u32 w, u32 h, u32 bpp, dumb_t** out)
{
    for (int i = 0; i < DRM_MAX_DUMB; i++) {
        if (g_dumb[i].used) continue;
        dumb_t* d = &g_dumb[i];
        kmemset(d, 0, sizeof(*d));
        d->w = w; d->h = h;
        d->bpp = (bpp + 7u) & ~7u;
        if (d->bpp != 32) d->bpp = 32;           /* shim: ARGB only        */
        d->pitch = w * 4;
        d->size = (u64)d->pitch * h;
        d->npages = (u32)((d->size + 0xFFFu) / 0x1000u);
        if (d->npages > DUMB_MAX_PAGES) return -1;
        for (u32 p = 0; p < d->npages; p++) {
            phys_addr_t pa = pmm_alloc_page();
            if (pa == 0) {
                for (u32 q = 0; q < p; q++) pmm_free_page(d->pages[q]);
                return -1;
            }
            d->pages[p] = pa;
            u64 s3 = vmm_pa_read_begin();
            kmemset((void*)PHYS_TO_VIRT(pa), 0, 0x1000);
            vmm_pa_read_end(s3);
        }
        d->used = true;
        *out = d;
        return i;
    }
    return -1;
}

/* ---- ioctl dispatch -------------------------------------------------------- */
/* FIX(#ioctl-arg-hole): the raw ioctl `arg` pointer (and the pointer
 * FIELDS inside the arg structs) were used completely unvalidated —
 * DRM_IOCTL_VERSION alone wrote nine u64s at ANY address, and
 * GETRESOURCES/GETCONNECTOR chased kernel pointers read from the
 * unvalidated struct. Everything now goes through the syscall layer's
 * user-range check (static bounds + kernel-window exclusion + USER
 * page walk). */
static int drm_arg_ok(u64 addr, u64 len)
{
    extern bool syscall_user_range_ok(u64, u64);
    if (!addr || !len) return -14;                 /* EFAULT        */
    return syscall_user_range_ok(addr, len) ? 0 : -14;
}

s64 drm_ioctl(s32 slot, u64 cmd, u64 arg)
{
    if (slot < 0 || slot >= DRM_MAX_FDS || !g_drmfds[slot].used) return -9;
    if (!arg) return -22;

    switch (cmd) {

    case DRM_IOCTL_VERSION: {
        /* drm_version (72 bytes): major, minor, patchlevel,
           name_len, name, date_len, date, desc_len, desc            */
        if (drm_arg_ok(arg, 72) != 0) return -14;
        u64* v = (u64*)(u64)arg;
        /* capture the caller-provided lengths BEFORE we overwrite
         * the struct (the old code tested v[3] AFTER writing 7 into
         * it — dead logic that never saw the user's name_len). */
        u64 name_len = v[3], date_len = v[5], desc_len = v[7];
        u64 name_ptr = v[4], date_ptr = v[6], desc_ptr = v[8];
        v[0] = 1; v[1] = 0; v[2] = 0;            /* shim version 1.0.0     */
        v[3] = 7;
        if (name_len >= 7 && name_ptr &&
            drm_arg_ok(name_ptr, 7) == 0) {
            const char* nm = "nullos";
            u8* dst = (u8*)name_ptr;
            for (int i = 0; i <= 6; i++) dst[i] = (u8)nm[i];
        }
        v[5] = 1;
        if (date_len >= 1 && date_ptr &&
            drm_arg_ok(date_ptr, 1) == 0) ((u8*)date_ptr)[0] = 0;
        v[7] = 1;
        if (desc_len >= 1 && desc_ptr &&
            drm_arg_ok(desc_ptr, 1) == 0) ((u8*)desc_ptr)[0] = 0;
        return 0;
    }

    case DRM_IOCTL_MODE_GETRESOURCES: {
        /* drm_mode_card_res (64 bytes) */
        if (drm_arg_ok(arg, 64) != 0) return -14;
        struct {
            u64 fb_ptr, crtc_ptr, conn_ptr, enc_ptr;
            u32 min_w, max_w, min_h, max_h;
            u32 count_fbs, count_crtcs, count_conns, count_encs;
        } __attribute__((packed)) * r = (void*)(u64)arg;
        if (r->count_crtcs && r->crtc_ptr &&
            drm_arg_ok(r->crtc_ptr, 4) == 0)
            ((u32*)(u64)r->crtc_ptr)[0] = DRM_NULOS_CRTC_ID;
        if (r->count_conns && r->conn_ptr &&
            drm_arg_ok(r->conn_ptr, 4) == 0)
            ((u32*)(u64)r->conn_ptr)[0] = DRM_NULOS_CONNECTOR_ID;
        if (r->count_encs && r->enc_ptr &&
            drm_arg_ok(r->enc_ptr, 4) == 0)
            ((u32*)(u64)r->enc_ptr)[0] = DRM_NULOS_ENCODER_ID;
        r->min_w = 640; r->max_w = 1024;
        r->min_h = 480; r->max_h = 768;
        r->count_fbs = 0;
        r->count_crtcs = 1;
        r->count_conns = 1;
        r->count_encs = 1;
        return 0;
    }

    case DRM_IOCTL_MODE_GETCONNECTOR: {
        /* drm_mode_get_connector (72 bytes) */
        if (drm_arg_ok(arg, 72) != 0) return -14;
        struct {
            u64 encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
            u32 count_modes, count_props, count_encoders;
            u32 encoder_id, connector_id, connection;
            u32 mm_width, mm_height, subpixel, pad;
        } __attribute__((packed)) * c = (void*)(u64)arg;
        static const drm_mode_modeinfo_k mode = {
            { '1','0','2','4','x','7','6','8', 0 },
            65000,
            1024, 1048, 1184, 1344,
            768, 771, 777, 806,
            0, 0x48                                /* PREFERRED|DRIVER      */
        };
        if (c->count_modes && c->modes_ptr &&
            drm_arg_ok(c->modes_ptr, sizeof(mode)) == 0)
            *(drm_mode_modeinfo_k*)(u64)c->modes_ptr = mode;
        c->count_modes = 1;
        c->count_props = 0;
        c->count_encoders = 1;
        if (c->encoders_ptr &&
            drm_arg_ok(c->encoders_ptr, 4) == 0)
            ((u32*)(u64)c->encoders_ptr)[0] = DRM_NULOS_ENCODER_ID;
        c->encoder_id = DRM_NULOS_ENCODER_ID;
        c->connector_id = DRM_NULOS_CONNECTOR_ID;
        c->connection = 1;                        /* DRM_MODE_CONNECTED    */
        c->mm_width = 304; c->mm_height = 228;
        c->subpixel = 0; c->pad = 0;
        return 0;
    }

    case DRM_IOCTL_MODE_GETENCODER: {
        /* drm_mode_get_encoder (20 bytes) */
        if (drm_arg_ok(arg, 20) != 0) return -14;
        struct {
            u32 encoder_type, encoder_id, crtc_id;
            u32 possible_crtcs, possible_clones;
        } __attribute__((packed)) * e = (void*)(u64)arg;
        e->encoder_type = 1;                      /* DRM_MODE_ENCODER_DAC  */
        e->encoder_id = DRM_NULOS_ENCODER_ID;
        e->crtc_id = DRM_NULOS_CRTC_ID;
        e->possible_crtcs = 1;
        e->possible_clones = 0;
        return 0;
    }

    case DRM_IOCTL_MODE_GETCRTC: {
        /* drm_mode_crtc (96 bytes) */
        if (drm_arg_ok(arg, 96) != 0) return -14;
        struct {
            u64 set_connectors_ptr;
            u32 count_connectors, crtc_id, fb_id, x, y, gamma_size;
            u32 mode_valid;
            drm_mode_modeinfo_k mode;
        } __attribute__((packed)) * c = (void*)(u64)arg;
        c->crtc_id = DRM_NULOS_CRTC_ID;
        if (g_crtc_fb >= 0 && g_dumb[g_crtc_fb].used)
            c->fb_id = (u32)(g_crtc_fb + 1);      /* handle = idx+1        */
        else
            c->fb_id = 0;
        c->x = 0; c->y = 0; c->gamma_size = 0;
        c->mode_valid = g_graph_on ? 1u : 0u;
        c->count_connectors = 0;
        return 0;
    }

    case DRM_IOCTL_MODE_SETCRTC: {
        if (drm_arg_ok(arg, 96) != 0) return -14;
        struct {
            u64 set_connectors_ptr;
            u32 count_connectors, crtc_id, fb_id, x, y, gamma_size;
            u32 mode_valid;
            drm_mode_modeinfo_k mode;
        } __attribute__((packed)) * c = (void*)(u64)arg;
        if (c->crtc_id != DRM_NULOS_CRTC_ID) return -22;
        s32 idx = (s32)c->fb_id - 1;
        if (idx < 0 || idx >= DRM_MAX_DUMB || !g_dumb[idx].used) return -22;
        u32 w = c->mode_valid ? c->mode.hdisplay : g_dumb[idx].w;
        u32 h = c->mode_valid ? c->mode.vdisplay : g_dumb[idx].h;
        if (!g_graph_on)
            dispi_set_mode(w, h, 32);
        g_crtc_fb = idx;
        lfb_copy_from(&g_dumb[idx]);
        serial_printf("[DRM] SETCRTC fb=%u %ux%u\n", c->fb_id, w, h);
        return 0;
    }

    case DRM_IOCTL_MODE_CREATE_DUMB: {
        /* drm_mode_create_dumb (40 bytes) */
        if (drm_arg_ok(arg, 40) != 0) return -14;
        struct {
            u64 height, width;
            u32 bpp, flags, handle, pitch;
            u64 size;
        } __attribute__((packed)) * c = (void*)(u64)arg;
        if (!c->width || !c->height || c->width > 4096u || c->height > 4096u)
            return -22;
        dumb_t* d = 0;
        s32 idx = dumb_create(c->width, (u32)c->height, c->bpp, &d);
        if (idx < 0) return -12;                  /* ENOMEM                */
        c->handle = (u32)(idx + 1);
        c->pitch = d->pitch;
        c->size = d->size;
        serial_printf("[DRM] DUMBCREATE h=%u %ux%ux%u size=%lu\n",
                      c->handle, c->width, (u32)c->height, c->bpp,
                      (unsigned long)d->size);
        return 0;
    }

    case DRM_IOCTL_MODE_MAP_DUMB: {
        /* drm_mode_map_dumb (16 bytes) */
        if (drm_arg_ok(arg, 16) != 0) return -14;
        struct {
            u32 handle, pad;
            u64 offset;
        } __attribute__((packed)) * m = (void*)(u64)arg;
        s32 idx = (s32)m->handle - 1;
        if (idx < 0 || idx >= DRM_MAX_DUMB || !g_dumb[idx].used) return -22;
        m->offset = (u64)m->handle << 32;
        return 0;
    }

    case DRM_IOCTL_MODE_DESTROY_DUMB: {
        if (drm_arg_ok(arg, 4) != 0) return -14;
        u32 handle = *(u32*)(u64)arg;
        s32 idx = (s32)handle - 1;
        if (idx < 0 || idx >= DRM_MAX_DUMB || !g_dumb[idx].used) return -22;
        for (u32 p = 0; p < g_dumb[idx].npages; p++)
            pmm_free_page(g_dumb[idx].pages[p]);
        g_dumb[idx].used = false;
        if (g_crtc_fb == idx) g_crtc_fb = -1;
        return 0;
    }

    case DRM_IOCTL_MODE_PAGEFLIP: {
        /* drm_mode_crtc_page_flip (32 bytes) */
        if (drm_arg_ok(arg, 32) != 0) return -14;
        struct {
            u32 crtc_id, fb_id, flags, pad;
            u64 user_data;
            u64 reserved;
        } __attribute__((packed)) * f = (void*)(u64)arg;
        if (f->crtc_id != DRM_NULOS_CRTC_ID) return -22;
        if (!g_graph_on) return -22;              /* flip before modeset   */
        s32 idx = (s32)f->fb_id - 1;
        if (idx < 0 || idx >= DRM_MAX_DUMB || !g_dumb[idx].used) return -22;
        g_crtc_fb = idx;
        g_drmfds[slot].flip_pending = true;
        g_drmfds[slot].flip_user = f->user_data;
        g_drmfds[slot].flip_deadline_ms =
            timer_get_uptime_ms() + VBLANK_PERIOD_MS;
        return 0;
    }

    default:
        return -25;                               /* ENOTTY                */
    }
}

/* ---- mmap hook: expose the dumb buffer's frame list --------------------- */
s32 drm_mmap_pages(u64 offset, u32 npages, phys_addr_t* out_pages)
{
    if ((offset >> 32) == 0) return -22;          /* handle lives in hi32  */
    if (offset & 0xFFFULL) return -22;
    u32 handle = (u32)(offset >> 32);
    s32 idx = (s32)handle - 1;
    if (idx < 0 || idx >= DRM_MAX_DUMB || !g_dumb[idx].used) return -22;
    u32 first = (u32)((offset & 0xFFFFFFFFu) / 0x1000u);
    if (first >= g_dumb[idx].npages) return -12;
    if ((u64)first + npages > g_dumb[idx].npages) return -12;
    for (u32 k = 0; k < npages; k++)
        out_pages[k] = g_dumb[idx].pages[first + k];
    return 0;
}
