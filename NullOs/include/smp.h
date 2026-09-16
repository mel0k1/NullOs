#ifndef SMP_H
#define SMP_H
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif
// ============================================================
// SMP (Symmetric Multiprocessing) for NullOs
// ============================================================
//
// Basic SMP: detect cores via ACPI MADT, start AP cores via IPI,
// per-CPU data structures, load IDT with per-CPU stacks.
//
// Limitations:
//   - Uses xAPIC IPI to start AP cores
//   - Simple round-robin scheduler balancing
//   - Shared kernel address space (no per-CPU page tables yet)
//   - UP (Uni-Processor) mode: BSP boots AP cores
// ============================================================

// Maximum number of CPUs
#define SMP_MAX_CPUS    4

// Per-CPU state
typedef enum {
    CPU_STATE_WAIT_SIPI,  // Waiting for SIPI
    CPU_STATE_RUNNING,   // Running user or kernel code
    CPU_STATE_HALTED,    // Halted (entered HLT)
    CPU_STATE_DEAD,      // Not present or failed POST
} cpu_state_t;

typedef struct {
    u8  id;              // Logical CPU ID (0 = BSP)
    u32 lapic_id;        // Physical LAPIC ID
    u64 lapic_base;      // MMIO base address
    volatile cpu_state_t state;   // Current state
    // Stack for AP startup (per-CPU, not heap)
    u8  ap_startup_stack[4096] __attribute__((aligned(16)));
    // Syscall entry RSP (per-CPU)
    u8  syscall_stack[8192] __attribute__((aligned(16)));
    // Task state (for future per-CPU scheduler)
    void* task_state;
    // FPU state (512 bytes, 64-byte aligned)
    u8  fpu_state[512] __attribute__((aligned(64)));
    // Spinlock for IPI synchronization
    volatile u32 ipi_lock;
    // Page fault counter (for detecting MP bugs)
    u32 pf_count;
} cpu_t;

// Initialize SMP: parse ACPI MADT, detect CPUs
void smp_init(void);

// Start an AP core by logical CPU ID
// Returns true if SIPI was delivered
bool smp_start_ap(u8 cpu_id);

// Start all detected AP cores
void smp_start_all(void);

// Print SMP info
void smp_print_info(void);

// Get CPU count
u32 smp_get_cpu_count(void);

// Get per-CPU state
const cpu_t* smp_get_cpu(u8 id);

// Shell command
void cmd_smp(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // SMP_H
