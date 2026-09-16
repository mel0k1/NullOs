#ifndef FAT32_H
#define FAT32_H

#include "types.h"
#include "ata.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// FAT32 constants
// ============================================================

#define FAT32_SECTOR_SIZE       512
#define FAT32_CLUSTER_MIN       1
#define FAT32_MAX_FILENAME      256
#define FAT32_MAX_PATH          260
#define FAT32_ATTR_READ_ONLY    0x01
#define FAT32_ATTR_HIDDEN       0x02
#define FAT32_ATTR_SYSTEM       0x04
#define FAT32_ATTR_VOLUME_ID    0x08
#define FAT32_ATTR_DIRECTORY    0x10
#define FAT32_ATTR_ARCHIVE      0x20
#define FAT32_ATTR_LFN          0x0F  // LFN entry marker

#define FAT32_ATTR_LFN_MASK     (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | \
                                 FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID | \
                                 FAT32_ATTR_DIRECTORY | FAT32_ATTR_ARCHIVE)

// Cluster values
#define FAT32_CLUSTER_FREE     0x00000000
#define FAT32_CLUSTER_EOC_MIN  0x0FFFFFF8
#define FAT32_CLUSTER_EOC_MAX  0x0FFFFFFF
#define FAT32_CLUSTER_BAD     0x0FFFFFF7

// ============================================================
// On-disk structures (packed)
// ============================================================

// BIOS Parameter Block (part of VBR)
typedef struct {
    u8  jmp[3];          // Jump instruction
    u8  oem_id[8];       // OEM identifier
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  num_fats;         // Usually 1 for FAT32
    u16 root_entry_count; // 0 for FAT32
    u16 total_sectors_16; // 0 if > 65535
    u8  media_type;
    u16 sectors_per_fat16; // 0 for FAT32
    u16 sectors_per_track;
    u16 num_heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    // FAT32 specific fields:
    u32 sectors_per_fat;  // Size of one FAT in sectors (offset 36)
    u16 ext_flags;        // Mirror flags (offset 40, u16 per FAT32 spec!)
    u16 fs_version;       // FAT32 version (0) (offset 42)
    u32 root_cluster;     // First cluster of root directory (offset 44)
    u16 fs_info_sector;   // FSINFO structure sector (usually 1) (offset 48)
    u16 backup_boot_sector;// Backup boot sector (usually 6) (offset 50)
    u8  reserved2[12];    // offset 52-63
    u8  drive_number;
    u8  reserved3;
    u8  boot_sig;         // 0x29
    u32 volume_id;
    u8  volume_label[11];
    u8  fs_type[8];      // "FAT32   "
    u8  reserved4[420];
    u16 boot_signature;  // 0xAA55
} __attribute__((packed)) fat32_bpb_t;

// Directory entry (32 bytes)
typedef struct {
    u8  name[8];         // Short name (8.3 format)
    u8  ext[3];          // Extension
    u8  attr;            // Attributes
    u8  nt_reserved;      // NT reserved
    u8  create_time_tenth;// Create time (tenths of second)
    u16 create_time;      // Create time (hours:min:sec)
    u16 create_date;      // Create date
    u16 access_date;      // Access date
    u16 first_cluster_hi; // High 16 bits of first cluster
    u16 write_time;       // Last write time
    u16 write_date;       // Last write date
    u16 first_cluster_lo; // Low 16 bits of first cluster
    u32 file_size;       // File size in bytes
} __attribute__((packed)) fat32_dir_entry_t;

// LFN directory entry (32 bytes)
typedef struct {
    u8  ord;             // Sequence number (OR attributes for first entry)
    u8  name1[10];       // Characters 1-5 of LFN entry
    u8  attr;            // Always 0x0F for LFN
    u8  type;            // Type (0 for LFN entry)
    u8  checksum;        // Checksum of short name
    u8  name2[12];       // Characters 6-11 of LFN entry
    u16 first_cluster;   // Always 0 for LFN
    u16 name3[2];        // Characters 12-13 of LFN entry
} __attribute__((packed)) fat32_lfn_entry_t;

// ============================================================
// Internal runtime structures
// ============================================================

// Parsed FAT32 volume information
typedef struct {
    u32  bytes_per_sector;
    u8   sectors_per_cluster;
    u32  cluster_size;      // bytes_per_sector * sectors_per_cluster
    u32  reserved_sectors;
    u32  fat_offset;         // Sector offset of first FAT
    u32  fat_sectors;        // Sectors per FAT
    u32  data_offset;       // First data sector
    u32  root_cluster;       // First cluster of root dir
    u32  total_clusters;
    u32  sectors_per_fat;
    u8   active_fat;         // Which FAT is active (0 or 1)
    u32  total_sectors;
    u8   channel;            // ATA channel (0=primary, 1=secondary)
    u8   drive;              // ATA drive (0=master, 1=slave)
    bool mounted;
    // Cached FAT
    u32  fat_cache_sector;   // Currently cached FAT sector
    u8   fat_cache[512];     // FAT sector data
} fat32_vol_t;

// Open file descriptor
#define FAT32_MAX_OPEN 8

typedef struct {
    bool in_use;
    u32  first_cluster;     // First cluster of file
    u32  current_cluster;   // Current read/write position
    u32  offset_in_cluster;  // Byte offset within current cluster
    u32  cluster_size;      // Bytes per cluster (from volume)
    u64  file_offset;       // Absolute file offset
    u32  file_size;         // File size in bytes
    u8   dir_entry[32];      // Copy of directory entry
    u32  dir_entry_sector;  // Sector containing dir entry
    u32  dir_entry_index;   // Index of entry within sector
} fat32_fd_t;

// ============================================================
// Public API
// ============================================================

// Mount FAT32 filesystem from an ATA drive
// Returns 0 on success, negative on error
int fat32_mount(u8 channel, u8 drive);

// Unmount
void fat32_unmount(void);

// Check if FAT32 is mounted
bool fat32_is_mounted(void);

// Get volume info
const fat32_vol_t* fat32_get_volume(void);

// List root directory entries
int fat32_list_root(char names[][FAT32_MAX_FILENAME], int max_entries);

// Open a file by name (in root directory only for simplicity)
// Returns fd (>=0) or negative error
int fat32_open(const char* name);

// Read from open file
// Returns bytes read, or negative on error
s64 fat32_read(int fd, void* buffer, u32 count);

// Write to open file (appends or creates)
// Returns bytes written, or negative on error
s64 fat32_write(int fd, const void* buffer, u32 count);

// Close file
void fat32_close(int fd);

// Get file size
u32 fat32_get_size(int fd);

// VFS bridge: expose mounted FAT root at /disk (backend-backed nodes)
void fat32_mount_vfs(void);
void fat32_umount_vfs(void);

// Shell command: fat32
void cmd_fat32(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // FAT32_H
