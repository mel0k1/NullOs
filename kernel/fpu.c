#include "../include/fpu.h"
#include "../include/vga.h"

// ============================================================
// FPU/SSE Initialization
// ============================================================
//
// The kernel is compiled with -mno-sse -mno-sse2, so the compiler
// won't generate SSE instructions in kernel code. However, user-mode
// programs may use SSE/FPU, and we need to save/restore the FPU state
// on interrupts and context switches to prevent corruption.
//
// CR0 bit 2 (EM) = 0: x87 FPU present
// CR0 bit 1 (MP) = 1: TS (task switched) monitored
// CR4 bit 9 (OSFXSR) = 1: OS supports FXSAVE/FXRSTOR
// CR4 bit 10 (OSXMMEXCPT) = 1: OS handles #XM (SIMD exception)

void fpu_init(void) {
    u64 cr0, cr4;

    // Read current CR0
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));

    // Clear EM (bit 2) — FPU is present, not emulated
    cr0 &= ~(1ULL << 2);

    // Set MP (bit 1) — monitor TS bit for lazy FPU switching
    cr0 |= (1ULL << 1);

    // Write CR0
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0) : "memory");

    // Read current CR4
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    // Set OSFXSR (bit 9) — OS uses FXSAVE/FXRSTOR
    cr4 |= (1ULL << 9);

    // Set OSXMMEXCPT (bit 10) — OS handles SIMD exceptions (#XM)
    cr4 |= (1ULL << 10);

    // Write CR4
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");

    // Initialize FPU with FNINIT
    __asm__ volatile ("fninit");

    vga_print("[FPU] FPU/SSE initialized (FXSAVE/FXRSTOR, CR0/CR4 configured)\n");
}

// ============================================================
// FPU Save / Restore
// ============================================================

void fpu_save(void* state) {
    // FXSAVE64 saves SSE+AVX state (512 bytes) with REX.W prefix
    __asm__ volatile (
        "fxsave64 (%0)"
        :
        : "r"(state)
        : "memory"
    );
}

void fpu_restore(void* state) {
    __asm__ volatile (
        "fxrstor64 (%0)"
        :
        : "r"(state)
        : "memory"
    );
}
