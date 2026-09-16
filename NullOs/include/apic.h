#ifndef APIC_H
#define APIC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Local APIC (each CPU has one, mapped via MSR or fixed MMIO)
// ============================================================

// Default Local APIC MMIO base address
#define LAPIC_DEFAULT_BASE  0xFEE00000ULL

// Local APIC register offsets (all 32-bit unless noted)
#define LAPIC_ID            0x020  // Local APIC ID
#define LAPIC_VERSION       0x030  // Local APIC Version
#define LAPIC_TPR           0x080  // Task Priority Register
#define LAPIC_EOI           0x0B0  // End of Interrupt
#define LAPIC_LDR           0x0D0  // Logical Destination Register
#define LAPIC_SVR           0x0F0  // Spurious Interrupt Vector Register
#define LAPIC_ISR           0x100  // In-Service Register (8 x 32-bit)
#define LAPIC_TMR           0x180  // Trigger Mode Register
#define LAPIC_IRR           0x200  // Interrupt Request Register
#define LAPIC_ESR           0x280  // Error Status Register
#define LAPIC_ICR_LO        0x300  // Interrupt Command Register (low 32 bits)
#define LAPIC_ICR_HI        0x310  // Interrupt Command Register (high 32 bits)
#define LAPIC_LVT_TIMER     0x320  // Local APIC Timer LVT
#define LAPIC_LVT_THERMAL   0x330  // Thermal Monitor LVT
#define LAPIC_LVT_PERF      0x340  // Performance Counter LVT
#define LAPIC_LVT_LINT0     0x350  // Local Interrupt 0 LVT
#define LAPIC_LVT_LINT1     0x360  // Local Interrupt 1 LVT
#define LAPIC_LVT_ERROR     0x370  // Error LVT
#define LAPIC_INITIAL_COUNT 0x380  // Timer Initial Count
#define LAPIC_CURRENT_COUNT 0x390  // Timer Current Count
#define LAPIC_DIVIDE        0x3E0  // Timer Divide Configuration

// Spurious Interrupt Vector Register bits
#define LAPIC_SVR_ENABLE    0x100  // APIC software enable

// LVT entry format bits
#define LAPIC_LVT_MASKED    (1 << 16)   // Interrupt masked
#define LAPIC_LVT_TRIGGER_LEVEL (1 << 15)  // Level-triggered (vs edge)
#define LAPIC_LVT_NMI       (0x04 << 8)  // NMI delivery mode
#define LAPIC_LVT_EXTINT    (0x07 << 8)  // External interrupt delivery

// Timer LVT bits
#define LAPIC_LVT_TIMER_ONESHOT   (0x00 << 17)
#define LAPIC_LVT_TIMER_PERIODIC  (0x01 << 17)
#define LAPIC_LVT_TIMER_TSC_DEAD  (0x02 << 17)

// ============================================================
// IO APIC (system-wide, usually at 0xFEC00000)
// ============================================================

#define IOAPIC_DEFAULT_BASE 0xFEC00000ULL

// IO APIC register indices
#define IOAPIC_ID        0x00  // IO APIC ID
#define IOAPIC_VERSION   0x01  // IO APIC Version
#define IOAPIC_IRQBASE   0x10  // First IRQ redirection entry (24 entries)

// IOREGSEL: select register index
// IOWIN: read/write data at offset 0x10
#define IOAPIC_REGSEL    0x00  // Register select
#define IOAPIC_IOWIN     0x10  // Data window

// Redirection entry bits (64-bit, accessed as two 32-bit writes)
#define IOAPIC_RE_MASK       (1ULL << 16)  // Mask interrupt
#define IOAPIC_RE_TRIGGER_LEVEL (1ULL << 15)  // Level-triggered
#define IOAPIC_RE_POLARITY_LOW  (1ULL << 13)  // Active low
#define IOAPIC_RE_DELIV_FIXED   (0x00ULL << 8)  // Fixed delivery
#define IOAPIC_RE_DEST_PHYSICAL (0x00ULL << 11)  // Physical destination mode

// ============================================================
// Public API
// ============================================================

// Initialize Local APIC and IO APIC
// Maps MMIO, enables local APIC, configures timer, redirects IRQs
void apic_init(void);

// Send End-of-Interrupt to Local APIC
void lapic_send_eoi(void);

// Get Local APIC ID
u32 lapic_get_id(void);

// Mask/unmask an IO APIC redirection entry (by IRQ number 0-23)
void ioapic_set_mask(u8 irq);
void ioapic_clear_mask(u8 irq);

// Set an IO APIC redirection entry (vector, delivery mode, etc.)
void ioapic_set_redirect(u8 irq, u8 vector, u16 flags);

// Get current APIC timer tick count
u32 lapic_timer_get_current(void);

// Check if APIC is available (MSR 0x1B)
bool apic_is_supported(void);

// Get LAPIC MMIO base address
u64 lapic_get_base(void);

// Send IPI (Inter-Processor Interrupt)
// dest_lapic_id: target APIC ID
// vector: interrupt vector (0-255)
// delivery_mode: 0=Fixed, 1=LowestPrio, 2=SMI, 4=NMI, 5=INIT, 6=Startup
// assert: true=assert, false=deassert
void lapic_send_ipi(u32 dest_lapic_id, u8 vector, u8 delivery_mode, bool assert);

// Send INIT IPI to a specific APIC ID
void lapic_send_init(u32 dest_lapic_id);

// Send Startup IPI (SIPI) to a specific APIC ID
void lapic_send_sipi(u32 dest_lapic_id, u8 vector);

#ifdef __cplusplus
}
#endif

#endif // APIC_H
