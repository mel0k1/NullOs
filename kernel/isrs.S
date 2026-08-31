/* Interrupt Service Routines for NullOs
 *
 * Includes FXSAVE/FXRSTOR for FPU/SSE state preservation.
 * A per-CPU kernel FPU save area is used for both ISR and scheduler.
 */

    .section .text

    .global isr_common_stub
    .global irq_common_stub
    .extern isr_handler
    .extern irq_handler
    .extern fpu_kernel_state

/* === CPU Exception Stubs (ISR 0-31) === */

.global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
.global isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
.global isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
.global isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

.global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
.global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

isr0:  pushq $0; pushq $0;  jmp isr_common_stub  /* Division By Zero */
isr1:  pushq $0; pushq $1;  jmp isr_common_stub  /* Debug */
isr2:  pushq $0; pushq $2;  jmp isr_common_stub  /* NMI */
isr3:  pushq $0; pushq $3;  jmp isr_common_stub  /* Breakpoint */
isr4:  pushq $0; pushq $4;  jmp isr_common_stub  /* Overflow */
isr5:  pushq $0; pushq $5;  jmp isr_common_stub  /* Bound Range Exceeded */
isr6:  pushq $0; pushq $6;  jmp isr_common_stub  /* Invalid Opcode */
isr7:  pushq $0; pushq $7;  jmp isr_common_stub  /* Device Not Available */
isr8:                    pushq $8;  jmp isr_common_stub  /* Double Fault (has err code) */
isr9:  pushq $0; pushq $9;  jmp isr_common_stub  /* Coprocessor Segment Overrun */
isr10:                   pushq $10; jmp isr_common_stub  /* Invalid TSS */
isr11:                   pushq $11; jmp isr_common_stub  /* Segment Not Present */
isr12:                   pushq $12; jmp isr_common_stub  /* Stack Fault */
isr13:                   pushq $13; jmp isr_common_stub  /* GPF */
isr14:                   pushq $14; jmp isr_common_stub  /* Page Fault */
isr15: pushq $0; pushq $15; jmp isr_common_stub  /* Reserved */
isr16: pushq $0; pushq $16; jmp isr_common_stub  /* x87 FP Exception */
isr17:                   pushq $17; jmp isr_common_stub  /* Alignment Check */
isr18: pushq $0; pushq $18; jmp isr_common_stub  /* Machine Check */
isr19: pushq $0; pushq $19; jmp isr_common_stub  /* SIMD FP Exception */
isr20: pushq $0; pushq $20; jmp isr_common_stub  /* Virtualization Exception */
isr21: pushq $0; pushq $21; jmp isr_common_stub  /* Control Protection */
isr22: pushq $0; pushq $22; jmp isr_common_stub  /* Reserved */
isr23: pushq $0; pushq $23; jmp isr_common_stub  /* Reserved */
isr24: pushq $0; pushq $24; jmp isr_common_stub  /* Reserved */
isr25: pushq $0; pushq $25; jmp isr_common_stub  /* Reserved */
isr26: pushq $0; pushq $26; jmp isr_common_stub  /* Reserved */
isr27: pushq $0; pushq $27; jmp isr_common_stub  /* Reserved */
isr28: pushq $0; pushq $28; jmp isr_common_stub  /* Reserved */
isr29: pushq $0; pushq $29; jmp isr_common_stub  /* Reserved */
isr30:                   pushq $30; jmp isr_common_stub  /* Security Exception */
isr31: pushq $0; pushq $31; jmp isr_common_stub  /* Reserved */

/* === IRQ Stubs (IRQ 0-15 -> vectors 32-47) === */

irq0:  pushq $0; pushq $32; jmp irq_common_stub  /* Timer */
irq1:  pushq $0; pushq $33; jmp irq_common_stub  /* Keyboard */
irq2:  pushq $0; pushq $34; jmp irq_common_stub  /* Cascade */
irq3:  pushq $0; pushq $35; jmp irq_common_stub  /* COM2 */
irq4:  pushq $0; pushq $36; jmp irq_common_stub  /* COM1 */
irq5:  pushq $0; pushq $37; jmp irq_common_stub  /* LPT2 */
irq6:  pushq $0; pushq $38; jmp irq_common_stub  /* Floppy */
irq7:  pushq $0; pushq $39; jmp irq_common_stub  /* LPT1 / Spurious */
irq8:  pushq $0; pushq $40; jmp irq_common_stub  /* RTC */
irq9:  pushq $0; pushq $41; jmp irq_common_stub  /* Free */
irq10: pushq $0; pushq $42; jmp irq_common_stub  /* Free */
irq11: pushq $0; pushq $43; jmp irq_common_stub  /* Free */
irq12: pushq $0; pushq $44; jmp irq_common_stub  /* PS/2 Mouse */
irq13: pushq $0; pushq $45; jmp irq_common_stub  /* FPU */
irq14: pushq $0; pushq $46; jmp irq_common_stub  /* Primary ATA */
irq15: pushq $0; pushq $47; jmp irq_common_stub  /* Secondary ATA */

/* === Common ISR Stub (Exceptions) === */
isr_common_stub:
    /* Save all general-purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Save FPU/SSE state to kernel save area */
    fxsave64 fpu_kernel_state(%rip)

    /* Pass pointer to register struct (rsp) as first argument (rdi) */
    movq %rsp, %rdi
    call isr_handler

    /* Restore FPU/SSE state */
    fxrstor64 fpu_kernel_state(%rip)

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Remove int_num and err_code from stack */
    addq $16, %rsp

    /* Return from interrupt */
    iretq

/* === Common IRQ Stub === */
irq_common_stub:
    /* Save all general-purpose registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Save FPU/SSE state to kernel save area */
    fxsave64 fpu_kernel_state(%rip)

    /* Pass pointer to register struct (rsp) as first argument (rdi) */
    movq %rsp, %rdi
    call irq_handler

    .global irq_resume_tail
irq_resume_tail:
    /* Restore FPU/SSE state */
    fxrstor64 fpu_kernel_state(%rip)

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Remove int_num and err_code from stack */
    addq $16, %rsp

    /* Return from interrupt */
    iretq

/* === Preempted-task resume entry ==============================
 * A task preempted inside an IRQ parks a context_switch-format frame
 * whose "return address" points here. On resume, [rsp] holds the base
 * of its saved IRQ frame (planted by scheduler_preempt). We restore
 * rsp and fall into the shared IRQ tail: fxrstor + pops + iretq.
 * ============================================================== */
    .global irq_task_resume
irq_task_resume:
    popq %rsp
    jmp irq_resume_tail


/* ============================================================ */
/* Kernel FPU save area — 512 bytes, 64-byte aligned.           */
/* Used by ISR stubs and the scheduler for FPU state swap.     */
/* ============================================================ */
    .section .bss
    .align 64
    .global fpu_kernel_state
fpu_kernel_state:
    .skip 512
