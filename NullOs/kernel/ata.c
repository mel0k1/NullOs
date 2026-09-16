#include "../include/ata.h"
#include "../include/pic.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/timer.h"

// ============================================================
// Internal helpers
// ============================================================

// Drive info for up to 4 drives
static ata_drive_t drives[2][2];  // [channel][master/slave]
static bool ata_initialized = false;

// Get base I/O port for a channel
static inline u16 ata_base(u8 channel) {
    return (channel == 0) ? ATA_PRIMARY_BASE : ATA_SECONDARY_BASE;
}

// Get control base I/O port for a channel
static inline u16 ata_control(u8 channel) {
    return (channel == 0) ? ATA_PRIMARY_CONTROL : ATA_SECONDARY_CONTROL;
}

// Read a byte from a register
static inline u8 ata_read_byte(u8 channel, u16 offset) {
    return inb(ata_base(channel) + offset);
}

// Write a byte to a register
static inline void ata_write_byte(u8 channel, u16 offset, u8 val) {
    outb(ata_base(channel) + offset, val);
}

// Read a word (16-bit) from data register
static inline u16 ata_read_word(u8 channel) {
    return inw(ata_base(channel) + ATA_DATA);
}

// Write a word (16-bit) to data register
static inline void ata_write_word(u8 channel, u16 val) {
    outw(ata_base(channel) + ATA_DATA, val);
}

// Wait until BSY bit is clear (with timeout)
static bool ata_wait_busy(u8 channel, u32 timeout_us) {
    u32 ticks_start = timer_get_ticks();
    while (1) {
        u8 status = inb(ata_control(channel));
        if (!(status & ATA_STATUS_BSY)) return true;
        // Check timeout: ~10us per tick at 100Hz
        if ((timer_get_ticks() - ticks_start) > (timeout_us / 10 + 1)) {
            return false;
        }
    }
}

// Wait for DRQ (data request) with timeout
static bool ata_wait_drq(u8 channel) {
    u32 start = timer_get_ticks();
    while (1) {
        u8 status = ata_read_byte(channel, ATA_STATUS);
        if (status & ATA_STATUS_ERR) return false;
        if (status & ATA_STATUS_DF) return false;
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) return true;
        if ((timer_get_ticks() - start) > 500) return false;  // 5 second timeout
    }
}

// Select drive and wait for it to be ready
static bool ata_select_drive(u8 channel, u8 drive) {
    u8 head = ATA_DH_LBA | ((drive == 1) ? ATA_DH_SLAVE : ATA_DH_MASTER);
    ata_write_byte(channel, ATA_DRIVE_HEAD, head);
    // Wait 400ns for drive selection
    for (volatile int i = 0; i < 4; i++) inb(ata_control(channel));
    return ata_wait_busy(channel, 1000);
}

// ============================================================
// Identify device
// ============================================================

static bool ata_identify_device(u8 channel, u8 drive) {
    ata_drive_t* d = &drives[channel][drive];
    kmemset(d, 0, sizeof(ata_drive_t));

    if (!ata_select_drive(channel, drive)) {
        return false;  // No drive present
    }

    // Check if a drive is actually present by reading status
    u8 status = ata_read_byte(channel, ATA_STATUS);
    if (status == 0xFF) {
        // No device on this channel
        return false;
    }

    // Send IDENTIFY command
    ata_write_byte(channel, ATA_SECTOR_COUNT, 0);
    ata_write_byte(channel, ATA_LBA_LOW, 0);
    ata_write_byte(channel, ATA_LBA_MID, 0);
    ata_write_byte(channel, ATA_LBA_HIGH, 0);
    ata_write_byte(channel, ATA_COMMAND, ATA_CMD_IDENTIFY);

    // Check status immediately
    status = ata_read_byte(channel, ATA_STATUS);
    if (status == 0) return false;  // No drive

    // Wait for data ready
    if (!ata_wait_drq(channel)) return false;

    // Read 256 words (512 bytes) of identify data
    u16 data[256];
    for (int i = 0; i < 256; i++) {
        data[i] = ata_read_word(channel);
    }

    d->present = true;

    // Extract model number (words 27-46, byte-swapped)
    for (int i = 0; i < 20; i++) {
        u16 w = data[27 + i];
        d->model[i * 2]     = (char)(w >> 8);
        d->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    d->model[ATA_MODEL_LEN] = '\0';

    // Trim trailing spaces from model
    for (int i = ATA_MODEL_LEN - 1; i >= 0 && d->model[i] == ' '; i--) {
        d->model[i] = '\0';
    }

    // Extract serial number (words 10-19)
    for (int i = 0; i < 10; i++) {
        u16 w = data[10 + i];
        d->serial[i * 2]     = (char)(w >> 8);
        d->serial[i * 2 + 1] = (char)(w & 0xFF);
    }
    d->serial[ATA_SERIAL_LEN] = '\0';

    // Extract firmware revision (words 23-26)
    for (int i = 0; i < 4; i++) {
        u16 w = data[23 + i];
        d->firmware[i * 2]     = (char)(w >> 8);
        d->firmware[i * 2 + 1] = (char)(w & 0xFF);
    }
    d->firmware[ATA_FIRMWARE_LEN] = '\0';

    // LBA28 sector count (words 60-61)
    d->sectors_28 = (u32)data[60] | ((u32)data[61] << 16);

    // LBA48 support and sector count (words 83, 86-87)
    d->capabilities = data[49];
    if (data[83] & (1 << 10)) {
        d->lba48_supported = true;
        d->sectors_48 = (u64)data[100] | ((u64)data[101] << 16) |
                        ((u64)data[102] << 32) | ((u64)data[103] << 48);
    } else {
        d->lba48_supported = false;
        d->sectors_48 = 0;
    }

    d->sector_size = 512;

    return true;
}

// ============================================================
// Public API
// ============================================================

void ata_init(void) {
    vga_print("[ATA] Probing IDE channels...\n");

    int found = 0;

    for (int ch = 0; ch < 2; ch++) {
        for (int dr = 0; dr < 2; dr++) {
            if (ata_identify_device(ch, dr)) {
                ata_drive_t* d = &drives[ch][dr];
                const char* ch_name = (ch == 0) ? "Primary" : "Secondary";
                const char* dr_name = (dr == 0) ? "Master" : "Slave";

                vga_printf("[ATA] %s %s: %s\n", ch_name, dr_name, d->model);
                vga_printf("[ATA]   Serial: %s, FW: %s\n", d->serial, d->firmware);
                vga_printf("[ATA]   LBA28: %u sectors (%u MB)\n",
                           (u32)d->sectors_28,
                           (u32)(d->sectors_28 * 512 / 1024 / 1024));

                if (d->lba48_supported) {
                    vga_printf("[ATA]   LBA48: %llu sectors (%llu GB)\n",
                               (unsigned long long)d->sectors_48,
                               (unsigned long long)(d->sectors_48 * 512 / 1024 / 1024 / 1024));
                }
                found++;
            }
        }
    }

    if (found == 0) {
        vga_print("[ATA] No drives detected.\n");
    } else {
        vga_printf("[ATA] %d drive(s) found.\n", found);
    }

    ata_initialized = true;
}

int ata_read_sectors(u8 channel, u8 drive, u32 lba, u8 count, void* buffer) {
    if (!ata_initialized || !drives[channel][drive].present) return -1;
    if (count == 0) return -1;  // count is u8: 1..255 (255 == max LBA28 burst)

    // Disable interrupts on this channel during PIO transfer
    outb(ata_control(channel), ATA_DC_nIEN);

    if (!ata_select_drive(channel, drive)) return -1;

    // Set up LBA28 read
    ata_write_byte(channel, ATA_SECTOR_COUNT, count);
    ata_write_byte(channel, ATA_LBA_LOW,  (u8)(lba & 0xFF));
    ata_write_byte(channel, ATA_LBA_MID,  (u8)((lba >> 8) & 0xFF));
    ata_write_byte(channel, ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFF));
    ata_write_byte(channel, ATA_DRIVE_HEAD,
                   ATA_DH_LBA | ((drive == 1) ? ATA_DH_SLAVE : ATA_DH_MASTER) |
                   (u8)((lba >> 24) & 0x0F));

    // Send READ SECTORS command
    ata_write_byte(channel, ATA_COMMAND, ATA_CMD_READ_SECTORS);

    // Read each sector
    u16* buf16 = (u16*)buffer;
    for (u8 s = 0; s < count; s++) {
        if (!ata_wait_drq(channel)) return -1;

        // Read 256 words = 512 bytes
        for (int i = 0; i < 256; i++) {
            buf16[s * 256 + i] = ata_read_word(channel);
        }

        // 400ns delay between sectors
        for (volatile int j = 0; j < 4; j++) inb(ata_control(channel));
    }

    // Re-enable interrupts
    outb(ata_control(channel), 0x00);

    return 0;
}

int ata_write_sectors(u8 channel, u8 drive, u32 lba, u8 count, const void* buffer) {
    if (!ata_initialized || !drives[channel][drive].present) return -1;
    if (count == 0) return -1;  // count is u8: 1..255

    outb(ata_control(channel), ATA_DC_nIEN);

    if (!ata_select_drive(channel, drive)) return -1;

    // Set up LBA28 write
    ata_write_byte(channel, ATA_SECTOR_COUNT, count);
    ata_write_byte(channel, ATA_LBA_LOW,  (u8)(lba & 0xFF));
    ata_write_byte(channel, ATA_LBA_MID,  (u8)((lba >> 8) & 0xFF));
    ata_write_byte(channel, ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFF));
    ata_write_byte(channel, ATA_DRIVE_HEAD,
                   ATA_DH_LBA | ((drive == 1) ? ATA_DH_SLAVE : ATA_DH_MASTER) |
                   (u8)((lba >> 24) & 0x0F));

    // Send WRITE SECTORS command
    ata_write_byte(channel, ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    // Write each sector
    const u16* buf16 = (const u16*)buffer;
    for (u8 s = 0; s < count; s++) {
        if (!ata_wait_drq(channel)) return -1;

        for (int i = 0; i < 256; i++) {
            ata_write_word(channel, buf16[s * 256 + i]);
        }

        // Wait for write to complete (flush)
        ata_wait_busy(channel, 50000);
    }

    outb(ata_control(channel), 0x00);
    return 0;
}

int ata_read_sectors_ext(u8 channel, u8 drive, u64 lba, u16 count, void* buffer) {
    if (!ata_initialized || !drives[channel][drive].present) return -1;
    if (!drives[channel][drive].lba48_supported) return -1;
    // ATA LBA48 semantics: a sector-count register value of 0 means
    // 65536 sectors. `count` is u16 so any value is encodable.
    u32 actual_count = (count == 0) ? 65536u : count;

    outb(ata_control(channel), ATA_DC_nIEN);

    if (!ata_select_drive(channel, drive)) return -1;

    // LBA48: sector count is written high byte first, then low byte.
    // A zero register value encodes 65536 sectors.
    u8 hi_count = (u8)(actual_count >> 8);
    u8 lo_count = (u8)(actual_count & 0xFF);

    ata_write_byte(channel, ATA_SECTOR_COUNT, hi_count);  // High count first
    ata_write_byte(channel, ATA_LBA_LOW,  (u8)((lba >> 24) & 0xFF));
    ata_write_byte(channel, ATA_LBA_MID,  (u8)((lba >> 32) & 0xFF));
    ata_write_byte(channel, ATA_LBA_HIGH, (u8)((lba >> 40) & 0xFF));
    ata_write_byte(channel, ATA_SECTOR_COUNT, lo_count);  // Low count
    ata_write_byte(channel, ATA_LBA_LOW,  (u8)(lba & 0xFF));
    ata_write_byte(channel, ATA_LBA_MID,  (u8)((lba >> 8) & 0xFF));
    ata_write_byte(channel, ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFF));
    ata_write_byte(channel, ATA_DRIVE_HEAD,
                   ATA_DH_LBA | ((drive == 1) ? ATA_DH_SLAVE : ATA_DH_MASTER));

    ata_write_byte(channel, ATA_COMMAND, ATA_CMD_READ_SECTORS_EXT);

    u16* buf16 = (u16*)buffer;
    for (u32 s = 0; s < actual_count; s++) {
        if (!ata_wait_drq(channel)) return -1;
        for (int i = 0; i < 256; i++) {
            buf16[s * 256 + i] = ata_read_word(channel);
        }
        for (volatile int j = 0; j < 4; j++) inb(ata_control(channel));
    }

    outb(ata_control(channel), 0x00);
    return 0;
}

int ata_write_sectors_ext(u8 channel, u8 drive, u64 lba, u16 count, const void* buffer) {
    if (!ata_initialized || !drives[channel][drive].present) return -1;
    if (!drives[channel][drive].lba48_supported) return -1;
    // Same count==0 -> 65536 semantics as the read path
    u32 actual_count = (count == 0) ? 65536u : count;

    outb(ata_control(channel), ATA_DC_nIEN);

    if (!ata_select_drive(channel, drive)) return -1;

    u8 hi_count = (u8)(actual_count >> 8);
    u8 lo_count = (u8)(actual_count & 0xFF);

    ata_write_byte(channel, ATA_SECTOR_COUNT, hi_count);
    ata_write_byte(channel, ATA_LBA_LOW,  (u8)((lba >> 24) & 0xFF));
    ata_write_byte(channel, ATA_LBA_MID,  (u8)((lba >> 32) & 0xFF));
    ata_write_byte(channel, ATA_LBA_HIGH, (u8)((lba >> 40) & 0xFF));
    ata_write_byte(channel, ATA_SECTOR_COUNT, lo_count);
    ata_write_byte(channel, ATA_LBA_LOW,  (u8)(lba & 0xFF));
    ata_write_byte(channel, ATA_LBA_MID,  (u8)((lba >> 8) & 0xFF));
    ata_write_byte(channel, ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFF));
    ata_write_byte(channel, ATA_DRIVE_HEAD,
                   ATA_DH_LBA | ((drive == 1) ? ATA_DH_SLAVE : ATA_DH_MASTER));

    ata_write_byte(channel, ATA_COMMAND, ATA_CMD_WRITE_SECTORS_EXT);

    const u16* buf16 = (const u16*)buffer;
    for (u32 s = 0; s < actual_count; s++) {
        if (!ata_wait_drq(channel)) return -1;
        for (int i = 0; i < 256; i++) {
            ata_write_word(channel, buf16[s * 256 + i]);
        }
        ata_wait_busy(channel, 50000);
    }

    outb(ata_control(channel), 0x00);
    return 0;
}

int ata_flush_cache(u8 channel, u8 drive) {
    if (!ata_initialized || !drives[channel][drive].present) return -1;

    if (!ata_select_drive(channel, drive)) return -1;

    ata_write_byte(channel, ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    return ata_wait_busy(channel, 100000) ? 0 : -1;
}

const ata_drive_t* ata_get_drive(u8 channel, u8 drive) {
    if (channel > 1 || drive > 1) return NULL;
    return &drives[channel][drive];
}

void ata_print_info(void) {
    if (!ata_initialized) {
        vga_print("[ATA] Not initialized.\n");
        return;
    }

    vga_print("\nATA/IDE Drive Information:\n");
    vga_print("--------------------------\n");

    const char* ch_names[] = {"Primary", "Secondary"};
    const char* dr_names[] = {"Master", "Slave"};

    for (int ch = 0; ch < 2; ch++) {
        for (int dr = 0; dr < 2; dr++) {
            const ata_drive_t* d = &drives[ch][dr];
            if (!d->present) continue;

            vga_printf("%s %s: %s\n", ch_names[ch], dr_names[dr], d->model);
            vga_printf("  Serial:  %s\n", d->serial);
            vga_printf("  FW:      %s\n", d->firmware);
            vga_printf("  LBA28:   %u sectors (%u MB)\n",
                       (u32)d->sectors_28,
                       (u32)(d->sectors_28 * 512 / 1024 / 1024));
            if (d->lba48_supported) {
                vga_printf("  LBA48:   %llu sectors\n",
                           (unsigned long long)d->sectors_48);
            }
            vga_printf("  Sector:  %u bytes\n", d->sector_size);
            vga_printf("  LBA48:   %s\n", d->lba48_supported ? "yes" : "no");
        }
    }
    vga_print("\n");
}

// ============================================================
// Shell command: ata info | ata read <ch> <dr> <lba> <count>
//               | ata write <ch> <dr> <lba> <count> <data>
// ============================================================

void cmd_ata(int argc, char** argv) {
    if (argc < 2) {
        vga_print("Usage:\n");
        vga_print("  ata info          - Show drive information\n");
        vga_print("  ata read <ch> <drv> <lba> <n>  - Read n sectors\n");
        vga_print("  ata write <ch> <drv> <lba> <n>  - Write n sectors (fills with pattern)\n");
        vga_print("  ch: 0=primary, 1=secondary  drv: 0=master, 1=slave\n");
        return;
    }

    if (kstrcmp(argv[1], "info") == 0) {
        ata_print_info();
        return;
    }

    if (kstrcmp(argv[1], "read") == 0) {
        if (argc < 6) {
            vga_print("Usage: ata read <ch> <drv> <lba> <count>\n");
            return;
        }
        u8 ch  = (u8)katoi(argv[2]);
        u8 dr  = (u8)katoi(argv[3]);
        u32 lba = (u32)katoi(argv[4]);
        u8 cnt = (u8)katoi(argv[5]);
        if (ch > 1 || dr > 1 || cnt == 0 || cnt > 4) {
            vga_print("ata: invalid parameters (max 4 sectors for display)\n");
            return;
        }

        // Read sectors into a temporary buffer
        u8 buf[4 * 512];
        kmemset(buf, 0, sizeof(buf));

        if (ata_read_sectors(ch, dr, lba, cnt, buf) != 0) {
            vga_print("ata: read failed (no drive or timeout)\n");
            return;
        }

        vga_printf("Read %u sector(s) from ch%u:drv%u LBA %u:\n", cnt, ch, dr, lba);

        // Dump first 64 bytes of each sector as hex
        for (u8 s = 0; s < cnt; s++) {
            u8* sector = buf + s * 512;
            vga_printf("--- Sector %u (LBA %u) ---\n", s, lba + s);
            for (int row = 0; row < 4; row++) {
                vga_printf("  %04X: ", row * 16);
                for (int col = 0; col < 16; col++) {
                    vga_printf("%02X ", sector[row * 16 + col]);
                }
                // ASCII
                vga_print(" |");
                for (int col = 0; col < 16; col++) {
                    u8 c = sector[row * 16 + col];
                    vga_putchar((c >= 32 && c < 127) ? c : '.');
                }
                vga_print("|\n");
            }
            vga_print("  ...\n");
        }
        return;
    }

    if (kstrcmp(argv[1], "write") == 0) {
        if (argc < 6) {
            vga_print("Usage: ata write <ch> <drv> <lba> <count>\n");
            vga_print("  Fills sectors with 0xAA pattern (destructive!)\n");
            return;
        }
        u8 ch  = (u8)katoi(argv[2]);
        u8 dr  = (u8)katoi(argv[3]);
        u32 lba = (u32)katoi(argv[4]);
        u8 cnt = (u8)katoi(argv[5]);
        if (ch > 1 || dr > 1 || cnt == 0) {
            vga_print("ata: invalid parameters\n");
            return;
        }

        vga_print("ata: WARNING: This will overwrite disk data!\n");
        vga_printf("ata: Writing %u sectors to ch%u:drv%u LBA %u...\n", cnt, ch, dr, lba);

        // 255*512 bytes is far too large for the kernel stack — use .bss
        static u8 buf[255 * 512];
        kmemset(buf, 0xAA, cnt * 512);

        if (ata_write_sectors(ch, dr, lba, cnt, buf) != 0) {
            vga_print("ata: write failed\n");
            return;
        }

        ata_flush_cache(ch, dr);
        vga_printf("ata: Wrote %u sectors successfully.\n", cnt);
        return;
    }

    vga_printf("ata: unknown subcommand '%s'\n", argv[1]);
}
