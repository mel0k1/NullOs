#include "../include/smp.h"
#include "../include/apic.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/mm.h"
#include "../include/idt.h"
#include "../include/timer.h"
#include "../include/acpi.h"

// ============================================================
// SMP Driver for NullOs
// ============================================================
//
// Detects CPU cores via ACPI MADT or CPUID fallback,
// starts AP cores via INIT+SIPI IPI sequence.
//
// The AP trampoline (ap_startup.S) is copied to 0x8000 and patched:
//   0x8080: PML4 physical address
//   0x8084: GDT pointer (limit + base)
//   0x8090: GDT entries (embedded in trampoline)
//   0x8100: AP stack top
//   0x8108: AP C entry point address
//   0x8110: AP ready flag (+4: cpu id)
// ============================================================

// Trampoline page address
#define AP_TRAMPOLINE_ADDR  0x8000ULL
#define AP_TRAMPOLINE_PAGE   (AP_TRAMPOLINE_ADDR / PAGE_SIZE)

// Offsets within trampoline (must match ap_startup.S)
#define TRAMP_OFF_PML4      0x80
#define TRAMP_OFF_GDTPTR    0x84
#define TRAMP_OFF_GDT       0x90
#define TRAMP_OFF_STACK     0x100
#define TRAMP_OFF_ENTRY     0x108
#define TRAMP_OFF_READY     0x110

// External: trampoline bounds (from ap_startup.S)
extern u8 ap_trampoline_start[];
extern u8 ap_trampoline_end[];

// Per-CPU state
static cpu_t cpus[SMP_MAX_CPUS];
static u32 smp_cpu_count = 0;
static bool smp_initialized = false;
static u32 bsp_lapic_id = 0xFFFFFFFF;

// Per-AP stacks (allocated from heap, not from the cpu_t struct)
#define AP_STACK_SIZE  8192
static u8* ap_stacks[SMP_MAX_CPUS];

// ============================================================
// MADT parsing (extract LAPIC IDs)
// ============================================================
//
// The MADT (Multiple APIC Description Table, signature "APIC") is
// located via the shared acpi_find_table() API from acpi.c — this file
// used to carry its own fragile duplicate of the RSDP/XSDT walker.
//
// Entry types:
//   0 = Processor Local APIC
//   1 = I/O APIC
//   2 = Interrupt Source Override
//   4 = Local APIC NMI

#define ACPI_MADT_LAPIC     0
#define ACPI_MADT_IOAPIC    1

typedef struct {
    u8  type;       // 0 = LAPIC
    u8  length;     // 8 bytes
    u8  acpi_id;    // ACPI processor ID
    u8  apic_id;    // Local APIC ID
    u32 flags;      // Bit 0: enabled
} __attribute__((packed)) madt_lapic_entry_t;

// Parse MADT to find LAPIC IDs
static u32 smp_parse_madt(u32* lapic_ids, u32 max_ids) {
    const acpi_sdt_header_t* madt = acpi_find_table("APIC");
    if (!madt) {
        vga_print("[SMP] MADT table not found\n");
        return 0;
    }

    u32 found = 0;
    u32 offset = 44;  // MADT entries start after header (36) + LAPIC ID (4) + flags (4)
    u32 end = madt->length;

    while (offset + 2 <= end && found < max_ids) {
        const u8* entry = (const u8*)madt + offset;
        u8 type = entry[0];
        u8 length = entry[1];

        if (length < 2) break;

        if (type == ACPI_MADT_LAPIC && length >= sizeof(madt_lapic_entry_t)) {
            const madt_lapic_entry_t* lapic =
                (const madt_lapic_entry_t*)entry;

            // Only count enabled processors
            if (lapic->flags & 1) {
                lapic_ids[found++] = lapic->apic_id;
            }
        }

        offset += length;
    }

    vga_printf("[SMP] MADT parsed: found %u enabled CPU(s)\n", found);
    return found;
}

// ============================================================
// AP trampoline setup
// ============================================================

// AP C entry point — called from ap_startup.S after mode transition
// Determines its own CPU ID by reading the stored value at 0x80D4
static void ap_c_entry(void) {
    // Read our CPU ID from the trampoline page
    volatile u32* stored_id =
        (volatile u32*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_READY + 4);
    u32 cpu_id = *stored_id;

    if (cpu_id == 0 || cpu_id >= SMP_MAX_CPUS) {
        vga_print("[SMP] AP: invalid CPU ID, halting\n");
        cli(); hlt();
        return;
    }

    cpu_t* cpu = &cpus[cpu_id];
    cpu->state = CPU_STATE_RUNNING;

    // Load kernel's IDT (BSP already set it up, and we share the same IDT
    // structure since we use the same page tables)
    extern struct idt_ptr idtp;
    extern void idt_set_gate(u8 num, u64 base, u16 sel, u8 flags);
    lidt(&idtp);

    vga_printf("[SMP] CPU%u (LAPIC %u) is alive in long mode!\n",
               cpu_id, cpu->lapic_id);

    // Signal readiness
    volatile u32* ready_flag =
        (volatile u32*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_READY);
    *ready_flag = cpu_id + 1;  // Non-zero = ready (1-indexed to avoid 0)

    // Enter idle loop — AP sits in HLT waiting for interrupts
    cpu->state = CPU_STATE_HALTED;
    while (1) {
        sti();
        hlt();
    }
}

static void smp_setup_trampoline(u8 cpu_id) {
    cpu_t* cpu = &cpus[cpu_id];

    // Calculate trampoline size
    u64 tramp_size = (u64)(ap_trampoline_end - ap_trampoline_start);
    if (tramp_size > PAGE_SIZE) {
        vga_print("[SMP] Trampoline too large!\n");
        return;
    }

    // Copy trampoline to 0x8000
    u8* dst = (u8*)(u64)AP_TRAMPOLINE_ADDR;
    kmemcpy(dst, ap_trampoline_start, (size_t)tramp_size);

    // Patch: PML4 physical address at offset 0x80
    u64 pml4_phys = vmm_get_kernel_pml4();
    u32* pml4_patch = (u32*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_PML4);
    *pml4_patch = (u32)pml4_phys;

    // Patch: GDT base at offset 0x88 (within GDTPtr structure)
    // The GDTPtr is at 0x8084: [2 bytes limit][4 bytes base][4 bytes high]
    // We need to patch the 4-byte base at offset 0x8084+2 = 0x8086
    u32* gdt_base_patch =
        (u32*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_GDTPTR + 2);
    *gdt_base_patch = (u32)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_GDT);

    // Patch: AP stack top at offset 0xC0
    u64 stack_top = (u64)ap_stacks[cpu_id] + AP_STACK_SIZE;
    u64* stack_patch = (u64*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_STACK);
    *stack_patch = stack_top;

    // Patch: AP entry point at offset 0xC8
    // We pass cpu_id as the argument via a wrapper.
    // Since the trampoline does `call *%rax`, we need the actual address.
    // We use a small stub that loads cpu_id into RDI and calls ap_c_entry.
    // For simplicity, call ap_c_entry directly with cpu_id in RDI.
    // But the trampoline doesn't set RDI... Let me patch it differently.
    //
    // Actually, let's create a small wrapper on the AP's stack that:
    //   1. Push cpu_id onto stack
    //   2. Call ap_c_entry
    // For now, we'll just set the entry point and have AP read CPU ID from
    // a known location.
    u64* entry_patch = (u64*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_ENTRY);
    *entry_patch = (u64)ap_c_entry;

    // Store cpu_id at a location the AP can read
    // We'll use the AP's stack area: push cpu_id on top of stack
    // Actually, let's store it at a fixed location the AP reads before calling
    // ap_c_entry. We'll put it just below the ready flag.
    u32* cpu_id_ptr = (u32*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_READY + 4);
    *cpu_id_ptr = cpu_id;

    // Clear ready flag
    volatile u32* ready_flag =
        (volatile u32*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_READY);
    *ready_flag = 0;

    vga_printf("[SMP] Trampoline patched for CPU%u (LAPIC %u)\n",
               cpu_id, cpu->lapic_id);
}

// ============================================================
// AP startup
// ============================================================

bool smp_start_ap(u8 cpu_id) {
    if (cpu_id == 0 || cpu_id >= SMP_MAX_CPUS) return false;
    if (!smp_initialized) return false;

    cpu_t* cpu = &cpus[cpu_id];
    if (cpu->lapic_id == 0xFFFFFFFF) return false;

    vga_printf("[SMP] Starting CPU%u (LAPIC %u)...\n",
               cpu_id, cpu->lapic_id);

    // Setup and patch trampoline for this CPU
    smp_setup_trampoline(cpu_id);

    // Wait a bit for memory writes to propagate
    for (volatile int i = 0; i < 10000; i++);

    // Step 1: Send INIT IPI
    vga_print("[SMP]   Sending INIT IPI...\n");
    lapic_send_init(cpu->lapic_id);

    // Wait 10ms (spec says > 10ms for INIT)
    for (volatile u64 i = 0; i < 2000000; i++);

    // Step 2: Send first SIPI
    // Vector = page number (0x8000 >> 12 = 0x08)
    vga_print("[SMP]   Sending SIPI #1...\n");
    lapic_send_sipi(cpu->lapic_id, 0x08);

    // Wait 200us
    for (volatile u64 i = 0; i < 40000; i++);

    // Step 3: Send second SIPI (required by some processors)
    vga_print("[SMP]   Sending SIPI #2...\n");
    lapic_send_sipi(cpu->lapic_id, 0x08);

    // Wait for AP to signal ready (up to 1 second)
    volatile u32* ready_flag =
        (volatile u32*)(u64)(AP_TRAMPOLINE_ADDR + TRAMP_OFF_READY);

    u64 timeout = 100000000;
    while (*ready_flag == 0 && timeout > 0) {
        timeout--;
    }

    if (*ready_flag != 0) {
        cpu->state = CPU_STATE_RUNNING;
        vga_printf("[SMP]   CPU%u started successfully!\n", cpu_id);
        return true;
    } else {
        cpu->state = CPU_STATE_DEAD;
        vga_printf("[SMP]   CPU%u failed to start (timeout)\n", cpu_id);
        return false;
    }
}

void smp_start_all(void) {
    if (!smp_initialized) return;

    vga_print("[SMP] Starting all AP cores...\n");
    u32 started = 0;

    for (u32 i = 1; i < smp_cpu_count && i < SMP_MAX_CPUS; i++) {
        if (smp_start_ap((u8)i)) {
            started++;
        }
    }

    vga_printf("[SMP] %u/%u AP core(s) started\n",
               started, smp_cpu_count - 1);
}

// ============================================================
// Initialization
// ============================================================

void smp_init(void) {
    if (smp_initialized) return;

    kmemset(cpus, 0, sizeof(cpus));
    kmemset(ap_stacks, 0, sizeof(ap_stacks));

    // Get BSP LAPIC ID
    bsp_lapic_id = lapic_get_id();
    vga_printf("[SMP] BSP LAPIC ID: %u\n", bsp_lapic_id);

    // Detect CPUs: try MADT first, then CPUID fallback
    u32 lapic_ids[SMP_MAX_CPUS];
    u32 found = smp_parse_madt(lapic_ids, SMP_MAX_CPUS);

    if (found == 0) {
        // Fallback: use CPUID to get logical processor count
        u32 eax, ebx, ecx, edx;
        cpuid(1, &eax, &ebx, &ecx, &edx);
        u32 logical_cpus = (ebx >> 16) & 0xFF;
        if (logical_cpus < 1) logical_cpus = 1;
        if (logical_cpus > SMP_MAX_CPUS) logical_cpus = SMP_MAX_CPUS;

        vga_printf("[SMP] CPUID reports %u logical CPU(s) (no MADT)\n",
                   logical_cpus);

        // Assume sequential LAPIC IDs (0, 1, 2, ...)
        found = logical_cpus;
        for (u32 i = 0; i < found; i++) {
            lapic_ids[i] = i;
        }
    }

    // Initialize CPU structures
    smp_cpu_count = found;
    for (u32 i = 0; i < found; i++) {
        cpus[i].id = (u8)i;
        cpus[i].lapic_id = lapic_ids[i];
        cpus[i].lapic_base = 0;  // Set during AP init
        cpus[i].state = CPU_STATE_DEAD;
        cpus[i].task_state = NULL;
        cpus[i].pf_count = 0;
        cpus[i].ipi_lock = 0;

        if (i == 0 && lapic_ids[0] == bsp_lapic_id) {
            cpus[i].state = CPU_STATE_RUNNING;
            vga_printf("[SMP] CPU0: BSP (LAPIC %u)\n", bsp_lapic_id);
        }

        // Allocate per-AP stack
        if (i > 0) {
            ap_stacks[i] = (u8*)kmalloc(AP_STACK_SIZE);
            if (ap_stacks[i]) {
                kmemset(ap_stacks[i], 0, AP_STACK_SIZE);
            } else {
                vga_printf("[SMP] Failed to allocate stack for CPU%u\n", i);
            }
        }
    }

    smp_initialized = true;
    vga_printf("[SMP] Initialized: %u CPU(s) detected\n", smp_cpu_count);
}

// ============================================================
// Info / Shell command
// ============================================================

u32 smp_get_cpu_count(void) {
    return smp_cpu_count;
}

const cpu_t* smp_get_cpu(u8 id) {
    if (id >= SMP_MAX_CPUS) return NULL;
    return &cpus[id];
}

void smp_print_info(void) {
    vga_print("\nSMP Info:\n");
    vga_print("--------\n");
    vga_printf("  BSP LAPIC ID: %u\n", bsp_lapic_id);
    vga_printf("  CPU count:     %u\n", smp_cpu_count);
    vga_printf("  Max CPUs:      %u\n", SMP_MAX_CPUS);
    vga_print("\n");

    for (u32 i = 0; i < smp_cpu_count; i++) {
        const char* state_str;
        switch (cpus[i].state) {
            case CPU_STATE_WAIT_SIPI: state_str = "Waiting SIPI"; break;
            case CPU_STATE_RUNNING:   state_str = "Running";      break;
            case CPU_STATE_HALTED:    state_str = "Halted";       break;
            case CPU_STATE_DEAD:      state_str = "Dead/Absent";  break;
            default:                  state_str = "Unknown";      break;
        }
        vga_printf("  CPU%u: LAPIC=%u, State=%s",
                   i, cpus[i].lapic_id, state_str);
        if (i == 0) vga_print(" (BSP)");
        vga_print("\n");
    }
}

void cmd_smp(int argc, char** argv) {
    (void)argc;
    if (argc >= 2 && kstrcmp(argv[1], "start") == 0) {
        smp_start_all();
    } else {
        smp_print_info();
    }
}
