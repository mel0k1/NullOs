/*
 * Linker script for MelOS64 static user programs (EL64, non-PIE).
 * All programs are linked at ELF_USER_LOAD_BASE (0x400000).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.dynamic)
        *(.got .got.plt)
    }
}
