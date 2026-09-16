/* 64-bit bootloader for NullOs */
/*
 * Entry protocol (both loaders):
 *   GRUB2 / Multiboot2 enters ELF64 kernels in 32-BIT PROTECTED MODE
 *   with paging OFF and CR4.PAE=0 (verified empirically:
 *   CR0=0x00000001, CR4=0, EFER=0 at _start).
 *
 * Bootstrap: PAE -> EFER.LME -> LGDT -> CR3 -> CR0.PG -> far jump.
 * vmm_init() later installs the kernel's real page tables.
 *
 * IMPORTANT (qemu TCG quirk): page-table pages MUST be statically
 * pre-filled in the file (.data), NOT built at runtime before the PG
 * enable. Runtime-written tables produce a spurious #PF whose CR2 is
 * mysteriously equal to the new CR0 value, even though the resulting
 * memory contents are byte-identical (bisected extensively — see
 * scripts/ notes). Static tables work everywhere.
 *
 * NOTE: keep the checksum formula 0x100000000 - (...). An earlier
 * revision used 0x10000000 which produced an invalid header and GRUB
 * rejected the image with "no multiboot header found".
 */

.section .multiboot_header, "a"
.align 8
MultibootHeader:
    .long 0xe85250d6           /* magic number */
    .long 0                    /* architecture (0 = i386) */
    .long MultibootHeaderEnd - MultibootHeader /* header length */
    .long 0x100000000 - (0xe85250d6 + 0 + (MultibootHeaderEnd - MultibootHeader)) /* checksum */

    /* Information request tag is OPTIONAL; an empty one (size<12)
     * makes GRUB's request counter underflow and reject the image. */
    .value 1                   /* type */
    .value 1                   /* flags = OPTIONAL */
    .long 8                    /* size */

    /* Entry address tag */
    .value 3                   /* type */
    .value 0                   /* flags */
    .long 12                   /* size */
    .long _start               /* entry address */

    /* End tag */
    .align 8
    .value 0
    .value 0
    .long 8

MultibootHeaderEnd:

.section .boot.text, "ax"
.global _start

.code32
_start:
    cli

    /* Stash multiboot magic (ESI) / MBI ptr (EBX); survive the switch */
    movl %eax, %esi

    /* Already in long mode? (future loaders) */
    movl %cr0, %eax
    testl $0x80000000, %eax
    jnz .Lalready_long

    /* ---- Switch on long mode ----
     * Order mirrors ap_startup.S (known-good in this repo):
     * CR4.PAE -> EFER.LME -> LGDT -> CR3 -> CR0.PG -> far jump. */
    movl %cr4, %eax
    orl  $0x20, %eax            /* CR4.PAE                        */
    movl %eax, %cr4

    movl $0xC0000080, %ecx      /* MSR_EFER                       */
    rdmsr
    orl  $0x100, %eax           /* EFER.LME                       */
    wrmsr

    lgdt (gdt64_desc - 0x40000000)   /* linear = PA (pre-paging) */

    movl $(boot_pml4 - 0x40000000), %eax
    movl %eax, %cr3             /* static identity+window tables    */

    movl %cr0, %eax
    orl  $0x80000001, %eax      /* CR0.PG | CR0.PE                */
    movl %eax, %cr0

    /* Far jump into the 64-bit CS completes the transition */
    ljmp $0x08, $start64

.Lalready_long:
    jmp  start64

/* ------------------------------------------------------------------ */
.code64
start64:
    /* Reload data segments with our GDT selectors */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Reconstruct kernel_main(mbi_ptr):
     *   ESI == 0x36d76289 -> Multiboot2 (GRUB), EBX = MBI2 pointer
     *   anything else     -> PVH/direct entry: pass NULL */
    cmpl $0x36d76289, %esi
    je   .Lhave_mbi

    xorl %edi, %edi
    jmp  .Lgo

.Lhave_mbi:
    movl %ebx, %edi             /* MBI lives below 4 GiB */

.Lgo:
    leaq stack_top(%rip), %rsp
    call kernel_main

hang:
    cli
    hlt
    jmp hang

/* Stack (64KB) */
.section .bss
.align 16
stack_bottom:
    .skip 65536
stack_top:

/* ------------------------------------------------------------------ */
/* Temporary long-mode page tables (statically filled!).               */
/* Replaced by vmm_init() once the kernel VMM is up.                   */
/*                                                                     */
/* #kernel-halffix layout (matches vmm_init()):                        */
/*   PML4[0] -> PDPT:                                                  */
/*     PDPT[0] = 1GiB identity @ 0GB   (VGA 0xb8000, MBI, AP trampoline) */
/*     PDPT[1] -> boot_pd_win: 512 x 2MiB, VA 1GB+i*2MB -> PA i*2MB    */
/*                     (THE WINDOW: all RAM + the kernel image)        */
/*     PDPT[2] = 1GiB identity @ 2GB   (PCI/LAPIC MMIO)                */
/*     PDPT[3] = 1GiB identity @ 3GB   (PCI/LAPIC MMIO)                */
/* The kernel executes at VA = PA + 1GB; the far jump below lands in   */
/* the window. Requires 1GiB-page support for PDPT[0,2,3] (every       */
/* long-mode CPU since ~2010; use `-cpu ...,+pdpe1gb` if ever needed). */
/*                                                                     */
/* IMPORTANT (qemu TCG quirk): page-table pages MUST be statically     */
/* pre-filled in the file (.data), NOT built at runtime before the PG  */
/* enable. Runtime-written tables produce a spurious #PF whose CR2 is  */
/* mysteriously equal to the new CR0 value, even though the resulting  */
/* memory contents are byte-identical (bisected extensively — see      */
/* scripts/ notes). Static tables work everywhere.                     */
/* ------------------------------------------------------------------ */
.section .data
.align 4096
.globl boot_pml4
boot_pml4:
    .quad boot_pdpt - 0x40000000 + 0x03   /* PML4[0]: CR3 wants PA */
    .quad 0                               /* (rest zero) */
    .skip 4096 - 16

.align 4096
boot_pdpt:
    .quad 0x00000083            /* PDPT[0]: 1GiB @ 0GB (identity)  */
    .quad boot_pd_win - 0x40000000 + 0x03   /* PDPT[1]: WINDOW PD  */
    .quad 0x80000083            /* PDPT[2]: 1GiB @ 2GB (identity)  */
    .quad 0xC0000083            /* PDPT[3]: 1GiB @ 3GB (identity)  */
    .skip 4096 - 32

.align 4096
boot_pd_win:
    /* 512 x 2MiB: VA 1GB+i*2MB -> PA i*2MB (PS | RW | P).
     * FULL 1GB coverage: PMM puts the bitmap at top-of-RAM (~536MB
     * on -m 512M), so partial windows #PF in pmm_init before vmm_init
     * ever gets to rebuild the tables. */
    pdw_i = 0
    .rept 512
    .quad pdw_i * 0x200000 + 0x83
    pdw_i = pdw_i + 1
    .endr

/* Minimal GDT: null | code64 | data64 (matches ap_startup.S layout).
 * Assembled while .code32 is active, so the GDTR image uses the
 * 16-bit-limit + 32-bit-base form expected by a 32-bit lgdt. */
.section .rodata
.align 8
gdt64_desc:
    .word 23                    /* limit */
    .long gdt64_table - 0x40000000   /* base = PA (linear pre-paging) */

.align 8
gdt64_table:
    .quad 0
    .quad 0x00AF9A000000FFFF    /* 0x08: Code64: L=1, G=1 */
    .quad 0x008F92000000FFFF    /* 0x10: Data64 */

/* ============================================================
 * PVH ELF note (XEN_ELFNOTE_PHYS32_ENTRY): lets PVH-capable loaders
 * enter the image directly; the same _start bootstrap serves them.
 * ============================================================ */
.section .note.pvh, "a"
.align 4
    .long 4                   /* namesz ("Xen")            */
    .long 8                   /* descsz                    */
    .long 18                  /* type = PHYS32_ENTRY       */
    .long 0x006e6558          /* name bytes: 'X''e''n''\0' */
    .align 8
    .quad _start              /* protected-mode entry      */
