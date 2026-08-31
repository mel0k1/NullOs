/*
 * dlcompat.c — freestanding glue for the vendored dlmalloc 2.7.2
 * (kernel/dlmalloc.c). Nothing here touches dlmalloc internals;
 * this file only supplies the small "host" surface dlmalloc expects:
 *
 *   memset / memcpy   — dlmalloc calls the plain C names; the kernel's
 *                       string.c ships kmemset/kmemcpy, we alias.
 *                       (Bonus: GCC in freestanding mode may emit calls
 *                       to memcpy/memset for struct copies anywhere in
 *                       the kernel — providing real definitions is the
 *                       canonical practice.)
 *   stderr / fprintf  — used only by dlmalloc_stats(); routed to the
 *                       serial console so heap diagnostics survive a
 *                       dead VGA and land in the QEMU serial log.
 *   errno             — lvalue for MALLOC_FAILURE_ACTION.
 *   dl_assert_fail    — DL_DEBUG builds only: halt with a report.
 *
 * Everything is reached through kernel/dlstubs/*.h which are added to
 * the include path ONLY for the dlmalloc.o compile rule (Makefile).
 */

#include <stddef.h>
#include "dlstubs/stdio.h"
#include "dlstubs/errno.h"
#include "dlstubs/assert.h"
#include <string.h>
#include "../include/vga.h"
#include "../include/serial.h"

/* ---- memset / memcpy aliases ------------------------------------ */

void* memset(void* dst, int val, size_t n) {
    return kmemset(dst, val, n);
}

void* memcpy(void* dst, const void* src, size_t n) {
    return kmemcpy(dst, src, n);
}

/* ---- stderr / fprintf → serial ----------------------------------- */

struct _dl_stub_FILE { int _unused; };
static struct _dl_stub_FILE dl_stderr_obj;
FILE* stderr = &dl_stderr_obj;

int fprintf(FILE* stream, const char* fmt, ...) {
    (void)stream;
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    serial_printf("%s", buf);
    return 0;
}

/* ---- errno -------------------------------------------------------- */

int errno = 0;

/* ---- DL_DEBUG assertion trap -------------------------------------- */

void dl_assert_fail(const char* expr, const char* file, int line) {
    serial_printf("[DLASSERT] %s at %s:%d — heap integrity broken\n",
                  expr, file, line);
    vga_print("\n*** DLMALLOC ASSERTION FAILED (see serial) ***\n");
    cli();
    while (1) { hlt(); }
}
