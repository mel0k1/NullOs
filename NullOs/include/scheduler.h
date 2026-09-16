#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"
#include "fpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_MAX_STACK_SIZE  (64 * 1024)  // 64 KB per task
#define TASK_MAX_TASKS        32
#define TASK_NAME_MAX         32

// Preemptive scheduling
#define SCHED_PREEMPT_QUANTUM  10  // ticks between preemptions (100ms at 100Hz)

// Task states
typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_FINISHED
} task_state_t;

// Task control block
typedef struct task {
    u64            id;
    char           name[TASK_NAME_MAX];
    task_state_t   state;

    // Stack
    u8*            stack;        // Bottom of allocated stack (kmalloc'd)
    u64            stack_size;
    u64*           rsp;          // Current stack pointer

    // For sleep
    u64            wake_tick;    // Tick when task should wake

    // Entry and context
    void (*entry)(void* arg);
    void*          arg;

    bool           active;       // Slot in use
    bool           started;      // Wrapper has run at least once
    u64            preempt_remaining;  // Ticks until preemption
    u64*           park_ksp;     // Sync-runner's park slot (run_to_completion)

    // Fork-v2: set while the task is self-blocking inside task_yield's
    // no-heir branch (inline hlt loop). While set, scheduler_tick MUST
    // NOT flip this task SLEEPING->READY: its continuation is NOT
    // parked anywhere yet, and a premature READY would let another
    // task pick a CONSUMED rsp (observed as CS=0x8 fetch faults).
    volatile u8    yield_block;

    // ---- Process extension (fork/exec/wait4) -----------------------
    u64            pml4_phys;    // Process address space; 0 = kthread
    u64            kstack_base;  // kmalloc'd kernel stack (processes)
    u64            kstack_top;   // syscall-entry stack top for this task
    s32            pid;          // >0 unique; 0 = kernel thread
    s32            ppid;         // creator pid
    s32            pgrp;         // POSIX process group leader pid
    s32            exit_code;
    bool           zombie;       // finished, awaiting wait4()
    // ---- Job-control stop state (partial signal heart) -------------
    u32            stop_sig;     // nonzero: parked by SIGSTOP(19)/SIGTSTP(20);
                                 // scheduler never dispatches until SIGCONT.
    bool           stop_reported;// one-shot latch: this stop already handed
                                 // to the parent via wait4(WUNTRACED).
    // ---- Minimal signal machine (#sigchld-mach) --------------------
    // SIGCHLD only: ash's dowait() installs a handler and blocks in
    // rt_sigsuspend until it fires; without real delivery it spins
    // forever. act/restorer come from rt_sigaction (musl passes the
    // SA_RESTORER trampoline), pending is latched by child exit, and
    // delivery happens at rt_sigsuspend (syscall return path patches
    // the trap frame). rt_sigreturn restores from the kernel-built
    // blob on the user stack.
    u64            sig_act;      // SIGCHLD handler VA; 0 = default(ignore)
    u64            sig_restorer; // SA_RESTORER trampoline VA
    u32            sig_pending;  // SIGCHLD pending latch
    u32            sig_in_handler; // no nested delivery inside handler
    // ---- syscall_frame_ptr ownership (#frame-ptr-park) -------------
    // The global syscall_frame_ptr is clobbered by ANY task's syscall
    // entry. A task parked INSIDE a syscall (blocking wait/yield) must
    // get its own frame pointer back on switch-in, or late delivery
    // paths (sigchld_deliver) patch a dead frame of another task.
    u64            saved_frame;  // syscall_frame_ptr at switch-out
    u64            user_rip;     // initial user entry (spawn path)
    u64            user_rsp;
    u64            brk_cur;      // per-process program break
    u64            mmap_cur;     // per-process mmap cursor
    u64            fs_base;      // user TLS base (MSR_FS_BASE) — restored
                                 // on every switch-in (#fs-base-no-switch)
    u8             fd_used[16];  // fd-table snapshot (syscall layer)
    s32            fd_fs[16];

    // Continuation frame used when this task is preempted inside an
    // IRQ: a context_switch-format frame whose 'return address' is
    // irq_task_resume and whose top holds the saved IRQ frame base.
    u64            cont[8];

    // FPU/SSE state (512 bytes, 64-byte aligned)
    u8             fpu_state[FPU_STATE_SIZE] __attribute__((aligned(FPU_ALIGN)));
} task_t;

// Initialize scheduler (call once, before any tasks)
void scheduler_init(void);

// Create a new task; returns task ID or -1 on error
s32 task_create(const char* name, void (*entry)(void*), void* arg, u64 stack_size);

// Yield to next ready task (cooperative switch)
void task_yield(void);

// Kernel context: run a READY task until its wrapper parks, then resume.
void task_run_to_completion(s32 slot);

// Sleep for at least `ms` milliseconds (cooperative)
void task_sleep_ms(u64 ms);

// Get current task ID, or -1 if kernel context
s32 task_get_id(void);

// Get current task pointer (or NULL if none)
task_t* task_get_current(void);

// Get task name by ID
const char* task_get_name(s32 id);

// Get number of active tasks
u32 task_get_count(void);

// Scheduler tick — call from timer_handler (wakes sleeping + preemption)
void scheduler_tick(void);

// Preemptive switch from timer IRQ context: parks the interrupted task
// with a resume continuation and context_switches to the next READY task.
// Called from irq_handler (vector 32) AFTER scheduler_tick; sends its own
// EOI when it actually switches. `regs` is the saved IRQ frame.
void scheduler_preempt(registers_t* regs);

// True if the task slot exists and has finished
bool task_is_finished(s32 id);

// Map a task ID (task_create result) to its table slot (-1 if gone)
s32 task_slot_of(s32 id);

// Total IRQ-driven preemption switches performed so far
u32 scheduler_preempt_count(void);

// Activate a task's execution environment: switch CR3 to its address
// space (kernel PML4 for threads / -1) and point syscall_entry at its
// kernel stack. Called at every context-switch site.
void sched_activate(s32 slot);

// Current task's pid (0 in kernel context)
s32 sched_current_pid(void);

// Wake the innermost sync-runner (orphan finisher / process exit).
// Never returns: switches away and eventually hlt()s.
void sched_exit_switch_away(void);

// Runnable-task counter maintenance for the process layer
void sched_active_count_add(void);
void sched_active_count_sub(void);

// Restore the "current task" pointer after a nested sync-run returns
// (proc_do_fork resumes the parent mid-syscall; without this the
// parent's subsequent syscalls report pid 0).
void sched_set_current(s32 slot);

// Print task list (for debug)
void task_list(void);

// Enable/disable preemption
void sched_set_preempt(bool enabled);
bool sched_get_preempt(void);

// Internal: assembly context switch (implemented in task_switch.S)
// Switches from current stack to new_rsp
// Returns the old stack pointer in the return value
u64 context_switch(u64 new_rsp);

// Like context_switch, but stores the pre-switch rsp into *park_out
// BEFORE switching away (self-parking for sync-runners).
u64 context_switch_park(u64 new_rsp, u64* park_out);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULER_H
