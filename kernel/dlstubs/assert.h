/*
 * Freestanding stub for <assert.h> — used ONLY when compiling the
 * vendored dlmalloc (kernel/dlmalloc.c, see Makefile rule).
 *
 * Relevant only for -DDL_DEBUG builds: dlmalloc 2.7.2 includes
 * <assert.h> when DL_DEBUG is set (and #defines assert() to a no-op
 * otherwise, so this file is never even referenced in production
 * builds). A failed dlmalloc assertion is a kernel heap integrity
 * violation — we report loudly over serial/VGA and halt, matching
 * the kernel's panic discipline.
 */
#ifndef _DL_STUB_ASSERT_H
#define _DL_STUB_ASSERT_H

void dl_assert_fail(const char* expr, const char* file, int line)
    __attribute__((noreturn));

#define assert(x) \
    ((x) ? (void)0 : dl_assert_fail(#x, __FILE__, __LINE__))

#endif /* _DL_STUB_ASSERT_H */
