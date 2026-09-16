/*
 * Freestanding stub for <errno.h> — used ONLY when compiling the
 * vendored dlmalloc (kernel/dlmalloc.c, see Makefile rule).
 *
 * dlmalloc 2.7.2 needs errno as a settable lvalue for the default
 * MALLOC_FAILURE_ACTION ("errno = ENOMEM;"). The kernel owns the
 * single definition in dlcompat.c.
 */
#ifndef _DL_STUB_ERRNO_H
#define _DL_STUB_ERRNO_H

extern int errno;

#define ENOMEM 12
#define EINVAL 22
#define EAGAIN 11
#define EINTR  4
#define ENOSYS 38

#endif /* _DL_STUB_ERRNO_H */
