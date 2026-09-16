; 64-bit bootloader for MelOS64
; Uses GRUB Multiboot2 to load into long mode

bits 64

section .multiboot_header
align 8
MultibootHeader:
    dd 0xe85250d6           ; magic number
    dd 0                    ; architecture (0 = i386, will be set to x86-64 by bootloader)
    dd MultibootHeaderEnd - MultibootHeader ; header length
    dd 0x10000000 - (0xe85250d6 + 0 + (MultibootHeaderEnd - MultibootHeader)) ; checksum

    ; Required tag: information
    dw 1                    ; type (required information)
    dw 0                    ; flags
    dd 8                    ; size

    ; Required tag: entry address
    dw 3                    ; type (entry address)
    dw 0                    ; flags
    dd 12                   ; size
    dd _start               ; entry address (relative)

    ; Required tag: end
    dw 0                    ; type (end)
    dw 0                    ; flags
    dd 8                    ; size

MultibootHeaderEnd:

section .text
global _start
extern kernel_main

_start:
    ; Check if we were booted by a Multiboot2 compliant boot loader
    cmp eax, 0x36d76289     ; MULTIBOOT2_BOOTLOADER_MAGIC
    jne hang

    ; Save multiboot info pointer in rdi (first argument for System V AMD64 ABI)
    mov rdi, rbx

    ; Disable interrupts
    cli

    ; Set up stack
    lea rsp, [stack_top]

    ; Call kernel main function (System V AMD64 ABI: first arg in rdi)
    call kernel_main

    ; Halt if kernel returns
hang:
    cli
    hlt
    jmp hang

; Data section
section .data
align 16

; Stack (64KB for 64-bit kernel)
align 16
stack_bottom: resb 65536
stack_top:
