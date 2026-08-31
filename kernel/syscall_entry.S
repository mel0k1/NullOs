/* ============================================================
 * syscall_entry.S — NullOs Linux-ABI syscall entry/exit
 * ============================================================
 *
 * SYSCALL does NOT switch stacks in hardware (unlike interrupts),
 * so the very first instructions save the user RSP and load the
 * dedicated kernel stack. NOTHING is ever pushed on the user stack.
 *
 * Full GPR + CPU-state frame built on the KERNEL stack
 * (offsets from final RSP, low→high):
 *
 *   +0    user RIP (copy)        ┐
 *   +8    user CS   (0x23)       │ IRETQ block
 *   +16   user RFLAGS            │
 *   +24   user RSP               │
 *   +32   user SS   (0x1B)       ┘
 *   +40   rax  (nr → ret value)
 *   +48   r11  (user rflags copy)
 *   +56   r10  (arg4)
 *   +64   r9   (unused)
 *   +72   r8   (unused)
 *   +80   rcx  (user rcx)
 *   +88   rdx  (arg3)
 *   +96   rdi  (arg1)
 *   +104  rsi  (arg2)
 *   +112  rbx
 *   +120  rbp
 *   +128  r12
 *   +136  r13
 *   +144  r14
 *   +152  r15
 *
 * C handler:  syscall_handler(nr, a1, a2, a3, a4)
 *   rdi=nr  rsi=a1  rdx=a2  rcx=a3  r8=a4
 *
 * Exit to user : restore GPRs, IRETQ consumes the low 5 qwords.
 * Exit to kernel (sys_exit): abandon process, jump to shell.
 * ============================================================ */

    .section .text
    .global syscall_entry
    .extern syscall_handler
    .extern syscall_kstack_top
    .extern syscall_user_rsp
    .extern handle_user_exit
    .extern sys_exit_pending
    .global syscall_frame_ptr
    .global syscall_restore_user
    .global fork_resume_child

#define USER_CS  0x23    /* UCODE_SEL | RPL3 */
#define USER_DS  0x1B    /* UDATA_SEL | RPL3 */
#define KERN_DS  0x10    /* GDT_KDATA_SEL    */

syscall_entry:
    /* ---- switch stacks FIRST (before any push) --------------------*/
    movq    %rsp, syscall_user_rsp(%rip)     /* save user RSP        */
    movq    syscall_kstack_top(%rip), %rsp   /* load kernel stack    */

    /* Stash the syscall number IMMEDIATELY: the segment loads below
     * go through %ax and would otherwise DESTROY it (this exact bug
     * made every syscall arrive as nr=0x10).                          */
    pushq   %rax

    /* kernel data segments (entry inherits user DS/ES/FS/GS)
     * FIX(#exec-big/C): FS and GS are left EXACTLY as user set them.
     * Loading a selector into FS clears its hidden 64-bit base, which
     * silently destroyed musl's TLS pointer installed via
     * arch_prctl(ARCH_SET_FS) -> first %fs:0 access after any syscall
     * crashed (BusyBox exit/stdio unlock path). Long mode ignores
     * DS/ES bases for normal addressing; kernel code never derefs
     * through FS/GS. */
    movw    $KERN_DS, %ax
    movw    %ax, %ds
    movw    %ax, %es

    cld                                       /* C code expects DF=0  */

    popq    %rax                              /* restore nr           */

    /* ---- build full frame on the kernel stack ---------------------*/
    /* Chronological pushes; the last five form the IRETQ image.      */
    pushq   %r15                              /* +152                 */
    pushq   %r14                              /* +144                 */
    pushq   %r13                              /* +136                 */
    pushq   %r12                              /* +128                 */
    pushq   %rbp                              /* +120                 */
    pushq   %rbx                              /* +112                 */
    pushq   %rsi                              /* +104 arg2            */
    pushq   %rdi                              /* +96  arg1            */
    pushq   %rdx                              /* +88  arg3            */
    pushq   %rcx                              /* +80  user rcx        */
    pushq   %r8                               /* +72  arg5 slot       */
    pushq   %r9                               /* +64  arg6 slot       */
    pushq   %r10                              /* +56  arg4            */
    pushq   %r11                              /* +48  user rflags     */
    pushq   %rax                              /* +40  syscall number  */

    pushq   $USER_DS                          /* +32 user SS          */
    pushq   syscall_user_rsp(%rip)            /* +24 user RSP         */
    pushq   %r11                              /* +16 user RFLAGS      */
    pushq   $USER_CS                          /* +8  user CS          */
    pushq   %rcx                              /* +0  user RIP         */

    /* ---- call C handler: (nr, a1, a2, a3, a4, a5, a6) -------------*/
    /* Frame offsets per ACTUAL push order (20 pushes total):
     *   nr=[+40](rax)  a1=[+96](rdi)  a2=[+104](rsi)
     *   a3=[+88](rdx)  a4=[+56](r10)  a5=[+72](r8)  a6=[+64](r9)       */
    movq    40(%rsp), %rdi                    /* nr                   */
    movq    96(%rsp), %rsi                    /* arg1                 */
    movq    104(%rsp), %rdx                   /* arg2                 */
    movq    88(%rsp), %rcx                    /* arg3                 */
    movq    56(%rsp), %r8                     /* arg4                 */
    movq    72(%rsp), %r9                     /* arg5 (=> C p6)       */
    movq    64(%rsp), %rax                    /* arg6 (scratch->rax)  */
    /* Capture trap-frame base NOW: rsp points exactly at the saved
     * user-RIP slot (SF_RIP). proc.c reads/writes the frame through
     * this pointer for fork/execve.                                */
    movq    %rsp, syscall_frame_ptr(%rip)

    /* ---- 7th parameter (user arg6) on the stack -------------------
     * FIX(#a6-off-by-one,#jc-regress): historical layout pushed arg7
     * FIRST and then padded, so the SysV 7th-parameter slot the
     * callee reads ([rsp] at call time) held the pad garbage. The
     * reanchor rewrite worked but disturbed park/wait4 resume flows
     * (#jc-regress); this THIRD shape keeps the proven sequence,
     * reserving both slots UP FRONT and placing arg7 exactly at
     * [rsp] before CALL:
     *   pre-call rsp  = F-16         (same as ever -> parity kept)
     *   callee arg7   = (ret@F-24)+8 = F-16 == [rsp]             */
    subq    $16, %rsp                         /* pad + arg7 slot      */
    movq    %rax, (%rsp)                      /* arg7 at call-base    */
    call    syscall_handler
    addq    $16, %rsp                         /* symmetric cleanup    */

    /* store return value into the rax slot                           */
    movq    %rax, 40(%rsp)

    /* ---- sys_exit requested? abandon process ----------------------*/
    cmpq    $0, sys_exit_pending(%rip)
    jne     .exit_to_kernel

    /* ---- restore user state (reverse of push order, skip IRETQ) ---*/
    /* NOTE: restore data segments BEFORE GPRs — the segment load uses
     * %ax which would otherwise destroy the syscall return value in
     * the rax slot. FS/GS intentionally untouched (see entry fix).  */
    .global syscall_restore_user
syscall_restore_user:
    movw    $USER_DS, %ax
    movw    %ax, %ds
    movw    %ax, %es

    movq    40(%rsp), %rax                    /* return value         */
    movq    48(%rsp), %r11
    movq    56(%rsp), %r10
    movq    64(%rsp), %r9
    movq    72(%rsp), %r8
    movq    80(%rsp), %rcx
    movq    88(%rsp), %rdx
    movq    96(%rsp), %rdi
    movq    104(%rsp), %rsi
    movq    112(%rsp), %rbx
    movq    120(%rsp), %rbp
    movq    128(%rsp), %r12
    movq    136(%rsp), %r13
    movq    144(%rsp), %r14
    movq    152(%rsp), %r15

    /* IRETQ consumes the low 40 bytes: rip, cs, rflags, rsp, ss      */
    iretq

/* ---------------------------------------------------------------
 * sys_exit path: the user process is done forever.
 * We are already on the dedicated kernel stack; enable interrupts
 * (SYSCALL cleared IF) and jump into the shell re-entry routine —
 * it never returns.
 * --------------------------------------------------------------- */
.exit_to_kernel:
    movq    40(%rsp), %rdi                    /* exit code (arg)      */
    sti
    jmp     handle_user_exit

/* ---------------------------------------------------------------
 * fork_resume_child — entry point of a forked child's continuation.
 * The child parks in context_switch-format; its frame slot [7] holds
 * the address of its COPIED syscall trap frame (on the child kernel
 * stack). We load that as RSP and jump into the shared restore path,
 * so the child "returns" from fork() with RAX=0 (patched by the
 * kernel when the frame was copied).
 * --------------------------------------------------------------- */
fork_resume_child:
    popq    %rsp
    jmp     syscall_restore_user

/* Saved user RSP for SYSCALL entry/exit (updated by both paths) */
    .section .data
    .global syscall_user_rsp
syscall_user_rsp:
    .quad 0

/* Current syscall trap-frame base (set on every syscall entry) */
    .global syscall_frame_ptr
syscall_frame_ptr:
    .quad 0
