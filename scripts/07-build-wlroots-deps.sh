#!/bin/bash
# 07-build-wlroots-deps.sh — pixman + libdrm + libdisplay-info -> stage/
# (musl-static; deps wlroots-0.20). Каждый со своим E2E-смоуком:
#   pixman  — software composite: solid red fill, проверка пикселя
#   libdrm  — linkage + drmGetDevices2(-EBADF) на мусорном fd
#   ldi     — разбор синтетического EDID 128Б (manufacturer = "NUL")
# ЛОВНИ Task 25: UAPI-заголовки ядра (asm/, linux/) обязаны лежать в
# musl-sysroot (копируются из linux-libc-dev хоста — см. worklog);
# hwdata (pnp.ids) нужен libdisplay-info на сборке.
set -eu
ROOT=/home/z/my-project; SRC=$ROOT/src; BUILD=$ROOT/build; STAGE=$ROOT/stage
. $ROOT/scripts/env-build.sh
mkdir -p "$BUILD/userland-musl"

# ---------- 0) smokes: пишем исходники РАНЬШЕ всего ----------
cat > /tmp/pixsmoke.c <<'EOF'
/* pixsmoke: solid red fill through pixman_image_fill_rectangles. */
#include <pixman.h>
#include <stdio.h>
int main(void) {
    uint32_t buf[64] = {0};
    pixman_image_t *dst = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, 8, 8, buf, 8 * 4);
    if (!dst) { puts("[pixsmoke] FAIL image"); return 1; }
    pixman_color_t red = { 0xffff, 0x0000, 0x0000, 0xffff };
    pixman_rectangle16_t r = { 0, 0, 8, 8 };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &red, 1, &r);
    pixman_image_unref(dst);
    int ok = (buf[0] == 0xffff0000u) && (buf[63] == 0xffff0000u);
    printf("[pixsmoke] px0=%08x px63=%08x %s\n", buf[0], buf[63],
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
EOF

cat > /tmp/drmsmoke.c <<'EOF'
/* drmsmoke: static libdrm linkage + ABI-shape probe. drmGetDevices2 on a
 * bogus fd must fail with -EBADF; drmGetVersion must reject a bogus fd. */
#include <stdio.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(void) {
    int n = drmGetDevices2(0, NULL, 0);
    printf("[drmsmoke] drmGetDevices2(badfd)=%d (expect negative)\n", n);
    drmVersionPtr v = drmGetVersion(-1);
    int ok = (n < 0) && (v == NULL);
    printf("[drmsmoke] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
EOF

cat > /tmp/ldismoke.c <<'EOF'
/* ldismoke: parse a synthetic 128-byte EDID; manufacturer must be "NUL". */
#include <stdio.h>
#include <string.h>
#include <libdisplay-info/info.h>
#include <libdisplay-info/edid.h>
int main(void) {
    unsigned char e[128];
    memset(e, 0, sizeof(e));
    e[0]=0x00; e[1]=0xFF; e[2]=0xFF; e[3]=0xFF; e[4]=0xFF; e[5]=0xFF;
    e[6]=0xFF; e[7]=0x00;
    /* PNP id "NUL": N=14,U=21,L=12 -> (14<<10)|(21<<5)|12 = 0x3AAC */
    e[8]=0x3A; e[9]=0xAC;
    e[10]=0x01; e[11]=0x00;         /* product code */
    e[16]=1; e[17]=26;              /* week, year 2026 */
    e[18]=1; e[19]=4;               /* EDID 1.4 */
    e[20]=0x80;                     /* digital */
    e[21]=30; e[22]=20;             /* 30x20 cm */
    for (int i = 54; i < 125; i += 18) e[i+3]=0x10;  /* unused desc */
    e[125]=0; e[126]=0;
    unsigned s = 0;
    for (int i = 0; i < 127; i++) s += e[i];
    e[127] = (unsigned char)((256 - (s & 0xFF)) & 0xFF);

    struct di_info *info = di_info_parse_edid(e, sizeof(e));
    if (!info) { puts("[ldismoke] FAIL parse"); return 1; }
    const struct di_edid *edid = di_info_get_edid(info);
    char mfr[4] = {0};  /* vp->manufacturer is char[3]: NOT NUL-terminated */
    int wcm = 0, hcm = 0;
    if (edid) {
        const struct di_edid_vendor_product *vp =
            di_edid_get_vendor_product(edid);
        if (vp) {
            mfr[0] = vp->manufacturer[0];
            mfr[1] = vp->manufacturer[1];
            mfr[2] = vp->manufacturer[2];
        }
        const struct di_edid_screen_size *ss = di_edid_get_screen_size(edid);
        if (ss) { wcm = ss->width_cm; hcm = ss->height_cm; }
    }
    int ok = (strcmp(mfr, "NUL") == 0);
    printf("[ldismoke] mfr=%s %dx%dcm ", mfr, wcm, hcm);
    printf("%s", ok ? "PASS" : "FAIL");
    di_info_destroy(info);
    return (mfr && strcmp(mfr, "NUL") == 0) ? 0 : 1;
}
EOF

# ---------- 1) pixman (meson, static musl) ----------
cd "$BUILD"
[ -d pixman-0.46.4 ] || tar xf "$SRC/pixman.tar.gz"
rm -rf pixman-build
meson setup pixman-build pixman-0.46.4 --prefix="$STAGE" --libdir=lib \
  --default-library=static \
  -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled > pix-setup.log 2>&1 \
  || { tail -30 pix-setup.log; exit 1; }
ninja -C pixman-build > pix-build.log 2>&1 || { tail -30 pix-build.log; exit 1; }
meson install -C pixman-build > pix-install.log 2>&1
[ -f "$STAGE/lib/libpixman-1.a" ] && echo "[OK] pixman -> stage"

# ---------- 2) libdrm (meson, static musl, минимальные драйверы) ----------
# ЛОВНЯ: intel/radeon/amdgpu/nouveau/vmwgfx/man-pages — feature-опции
# (enabled/disabled/auto), tests — boolean. Смешивать нельзя.
cd "$BUILD"
[ -d libdrm-2.4.134 ] || tar xf "$SRC/libdrm.tar.xz"
rm -rf libdrm-build
meson setup libdrm-build libdrm-2.4.134 --prefix="$STAGE" --libdir=lib \
  --default-library=static \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
  -Dvmwgfx=disabled -Dtests=false -Dman-pages=disabled > drm-setup.log 2>&1 \
  || { tail -30 drm-setup.log; exit 1; }
ninja -C libdrm-build > drm-build.log 2>&1 || { tail -30 drm-build.log; exit 1; }
meson install -C libdrm-build > drm-install.log 2>&1
[ -f "$STAGE/lib/libdrm.a" ] && echo "[OK] libdrm -> stage"

# ---------- 3) libdisplay-info (meson, static musl) ----------
cd "$BUILD"
[ -d libdisplay-info-0.3.0 ] || tar xf "$SRC/libdisplay-info.tar.bz2"
rm -rf ldi-build
meson setup ldi-build libdisplay-info-0.3.0 --prefix="$STAGE" --libdir=lib \
  --default-library=static > ldi-setup.log 2>&1 \
  || { tail -30 ldi-setup.log; exit 1; }
ninja -C ldi-build > ldi-build.log 2>&1 || { tail -30 ldi-build.log; exit 1; }
meson install -C ldi-build > ldi-install.log 2>&1
[ -f "$STAGE/lib/libdisplay-info.a" ] && echo "[OK] libdisplay-info -> stage"

# ---------- 4) компиляция и запуск смоуков ----------
$CC -static -Os -o "$BUILD/userland-musl/pixmansmoke" /tmp/pixsmoke.c \
  $(pkg-config --cflags pixman-1) $(pkg-config --libs pixman-1)
$CC -static -Os -o "$BUILD/userland-musl/drmsmoke" /tmp/drmsmoke.c \
  $(pkg-config --cflags libdrm) $(pkg-config --libs libdrm)
$CC -static -Os -o "$BUILD/userland-musl/ldismoke" /tmp/ldismoke.c \
  $(pkg-config --cflags libdisplay-info) $(pkg-config --libs libdisplay-info) -lm

file "$BUILD/userland-musl/"*smoke | grep -c "statically linked"

"$BUILD/userland-musl/pixmansmoke" || exit 1
"$BUILD/userland-musl/drmsmoke" || exit 1
"$BUILD/userland-musl/ldismoke" || exit 1
echo "[MILESTONE] wlroots-deps Layer-0.6 builds PASS"
