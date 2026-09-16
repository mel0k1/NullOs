#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- syscall trap-frame layout (must match syscall_entry.S pushes) --
// Frame base points at the saved user RIP; offsets in qwords.
#define SF_RIP    0
#define SF_CS     8
#define SF_RFLAGS 16
#define SF_RSP    24
#define SF_SS     32
#define SF_RAX    40
#define SF_R11    48
#define SF_R10    56
#define SF_R9     64
#define SF_R8     72
#define SF_RCX    80
#define SF_RDX    88
#define SF_RDI    96
#define SF_RSI    104
#define SF_RBX    112
#define SF_RBP    120
#define SF_R12    128
#define SF_R13    136
#define SF_R14    144
#define SF_R15    152
#define SF_SIZE   160

// Per-process VM region defaults (also used by syscall layer)
#define PROC_BRK_BASE   0x10000000ULL   /* 256 MB */
#define PROC_BRK_MAX    0x40000000ULL   /* 1 GB   */
#define PROC_MMAP_BASE  0x20000000ULL   /* 512 MB */
#define PROC_MMAP_MAX   0x30000000ULL   /* 768 MB */

// Pointer to the current syscall trap frame (set by syscall_entry.S
// right before calling the C handler). NULL outside syscalls.
extern u64 syscall_frame_ptr;

// Spawn a new PROCESS running `path` with argv. Returns task slot >=0.
// The caller is expected to task_run_to_completion(slot) afterwards.
s32  process_spawn(const char* path, char* const argv[], int argc);

// Exit the current process: mark zombie with code and switch away to
// the runner. Never returns.
void proc_exit_current(s32 code);

// Core implementations backing the fork/execve/wait4 syscalls.
s32  proc_do_fork(void);
s64  proc_do_execve(const char* path_u, char* const argv_u[]);
s32  proc_do_wait4(s32 pid, u64 status_user, bool nohang, bool wuntraced);

// Per-process VM cursors (fall back to defaults outside a process)
u64  proc_get_brk(void);   void proc_set_brk(u64 v);
u64  proc_get_mmap(void);  void proc_set_mmap(u64 v);

// Re-activate the CALLING process environment (cr3 + kernel stack)
// after a nested sync-run (e.g. fork's child run) returns.
void proc_reactivate_current(void);

#ifdef __cplusplus
}
#endif
#endif // PROC_H