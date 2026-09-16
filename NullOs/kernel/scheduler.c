#include "../include/scheduler.h"
#include "../include/mm.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/timer.h"
#include "../include/fpu.h"
#include "../include/pic.h"
#include "../include/serial.h"
#include "../include/gdt.h"

// Default syscall kernel stack (set by elf.c legacy path / boot).
extern u64 syscall_kstack_top;
extern u64 syscall_kstack_default;

// fd-table snapshots live in the syscall layer but are OWNED per-task:
// the global proc_fds[] array belongs to whoever is CURRENT with a
// userland pid. Every real task-switch transfers ownership (save the
// leaver's snapshot, load the enterer's). Declared here manually to
// keep scheduler.h free of syscall-layer dependencies.
extern void syscall_fds_save(task_t* t);
extern void syscall_fds_load(task_t* t);
extern void syscall_process_reset(void);

// ============================================================
// Task table and state
// ============================================================

static task_t tasks[TASK_MAX_TASKS];
static u32    task_count     = 0;  // total ever created (for IDs)
static s32    current_task_id = -1;  // -1 = kernel context
static u32    active_count   = 0;  // runnable tasks
/* IRQ preemption is fully implemented (scheduler_preempt) but OFF by
 * default until it gets more soak time — the kernel ships with the
 * proven cooperative scheduler. Toggle at runtime: `preempt on|off`. */
static bool   preempt_enabled = false;

/* Innermost sync-runner park slot. An orphan finisher (a task that was
 * preempted mid-run and completes WITHOUT owning a park slot) wakes the
 * runner through this, otherwise it would halt forever holding the CPU. */
static u64* g_park_ksp = NULL;
static volatile bool preempt_pending = false;  // set by timer IRQ

// Resume entry for tasks preempted inside an IRQ (isrs.S)
extern void irq_task_resume(void);

/* Transfer fd-table ownership across a context switch.
 *   from: slot being left  (-1 => kernel context, nothing to save)
 *   to  : slot being entered (-1/-kthread => nothing to load)
 * Cheap: PROC_FD_MAX(16) entries x2. Called at EVERY switch site
 * (task_yield / scheduler_preempt / run_to_completion / exit-away).
 * Correctness for fork-v2 depends on this: with concurrent children,
 * an execve() wiping proc_fds[] must never leak into the parent view. */
static void sched_fds_on_switch(s32 from, s32 to)
{
    task_t* ft = (from >= 0 && from < TASK_MAX_TASKS) ? &tasks[from] : NULL;
    if (ft && ft->active && ft->pid > 0) {
        syscall_fds_save(ft);
        /* #frame-ptr-park: same ownership rule as the fd table — the
         * global syscall_frame_ptr belongs to whoever is actually in
         * a syscall. Park it with the switch-out task, restore it for
         * the switch-in task (a task parked inside a blocking wait
         * resumes and may still patch its own trap frame, e.g.
         * sigchld_deliver). */
        extern u64 syscall_frame_ptr;
        ft->saved_frame = syscall_frame_ptr;
    }
    task_t* tt = (to >= 0 && to < TASK_MAX_TASKS) ? &tasks[to] : NULL;
    if (tt && tt->active && tt->pid > 0) {
        syscall_fds_load(tt);
        extern u64 syscall_frame_ptr;
        syscall_frame_ptr = tt->saved_frame;
    }
}

/* Pick the next READY (not stop-parked) task after `after`,
 * round-robin; -1 when none. Shared by yield/preempt/exit fallback. */
static s32 sched_pick_next_ready(s32 after)
{
    for (u32 i = 1; i <= TASK_MAX_TASKS; i++) {
        s32 idx = (s32)((u32)(after + i) % TASK_MAX_TASKS);
        if (idx < 0 || idx >= TASK_MAX_TASKS || idx == after) continue;
        if (tasks[idx].active && tasks[idx].state == TASK_READY &&
            !tasks[idx].stop_sig)
            return idx;
    }
    return -1;
}

// ============================================================
// task_entry_wrapper: runs the task function, then marks task finished
// and parks back to whoever sync-ran it.
// ============================================================

static void task_entry_wrapper(void) {
    task_t* t = task_get_current();
    if (t) t->started = true;
    if (t && t->entry) {
        t->entry(t->arg);
    }

    // Task finished — mark and yield
    if (t) {
        t->state = TASK_FINISHED;
        if (active_count > 0) active_count--;
    }

    // Park: switch back to the kernel frame that started us (sync-run).
    // NEVER fall through — the initial stack has no valid return address,
    // a plain `ret` here jumps to garbage and hangs the machine.
    if (t && t->park_ksp) {
        context_switch((u64)*t->park_ksp);
    }

    // Orphan finisher: we were preempted earlier and completed without a
    // park slot of our own. Wake the innermost sync-runner — otherwise we
    // would halt forever holding the CPU and starve every remaining task.
    if (g_park_ksp) {
        context_switch((u64)*g_park_ksp);
    }

    // If nobody parked us (spawned outside sync-run), halt forever.
    cli();
    for (;;) hlt();
}

// ============================================================
// Public API
// ============================================================

// Run a READY task synchronously: switches to it and returns only after
// its wrapper parks. MUST be called from kernel context (current == -1).
// With IRQ preemption enabled the task may be switched away from and back
// multiple times before it finishes — this frame only resumes on finish.
void task_run_to_completion(s32 slot)
{
    /* FIX(#r2c-bounds-order): bounds check BEFORE the diagnostic
     * printf dereferenced tasks[slot] — slot=-1 read tasks[-1]
     * (latent OOB for any future caller). */
    if (slot < 0 || slot >= TASK_MAX_TASKS) return;
    serial_printf("[R2C] enter slot=%d pid=%lu t=%lx\n", slot,
                  (unsigned long)tasks[slot].pid,
                  (unsigned long)(u64)&tasks[slot]);
    if (!tasks[slot].active || tasks[slot].state != TASK_READY) {
        /* DIAG(#bb-load): explain a refused sync-run */
        serial_printf("[R2C] REFUSED slot=%d active=%d state=%d pml4=%lx\n",
                      (int)slot, tasks[slot].active, (int)tasks[slot].state,
                      (unsigned long)tasks[slot].pml4_phys);
        return;
    }

    u64 ksp;
    tasks[slot].park_ksp = &ksp;
    g_park_ksp = &ksp;                    /* orphans wake us through this */

    /* fd-ownership handoff around the park switch: caller may be a
     * kernel thread (-1) or a NESTED process running another spawn.
     * The target gets its snapshot loaded; on return we restore ours
     * (or stdio-only defaults when back in kernel context). */
    s32 owner = current_task_id;
    sched_fds_on_switch(owner, slot);

    tasks[slot].preempt_remaining = SCHED_PREEMPT_QUANTUM;
    tasks[slot].state = TASK_RUNNING;
    current_task_id = slot;
    sched_activate(slot);
    {
        /* stage the incoming task's FPU image for any fxrstor path */
        extern u8 fpu_kernel_state[];
        kmemcpy(fpu_kernel_state, tasks[slot].fpu_state, 512);
    }

    /* Park correctly: context_switch_park stores our rsp into ksp
     * BEFORE switching away, so the finishing task's wrapper can
     * switch back to a VALID stack pointer. */
    serial_printf("[R2C] go slot=%d pid=%ld st=%d rsp=%lx pml4=%lx\n",
                  slot, (s64)tasks[slot].pid,
                  (int)tasks[slot].state,
                  (unsigned long)(u64)tasks[slot].rsp,
                  (unsigned long)tasks[slot].pml4_phys);
    context_switch_park((u64)tasks[slot].rsp, &ksp);

    // Resumed here by the wrapper's park-switch (task finished)
    serial_printf("[R2C] back from slot=%d\n", slot);
    /* fd-ownership back to the runner: save the finished zombie's
     * stale table (harmless) and restore OUR snapshot — or reset to
     * stdio-only when the runner is plain kernel context. Without the
     * kernel-context branch the finisher's (possibly execve-wiped)
     * array would stick and poison later boot-time shell work.      */
    sched_fds_on_switch(slot, owner);
    if (owner < 0) syscall_process_reset();
    current_task_id = -1;
    tasks[slot].park_ksp = NULL;
    g_park_ksp = NULL;
    sched_activate(-1);
}

void scheduler_init(void) {
    kmemset(tasks, 0, sizeof(tasks));
    task_count = 0;
    current_task_id = -1;
    active_count = 0;
    vga_print("[SCHED] Cooperative scheduler initialized\n");
}

// Reclaim slots of FINISHED tasks: free their stacks and release the
// slot. Previously finished tasks kept `active = true` forever, so each
// `spawn` permanently leaked a 64 KB stack and a task slot until the
// table ran out. Safe to call from task context (never from IRQ).
static void scheduler_reap_finished(void) {
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        if (!tasks[i].active || tasks[i].state != TASK_FINISHED) continue;
        /* #zombie-reap-race: a finished PROCESS is a zombie owned by
         * its parent's wait4. Deactivating it here made every zombie
         * vanish from the wait4/proc_wait_event scans a scheduler
         * tick after exit, so busybox ash's `wait` never found its
         * background children (only deliveries that won the race
         * against this reclaimer ever reaped). Zombies with a living
         * parent are untouchable here; orphans are released now. */
        if (tasks[i].zombie) {
            s32 ppid = tasks[i].ppid;
            bool parent_alive = false;
            if (ppid > 0) {
                for (s32 j = 0; j < TASK_MAX_TASKS; j++) {
                    if (tasks[j].active && tasks[j].pid == ppid) {
                        parent_alive = true;
                        break;
                    }
                }
            }
            if (parent_alive) continue;      /* wait4 owns this zombie */
            { extern void proc_reap_orphan(task_t* t);
              proc_reap_orphan(&tasks[i]); } /* orphan: release MM now */
            continue;
        }
        tasks[i].active = false;
        if (tasks[i].stack) {
            kfree(tasks[i].stack);
            tasks[i].stack = NULL;
        }
    }
}

s32 task_create(const char* name, void (*entry)(void*), void* arg, u64 stack_size) {
    // Free resources of already-finished tasks first
    scheduler_reap_finished();

    // Find free slot
    s32 slot = -1;
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        if (!tasks[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        vga_print("[SCHED] ERROR: no free task slots\n");
        return -1;
    }

    // Allocate stack
    if (stack_size == 0) stack_size = TASK_MAX_STACK_SIZE;
    if (stack_size < 4096) stack_size = 4096;
    stack_size = (stack_size + 15) & ~15ULL;  // 16-byte align

    u8* stack = (u8*)kmalloc(stack_size);
    if (!stack) {
        vga_print("[SCHED] ERROR: cannot allocate task stack\n");
        return -1;
    }
    kmemset(stack, 0, stack_size);

    task_t* t = &tasks[slot];
    kmemset(t, 0, sizeof(task_t));

    t->id = task_count++;
    if (name) {
        kstrncpy(t->name, name, TASK_NAME_MAX - 1);
    } else {
        ksnprintf(t->name, TASK_NAME_MAX, "task-%u", (u32)t->id);
    }
    t->state  = TASK_READY;
    t->stack  = stack;
    t->stack_size = stack_size;
    t->entry  = entry;
    t->arg    = arg;
    t->active = true;
    t->preempt_remaining = SCHED_PREEMPT_QUANTUM;
    t->park_ksp = NULL;

    /* Fresh tasks need a VALID FPU state image: they may be resumed
     * through the IRQ tail's fxrstor, and restoring zeros faults. */
    {
        static u8 init_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(64)));
        static bool fpu_template_ready = false;
        if (!fpu_template_ready) {
            __asm__ volatile("fninit");
            fpu_save(init_fpu_state);
            fpu_template_ready = true;
        }
        kmemcpy(t->fpu_state, init_fpu_state, FPU_STATE_SIZE);
    }

    // Set up initial stack so that context_switch's `ret`
    // jumps to task_entry_wrapper.
    //
    // Stack grows downward. We put the "return address"
    // at the top of the stack. When context_switch restores
    // callee-saved regs and does `ret`, it pops this address.
    //
    // context_switch does:
    //   save: push rbx, rbp, r12, r13, r14, r15  (6 qwords)
    //   switch rsp
    //   restore: pop r15, r14, r13, r12, rbp, rbx  (6 qwords, LIFO)
    //   ret -> pops 1 qword (the return address)
    // So total = 7 qwords from rsp.
    //
    // Stack layout (high addr to low):
    //   sp[-1] = task_entry_wrapper  (return address)
    //   sp[-2] = r15  (popped first by context_switch)
    //   sp[-3] = r14
    //   sp[-4] = r13
    //   sp[-5] = r12
    //   sp[-6] = rbp
    //   sp[-7] = rbx  (popped last)  <- rsp points here

    u64* sp = (u64*)(stack + stack_size);

    // Return address (popped by `ret` in context_switch)
    sp[-1] = (u64)task_entry_wrapper;

    // Callee-saved registers: context_switch pushes rbx,rbp,r12,r13,r14,r15
    // then pops r15,r14,r13,r12,rbp,rbx (reverse/LIFO order).
    // So on the stack (low addr = top of stack):
    //   [rsp+0]  = r15  (popped first)
    //   [rsp+8]  = r14
    //   [rsp+16] = r13
    //   [rsp+24] = r12
    //   [rsp+32] = rbp
    //   [rsp+40] = rbx  (popped last)
    //   [rsp+48] = return address (popped by ret)
    sp[-2] = 0;  // r15
    sp[-3] = 0;  // r14
    sp[-4] = 0;  // r13
    sp[-5] = 0;  // r12
    sp[-6] = 0;  // rbp
    sp[-7] = 0;  // rbx

    // rsp for this task points to sp[-7] (where r15 is, first to be popped)
    t->rsp = &sp[-7];

    active_count++;

    vga_printf("[SCHED] Task '%s' created (id=%u, stack=%u KB)\n",
               t->name, (u32)t->id, (u32)(stack_size / 1024));
    return (s32)t->id;
}

void task_yield(void) {
    // Find next ready task
    s32 old_id = current_task_id;
    s32 next_id = -1;

    // Round-robin: start searching after current task
    if (old_id >= 0) {
        s32 start = (old_id + 1) % TASK_MAX_TASKS;
        for (u32 i = 0; i < TASK_MAX_TASKS; i++) {
            s32 idx = (start + i) % TASK_MAX_TASKS;
            if (tasks[idx].active && tasks[idx].state == TASK_READY &&
                !tasks[idx].stop_sig) {          /* stopped => not runnable */
                next_id = idx;
                break;
            }
        }
    } else {
        // Kernel context: find any ready task
        for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
            if (tasks[i].active && tasks[i].state == TASK_READY &&
                !tasks[i].stop_sig) {
                next_id = i;
                break;
            }
        }
    }

    if (next_id < 0) {
        // No READY tasks: the current task simply keeps running.
        // (The old code cleared current_task_id here, which orphaned
        // sync-run tasks mid-flight — their wrapper could no longer
        // find its TCB to park back to the runner and hlt'd forever.)
        //
        // FORK-V2 (#spin-sleep): if we are formally SLEEPING there is
        // nobody to hand over to AND returning would burn CPU at full
        // speed inside nanosleep. Park silently until OUR OWN deadline
        // flips us back (scheduler_tick runs on IRQs which fire freely
        // inside the hlt window). No other agent can steal the CPU
        // meanwhile: scans only ever run from whoever holds it — us.
        if (old_id >= 0 && tasks[old_id].state == TASK_SLEEPING) {
            /* [BISECT-C1-DISABLED] self-wake variant restored to dbg5
             * semantics: busy-hlt until deadline WITHOUT tick-hide. */
            while ((int)tasks[old_id].state == TASK_SLEEPING &&
                   timer_get_ticks() < tasks[old_id].wake_tick) {
                __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
            }
            return;
        }
        // FORK-V2 (#irq-starvation): a spinner here (e.g. wait4 with
        // nothing reapable yet) holds the CPU with IF=0 FOREVER under
        // v1 sync-run this loop exited instantly; under concurrency it
        // would permanently mask the timer -> sleeping tasks never
        // wake, keyboard dies, deadlock. One safe breath opens the
        // interrupt window every pass (hlt returns on first IRQ).
        __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
        return;
    }

    // Mark current task as ready (if it was running)
    if (old_id >= 0 && tasks[old_id].active && tasks[old_id].state == TASK_RUNNING) {
        tasks[old_id].state = TASK_READY;
    }

    // Switch to next task
    tasks[next_id].state = TASK_RUNNING;
    tasks[next_id].preempt_remaining = SCHED_PREEMPT_QUANTUM;
    current_task_id = next_id;
    /* #pipe-fd-switch FIX: transfer fd-table ownership on the
     * cooperative switch path TOO. The fork-v2 comment below claimed
     * this transfer for years, but the call itself was MISSING — a
     * fork child first resumed through task_yield started on the
     * parent's stale global view (no snapshot load), so ash
     * pipelines lost pipe fds / EOF state. Same contract as
     * task_run_to_completion / sched_exit_switch_away. */
    sched_fds_on_switch(old_id, next_id);
    sched_activate(next_id);
    {
        extern u8 fpu_kernel_state[];
        kmemcpy(fpu_kernel_state, tasks[next_id].fpu_state, 512);
    }

    // FPU save/restore around context switch
    // Save current task's FPU state
    if (old_id >= 0 && tasks[old_id].active) {
        fpu_save(tasks[old_id].fpu_state);
    }

    /* FORK-V2 FIX (#lazy-rsp): record the LEAVER's continuation
     * EAGERLY. The naive `old_rsp = context_switch(...); tasks[old]= `
     * idiom records nothing until the leaver is resumed AGAIN — under
     * concurrent tasks any third party picking a yield-suspended task
     * would then use a STALE rsp and re-enter consumed code. Park-store
     * writes the post-push top BEFORE switching away: every READY slot
     * is pickable from now on (same contract preemption/fresh tasks
     * always had). */
    {
        s32 leaver = old_id;
        u64 new_rsp = (u64)tasks[next_id].rsp;
        if (leaver >= 0 && leaver < TASK_MAX_TASKS) {
            /* park_out writes the post-push rsp INTO our field:
             * *(u64*)&tasks[leaver].rsp = <top-of-leaver-frame> */
            context_switch_park(new_rsp, (u64*)&tasks[leaver].rsp);
        } else {
            context_switch(new_rsp);      /* kernel ctx: nothing to save */
        }
    }

    // We resume here when somebody ELSE switched back to US (through
    // another task's yield/pick). FPU restore for whoever is current.
    if (current_task_id >= 0) {
        fpu_restore(tasks[current_task_id].fpu_state);
    }
}

void task_sleep_ms(u64 ms) {
    task_t* t = task_get_current();
    if (!t) {
        // Kernel context: just busy-wait
        timer_sleep_ms(ms);
        return;
    }

    t->state = TASK_SLEEPING;
    // Avoid overflow: distribute multiplication
    u64 whole_secs = ms / 1000ULL;
    u64 rem_ms = ms % 1000ULL;
    u64 target = timer_get_ticks() + whole_secs * TIMER_FREQ + (rem_ms * TIMER_FREQ + 999) / 1000;

    // Without storing the deadline the task would be woken by
    // scheduler_tick() immediately (wake_tick was still 0).
    t->wake_tick = target;
    serial_printf("[SLEEP] pid=%ld now=%lu tgt=%lu\n",
                  (s64)t->pid, (unsigned long)timer_get_ticks(),
                  (unsigned long)target);

    // Yield to another task
    // We need to yield, but task_yield will find next READY task.
    // Sleeping tasks are skipped.
    task_yield();
}

// Called from timer_handler to wake sleeping tasks and trigger preemption
void scheduler_tick(void) {
    { extern void vmm_pml4_guard(int); vmm_pml4_guard(6); }
    u64 now = timer_get_ticks();
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        if (tasks[i].active && tasks[i].state == TASK_SLEEPING) {
            if (now >= tasks[i].wake_tick) {
                tasks[i].state = TASK_READY;
                tasks[i].preempt_remaining = SCHED_PREEMPT_QUANTUM;
                serial_printf("[WAKE] slot=%d pid=%ld now=%lu tgt=%lu\n",
                              i, (s64)tasks[i].pid,
                              (unsigned long)now,
                              (unsigned long)tasks[i].wake_tick);
            }
        }
    }

    // Preemptive: signal that we should switch on next yield check
    if (preempt_enabled && current_task_id >= 0) {
        task_t* cur = &tasks[current_task_id];
        if (cur->active && cur->state == TASK_RUNNING) {
            if (cur->preempt_remaining > 0) {
                cur->preempt_remaining--;
            }
            if (cur->preempt_remaining == 0) {
                preempt_pending = true;
                cur->preempt_remaining = SCHED_PREEMPT_QUANTUM;
            }
        }
    }
}

void sched_set_preempt(bool enabled) {
    preempt_enabled = enabled;
}

bool sched_get_preempt(void) {
    return preempt_enabled;
}

// Check and perform preemption if pending (call from safe points)
void sched_check_preempt(void) {
    if (preempt_pending && current_task_id >= 0) {
        preempt_pending = false;
        task_yield();
    }
}

// ============================================================
// True preemption from the timer IRQ (vector 32).
//
// Called from irq_handler after scheduler_tick(), still on the
// interrupted task's kernel stack, interrupts disabled by the IRQ
// gate. If another task is READY:
//
//  1. Save the current task's FPU state.
//  2. Plant a context_switch-format continuation frame BELOW the C
//     handler's frame (in the abandoned locals area of irq_handler /
//     scheduler_preempt — we never return through them). Its "return
//     address" is irq_task_resume (isrs.S): on later resume it loads
//     the saved frame base into rsp and falls into the shared IRQ
//     tail (fxrstor + pops + iretq), so the task continues exactly as
//     if the interrupt had just returned.
//     NOTE: planting must go DOWNWARD — above the frame there may be
//     no mapped stack left (shallow interrupt depth).
//  3. Send EOI ourselves — we never return to irq_handler's EOI.
//  4. Stage the next task's FPU state into fpu_kernel_state so the
//     tail's fxrstor restores the RIGHT state, then context_switch.
//
// registers_t is 22 qwords (r15@0 ... ss@168); at C entry rsp sits
// just below the frame (base-8 is irq_handler's return address).
// ============================================================
#define REGS_SIZE         176

static u32 g_preempt_switches = 0;
u32 scheduler_preempt_count(void) { return g_preempt_switches; }

// ============================================================
// Execution-environment activation (address space + kernel stack)
// ============================================================

static u64 cr3_current = 0;   /* 0 = unknown */

void sched_activate(s32 slot) {
    task_t* t = (slot >= 0 && slot < TASK_MAX_TASKS && tasks[slot].active)
                ? &tasks[slot] : NULL;

    u64 want = (t && t->pml4_phys) ? t->pml4_phys
                                   : /*kernel*/ vmm_get_kernel_pml4();
    if (cr3_current != want) {
        vmm_switch_pml4(want);            /* also retargets vmm "active" */
        cr3_current = want;
    }

    syscall_kstack_top = (t && t->pid > 0) ? t->kstack_top
                                           : syscall_kstack_default;
    if (t && t->pid > 0) {
        tss_set_kernel_stack(t->kstack_top);
    }
    /* #fs-base-no-switch: restore the user TLS base UNCONDITIONALLY on
     * every switch-in. The MSR persists across tasks, and an execve
     * swaps images — a stale base points into the PREVIOUS image's
     * TLS (ash died on musl's stack-canary read %fs:0x28 after a
     * fork+exec child with a different image had moved the MSR). */
    wrmsr(0xC0000100, (t && t->fs_base) ? t->fs_base : 0ULL);
}

s32 sched_current_pid(void) {
    task_t* t = task_get_current();
    return t ? t->pid : 0;
}

/* Accessor for proc.c (needs the table to allocate slots) */
void* scheduler_task_table(void) {
    return (void*)tasks;
}

void sched_active_count_add(void) { active_count++; }
void sched_active_count_sub(void) { if (active_count > 0) active_count--; }

void sched_set_current(s32 slot) {
    current_task_id = slot;
}

/* Called by a finishing PROCESS (proc_exit_current): abandon this
 * context and resume whoever waits for us — our sync-runner if we
 * have one, else the innermost runner (orphan wake). Never returns.
 * Falls into hlt only when nobody at all is waiting. */
void sched_exit_switch_away(void) {
    task_t* t = task_get_current();
    /* We are typically inside a SYSCALL handler here: FMASK cleared
     * IF on entry. The context we switch TO must run with interrupts
     * enabled (the runner/shell hlt()s waiting for input), so enable
     * them BEFORE the stack switch — same handoff rule as preemption. */
    sti();
    /* fork-v2: persist OUR fd-table before abandoning the CPU — the
     * zombie keeps an accurate snapshot, and whichever task runs next
     * loads its own through the transfer below or its own switches. */
    if (t) syscall_fds_save(t);
    if (t && t->park_ksp) {
        context_switch((u64)*t->park_ksp);
    }
    /* NOTE: the old global g_park_ksp wake lives here in v1 lore but
     * is DELIBERATELY GONE: with concurrent (fork-v2) children the
     * innermost runner might be parked for a DIFFERENT slot than ours
     * — waking it would make the runner believe ITS sync-run finished
     * (classic wrong-bird wake). Ring-3 processes either carry their
     * OWN park_ksp (spawn/r2c targets, branch above) or are v2
     * launched (fork_resume_child continuations, park_ksp==NULL,
     * handled by the direct hand-off below). Kernel threads never
     * reach this function (their wrapper owns the orphan logic). */
    /* fork-v2 orphan-exit: nobody parked awaiting us (no sync-runner).
     * Old behaviour — cli();hlt() forever — WEDGED the whole box once
     * concurrent tasks exist, because current_task_id still pointed at
     * this dead slot and nothing would ever pick another READY task.
     * Hand the CPU directly to the next runnable sibling instead. */
    s32 me = task_get_id();
    s32 nxt = sched_pick_next_ready(me);
    serial_printf("[EXIT-AWAY] me=%d nxt=%d st=%d rsp=%lx\n", me,
                  nxt,
                  (nxt >= 0) ? (int)tasks[nxt].state : -1,
                  (nxt >= 0) ? (unsigned long)(u64)tasks[nxt].rsp : 0UL);
    if (nxt >= 0) {
        task_t* n = &tasks[nxt];
        n->preempt_remaining = SCHED_PREEMPT_QUANTUM;
        n->state = TASK_RUNNING;
        current_task_id = nxt;
        g_preempt_switches++;             /* direct handoff            */
        sched_fds_on_switch(me, nxt);     /* self already saved above  */
        sched_activate(nxt);
        {
            extern u8 fpu_kernel_state[];
            kmemcpy(fpu_kernel_state, n->fpu_state, 512);
        }
        /* Dying context is abandoned mid-frame (like every other exit
         * path): we never return through this C frame. */
        context_switch((u64)n->rsp);
        for (;;) { __asm__ volatile ("hlt"); }
    }
    /* fork-v2 idle-sentinel: no sibling was runnable AT THIS INSTANT,
     * but timer ticks may mark sleepers READY any moment — and under
     * concurrency the LAST exited context still "holds" the CPU
     * pointer. Old code hlt'd TERMINALLY here, wedging everything
     * woke up later (observed: [WAKE] fires, then eternal silence).
     * Rescan-on-interrupt instead: wake on every IRQ, look again,
     * hand off when something became runnable. This dying frame is
     * otherwise irrelevant — abandon it the moment we find a heir. */
    for (;;) {
        __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
        s32 nx2 = sched_pick_next_ready(current_task_id);
        if (nx2 < 0) continue;
        {
            task_t* n2 = &tasks[nx2];
            serial_printf("[EXIT-IDLE] handoff -> %d pid=%ld\n", nx2,
                          (s64)n2->pid);
            n2->preempt_remaining = SCHED_PREEMPT_QUANTUM;
            n2->state = TASK_RUNNING;
            current_task_id = nx2;
            g_preempt_switches++;
            sched_fds_on_switch(-1, nx2);
            sched_activate(nx2);
            {
                extern u8 fpu_kernel_state[];
                kmemcpy(fpu_kernel_state, n2->fpu_state, 512);
            }
            fpu_restore(n2->fpu_state);
            context_switch((u64)n2->rsp);
            for (;;) { __asm__ volatile ("hlt"); }
        }
    }
}

void scheduler_preempt(registers_t* regs)
{
    if (!preempt_enabled) return;
    /* Only switch on quantum expiry (flag set by scheduler_tick) */
    if (!preempt_pending) return;
    preempt_pending = false;

    s32 cur_id = current_task_id;
    if (cur_id < 0 || cur_id >= TASK_MAX_TASKS) return;
    task_t* cur = &tasks[cur_id];
    if (!cur->active || cur->state != TASK_RUNNING) return;
    if (!regs || regs->int_num != 32) return;

    /* Pick next READY task, round-robin after current */
    s32 next = -1;
    for (u32 i = 1; i < TASK_MAX_TASKS; i++) {
        s32 idx = (s32)((cur_id + i) % TASK_MAX_TASKS);
        if (idx == cur_id) continue;
        if (tasks[idx].active && tasks[idx].state == TASK_READY &&
            !tasks[idx].stop_sig) {              /* stopped => not runnable */
            next = idx;
            break;
        }
    }
    if (next < 0) return;                    /* sole runner: keep going */

    /* FPU: save outgoing; stage INCOMING so the shared IRQ-tail
     * fxrstor restores the right state for whoever we switch to.
     * Fresh (never-run) tasks don't pass the tail — no staging harm. */
    fpu_save(cur->fpu_state);
    {
        extern u8 fpu_kernel_state[];
        kmemcpy(fpu_kernel_state, tasks[next].fpu_state, 512);
    }

    /* Plant THIS task's continuation in its own TCB, then switch
     * DIRECTLY to next's parked context:
     *   - fresh task  -> context_switch rets into its entry wrapper
     *   - preempted   -> rets into irq_task_resume via its cont frame */
    u64* c = cur->cont;
    c[0] = 0;                            /* rbx */
    c[1] = 0;                            /* rbp */
    c[2] = 0;                            /* r12 */
    c[3] = 0;                            /* r13 */
    c[4] = 0;                            /* r14 */
    c[5] = 0;                            /* r15 */
    c[6] = (u64)&irq_task_resume;        /* 'ret' target            */
    c[7] = (u64)regs;                    /* frame base for popq rsp */
    cur->rsp = c;

    cur->state = TASK_READY;
    tasks[next].state = TASK_RUNNING;
    tasks[next].preempt_remaining = SCHED_PREEMPT_QUANTUM;
    current_task_id = next;
    g_preempt_switches++;

    /* #pipe-fd-switch FIX: fd-table transfer on the preemptive switch
     * path (was missing — same root as the task_yield site). */
    sched_fds_on_switch(cur_id, next);

    /* Stage the incoming task's FPU image for the shared fxrstor */
    {
        extern u8 fpu_kernel_state[];
        kmemcpy(fpu_kernel_state, tasks[next].fpu_state, 512);
    }
    sched_activate(next);

    /* This IRQ will never return through its own tail: ACK it now */
    pic_send_eoi(0);

    /* Interrupt-flag handoff: we are inside an interrupt gate, so IF=0
     * right now. A task resumed through its saved IRET frame restores
     * its own flags — but a FRESH task (entering via the wrapper's
     * `ret`) would inherit IF=0 and run forever without ticks. Enable
     * interrupts for that case right before the stack switch; a nested
     * tick in the one-instruction window lands on the abandoned frame
     * and is harmless (pending is already cleared). */
    if (!tasks[next].started) {
        __asm__ volatile ("sti");
    }

    context_switch((u64)tasks[next].rsp);

    /* Unreachable: the switch lands in `next` and this frame is abandoned */
    for (;;) { __asm__ volatile ("hlt"); }
}

bool task_is_finished(s32 id) {
    if (id < 0 || id >= TASK_MAX_TASKS) return true;
    return !tasks[id].active || tasks[id].state == TASK_FINISHED;
}

// Map a task ID (from task_create) to its table slot. IDs and slots
// diverge as soon as finished tasks are reaped and slots get reused.
s32 task_slot_of(s32 id) {
    if (id < 0) return -1;
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        if (tasks[i].active && tasks[i].id == (u64)id) return i;
    }
    return -1;
}

s32 task_get_id(void) {
    return current_task_id;
}

task_t* task_get_current(void) {
    if (current_task_id < 0 || current_task_id >= TASK_MAX_TASKS) return NULL;
    if (!tasks[current_task_id].active) return NULL;
    return &tasks[current_task_id];
}

const char* task_get_name(s32 id) {
    if (id < 0 || id >= TASK_MAX_TASKS) return "(none)";
    if (!tasks[id].active) return "(none)";
    return tasks[id].name;
}

u32 task_get_count(void) {
    return active_count;
}

void task_list(void) {
    vga_print("\nTask List:\n");
    vga_print("-----------\n");
    vga_printf("%-4s %-16s %-10s %-8s\n", "ID", "Name", "State", "Stack");
    vga_print("---- ---------------- ---------- --------\n");

    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        if (!tasks[i].active) continue;

        const char* state_str;
        switch (tasks[i].state) {
            case TASK_READY:    state_str = "READY"; break;
            case TASK_RUNNING:  state_str = "RUNNING"; break;
            case TASK_SLEEPING: state_str = "SLEEPING"; break;
            case TASK_FINISHED: state_str = "FINISHED"; break;
            default:            state_str = "???"; break;
        }

        vga_printf("%-4u %-16s %-10s %u KB\n",
                   (u32)tasks[i].id, tasks[i].name,
                   state_str, (u32)(tasks[i].stack_size / 1024));
    }
    vga_printf("\nTotal active: %u tasks\n", active_count);
}
