/* Linker script for NullOs 64-bit kernel — #kernel-halffix layout.
 *
 * VA = PA + 0x40000000 (1 GB) for the main kernel image.
 *   - .boot stays IDENTITY (VA == PA at 1MB): GRUB/PVH enter here in
 *     32-bit protected mode with paging OFF, so the multiboot header,
 *     the ELF entry and the whole pre-paging stub must be linked at
 *     their physical addresses.
 *   - Everything else is linked at VAs above the 1 GB line with LMA
 *     right after .boot, so GRUB loads the segments by p_paddr exactly
 *     like before, but the kernel EXECUTES above the user VA range
 *     (user images are pre-linked at 0x400000 and can no longer shadow
 *     kernel .text/.data/.bss in a child CR3 — the #kernel-bss-shadow
 *     regression that killed the first spawn after the mbedtls/ext4
 *     integration grew the image past the old 4MB identity limit).
 *
 * CRITICAL: .text/.rodata/.data/.vtlow are merged into ONE output
 * section (.high) so that VMA == LMA + 0x40000000 holds EXACTLY for
 * every byte. ld applies input-section alignment identically to the
 * VMA and LMA views of a single output section; with SEPARATE output
 * sections each VMA got re-aligned while my AT() LMA chain stayed
 * packed — a ~14-byte skew that made the 2MB window translate .rodata
 * VAs to the WRONG physical frames (bytes loaded 0xE pages off).
 *
 * The boot page tables (boot64.S) and vmm_init() both map:
 *   PML4[0]->PDPT[0] = identity 0-1GB   (VGA 0xb8000, MBI, AP tramp)
 *   PML4[0]->PDPT[1] = WINDOW: VA 1GB+i*2MB -> PA i*2MB  (RAM + kernel)
 *   PML4[0]->PDPT[2,3] = identity 2-4GB (PCI/LAPIC MMIO)
 */

/* Define entry point */
ENTRY(_start)

/* Define memory layout */
SECTIONS
{
    /* ---- Identity bootstrap (32-bit, pre-paging) ---- */
    . = 0x100000;

    .boot : ALIGN(4K)
    {
        __boot_start_pa = .;
        KEEP(*(.multiboot_header))
        *(.boot)
        *(.boot.*)
        __boot_end_pa = .;
    }

    /* PVH note — identity too: qemu -kernel reads it by file offset,
     * and keeping it low avoids LMA/VMA bookkeeping for notes. It fits
     * inside the 4K alignment padding before the high part. */
    .note : ALIGN(8)
    {
        *(.note.pvh)
    }

    /* ---- High-half kernel: VAs above user space ---- */
    __high_pa = ALIGN(__boot_end_pa, 4K);

    . = 0x40000000 + __high_pa;

    .high : AT(__high_pa)
    {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        __vtlow_start = .;
        KEEP(*(.vtlow))
        KEEP(*(.vtlow.*))
        __vtlow_end = .;
    }

    /* Uninitialized data (BSS) — VMA only, occupies no file space.
     * 4K-aligned start so PA = VMA - 1GB stays page-aligned for PMM. */
    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        __bss_end = .;
    }

    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }

    /* End of kernel (VA). Physical end must INCLUDE .bss: the window
     * maps .bss VAs at PA = VA - 1GB, and PMM has to reserve those
     * frames — computing the physical end from .high's LMA alone let
     * pmm_alloc_page hand kernel-.bss/stack frames to user images
     * (seg0 pas[0]=0x314000 == the boot stack's frame!) and the first
     * build_user_image kmemset wiped the kernel stack mid-loop. */
    __kernel_end = .;
    __kernel_phys_end = __bss_end - 0x40000000;
}
