/* ============================================
 * Cooperative task context switch for NullOs
 * ============================================ */

    .section .text

/* u64 context_switch(u64 new_rsp)
 * rdi = new_rsp (pointer to saved rsp on new task's stack)
 * Returns old rsp in rax
 *
 * On entry, we save all callee-saved registers to the OLD stack,
 * then switch to the NEW stack and restore its callee-saved registers.
 *
 * Stack layout (top to bottom, i.e. low to high address):
 *   [task_switch return address]  <- rsp when entering from C
 *   [rbx] [rbp] [r12] [r13] [r14] [r15] [rip for task entry]
 *   ^-- rsp after push = the "saved rsp" stored in task->rsp
 *
 * For a fresh task, the stack is pre-set so the last qword
 * is the task entry point (jumped to via ret).
 */

    .global context_switch

context_switch:
    /* Save callee-saved registers onto CURRENT stack */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* Save current RSP into the return value */
    movq    %rsp, %rax

    /* Switch to new stack */
    movq    %rdi, %rsp

    /* Restore callee-saved registers from NEW stack */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx

    /* Return (pops return address from new stack) */
    /* For a fresh task, this "return address" is actually
     * the task entry point set by task_create */
    ret

/* u64 context_switch_park(u64 new_rsp, u64* park_out)
 * rdi = new_rsp, rsi = park_out
 *
 * Like context_switch, but stores the CURRENT (pre-switch) stack
 * pointer into *park_out BEFORE switching away. This lets a runner
 * park itself correctly: by the time the spawned task later decides
 * to park back, *park_out already holds a valid resume rsp — the
 * naive `ksp = context_switch(...)` idiom could never work, since
 * the assignment only runs after the (never-taken) return.
 */
    .global context_switch_park
context_switch_park:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    /* Save current RSP into *park_out BEFORE leaving */
    movq    %rsp, (%rsi)

    /* Switch to new stack */
    movq    %rdi, %rsp

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx

    ret
