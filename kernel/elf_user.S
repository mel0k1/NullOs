/* elf_user.S — iretq-to-usermode stub for NullOs ELF loader
 *
 * void iret_to_usermode(u64 user_rip, u64 user_rsp,
 *                       u64 user_rflags, u64 user_cs, u64 user_ss);
 *
 * Builds the IRETQ frame on the kernel stack and executes iretq.
 * After iretq, CPU is in ring 3 with the given CS/SS/RSP/RIP/RFLAGS.
 *
 * Also saves user_rsp into syscall_user_rsp so that syscall_entry.S
 * can recover it on SYSCALL (hardware doesn't save user RSP).
 */

    .section .text
    .global iret_to_usermode
    .extern syscall_user_rsp

/* C prototype:
 *   void iret_to_usermode(u64 rip, u64 rsp, u64 rflags, u64 cs, u64 ss)
 *   rdi = rip, rsi = rsp, rdx = rflags, rcx = cs, r8 = ss
 */
iret_to_usermode:
    /* Save user RSP into the global for syscall_entry.S */
    movq    %rsi, syscall_user_rsp(%rip)

    /* Build IRETQ frame (push in reverse order of what iretq pops):
     *   iretq pops: RIP, CS, RFLAGS, RSP, SS
     *   so we push: SS, RSP, RFLAGS, CS, RIP
     *
     * RPL bits (low 2) of CS and SS MUST be 3 for ring-3 entry:
     *   USER_SS = 0x1B (0x18 | 3)
     *   USER_CS = 0x23 (0x20 | 3) */
    orq     $3, %r8         /* SS |= RPL3                    */
    orq     $3, %rcx        /* CS |= RPL3                    */
    pushq   %r8             /* SS                            */
    pushq   %rsi            /* RSP                           */
    pushq   %rdx            /* RFLAGS                        */
    pushq   %rcx            /* CS                            */
    pushq   %rdi            /* RIP                           */

    /* Clear RAX — user code sees return value 0 */
    xorq    %rax, %rax

    /* Clear all other GPRs for cleanliness */
    xorq    %rbx, %rbx
    xorq    %rcx, %rcx
    xorq    %rdx, %rdx
    xorq    %rsi, %rsi
    xorq    %rdi, %rdi
    xorq    %rbp, %rbp
    xorq    %r8, %r8
    xorq    %r9, %r9
    xorq    %r10, %r10
    xorq    %r11, %r11
    xorq    %r12, %r12
    xorq    %r13, %r13
    xorq    %r14, %r14
    xorq    %r15, %r15

    /* Set DS/ES/FS/GS to user data segment (RPL3!) */
    movw    $0x1B, %ax      /* GDT_UDATA_SEL | RPL3 */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs

    /* iretq pops: RIP, CS, RFLAGS, RSP, SS — transitions to ring 3 */
    iretq
