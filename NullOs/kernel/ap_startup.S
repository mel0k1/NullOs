/*
 * ap_startup.S - AP (Application Processor) trampoline for NullOs SMP
 *
 * This code is copied to physical address 0x8000 at runtime by smp_init().
 * After SIPI, the AP starts executing here in 16-bit real mode.
 * The trampoline transitions: 16-bit real -> 32-bit protected -> 64-bit long mode,
 * then jumps to the C function ap_c_entry() in the kernel.
 *
 * Patchable locations (absolute addresses, patched by smp.c after copy):
 *   0x8080: PML4 physical address (4 bytes)
 *   0x8084: GDT pointer (6 bytes: 2-byte limit + 4-byte base)
 *   0x8090: GDT entries (24 bytes: null + code64 + data64)
 *   0x80C0: AP stack top (8 bytes)
 *   0x80C8: AP C entry point address (8 bytes)
 *   0x80D0: AP ready flag (4 bytes) - set to 1 by AP after init
 */

    .section .text.ap_trampoline, "ax", @progbits
    .global ap_trampoline_start
    .global ap_trampoline_end

ap_trampoline_start:
    .code16gcc

    /* Disable interrupts */
    cli

    /* Set up segment registers */
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7000, %sp

    /* === Enable PAE (CR4.PAE = bit 5) === */
    movl    %cr4, %eax
    orl     $0x20, %eax
    movl    %eax, %cr4

    /* === Load PML4 physical address === */
    /* PML4 addr is stored at absolute address 0x8080 */
    movw    $0x8080, %bx
    movl    (%bx), %eax
    movl    %eax, %cr3

    /* === Enable Long Mode (EFER.LME = bit 8) === */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x100, %eax
    wrmsr

    /* === Enable Paging (CR0.PG = bit 31) + Protected Mode (CR0.PE = bit 0) === */
    movl    %cr0, %eax
    orl     $0x80000001, %eax
    movl    %eax, %cr0

    /* === Load GDT === */
    /* GDT pointer is at 0x8084 (6 bytes: limit + base) */
    movw    $0x8084, %bx
    lgdt    (%bx)

    /* === Far jump to 64-bit code === */
    /* CS = 0x08 (2nd GDT entry = 64-bit code, DPL0) */
    /* Offset = 0x80A8 (where 64-bit code begins) */
    ljmp    $0x08, $0x80A8

    /* ============================================================ */
    /* Patchable data area                                        */
    /* ============================================================ */

    /* Pad to offset 0x80 from start */
    .org    ap_trampoline_start + 0x80

    /* PML4 physical address - patched by smp.c */
    .long   0

    /* GDT pointer at offset 0x84 */
    .word   23          /* Limit: 3 entries * 8 bytes - 1 */
    .long   0           /* Base: patched to physical address of GDT below */
    .long   0           /* High 32 bits of base (0 for low addresses) */

    /* GDT entries at offset 0x90 (3 entries = 24 bytes, ends at 0xA8) */
    .org    ap_trampoline_start + 0x90
ap_temp_gdt:
    .quad   0                       /* 0x00: Null descriptor */
    .quad   0x00AF9A000000FFFF       /* 0x08: Code64: P=1,L=1,DPL=0,Code/Read,G=1 */
    .quad   0x008F92000000FFFF       /* 0x10: Data64: P=1,DPL=0,Data/Write,G=1,L=0 */

    /* ============================================================ */
    /* 64-bit entry point                                         */
    /* ============================================================ */
    .org    ap_trampoline_start + 0xA8
    .code64

ap_entry_64:
    /* Load data segments */
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    /* Load stack from patched location at 0x8100 */
    movq    0x8100, %rsp

    /* Clear direction flag */
    cld

    /* Enable interrupts */
    sti

    /* Call AP C entry point from patched location at 0x8108 */
    movq    0x8108, %rax
    call    *%rax

    /* If ap_c_entry returns, halt */
    cli
1:
    hlt
    jmp     1b

    /* ============================================================ */
    /* Patchable data area                                          */
    /* Placed at 0x100+ so it can never overlap the variable-length */
    /* 64-bit code above (.org errors out at build time if it does) */
    /* Offsets must match TRAMP_OFF_* constants in kernel/smp.c     */
    /* ============================================================= */

    .org    ap_trampoline_start + 0x100
    /* AP stack top - patched by smp.c */
    .quad   0

    .org    ap_trampoline_start + 0x108
    /* AP C entry point (virtual address) - patched by smp.c */
    .quad   0

    .org    ap_trampoline_start + 0x110
    /* AP ready flag - set non-zero by AP after successful init */
    .long   0
    /* CPU id passed to the AP (read by ap_c_entry) */
    .long   0

ap_trampoline_end:
