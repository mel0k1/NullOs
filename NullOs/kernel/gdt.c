#include "../include/gdt.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/mm.h"
#include "../include/serial.h"

// TSS instance (must be static, not on stack)
static tss_t kernel_tss;
static u64 tss_stack[2048];  // 16 KB IST1 stack for exceptions

// GDT entry structure (8 bytes low + 8 bytes high for 64-bit)
struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
} __attribute__((packed));

struct gdt_pointer {
    u16 limit;
    u64 base;
} __attribute__((packed));

// 10 GDT entries: null, kcode, kdata, ucode, udata, tss_low, tss_high, 3 free
#define GDT_ENTRIES 10
// GDT base must be 8-byte aligned: descriptor fetches index base+sel*8,
// and some environments fault or misbehave on unaligned GDTR bases.
static struct gdt_entry gdt[GDT_ENTRIES] __attribute__((aligned(16)));
static struct gdt_pointer gdt_ptr __attribute__((aligned(16)));

// Assembly function to load GDT and reload segment registers
extern void gdt_flush(u64 gdt_ptr_addr);

// Assembly function to load TSS
extern void tss_flush(u16 tss_selector);

static void gdt_set_entry(int idx, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[idx].limit_low  = limit & 0xFFFF;
    gdt[idx].base_low   = base & 0xFFFF;
    gdt[idx].base_mid   = (base >> 16) & 0xFF;
    gdt[idx].access     = access;
    gdt[idx].granularity = (gran & 0x0F) | ((limit >> 16) & 0xF0);
    gdt[idx].base_high  = (base >> 24) & 0xFF;
}

// Set a 64-bit code segment (L bit set, D bit clear)
static void gdt_set_code64(int idx, u8 dpl, bool conforming) {
    // In 64-bit mode, base/limit are ignored (except for non-canonical check)
    // Access byte: P=1, DPL, S=1 (code/data segment!), Type
    //   Non-conforming code: E=0, DC=0, R=1, A=0 → 0xA
    //   Conforming code:   E=1, DC=0, R=1, A=0 → 0xE
    //
    // NOTE: the S bit (0x10) is MANDATORY — without it the descriptor is a
    // SYSTEM descriptor and every segment load raises #GP(err=selector).
    // This missing bit was THE reason the kernel could never boot.
    u8 type = conforming ? 0xE : 0xA;
    u8 access = 0x80 | (dpl << 5) | 0x10 | type;  // P, DPL, S=1, Type

    gdt[idx].limit_low  = 0;
    gdt[idx].base_low   = 0;
    gdt[idx].base_mid   = 0;
    gdt[idx].access     = access;
    gdt[idx].granularity = 0x20;  // L=1 (long mode), D=0
    gdt[idx].base_high  = 0;
}

// Set a data segment
static void gdt_set_data(int idx, u8 dpl) {
    u8 access = 0x80 | (dpl << 5) | 0x10 | 0x2;  // P, DPL, S=1, Data+W
    gdt[idx].limit_low  = 0xFFFF;
    gdt[idx].base_low   = 0;
    gdt[idx].base_mid   = 0;
    gdt[idx].access     = access;
    gdt[idx].granularity = 0x40;  // G=1, D/B=0 (ignored in 64-bit)
    gdt[idx].base_high  = 0;
}

void gdt_init(void) {
    kmemset(&gdt, 0, sizeof(gdt));
    kmemset(&kernel_tss, 0, sizeof(tss_t));
    kmemset(tss_stack, 0, sizeof(tss_stack));

    // Entry 0: Null descriptor
    gdt_set_entry(0, 0, 0, 0, 0);

    // Entry 1: Kernel Code (64-bit, DPL0)
    gdt_set_code64(1, 0, false);

    // Entry 2: Kernel Data (DPL0)
    gdt_set_data(2, 0);

    // Entry 3: User Data (DPL3) — placed before user code for SYSRET compatibility
    // SYSRETQ: CS = STAR[47:32]+16 = 0x18 (this entry, treated as code by SYSRET)
    // We use IRETQ for syscall return, but this layout is still correct for
    // any future SYSRET usage or if we revert to it.
    gdt_set_data(3, 3);

    // Entry 4: User Code (64-bit, DPL3)
    gdt_set_code64(4, 3, false);

    // Entry 5-6: TSS (16-byte system descriptor).
    // Build as two u64s with CORRECT byte layout:
    //   low:  [15:0]  limit    [39:16] base 23:0  [43:40] type=0x9
    //         [47:44] flags(0) [51:48] limit19:16
    //   high: base 63:32
    // The previous code wrote through a u16* and put the access byte at
    // offset 6 instead of 5, making the descriptor not-present -> #GP(0x28)
    // on every ltr.
    {
        u64 tss_addr = (u64)&kernel_tss;
        u64 tss_limit = sizeof(tss_t) - 1;

        /* #tss-identity-overlap: the descriptor must encode base[31:24]
         * at bits 52-59. The old packing masked the base to 24 bits and
         * left bits 52-59 ZERO, so the effective TSS linear address was
         * &kernel_tss & 0xFFFFFF = 0x40A080 — a plain identity-window
         * address. The identity window is SHARED with user images: any
         * busybox-sized ELF (text at VA 0x400000..0x4F7450) overwrites
         * the split-PT PTE for VA 0x40A000 with an image frame, so the
         * CPU then reads a FAKE TSS out of the image. The fake rsp0 is
         * whatever image bytes sit at +0x84 — canonical-unmapped gives
         * #PF-on-delivery, zero gives #SS — either way the next timer
         * tick from ring 3 goes #SS/#DF -> triple fault, silently, at
         * an arbitrary point of the ash script (#bb-spawn-race).
         * With the full base the TR reads the TSS through the kernel
         * .high linear mapping (0x4040A080), which no user image can
         * shadow (images live below 512MB; the hole [1GB,2GB) is not
         * handed to brk/mmap). */
        u64 lo = (tss_limit & 0xFFFF)
               | ((tss_addr & 0xFFFFFFULL) << 16)
               | (0x89ULL << 40)                       /* P=1, DPL=0, S=0, type=0x9 */
               | (((tss_limit >> 16) & 0xFULL) << 48)  /* limit[19:16]+avl/l/d/g  */
               | (((tss_addr >> 24) & 0xFFULL) << 56); /* base[31:24] = byte 7   */

        u64 hi = (tss_addr >> 32) & 0xFFFFULL;

        kmemcpy(&gdt[5], &lo, sizeof(lo));
        kmemcpy(&gdt[6], &hi, sizeof(hi));
    }

    // Set up TSS
    kernel_tss.rsp0 = (u64)(tss_stack + 2048);  // Top of IST1 stack
    kernel_tss.ist1 = (u64)(tss_stack + 2048);   // IST1 for double fault
    kernel_tss.iomap_base = sizeof(tss_t);  // No I/O permission bitmap

    // Load GDT
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (u64)&gdt;
    gdt_flush((u64)&gdt_ptr);

    // Load TSS
    tss_flush(GDT_TSS_SEL);

    vga_print("[GDT] GDT + TSS loaded (kcode=0x08 kdata=0x10 udata=0x18 ucode=0x20 tss=0x28, IST1 stack)\n");
}

void tss_set_kernel_stack(u64 rsp0) {
    static u64 last = 0; static bool init = false;
    if (!init || rsp0 != last) {
        serial_printf("[TSSSET] rsp0=%lx ra=%lx\n",
                      (unsigned long)rsp0,
                      (unsigned long)__builtin_return_address(0));
        last = rsp0; init = true;
    }
    kernel_tss.rsp0 = rsp0;
}

const tss_t* tss_get(void) {
    return &kernel_tss;
}
