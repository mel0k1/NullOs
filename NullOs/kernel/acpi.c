#include "../include/acpi.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/serial.h"

// ============================================================
// ACPI Shutdown — finds RSDP -> XSDT/RSDT -> FADT -> PM1a_CNT
// ============================================================
//
// The shutdown mechanism uses the PM1a_CNT register:
//   Write SLP_TYPa | SLP_EN (bit 13) to PM1a_CNT
// The SLP_TYPa value is bits [12:10] of the FADT flags/dsdt.
// We use SLP_TYPa = 0 (S5 power off) for a simple shutdown.
//
// The actual S5 sleep type varies by machine, but SLP_TYPa=0 works
// on QEMU/Bochs and most real hardware for S5 (power off).
// ============================================================

// Cached shutdown register info
static bool  acpi_found = false;
static u64   pm1a_cnt_addr = 0;    // I/O port or MMIO address of PM1a_CNT
static u8    pm1a_cnt_len = 0;     // Register width in bytes
static bool  pm1a_is_io = true;    // true = I/O port, false = MMIO
static u8    slp_typa = 0;          // S5 sleep type value from DSDT \\_S5

// GAS space ID values
#define ACPI_GAS_IO       1
#define ACPI_GAS_MMIO     0

// ============================================================
// RSDP search
// ============================================================

// Search for RSDP in a memory region
static const acpi_rsdp_t* rsdp_search(u64 start, u64 end) {
    // RSDP is 16-byte aligned
    for (u64 addr = start; addr + sizeof(acpi_rsdp_t) <= end; addr += 16) {
        const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)(u64)addr;

        // Check signature "RSD PTR " (8 bytes)
        if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' ||
            rsdp->signature[2] != 'D' || rsdp->signature[3] != ' ' ||
            rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T' ||
            rsdp->signature[6] != 'R' || rsdp->signature[7] != ' ') {
            continue;
        }

        // Verify checksum (sum of all bytes = 0)
        const u8* p = (const u8*)rsdp;
        u8 sum = 0;
        u32 len = (rsdp->revision >= 2) ? rsdp->length : 20;
        for (u32 i = 0; i < len; i++) {
            sum += p[i];
        }
        if (sum == 0) {
            return rsdp;
        }
    }
    return NULL;
}

// Search main BIOS area (0xE0000 - 0xFFFFF) and EBDA
static const acpi_rsdp_t* find_rsdp(void) {
    const acpi_rsdp_t* rsdp;

    // 1. Search EBDA (Extended BIOS Data Area)
    // EBDA address is at 0x40E (16-bit segment pointer).
    // Read via asm: dereferencing a literal address makes GCC emit
    // -Warray-bounds ("subscript outside array bounds of volatile u16[0]").
    u16 ebda_seg;
    __asm__ volatile ("movzwl 0x40E, %k0" : "=r"(ebda_seg) :: "memory");
    u64 ebda_addr = (u64)ebda_seg << 4;
    if (ebda_addr != 0) {
        rsdp = rsdp_search(ebda_addr, ebda_addr + 1024);
        if (rsdp) return rsdp;
    }

    // 2. Search main BIOS area (0xE0000 - 0xFFFFF)
    rsdp = rsdp_search(0xE0000, 0x100000);
    if (rsdp) return rsdp;

    return NULL;
}

// ============================================================
// Table lookup via XSDT (preferred) or RSDT (fallback)
// ============================================================

// Find an ACPI table by 4-byte signature
const acpi_sdt_header_t* acpi_find_table(const char* signature) {
    const acpi_rsdp_t* rsdp = find_rsdp();
    if (!rsdp) return NULL;

    vga_print("[ACPI] RSDP at 0x"); vga_print_hex((u64)rsdp);
    vga_print(" rsdt=0x"); vga_print_hex(rsdp->rsdt_address);
    vga_print(" xsdt=0x"); vga_print_hex((u64)rsdp->xsdt_address);
    vga_print(" rev="); vga_print_int(rsdp->revision); vga_print("\n");

    // Check XSDT first (ACPI 2.0+)
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        const acpi_sdt_header_t* xsdt =
            (const acpi_sdt_header_t*)(u64)rsdp->xsdt_address;

        if (xsdt->signature[0] == 'X' && xsdt->signature[1] == 'S' &&
            xsdt->signature[2] == 'D' && xsdt->signature[3] == 'T') {

            u64 entry_count = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
            const u64* entries = (const u64*)((u64)xsdt + sizeof(acpi_sdt_header_t));

            for (u64 i = 0; i < entry_count; i++) {
                const acpi_sdt_header_t* table =
                    (const acpi_sdt_header_t*)(u64)entries[i];
                if (table->signature[0] == signature[0] &&
                    table->signature[1] == signature[1] &&
                    table->signature[2] == signature[2] &&
                    table->signature[3] == signature[3]) {
                    return table;
                }
            }
        }
    }

    // Fallback: RSDT (32-bit entries, ACPI 1.0)
    if (rsdp->rsdt_address != 0) {
        const acpi_sdt_header_t* rsdt =
            (const acpi_sdt_header_t*)(u64)rsdp->rsdt_address;

        if (rsdt->signature[0] == 'R' && rsdt->signature[1] == 'S' &&
            rsdt->signature[2] == 'D' && rsdt->signature[3] == 'T') {

            u64 entry_count = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
            serial_printf("[ACPI] RSDT@%lx sig=%c%c%c%c len=%ld n=%ld\n",
                          (u64)rsdt, rsdt->signature[0], rsdt->signature[1],
                          rsdt->signature[2], rsdt->signature[3],
                          (s64)rsdt->length, (s64)entry_count);

            const u32* entries = (const u32*)((u64)rsdt + sizeof(acpi_sdt_header_t));

            for (u64 i = 0; i < entry_count; i++) {
                const acpi_sdt_header_t* table =
                    (const acpi_sdt_header_t*)(u64)entries[i];
                if (table->signature[0] == signature[0] &&
                    table->signature[1] == signature[1] &&
                    table->signature[2] == signature[2] &&
                    table->signature[3] == signature[3]) {
                    return table;
                }
            }
        }
    }

    return NULL;
}

// ============================================================
// Parse GAS (Generic Address Structure) — 12 bytes
// ============================================================
// Byte 0-7:   Address (64-bit)
// Byte 8:     Address Space ID (0=MMIO, 1=IO, 2=PCI)
// Byte 9:     Register Bit Width
// Byte 10:    Register Bit Offset
// Byte 11:    Access Size (0=undefined, 1=byte, 2=word, 3=dword, 4=qword)

static void parse_gas(const u8* gas, u64* addr, bool* is_io, u8* width) {
    u64 a = 0;
    for (int i = 7; i >= 0; i--) {
        a = (a << 8) | gas[i];
    }
    *addr = a;
    *is_io = (gas[8] == ACPI_GAS_IO);
    *width = gas[9];
}

// ============================================================
// DSDT \_S5 parser — extract SLP_TYPa for S5 shutdown
// ============================================================
//
// The \_S5 object in DSDT is typically a Package with 4 elements:
//   Package(4) { \_S5a, \_S5b, 0, 0 }
// where \_S5a = PM1a_CNT sleep type (lower 2 bits of SLP_TYP for S5).
//
// AML encoding of Package(4) { byte, byte, byte, byte }:
//   0x08 0x04       — OpPackage, length 4
//   0x0A 0x0A       — BytePrefix, value 10 (hex) → WRONG for actual S5
//   Actually, the simplest pattern is:
//   0x08 0x04 0x0A XX 0x0A YY 0x0A 0x00 0x0A 0x00
//   where XX = SLP_TYPa, YY = SLP_TYPb (or zeros)
//
// But real firmware varies. We scan for the byte sequence:
//   0x08 0x04 followed by at least one 0x0A prefix byte
// and extract the first value after 0x0A as SLP_TYPa.
//
// Alternative common pattern (Windows-style):
//   \_S5 as a one-byte Package(1) or two-byte Package(2):
//   0x08 0x01 0x0A XX
// ============================================================

static void dsdt_find_s5_sleep_type(const acpi_sdt_header_t* dsdt) {
    if (!dsdt) return;
    u32 len = dsdt->length;
    if (len < 20) return;

    const u8* data = (const u8*)dsdt;

    // Scan for Package opcode (0x08) followed by length 0x04
    // or length 0x01 / 0x02 (some firmware uses shorter packages)
    for (u32 i = 0; i + 6 < len; i++) {
        // Look for "_S5" or "\_S5" name path in AML
        // NameOp(0x08) for Package, but 0x08 is also PackageOp
        // Actually: we search for the byte pattern 0x08 0x04 after a _S5 name

        // Simple approach: find Package(4) = 0x08 0x04 near "_S5"
        // Check for '_','S','5' name in AML (4 chars with null or root char prefix)
        // AML NamePath for _S5: 5F 53 35 00 ("_S5\0")
        if (data[i] == '_' && i + 3 < len &&
            data[i+1] == 'S' && data[i+2] == '5') {
            // Found "_S5" — now scan forward for Package opcode
            for (u32 j = i + 3; j + 3 < len; j++) {
                if (data[j] == 0x08) {  // PackageOp
                    u8 pkg_len = data[j + 1];
                    if (pkg_len == 0x04 && j + 2 + 4 <= len) {
                        // Package(4) — extract first byte value
                        // Format: 08 04 0A XX 0A YY 0A 00 0A 00
                        if (data[j+2] == 0x0A) {  // BytePrefix
                            slp_typa = data[j+3] & 0x07;  // SLP_TYPa is bits [2:0] in sleep value
                            vga_printf("[ACPI] DSDT \\S5 found: SLP_TYPa=%u (Package 4)\n", slp_typa);
                            return;
                        }
                    } else if (pkg_len == 0x01 && j + 2 + 2 <= len) {
                        // Package(1) — single byte
                        if (data[j+2] == 0x0A) {
                            slp_typa = data[j+3] & 0x07;
                            vga_printf("[ACPI] DSDT \\S5 found: SLP_TYPa=%u (Package 1)\n", slp_typa);
                            return;
                        }
                    } else if (pkg_len == 0x02 && j + 2 + 4 <= len) {
                        // Package(2) — two bytes (SLP_TYPa, SLP_TYPb)
                        if (data[j+2] == 0x0A) {
                            slp_typa = data[j+3] & 0x07;
                            vga_printf("[ACPI] DSDT \\S5 found: SLP_TYPa=%u (Package 2)\n", slp_typa);
                            return;
                        }
                    }
                }
            }
        }
    }

    // Second pass: just look for any Package(4) { 0x0A XX, ... } pattern
    // Some firmware puts _S5 in a scope, so name search might miss it.
    // As a heuristic, search near the end of DSDT where _S5 is often defined.
    u32 scan_start = (len > 256) ? len - 256 : 20;
    for (u32 i = scan_start; i + 6 < len; i++) {
        if (data[i] == 0x08 && data[i+1] == 0x04) {
            // Check if followed by BytePrefix pattern
            if (data[i+2] == 0x0A && data[i+4] == 0x0A) {
                slp_typa = data[i+3] & 0x07;
                vga_printf("[ACPI] DSDT \\S5 heuristic: SLP_TYPa=%u\n", slp_typa);
                return;
            }
        }
    }

    vga_print("[ACPI] DSDT \\S5 not found, using SLP_TYPa=0 (QEMU compatible)\n");
}

// ============================================================
// Public API
// ============================================================

void acpi_init(void) {
    const acpi_sdt_header_t* fadt_hdr = acpi_find_table("FACP");
    if (!fadt_hdr) {
        vga_print("[ACPI] FADT not found, shutdown will use QEMU fallback\n");
        acpi_found = false;
        return;
    }

    const acpi_fadt_t* fadt = (const acpi_fadt_t*)fadt_hdr;

    // Use X_PM1a_CNT_BLK (GAS, ACPI 2.0+) if available, else PM1a_CNT_BLK (I/O port)
    // The 64-bit GAS is at offset 208 in FADT (x_pm1a_cnt_blk)
    // Check if the table is large enough
    if (fadt_hdr->length >= 208 + 12) {
        // Use the 64-bit extended PM1a_CNT register (GAS)
        u8 width;
        parse_gas(fadt->x_pm1a_cnt_blk, &pm1a_cnt_addr, &pm1a_is_io, &width);
        pm1a_cnt_len = width / 8;
        if (pm1a_cnt_len == 0) pm1a_cnt_len = 2; // default 16-bit
    }

    // Fallback to legacy I/O port
    if (pm1a_cnt_addr == 0 && fadt->pm1a_cnt_blk != 0) {
        pm1a_cnt_addr = fadt->pm1a_cnt_blk;
        pm1a_is_io = true;
        pm1a_cnt_len = fadt->pm1_cnt_len;
        if (pm1a_cnt_len == 0) pm1a_cnt_len = 2;
    }

    if (pm1a_cnt_addr != 0) {
        acpi_found = true;
        vga_print("[ACPI] FADT found at 0x");
        vga_print_hex((u64)fadt_hdr);
        vga_print(", PM1a_CNT=");
        if (pm1a_is_io) vga_print("io "); else vga_print("mmio ");
        vga_print_hex(pm1a_cnt_addr);
        vga_print(" len=");
        vga_print_int(pm1a_cnt_len);
        vga_print("\n");

        // Parse DSDT to find \_S5 sleep type for proper shutdown
        const acpi_sdt_header_t* dsdt = NULL;
        // Try X_DSDT first (64-bit, ACPI 2.0+)
        if (fadt->x_dsdt != 0) {
            dsdt = (const acpi_sdt_header_t*)(u64)fadt->x_dsdt;
        }
        // Fallback to DSDT (32-bit)
        if (!dsdt && fadt->dsdt != 0) {
            dsdt = (const acpi_sdt_header_t*)(u64)fadt->dsdt;
        }
        if (dsdt && dsdt->length >= 20) {
            dsdt_find_s5_sleep_type(dsdt);
        } else {
            vga_print("[ACPI] DSDT not accessible, using SLP_TYPa=0\n");
        }
    } else {
        vga_print("[ACPI] FADT found but PM1a_CNT is zero, shutdown will use QEMU fallback\n");
    }
}

bool acpi_shutdown_available(void) {
    return acpi_found;
}

void acpi_shutdown(void) {
    if (!acpi_found || pm1a_cnt_addr == 0) {
        // Fallback: QEMU ACPI shutdown port
        outw(0xB004, 0x2000);
        return;
    }

    // S5 shutdown: write SLP_TYPa | SLP_EN to PM1a_CNT
    // SLP_EN = bit 13 (0x2000)
    // SLP_TYPa = bits [12:10] of the value (shifted left by 10)
    // We cached slp_typa during acpi_init().
    // If DSDT parsing failed, slp_typa = 0 which works on QEMU/Bochs.
    u16 value = (u16)(slp_typa << 10) | 0x2000;

    if (pm1a_is_io) {
        if (pm1a_cnt_len == 4) {
            outl((u16)pm1a_cnt_addr, value);
        } else {
            outw((u16)pm1a_cnt_addr, value);
        }
    } else {
        // MMIO
        if (pm1a_cnt_len == 4) {
            volatile u32* reg = (volatile u32*)(u64)pm1a_cnt_addr;
            *reg = value;
        } else {
            volatile u16* reg = (volatile u16*)(u64)pm1a_cnt_addr;
            *reg = value;
        }
    }
}
