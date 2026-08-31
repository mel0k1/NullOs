#ifndef GDT_H
#define GDT_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// GDT segment selectors
// Layout: 0=null, 1=kcode, 2=kdata, 3=udata, 4=ucode, 5-6=tss
// This layout ensures SYSRET works correctly:
//   SYSCALL entry:  CS = STAR[47:32] = 0x08 (kcode), SS = 0x08+8 = 0x10 (kdata)
//   SYSRETQ return: CS = STAR[47:32]+16 = 0x18 (udata), SS = 0x08+8 = 0x10
//   (SS still wrong, so we use IRETQ for syscall return — see syscall_entry.S)
#define GDT_NULL_SEL       0x00
#define GDT_KCODE_SEL      0x08
#define GDT_KDATA_SEL      0x10
#define GDT_UDATA_SEL      0x18
#define GDT_UCODE_SEL      0x20
#define GDT_TSS_SEL        0x28

/* Selectors as LOADED into segment registers by ring-3 code.
 * The low two bits are the Requested Privilege Level and MUST be 3
 * for anything executed at CPL=3. Raw indexes above are only for
 * addressing the GDT itself. */
#define USER_CS            0x23    /* GDT_UCODE_SEL | RPL3 */
#define USER_SS            0x1B    /* GDT_UDATA_SEL | RPL3 */

// TSS structure (minimal for 64-bit)
typedef struct {
    u32 reserved0;     // 0x00
    u64 rsp0;          // 0x04: Kernel stack for ring transitions
    u64 rsp1;          // 0x0C
    u64 rsp2;          // 0x14
    u64 reserved1;     // 0x1C
    u64 reserved2;     // 0x24
    u64 ist1;          // 0x2C: Interrupt Stack Table 1
    u64 ist2;          // 0x34
    u64 ist3;          // 0x3C
    u64 ist4;          // 0x44
    u64 ist5;          // 0x4C
    u64 ist6;          // 0x54
    u64 ist7;          // 0x5C
    u64 reserved3;     // 0x64
    u64 reserved4;     // 0x6C
    u16 reserved5;     // 0x74
    u16 iomap_base;    // 0x76: I/O map base offset
} __attribute__((packed)) tss_t;

// Initialize GDT + TSS and load them
void gdt_init(void);

// Set the kernel stack pointer used for ring0 transitions (TSS.RSP0)
void tss_set_kernel_stack(u64 rsp0);

// Get current TSS
const tss_t* tss_get(void);

#ifdef __cplusplus
}
#endif

#endif // GDT_H
