#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Spinlocks — minimal SMP-safe locking primitives.
//
// Zero-initialized (BSS) spinlocks are unlocked, so static
// definitions need no explicit initialization.
//
// Rules of use:
//   * Never call kmalloc/kfree/pmm_* while holding a lock from
//     a DIFFERENT subsystem than the one the lock protects.
//   * In IRQ context always use spin_lock_irqsave/spin_unlock_irqrestore.
//   * Hold times must be short: no device polling, no console output
//     while locked (VGA writes are slow).
// ============================================================

typedef volatile u32 spinlock_t;

#define SPINLOCK_INIT 0

// Plain lock/unlock: for contexts where interrupts are already disabled
// or where the critical section never touches IRQ-shared data.
static inline void spin_lock(spinlock_t* l) {
    for (;;) {
        // Atomic exchange: returns previous value; 0 means we acquired.
        if (__atomic_exchange_n(l, 1u, __ATOMIC_ACQUIRE) == 0) {
            return;
        }
        // Backoff: pause while spinning to reduce bus contention
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            __asm__ volatile ("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t* l) {
    __atomic_store_n(l, 0u, __ATOMIC_RELEASE);
}

// Save RFLAGS, disable interrupts, then take the lock.
// Returns the flags to be passed to spin_unlock_irqrestore().
static inline u64 spin_lock_irqsave(spinlock_t* l) {
    u64 flags;
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    spin_lock(l);
    return flags;
}

// Release the lock and restore the interrupt state captured at lock time.
static inline void spin_unlock_irqrestore(spinlock_t* l, u64 flags) {
    spin_unlock(l);
    if (flags & CPU_FLAG_IF) {
        sti();
    }
}

static inline bool spin_is_locked(spinlock_t* l) {
    return __atomic_load_n(l, __ATOMIC_RELAXED) != 0;
}

#ifdef __cplusplus
}
#endif

#endif // SPINLOCK_H
