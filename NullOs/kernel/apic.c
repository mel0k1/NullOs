#include "../include/apic.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/pic.h"
#include "../include/idt.h"

// ============================================================
// MMIO access helpers
// ============================================================

static volatile u32* lapic = NULL;  // Mapped LAPIC registers
static volatile u32* ioapic = NULL; // Mapped IOAPIC registers

static inline u32 lapic_read(u32 offset) {
    return lapic[offset / 4];
}

static inline void lapic_write(u32 offset, u32 val) {
    lapic[offset / 4] = val;
}

static inline u32 ioapic_read(u8 reg) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    return ioapic[IOAPIC_IOWIN / 4];
}

static inline void ioapic_write(u8 reg, u32 val) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    ioapic[IOAPIC_IOWIN / 4] = val;
}

// ============================================================
// Temporary identity mapping for APIC MMIO pages
// We use direct physical access since the kernel is
// identity-mapped in the first 128MB.
// ============================================================

// ============================================================
// Check APIC support via CPUID
// ============================================================

bool apic_is_supported(void) {
    u32 eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    return (edx & (1 << 9)) != 0;  // CPUID.01H:EDX.APIC[bit 9]
}

// ============================================================
// Get/set MSR 0x1B (IA32_APIC_BASE)
// ============================================================

static u64 apic_read_msr(void) {
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));
    return ((u64)hi << 32) | lo;
}

static void apic_write_msr(u64 val) {
    u32 lo = (u32)val;
    u32 hi = (u32)(val >> 32);
    __asm__ volatile ("wrmsr" :: "a"(lo), "d"(hi), "c"(0x1B));
}

// ============================================================
// Initialize Local APIC
// ============================================================

static void lapic_init(void) {
    u64 msr = apic_read_msr();

    // Enable APIC globally via MSR
    msr |= (1 << 11);  // APIC Global Enable
    apic_write_msr(msr);

    // Get physical base address from MSR (bits 12-35, page-aligned)
    u64 lapic_phys = msr & 0xFFFFF000ULL;

    // Since we're identity-mapped, we can use the physical address directly
    lapic = (volatile u32*)(u64)lapic_phys;

    // Read APIC ID
    u32 id = lapic_read(LAPIC_ID) >> 24;
    vga_printf("[APIC] Local APIC ID: %u, base: 0x%lx\n", id, lapic_phys);

    // Read version
    u32 version = lapic_read(LAPIC_VERSION) & 0xFF;
    u32 max_lvt = ((lapic_read(LAPIC_VERSION) >> 16) & 0xFF) + 1;
    vga_printf("[APIC] Version: %u, max LVT entries: %u\n", version, max_lvt);

    // Spurious Interrupt Vector Register
    // Set to a vector we don't use (vector 0xFF = IRQ 223)
    // and enable the APIC software
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);

    // Mask all LVT entries initially
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0,   LAPIC_LVT_MASKED | LAPIC_LVT_EXTINT);
    lapic_write(LAPIC_LVT_LINT1,   LAPIC_LVT_MASKED | LAPIC_LVT_NMI);
    lapic_write(LAPIC_LVT_ERROR,   LAPIC_LVT_MASKED);

    // Set Task Priority Register to 0 (accept all interrupts)
    lapic_write(LAPIC_TPR, 0);

    // Acknowledge any pending interrupts
    lapic_write(LAPIC_EOI, 0);

    vga_print("[APIC] Local APIC enabled.\n");
}

// ============================================================
// Initialize IO APIC
// ============================================================

static void ioapic_init(void) {
    // IO APIC is typically at 0xFEC00000 (identity-mapped)
    ioapic = (volatile u32*)(u64)IOAPIC_DEFAULT_BASE;

    // Read IO APIC ID
    u32 io_id = ioapic_read(IOAPIC_ID) >> 24;
    vga_printf("[APIC] IO APIC ID: %u\n", io_id);

    // Read version
    u32 io_ver = ioapic_read(IOAPIC_VERSION) & 0xFF;
    u32 max_irq = ((ioapic_read(IOAPIC_VERSION) >> 16) & 0xFF) + 1;
    vga_printf("[APIC] IO APIC version: %u, max redirections: %u\n", io_ver, max_irq);

    // Mask all IRQ redirection entries first
    for (u8 i = 0; i < max_irq && i < 24; i++) {
        ioapic_set_mask(i);
    }

    // ============================================================
    // Set up default IRQ redirections
    // Map ISA IRQs to vectors 32+ (same as PIC for compatibility)
    // ============================================================

    // IRQ 0 -> Vector 32 (PIT Timer), edge-triggered, active-high
    ioapic_set_redirect(0,  32, 0);
    // IRQ 1 -> Vector 33 (Keyboard)
    ioapic_set_redirect(1,  33, 0);
    // IRQ 2 -> Vector 34 (Cascade, mask it)
    ioapic_set_redirect(2,  34, 0);
    ioapic_set_mask(2);  // No cascade needed with APIC
    // IRQ 4 -> Vector 36 (COM1)
    ioapic_set_redirect(4,  36, 0);
    // IRQ 8 -> Vector 40 (RTC)
    ioapic_set_redirect(8,  40, 0);
    // IRQ 12 -> Vector 44 (PS/2 Mouse)
    ioapic_set_redirect(12, 44, 0);
    // IRQ 14 -> Vector 46 (Primary ATA)
    ioapic_set_redirect(14, 46, 0);
    // IRQ 15 -> Vector 47 (Secondary ATA)
    ioapic_set_redirect(15, 47, 0);

    vga_print("[APIC] IO APIC configured (ISA IRQs redirected).\n");
}

// ============================================================
// Public API
// ============================================================

void apic_init(void) {
    if (!apic_is_supported()) {
        vga_print("[APIC] APIC not supported by CPU, falling back to PIC.\n");
        return;
    }

    vga_print("[APIC] Initializing APIC subsystem...\n");

    // Initialize Local APIC
    lapic_init();

    // Initialize IO APIC
    ioapic_init();

    // Mask all PIC interrupts (we use APIC now)
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    vga_print("[APIC] PIC masked. APIC is now the primary interrupt controller.\n");
}

void lapic_send_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

u32 lapic_get_id(void) {
    if (!lapic) return 0xFFFFFFFF;
    return lapic_read(LAPIC_ID) >> 24;
}

u64 lapic_get_base(void) {
    u64 msr = apic_read_msr();
    return msr & 0xFFFFF000ULL;
}

void lapic_send_ipi(u32 dest_lapic_id, u8 vector, u8 delivery_mode, bool assert) {
    if (!lapic) return;

    u32 icr_lo = 0;
    u32 icr_hi = 0;

    // Delivery mode (bits 8-10)
    icr_lo |= ((u32)(delivery_mode & 0x07) << 8);
    // Vector (bits 0-7)
    icr_lo |= (vector & 0xFF);
    // Destination mode: physical (bit 11 = 0)
    icr_lo &= ~(1 << 11);
    // Level: assert=1, deassert=0 (bit 14)
    if (assert) icr_lo |= (1 << 14);
    // Destination APIC ID (bits 56-63 of ICR, in high register)
    icr_hi = (dest_lapic_id & 0xFF) << 24;

    // Write high first, then low (writing low triggers the IPI)
    lapic_write(LAPIC_ICR_HI, icr_hi);
    lapic_write(LAPIC_ICR_LO, icr_lo);

    // Wait for delivery status bit to clear
    u32 start = 0;
    while ((lapic_read(LAPIC_ICR_LO) & (1 << 12)) && start < 1000000) {
        start++;
    }
}

void lapic_send_init(u32 dest_lapic_id) {
    // INIT IPI: delivery mode 5, vector 0, assert
    lapic_send_ipi(dest_lapic_id, 0x00, 5, true);
}

void lapic_send_sipi(u32 dest_lapic_id, u8 vector) {
    // SIPI: delivery mode 6, vector = startup page
    lapic_send_ipi(dest_lapic_id, vector, 6, true);
}

u32 lapic_timer_get_current(void) {
    if (!lapic) return 0;
    return lapic_read(LAPIC_CURRENT_COUNT);
}

void ioapic_set_mask(u8 irq) {
    if (!ioapic || irq >= 24) return;
    u32 entry = ioapic_read(IOAPIC_IRQBASE + irq * 2);
    entry |= IOAPIC_RE_MASK;
    ioapic_write(IOAPIC_IRQBASE + irq * 2, entry);
}

void ioapic_clear_mask(u8 irq) {
    if (!ioapic || irq >= 24) return;
    u32 entry = ioapic_read(IOAPIC_IRQBASE + irq * 2);
    entry &= ~(u32)IOAPIC_RE_MASK;
    ioapic_write(IOAPIC_IRQBASE + irq * 2, entry);
}

void ioapic_set_redirect(u8 irq, u8 vector, u16 flags) {
    if (!ioapic || irq >= 24) return;

    u64 entry = vector;  // Vector
    entry |= IOAPIC_RE_DELIV_FIXED;  // Fixed delivery mode
    entry |= IOAPIC_RE_DEST_PHYSICAL;  // Physical destination

    if (flags & 0x01) entry |= IOAPIC_RE_TRIGGER_LEVEL;
    if (flags & 0x02) entry |= IOAPIC_RE_POLARITY_LOW;

    // Destination APIC ID = 0 (BSP)
    entry |= ((u64)0 << 56);

    // Mask it by default — caller must explicitly unmask
    entry |= IOAPIC_RE_MASK;

    // Write low 32 bits
    ioapic_write(IOAPIC_IRQBASE + irq * 2, (u32)(entry & 0xFFFFFFFF));
    // Write high 32 bits
    ioapic_write(IOAPIC_IRQBASE + irq * 2 + 1, (u32)(entry >> 32));
}

