#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>

// Basic types
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef u64      phys_addr_t;
typedef u64      virt_addr_t;

// Boolean type
#ifndef __cplusplus
typedef enum {
    false = 0,
    true = 1
} bool;
#endif

// System call numbers live in include/syscall.h (Linux x86_64 ABI)

// File system constants
#define MAX_PATH        256
#define MAX_FILES       64
#define MAX_FILENAME    64
#define FS_MAGIC        0x4D454C4F534653  // "NULFS"

// Register state for interrupts — passed as pointer to rsp in ISR stubs.
//
// After isr_common_stub pushes all GPRs and calls isr_handler:
//   call pushes return address, but rdi = old rsp (address of r15).
//
// Stack layout from regs (= rdi, pointing at r15 on the stack):
//
//   [regs+0]    r15          <- pushed last by common_stub
//   [regs+8]    r14
//   [regs+16]   r13
//   [regs+24]   r12
//   [regs+32]   r11
//   [regs+40]   r10
//   [regs+48]   r9
//   [regs+56]   r8
//   [regs+64]   rbp
//   [regs+72]   rdi
//   [regs+80]   rsi
//   [regs+88]   rdx
//   [regs+96]   rcx
//   [regs+104]  rbx
//   [regs+112]  rax          <- pushed first by common_stub
//   [regs+120]  int_num      <- pushed by ISR stub (or $0 for fake err_code)
//   [regs+128]  err_code     <- pushed by CPU (or fake $0 by stub)
//   [regs+136]  rip          <- pushed by CPU on interrupt
//   [regs+144]  cs           <- pushed by CPU
//   [regs+152]  rflags       <- pushed by CPU
//   [regs+160]  rsp          <- pushed by CPU ONLY on privilege change (Ring 3->0)
//   [regs+168]  ss           <- pushed by CPU ONLY on privilege change (Ring 3->0)
//
struct registers {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_num, err_code, rip, cs, rflags;
    u64 rsp;   // Valid only when (cs & 3) != 0 (interrupted from Ring 3)
    u64 ss;    // Valid only when (cs & 3) != 0
};
typedef struct registers registers_t;

// Inline assembly helpers
static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(u16 port, u32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void cli() {
    __asm__ volatile ("cli");
}

static inline void sti() {
    __asm__ volatile ("sti");
}

static inline void hlt() {
    __asm__ volatile ("hlt");
}

/* #tls-if-leak: IRQ-wait that preserves the CALLER's IF state.
 * The old "sti\nhlt\ncli" tail pinned IF=0 on every loop exit:
 * correct for the syscall path (SYSCALL clears IF and the exit path
 * expects it back), but FATAL for ring-0 shell commands — after the
 * first https the kernel main thread hlt()ed forever with IF=0 and
 * the keyboard/timer died (prompt printed, zero reaction to keys).
 * Semantics: save EFLAGS -> sti -> hlt -> restore EFLAGS.
 *   syscall context (IF=0): behaves exactly like sti\nhlt\ncli
 *   kernel cmd context (IF=1): IRQs stay ON after the wait            */
static inline void hlt_irq_restore(void) {
    u64 eflags;
    __asm__ volatile (
        "pushfq\n\tpopq %0\n\tsti\n\thlt\n\tpushq %0\n\tpopfq"
        : "=r"(eflags)
        :
        : "memory");
}

static inline void lidt(void* ptr) {
    __asm__ volatile ("lidt (%0)" :: "r"(ptr));
}

static inline void lgdt(void* ptr) {
    __asm__ volatile ("lgdt (%0)" :: "r"(ptr));
}

static inline u64 rdmsr(u32 msr) {
    u32 low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((u64)high << 32) | low;
}

static inline void wrmsr(u32 msr, u64 value) {
    u32 low = value & 0xFFFFFFFF;
    u32 high = value >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline u64 rdtsc() {
    u32 low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((u64)high << 32) | low;
}

static inline void cpuid(u32 func, u32* eax, u32* ebx, u32* ecx, u32* edx) {
    __asm__ volatile ("cpuid" 
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(func));
}

static inline void invlpg(virt_addr_t addr) {
    __asm__ volatile ("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void wbinvd() {
    __asm__ volatile ("wbinvd");
}

// Memory barriers
#define mb()  __asm__ volatile ("mfence" ::: "memory")
#define rmb() __asm__ volatile ("lfence" ::: "memory")
#define wmb() __asm__ volatile ("sfence" ::: "memory")

// CPU flags
#define CPU_FLAG_CF   (1 << 0)
#define CPU_FLAG_PF   (1 << 2)
#define CPU_FLAG_AF   (1 << 4)
#define CPU_FLAG_ZF   (1 << 6)
#define CPU_FLAG_SF   (1 << 7)
#define CPU_FLAG_TF   (1 << 8)
#define CPU_FLAG_IF   (1 << 9)
#define CPU_FLAG_DF   (1 << 10)
#define CPU_FLAG_OF   (1 << 11)

// ============================================================
// Multiboot2 structures
// ============================================================

struct multiboot2_info {
    u32 total_size;
    u32 reserved;
};

struct multiboot2_tag {
    u32 type;
    u32 size;
};

#define MBI_TAG_TYPE_END          0
#define MBI_TAG_TYPE_CMDLINE      1
#define MBI_TAG_TYPE_BOOT_LOADER  2
#define MBI_TAG_TYPE_MODULE       3
#define MBI_TAG_TYPE_BASIC_MEM    4
#define MBI_TAG_TYPE_BOOTDEV      5
#define MBI_TAG_TYPE_MMAP         6
#define MBI_TAG_TYPE_FRAMEBUFFER  8

struct multiboot2_tag_mmap {
    u32 type;          // 6
    u32 size;
    u32 entry_size;
    u32 entry_version;
    // followed by mmap_entry_t entries
};

struct multiboot2_mmap_entry {
    u64 base_addr;
    u64 length;
    u32 type;          // 1 = usable RAM, 2 = reserved, etc.
    u32 reserved;
};

// ============================================================
// VMM page table flags
// ============================================================

#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITE     (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_PWT       (1ULL << 3)  // Write-through
#define VMM_PCD       (1ULL << 4)  // Cache disable
#define VMM_ACCESSED  (1ULL << 5)
#define VMM_DIRTY     (1ULL << 6)
#define VMM_PS        (1ULL << 7)  // Page size (2MB in PD)
#define VMM_GLOBAL    (1ULL << 8)
#define VMM_NX        (1ULL << 63) // No execute

// Common flag combinations
#define VMM_FLAGS_KR  (VMM_PRESENT | VMM_WRITE)                      // Kernel, Read-Write
#define VMM_FLAGS_KX  (VMM_PRESENT | VMM_WRITE | VMM_GLOBAL)        // Kernel, RW + Global
#define VMM_FLAGS_KRX (VMM_PRESENT | VMM_WRITE)                      // Kernel, RW (NX not set)

#endif // TYPES_H
