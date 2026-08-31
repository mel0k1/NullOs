/* GDT/TSS flush routines for NullOs */

.section .text

/* void gdt_flush(u64 gdt_ptr_addr) */
.global gdt_flush
gdt_flush:
    /* RDI = pointer to gdt_pointer struct */
    lgdt (%rdi)

    /* Reload code segment with far return */
    pushq $0x08          /* Kernel code selector */
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    /* Reload data segments */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    ret

/* void tss_flush(u16 tss_selector) */
.global tss_flush
tss_flush:
    ltr %di
    ret
