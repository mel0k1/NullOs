/*
 * crt0 — entry point for NullOs user programs.
 *
 * The kernel iretq's into _start with the System V AMD64 ABI stack
 * layout prepared by the ELF loader (same as Linux):
 *
 *   rsp+0        argc
 *   rsp+8        argv[0] .. argv[argc-1], NULL
 *   ...          envp = NULL (empty environment)
 *   ...          auxv AT_NULL pair
 *
 * Syscall ABI — standard Linux x86_64:
 *   rax = syscall number, rdi/rsi/rdx/r10 = args
 */

    .section .text
    .global _start
    .extern main

_start:
    xorq    %rbp, %rbp           /* mark outermost frame          */

    movq    (%rsp), %rdi         /* argc                          */
    leaq    8(%rsp), %rsi        /* argv                          */

    andq    $-16, %rsp           /* re-align stack for call       */
    call    main                 /* int main(int, char**)         */

    /* exit(main_return_value) — Linux SYS_exit = 60 */
    movl    %eax, %edi
    movl    $60, %eax
    syscall

1:  jmp     1b                   /* never reached                 */
