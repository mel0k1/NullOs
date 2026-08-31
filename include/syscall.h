#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Linux x86_64 system call numbers (binfmt-compatible ABI).
//
// The long-term goal is binary compatibility with static Linux
// binaries linked against musl libc, so the table uses the
// standard x86_64 numbering instead of custom IDs:
//
//   rax = syscall number
//   rdi = arg1   rsi = arg2   rdx = arg3   r10 = arg4
//
// Return value in rax; negative errno-style values (-9 EBADF,
// -14 EFAULT, ...) follow the Linux convention.
// ============================================================

#define LINUX_NR_read    0
#define LINUX_NR_write   1
#define LINUX_NR_open    2     /* future */
#define LINUX_NR_close   3     /* future */
#define LINUX_NR_mmap    9
#define LINUX_NR_ioctl   16
#define LINUX_NR_poll    7
#define LINUX_NR_writev  20
#define LINUX_NR_munmap  11
#define LINUX_NR_set_tid_addr 96
#define LINUX_NR_getpid   39
#define LINUX_NR_clock_gettime 228
#define LINUX_NR_exit    60
#define LINUX_NR_readv   19
#define LINUX_NR_brk     12

#define LINUX_EPERM       1
#define LINUX_ENOENT      2
#define LINUX_EAGAIN     11
#define LINUX_EBADF       9
#define LINUX_EFAULT     14
#define LINUX_EEXIST     17
#define LINUX_ENOTDIR    20
#define LINUX_EINVAL     22
#define LINUX_ENAMETOOLONG 36

#define SYSCALL_TABLE_SIZE 64

// Core handler: num + up to 4 args (enough for read/write/exit)
u64 syscall_handler(u64 num, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);

void syscall_init(void);

// Fresh fd table for a new user process (console fds reserved)
void syscall_process_reset(void);

// Set by sys_exit; checked by syscall_entry.S to return to kernel
extern volatile u64 sys_exit_pending;

#ifdef __cplusplus
}
#endif

#endif // SYSCALL_H
