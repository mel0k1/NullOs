/*
 * drmtest — NullOs DRM-lite E2E prober (Layer-2 dwl readiness).
 *
 * Exercises /dev/dri/card0 the way wlroots' DRM backend does:
 *   1. open + DRM_IOCTL_VERSION (driver name "nullos")
 *   2. GETRESOURCES / GETCONNECTOR / GETENCODER topology walk
 *   3. CREATE_DUMB 1024x768x32 -> MAP_DUMB -> mmap (frame list)
 *   4. paint pattern A, SETCRTC (DISPI modeset to the real LFB)
 *   5. CREATE_DUMB B, paint pattern B, PAGEFLIP -> blocking read()
 *      returns the drm_event_vblank completion (user_data match)
 *   6. verify pattern B bytes through the mapping, destroy buffers
 *      and close (kernel restores VGA text mode)
 *
 * Host-side screendumps (HMP screendump) verify the visible pixels.
 * exit(0) == all stages passed. Prints DRMTEST ALL-PASS on the text
 * console after close (text mode is back by then).
 */

#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_mmap      9
#define SYS_munmap    11
#define SYS_ioctl     16
#define SYS_exit      60
#define SYS_timerfd_create  253
#define SYS_timerfd_settime 254

#define O_RDWR        2
#define PROT_READ     1
#define PROT_WRITE    2
#define MAP_SHARED    1

#define DRM_IOCTL_VERSION           0xC0486400u
#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0u
#define DRM_IOCTL_MODE_GETCRTC      0xC06064A1u
#define DRM_IOCTL_MODE_SETCRTC      0xC06064A2u
#define DRM_IOCTL_MODE_GETENCODER   0xC01464A6u
#define DRM_IOCTL_MODE_GETCONNECTOR 0xC04864A7u
#define DRM_IOCTL_MODE_PAGEFLIP     0xC02064B0u
#define DRM_IOCTL_MODE_CREATE_DUMB  0xC02864B2u
#define DRM_IOCTL_MODE_MAP_DUMB     0xC01064B3u
#define DRM_IOCTL_MODE_DESTROY_DUMB 0xC00464B4u

#define DRM_EVENT_FLIP_COMPLETE 2

#define W 1024
#define H 768
#define PITCH (W * 4)

struct modeinfo_k {
    char name[32];
    unsigned int clock;
    unsigned short hdisplay, hsync_start, hsync_end, htotal;
    unsigned short vdisplay, vsync_start, vsync_end, vtotal;
    unsigned int flags, type;
};

static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}
static long sc3(long nr, long a, long b, long c) {
    return sc6(nr, a, b, c, 0, 0, 0);
}
static long sc2(long nr, long a, long b) { return sc3(nr, a, b, 0); }
static long sc1(long nr, long a) { return sc3(nr, a, 0, 0); }

static void wr(const char* s, long n) { sc3(SYS_write, 1, (long)s, n); }
static void say(const char* s) { long n = 0; while (s[n]) n++; wr(s, n); }
static void sayu(const char* s, long v) {
    char b[64]; long n = 0;
    while (s[n]) n++;
    for (long i = 0; i < n; i++) b[i] = s[i];
    char d[24]; long di = 0;
    if (v == 0) d[di++] = '0';
    while (v > 0) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
    for (long i = di - 1; i >= 0; i--) b[n++] = d[i];
    b[n++] = '\n';
    wr(b, n);
}

/* one-shot timerfd sleep (proven in wltest t2) */
static void sleep_ms(long ms) {
    long tfd = sc3(SYS_timerfd_create, 1 /*CLOCK_MONOTONIC*/, 0, 0);
    if (tfd < 0) { for (volatile long i = 0; i < 20000000; i++) {} return; }
    struct { struct { long sec, nsec; } interval, value; } its;
    its.interval.sec = 0; its.interval.nsec = 0;
    its.value.sec = ms / 1000;
    its.value.nsec = (ms % 1000) * 1000000L;
    sc3(SYS_timerfd_settime, tfd, 0, (long)&its);
    unsigned long exp;
    sc3(SYS_read, tfd, (long)&exp, 8);
    sc1(SYS_close, tfd);
}

int main(void) {
    say("[DRMTEST] start\n");

    long fd = sc3(SYS_open, (long)"/dev/dri/card0", O_RDWR, 0);
    if (fd < 0) { say("[DRMTEST] FAIL open\n"); return 1; }

    /* 1. VERSION */
    {
        unsigned long v[9];
        for (int i = 0; i < 9; i++) v[i] = 0;
        char namebuf[16] = {0};
        v[3] = 16; v[4] = (unsigned long)namebuf;
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_VERSION, (long)v) != 0) {
            say("[DRMTEST] FAIL version\n"); return 1;
        }
        if (namebuf[0] != 'n' || namebuf[1] != 'u') {
            say("[DRMTEST] FAIL driver name\n"); return 1;
        }
        sayu("[DRMTEST] driver v", v[0]); /* expect 1.0.0 */
    }

    /* 2. topology */
    {
        unsigned long r[8];   /* drm_mode_card_res: 64 bytes */
        unsigned int* rc32 = (unsigned int*)r;
        for (int i = 0; i < 16; i++) rc32[i] = 0;
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_GETRESOURCES, (long)r) != 0) {
            say("[DRMTEST] FAIL resources\n"); return 1;
        }
        if (rc32[13] /* count_crtcs */ != 1) {
            say("[DRMTEST] FAIL crtcs\n"); return 1;
        }
        unsigned int crtc_ids[4], conn_ids[4], enc_ids[4];
        r[1] = (unsigned long)crtc_ids; rc32[13] = 1;
        r[2] = (unsigned long)conn_ids; rc32[14] = 1;
        r[3] = (unsigned long)enc_ids;  rc32[15] = 1;
        sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_GETRESOURCES, (long)r);
        if (crtc_ids[0] == 0 || conn_ids[0] == 0) {
            say("[DRMTEST] FAIL ids\n"); return 1;
        }
        sayu("[DRMTEST] topology crtc ", crtc_ids[0]);

        /* connector: pull the mode (drm_mode_get_connector: 72 bytes) */
        unsigned char cb[72];
        for (int i = 0; i < 72; i++) cb[i] = 0;
        struct modeinfo_k modes[1];
        for (int i = 0; i < 60; i++) ((char*)modes)[i] = 0;
        unsigned long* q = (unsigned long*)cb;
        unsigned int* c32 = (unsigned int*)(cb + 32);
        q[0] = 0;                        /* encoders_ptr */
        q[1] = (unsigned long)modes;     /* modes_ptr    */
        q[2] = 0; q[3] = 0;
        c32[0] = 1;                      /* count_modes @32 */
        c32[4] = conn_ids[0];            /* connector_id @48 */
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_GETCONNECTOR, (long)cb) != 0) {
            say("[DRMTEST] FAIL connector\n"); return 1;
        }
        if (modes[0].hdisplay != 1024 || modes[0].vdisplay != 768) {
            say("[DRMTEST] FAIL mode geometry\n"); return 1;
        }
        sayu("[DRMTEST] mode ", modes[0].hdisplay);
        sayu("[DRMTEST] x ", modes[0].vdisplay);
    }

    /* 3. dumb buffer A + mmap */
    unsigned long hA, hB;
    unsigned int pitchA = 0, pitchB = 0;
    unsigned long szA = 0, szB = 0;
    unsigned long mapA, mapB;
    {
        unsigned char cd[40];
        unsigned long* q = (unsigned long*)cd;
        unsigned int* c32 = (unsigned int*)(cd + 16);
        q[0] = H; q[1] = W;
        c32[0] = 32; c32[1] = 0;         /* bpp, flags */
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_CREATE_DUMB, (long)cd) != 0) {
            say("[DRMTEST] FAIL create A\n"); return 1;
        }
        hA = c32[2]; pitchA = c32[3]; szA = q[4]; /* size @32 */
        if (pitchA != PITCH || szA != (unsigned long)PITCH * H) {
            say("[DRMTEST] FAIL dumb A geometry\n"); return 1;
        }
        sayu("[DRMTEST] dumbA handle ", hA);

        unsigned char cm[16];
        unsigned int* m32 = (unsigned int*)cm;
        unsigned long* m64 = (unsigned long*)cm;
        m32[0] = (unsigned int)hA; m32[1] = 0;
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_MAP_DUMB, (long)cm) != 0) {
            say("[DRMTEST] FAIL map A\n"); return 1;
        }
        if (m64[1] != ((unsigned long)hA << 32)) {
            say("[DRMTEST] FAIL map A offset\n"); return 1;
        }
        mapA = sc6(SYS_mmap, 0, szA, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, m64[1]);
        if ((long)mapA < 0 && (long)mapA > -4096) {
            say("[DRMTEST] FAIL mmap A\n"); return 1;
        }
        sayu("[DRMTEST] mmapA ", mapA >> 12);
    }

    /* 4. pattern A + SETCRTC */
    {
        volatile unsigned int* px = (volatile unsigned int*)mapA;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                px[y * W + x] = 0xFF000000u |
                                ((unsigned int)(x * 255 / W) << 16) |  /* R */
                                ((unsigned int)(y * 255 / H) << 8)  |  /* G */
                                0x40u;                                 /* B */
        /* SETCRTC with the mode from the connector */
        unsigned char cs[96];
        for (int i = 0; i < 96; i++) cs[i] = 0;
        unsigned long* q = (unsigned long*)cs;
        unsigned int* c32 = (unsigned int*)(cs + 8);
        struct modeinfo_k modes[1];
        for (int i = 0; i < 60; i++) ((char*)modes)[i] = 0;
        modes[0].hdisplay = 1024; modes[0].vdisplay = 768;
        c32[1] = 10;                     /* crtc_id */
        c32[2] = (unsigned int)hA;       /* fb_id   */
        c32[6] = 1;                      /* mode_valid */
        /* copy mode struct at offset 36 */
        char* dst = cs + 36;
        char* src = (char*)modes;
        for (int i = 0; i < 60; i++) dst[i] = src[i];
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_SETCRTC, (long)cs) != 0) {
            say("[DRMTEST] FAIL setcrtc\n"); return 1;
        }
        say("[DRMTEST] SETCRTC-OK pattern A up\n");
    }

    sleep_ms(2000);

    /* 5. buffer B + PAGEFLIP + event read */
    {
        unsigned char cd[40];
        unsigned long* q = (unsigned long*)cd;
        unsigned int* c32 = (unsigned int*)(cd + 16);
        q[0] = H; q[1] = W;
        c32[0] = 32; c32[1] = 0;
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_CREATE_DUMB, (long)cd) != 0) {
            say("[DRMTEST] FAIL create B\n"); return 1;
        }
        hB = c32[2]; pitchB = c32[3]; szB = q[4]; /* size @32 */

        unsigned char cm[16];
        unsigned int* m32 = (unsigned int*)cm;
        unsigned long* m64 = (unsigned long*)cm;
        m32[0] = (unsigned int)hB; m32[1] = 0;
        sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_MAP_DUMB, (long)cm);
        mapB = sc6(SYS_mmap, 0, szB, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, m64[1]);
        if ((long)mapB < 0 && (long)mapB > -4096) {
            say("[DRMTEST] FAIL mmap B\n"); return 1;
        }

        volatile unsigned int* px = (volatile unsigned int*)mapB;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                px[y * W + x] = ((x / 32) & 1) ? 0xFF20FF20u  /* green  */
                                               : 0xFF2020FFu;  /* red   */

        unsigned char cf[32];            /* drm_mode_crtc_page_flip */
        unsigned int* f32 = (unsigned int*)cf;
        unsigned long* f64 = (unsigned long*)cf;
        f32[0] = 10;                     /* crtc_id   @0  */
        f32[1] = (unsigned int)hB;       /* fb_id     @4  */
        f32[2] = 0;                      /* flags     @8  */
        f32[3] = 0;                      /* pad       @12 */
        f64[2] = 0xDEADBEEFul;           /* user_data @16 */
        if (sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_PAGEFLIP, (long)cf) != 0) {
            say("[DRMTEST] FAIL pageflip\n"); return 1;
        }

        unsigned char ev[64];
        long n = sc3(SYS_read, fd, (long)ev, 64);
        if (n != 32) { say("[DRMTEST] FAIL event read\n"); return 1; }
        unsigned int* e32 = (unsigned int*)ev;
        unsigned long* e64 = (unsigned long*)ev;
        if (e32[0] != DRM_EVENT_FLIP_COMPLETE || e64[1] != 0xDEADBEEFul) {
            say("[DRMTEST] FAIL event content\n"); return 1;
        }
        sayu("[DRMTEST] FLIP-OK seq ", e32[6]);

        /* readback through our own mapping */
        if (px[10 * W + 10] != 0xFF2020FFu) {
            say("[DRMTEST] FAIL readback\n"); return 1;
        }
        say("[DRMTEST] READBACK-OK\n");
    }

    sleep_ms(3000);                      /* keep pattern B on screen    */

    /* 6. teardown */
    {
        unsigned int dh = (unsigned int)hA;
        sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_DESTROY_DUMB, (long)&dh);
        dh = (unsigned int)hB;
        sc3(SYS_ioctl, fd, DRM_IOCTL_MODE_DESTROY_DUMB, (long)&dh);
        sc2(SYS_munmap, mapA, szA);
        sc2(SYS_munmap, mapB, szB);
        sc1(SYS_close, fd);
        say("[DRMTEST] closed — text console back\n");
        say("[DRMTEST] ALL-PASS\n");
    }
    return 0;
}
