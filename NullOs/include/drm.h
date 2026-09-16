#ifndef DRM_H
#define DRM_H

/*
 * drm — NullOs DRM-lite shim on /dev/dri/card0 (wlroots substrate).
 *
 * Layer-2 dwl-readiness (README step 5): wlroots' DRM backend talks
 * ioctl(DRM_*) on a card device. NullOs has no GEM/TTM/EGL; the honest
 * translation layer is:
 *
 *   - QEMU std-VGA "Bochs DISPI" registers (io 0x1CE/0x1CF) switch the
 *     emulated display into a real linear framebuffer mode;
 *   - the LFB physical base is the card's PCI BAR0 (identity-mapped
 *     2-4 GB window, supervisor-only PTEs);
 *   - dumb buffers are plain PMM frames; CREATE_DUMB/MAP_DUMB/mmap
 *     hand them to ring 3 (wl_shm-style page tables);
 *   - SETCRTC programs DISPI and copies the scanout fb to the LFB;
 *     PAGEFLIP queues the copy at the next simulated vblank (~60 Hz,
 *     deadline from the PIT uptime clock) and read() on the card fd
 *     returns the drm_event_vblank completion — the exact contract
 *     libdrm's drmHandleEvent consumes.
 *
 * ioctl command numbers and struct layouts match libdrm's drm.h for
 * x86_64 so future ports can reuse real headers.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ioctl command numbers (libdrm x86_64) ------------------------- */
#define DRM_IOCTL_VERSION        0xC0486400u
#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0u
#define DRM_IOCTL_MODE_GETCRTC   0xC06064A1u
#define DRM_IOCTL_MODE_SETCRTC   0xC06064A2u
#define DRM_IOCTL_MODE_GETENCODER 0xC01464A6u
#define DRM_IOCTL_MODE_GETCONNECTOR 0xC04864A7u
#define DRM_IOCTL_MODE_PAGEFLIP  0xC02064B0u
#define DRM_IOCTL_MODE_CREATE_DUMB 0xC02864B2u
#define DRM_IOCTL_MODE_MAP_DUMB  0xC01064B3u
#define DRM_IOCTL_MODE_DESTROY_DUMB 0xC00464B4u

/* drm_event / drm_event_vblank (read() on the card fd) */
#define DRM_EVENT_VBLANK        1
#define DRM_EVENT_FLIP_COMPLETE 2

typedef struct {
    u32 type;
    u32 length;
} drm_event_hdr_k;

typedef struct {
    drm_event_hdr_k base;        /* type=2, length=32 */
    u64 user_data;
    u32 tv_sec, tv_usec;
    u32 sequence;
    u32 crtc_id;
} drm_event_vblank_k;

/* drm_mode_modeinfo (60 bytes) */
typedef struct {
    char name[32];
    u32  clock;
    u16  hdisplay, hsync_start, hsync_end, htotal;
    u16  vdisplay, vsync_start, vsync_end, vtotal;
    u32  flags, type;
} drm_mode_modeinfo_k;

/* ---- object ids (stable, shim-chosen) ------------------------------ */
#define DRM_NULOS_CRTC_ID       10
#define DRM_NULOS_ENCODER_ID    20
#define DRM_NULOS_CONNECTOR_ID  25

/* ---- kernel API (syscall.c dispatch) -------------------------------- */
void drm_init(void);                        /* boot: PCI BAR0 + DISPI probe */
bool drm_available(void);

s32  drm_open(u32 open_flags);              /* fd slot or -1                */
void drm_ref_inc(s32 slot);
void drm_ref_dec(s32 slot);                 /* last: text mode back         */
bool drm_slot_used(s32 slot);
bool drm_ready(s32 slot);                   /* flip event deliverable?      */
s64  drm_read(s32 slot, u8* buf, u64 count, bool nonblock);
s64  drm_ioctl(s32 slot, u64 cmd, u64 arg); /* all DRM_IOCTL_MODE_*         */

/* mmap hook: offset encodes (handle << 32); fills out_pages with the
 * dumb buffer's physical frames for syscall.c to map (VA policy lives
 * there, same as the memfd branch). Returns 0 or negative errno. */
s32 drm_mmap_pages(u64 offset, u32 npages, phys_addr_t* out_pages);

/* device-backed region registry (munmap must NOT free these frames;
 * also used by the memfd wl_shm path). Returns true if `page` is
 * inside a registered mapping (caller then unmaps without freeing). */
void devmap_register(u64 start, u64 npages);
bool devmap_contains(u64 page);
void devmap_release(u64 start, u64 npages); /* exact-range unregister       */

#ifdef __cplusplus
}
#endif

#endif /* DRM_H */
