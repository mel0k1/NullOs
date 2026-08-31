#ifndef ATA_H
#define ATA_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ATA/IDE I/O ports
#define ATA_PRIMARY_BASE       0x1F0
#define ATA_PRIMARY_CONTROL    0x3F6
#define ATA_SECONDARY_BASE     0x170
#define ATA_SECONDARY_CONTROL  0x376

// Register offsets (from base)
#define ATA_DATA          0x00  // Data register (16-bit)
#define ATA_ERROR         0x01  // Error register
#define ATA_FEATURES      0x01  // Features register (write)
#define ATA_SECTOR_COUNT  0x02  // Sector count
#define ATA_LBA_LOW       0x03  // LBA low byte
#define ATA_LBA_MID       0x04  // LBA mid byte
#define ATA_LBA_HIGH      0x05  // LBA high byte
#define ATA_DRIVE_HEAD    0x06  // Drive/Head register
#define ATA_STATUS        0x07  // Status register (read)
#define ATA_COMMAND       0x07  // Command register (write)

// Control register offsets (from control base)
#define ATA_CONTROL_ALT_STATUS  0x00  // Alternate status (read)
#define ATA_CONTROL_DEV_CONTROL 0x00  // Device control (write)

// Status register bits
#define ATA_STATUS_BSY     0x80  // Busy
#define ATA_STATUS_DRDY    0x40  // Drive ready
#define ATA_STATUS_DF      0x20  // Device fault
#define ATA_STATUS_DSC     0x10  // Device seek complete
#define ATA_STATUS_DRQ     0x08  // Data request ready
#define ATA_STATUS_CORR    0x04  // Corrected data
#define ATA_STATUS_IDX     0x02  // Index
#define ATA_STATUS_ERR     0x01  // Error

// Error register bits
#define ATA_ERR_AMNF   0x01  // Address Mark Not Found
#define ATA_ERR_TKZNF  0x02  // Track Zero Not Found
#define ATA_ERR_ABRT   0x04  // Aborted command
#define ATA_ERR_MCR    0x08  // Media Change Request
#define ATA_ERR_IDNF   0x10  // ID Not Found
#define ATA_ERR_MC     0x20  // Media Changed
#define ATA_ERR_UNC    0x40  // Uncorrectable Data Error
#define ATA_ERR_BBK    0x80  // Bad Block Detected

// Drive/Head register bits
#define ATA_DH_MASTER   0x00  // Select master drive
#define ATA_DH_SLAVE    0x10  // Select slave drive
#define ATA_DH_LBA      0x40  // Use LBA addressing mode

// Device Control bits
#define ATA_DC_nIEN     0x02  // Disable interrupts
#define ATA_DC_SRST     0x04  // Software reset

// Commands
#define ATA_CMD_READ_SECTORS       0x20  // Read sectors with retry
#define ATA_CMD_WRITE_SECTORS      0x30  // Write sectors with retry
#define ATA_CMD_READ_DMA           0xC8  // Read DMA with retry
#define ATA_CMD_WRITE_DMA          0xCA  // Write DMA with retry
#define ATA_CMD_IDENTIFY           0xEC  // Identify device
#define ATA_CMD_READ_SECTORS_EXT   0x24  // Read sectors (LBA48)
#define ATA_CMD_WRITE_SECTORS_EXT  0x34  // Write sectors (LBA48)
#define ATA_CMD_FLUSH_CACHE        0xE7  // Flush cache
#define ATA_CMD_SET_FEATURES       0xEF  // Set features

// Identify device info structure (partial, key fields)
typedef struct {
    u16 words[256];  // Raw 512 bytes of identify data
} ata_identify_t;

// Drive info extracted from identify
#define ATA_SERIAL_LEN    20
#define ATA_MODEL_LEN     40
#define ATA_FIRMWARE_LEN  8

typedef struct {
    bool present;
    bool lba48_supported;
    u32  sectors_28;       // Max LBA28 sectors
    u64  sectors_48;       // Max LBA48 sectors (0 if not supported)
    char model[ATA_MODEL_LEN + 1];
    char serial[ATA_SERIAL_LEN + 1];
    char firmware[ATA_FIRMWARE_LEN + 1];
    u16  sector_size;      // Usually 512
    u16  capabilities;
} ata_drive_t;

// Initialize ATA driver (probes primary & secondary channels)
void ata_init(void);

// Read sectors from drive (LBA28, PIO mode)
// Returns 0 on success, -1 on error
int ata_read_sectors(u8 channel,   // 0=primary, 1=secondary
                      u8 drive,     // 0=master, 1=slave
                      u32 lba,      // Starting LBA
                      u8 count,     // Number of sectors (1-255)
                      void* buffer);

// Write sectors to drive (LBA28, PIO mode)
int ata_write_sectors(u8 channel, u8 drive, u32 lba, u8 count, const void* buffer);

// Read sectors using LBA48 (for drives > 137 GB)
int ata_read_sectors_ext(u8 channel, u8 drive, u64 lba, u16 count, void* buffer);

// Write sectors using LBA48
int ata_write_sectors_ext(u8 channel, u8 drive, u64 lba, u16 count, const void* buffer);

// Flush drive cache
int ata_flush_cache(u8 channel, u8 drive);

// Get drive info (must call ata_init first)
const ata_drive_t* ata_get_drive(u8 channel, u8 drive);

// Print drive information
void ata_print_info(void);

// Shell command: ata info / ata read / ata write
void cmd_ata(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // ATA_H
