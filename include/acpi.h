#ifndef ACPI_H
#define ACPI_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// RSDP (Root System Description Pointer)
typedef struct {
    char    signature[8];     // "RSD PTR "
    u8      checksum;
    char    oem_id[6];
    u8      revision;         // 0 = ACPI 1.0, 2+ = ACPI 2.0+
    u32     rsdt_address;     // Physical address of RSDT (32-bit)
    // ACPI 2.0+ fields:
    u32     length;           // Length of this table
    u64     xsdt_address;     // Physical address of XSDT (64-bit)
    u8      extended_checksum;
    u8      reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

// ACPI SDT header (common to all ACPI tables)
typedef struct {
    char    signature[4];     // e.g. "FACP", "APIC", "DSDT"
    u32     length;           // Total table length
    u8      revision;
    u8      checksum;
    char    oem_id[6];
    char    oem_table_id[8];
    u32     oem_revision;
    u32     creator_id;
    u32     creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

// FADT (Fixed ACPI Description Table)
// We only define the fields we need for shutdown
typedef struct {
    acpi_sdt_header_t header;
    u32     fadt_len;         // FADT length (often == header.length)
    u32     firmware_ctrl;
    u32     dsdt;
    u8      reserved0;
    u8      preferred_pm_profile;
    u16     sci_int;
    u32     smi_cmd;
    u8      acpi_enable;
    u8      acpi_disable;
    u8      s4bios_req;
    u8      pstate_cnt;
    u32     pm1a_evt_blk;
    u32     pm1b_evt_blk;
    u32     pm1a_cnt_blk;
    u32     pm1b_cnt_blk;
    u32     pm2_cnt_blk;
    u32     pm_tmr_blk;
    u32     gpe0_blk;
    u32     gpe1_blk;
    u8      pm1_evt_len;
    u8      pm1_cnt_len;
    u8      pm2_cnt_len;
    u8      pm_tmr_len;
    u8      gpe0_blk_len;
    u8      gpe1_blk_len;
    u8      gpe1_base;
    u8      cst_cnt;
    u16     p_lvl2_lat;
    u16     p_lvl3_lat;
    u16     flush_size;
    u16     flush_stride;
    u8      duty_offset;
    u8      duty_width;
    u8      day_alrm;
    u8      mon_alrm;
    u8      century;
    u16     iapc_boot_arch;
    u8      reserved1;
    u32     flags;
    // ACPI 2.0+ reset register (Generic Address Structure)
    u8      reset_reg[12];   // GAS: addr(8) + space(1) + bit_width(1) + bit_offset(1) + access_size(1)
    u8      reset_value;
    u8      reserved2[3];
    // 64-bit versions of PM registers (ACPI 2.0+)
    u64     x_firmware_ctrl;
    u64     x_dsdt;
    // GAS structures for PM registers
    u8      x_pm1a_evt_blk[12];
    u8      x_pm1b_evt_blk[12];
    u8      x_pm1a_cnt_blk[12];
    u8      x_pm1b_cnt_blk[12];
    u8      x_pm2_cnt_blk[12];
    u8      x_pm_tmr_blk[12];
    u8      x_gpe0_blk[12];
    u8      x_gpe1_blk[12];
} __attribute__((packed)) acpi_fadt_t;

// Initialize ACPI: find RSDP, parse FADT, cache shutdown info
void acpi_init(void);

// Find an ACPI table by 4-byte signature (e.g. "FACP", "APIC", "DSDT").
// Walks XSDT (ACPI 2.0+) with RSDT fallback. Returns NULL if not found.
const acpi_sdt_header_t* acpi_find_table(const char* signature);

// Shut down the machine using ACPI (if available)
// Falls back to QEMU port 0xB004 if ACPI not found
void acpi_shutdown(void);

// Returns true if ACPI shutdown is available
bool acpi_shutdown_available(void);

#ifdef __cplusplus
}
#endif

#endif // ACPI_H
