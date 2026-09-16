#include "../include/fat32.h"
#include "../include/ata.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/timer.h"

// ============================================================
// FAT32 Filesystem Driver for NullOs
// ============================================================
// Reads FAT32 partitions from ATA drives.
// Supports: mount, open, read, write, close, directory listing.
//
// Limitations:
//   - Root directory only (no subdirectory navigation)
//   - No long filename creation (reads LFN, writes 8.3)
//   - Single mounted volume at a time
// ============================================================

// Mounted volume
static fat32_vol_t vol;
static bool fat32_mounted = false;

// Sector read/write buffer
static u8 sector_buf[FAT32_SECTOR_SIZE];

// Open file descriptors
static fat32_fd_t fds[FAT32_MAX_OPEN];

// ============================================================
// Low-level: read/write ATA sectors
// ============================================================

static bool read_sector(u32 lba) {
    return ata_read_sectors(vol.channel, vol.drive, lba, 1, sector_buf) == 0;
}

static bool write_sector(u32 lba) {
    return ata_write_sectors(vol.channel, vol.drive, lba, 1, sector_buf) == 0;
}

// ============================================================
// FAT table access (with single-sector cache)
// ============================================================

static u32 fat_read_cluster(u32 cluster) {
    u32 fat_index = cluster * 4;  // Each FAT32 entry is 4 bytes
    u32 fat_sector = vol.fat_offset + fat_index / FAT32_SECTOR_SIZE;
    u32 fat_offset_in_sector = fat_index % FAT32_SECTOR_SIZE;

    if (fat_sector != vol.fat_cache_sector) {
        if (!read_sector(fat_sector)) return FAT32_CLUSTER_BAD;
        kmemcpy(vol.fat_cache, sector_buf, FAT32_SECTOR_SIZE);
        vol.fat_cache_sector = fat_sector;
    }

    u32 entry;
    kmemcpy(&entry, vol.fat_cache + fat_offset_in_sector, 4);
    return entry & 0x0FFFFFFF;
}

static bool fat_write_cluster(u32 cluster, u32 value) {
    u32 fat_index = cluster * 4;
    u32 fat_sector = vol.fat_offset + fat_index / FAT32_SECTOR_SIZE;
    u32 fat_offset_in_sector = fat_index % FAT32_SECTOR_SIZE;

    if (fat_sector != vol.fat_cache_sector) {
        if (!read_sector(fat_sector)) return false;
        kmemcpy(vol.fat_cache, sector_buf, FAT32_SECTOR_SIZE);
        vol.fat_cache_sector = fat_sector;
    }

    kmemcpy(vol.fat_cache + fat_offset_in_sector, &value, 4);
    kmemcpy(sector_buf, vol.fat_cache, FAT32_SECTOR_SIZE);
    return write_sector(fat_sector);
}

// ============================================================
// Cluster chain operations
// ============================================================

static u32 cluster_to_sector(u32 cluster) {
    return vol.data_offset + (cluster - 2) * vol.sectors_per_cluster;
}

static bool is_eoc(u32 cluster) {
    return cluster >= FAT32_CLUSTER_EOC_MIN && cluster <= FAT32_CLUSTER_EOC_MAX;
}

static u32 alloc_cluster(void) {
    for (u32 i = 2; i < vol.total_clusters + 2; i++) {
        u32 entry = fat_read_cluster(i);
        if (entry == FAT32_CLUSTER_FREE) {
            fat_write_cluster(i, FAT32_CLUSTER_EOC_MIN);
            // Invalidate cache
            vol.fat_cache_sector = 0xFFFFFFFF;
            return i;
        }
    }
    return 0;  // No free clusters
}

static u32 count_clusters(u32 start) {
    u32 count = 0;
    u32 c = start;
    // Guard against corrupt FAT chains that loop forever
    while (c >= 2 && !is_eoc(c) && c != FAT32_CLUSTER_BAD &&
           count <= vol.total_clusters) {
        count++;
        c = fat_read_cluster(c);
    }
    return count;
}

// ============================================================
// 8.3 filename helpers
// ============================================================

static void to_upper(u8* str, int len) {
    for (int i = 0; i < len; i++) {
        if (str[i] >= 'a' && str[i] <= 'z') str[i] -= 32;
    }
}

static void to_lower_str(char* str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] >= 'A' && str[i] <= 'Z') str[i] += 32;
    }
}

// Convert user filename to 8.3 format
// Input: "FILENAME.EXT" (case-insensitive)
// Output: 8+3 byte array (space-padded, upper)
static void name_to_83(const char* input, u8 name[11]) {
    kmemset(name, ' ', 11);

    // Find extension
    const char* dot = kstrrchr(input, '.');
    if (dot && dot != input) {
        // Copy extension (up to 3 chars)
        int ext_len = kstrlen(dot + 1);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) {
            name[8 + i] = dot[i + 1];
        }
    }

    // Copy basename (up to 8 chars)
    int base_len = dot ? (int)(dot - input) : (int)kstrlen(input);
    if (base_len > 8) base_len = 8;
    for (int i = 0; i < base_len; i++) {
        name[i] = input[i];
    }

    to_upper(name, 11);
}

// Convert 8.3 name from dir entry to display string
static void name_from_83(const u8 name83[11], char* out) {
    int pos = 0;

    // Copy base name, strip trailing spaces
    for (int i = 0; i < 8; i++) {
        if (name83[i] == ' ') {
            if (pos > 0) break;  // Skip leading spaces
            continue;
        }
        out[pos++] = name83[i];
    }

    // Check for extension
    bool has_ext = false;
    for (int i = 8; i < 11; i++) {
        if (name83[i] != ' ') { has_ext = true; break; }
    }

    if (has_ext) {
        out[pos++] = '.';
        for (int i = 8; i < 11; i++) {
            out[pos++] = name83[i];
        }
    }

    out[pos] = '\0';
}

// Compute 8.3 checksum (for LFN validation)
static u8 lfn_checksum(const u8* short_name) {
    u8 sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i];
    }
    return sum;
}

// ============================================================
// Directory entry access
// ============================================================

static u32 root_dir_sector(void) {
    return cluster_to_sector(vol.root_cluster);
}

static u32 root_dir_sectors(void) {
    // Root dir size = cluster_size * chain length
    u32 clusters = count_clusters(vol.root_cluster);
    return clusters * vol.sectors_per_cluster;
}

static bool read_dir_entry(u32 dir_sector, u32 index, fat32_dir_entry_t* entry) {
    if (!read_sector(dir_sector)) return false;
    u32 offset = index * sizeof(fat32_dir_entry_t);
    if (offset + sizeof(fat32_dir_entry_t) > FAT32_SECTOR_SIZE) return false;
    kmemcpy(entry, sector_buf + offset, sizeof(fat32_dir_entry_t));
    return true;
}

static bool write_dir_entry(u32 dir_sector, u32 index, const fat32_dir_entry_t* entry) {
    if (!read_sector(dir_sector)) return false;
    u32 offset = index * sizeof(fat32_dir_entry_t);
    if (offset + sizeof(fat32_dir_entry_t) > FAT32_SECTOR_SIZE) return false;
    kmemcpy(sector_buf + offset, entry, sizeof(fat32_dir_entry_t));
    return write_sector(dir_sector);
}

// Find a free directory entry in root dir
// Returns true and sets sector/index on success
static bool find_free_dir_entry(u32* out_sector, u32* out_index) {
    u32 sectors = root_dir_sectors();
    u32 entries_per_sector = FAT32_SECTOR_SIZE / sizeof(fat32_dir_entry_t);
    u32 start = root_dir_sector();

    for (u32 s = 0; s < sectors; s++) {
        if (!read_sector(start + s)) return false;
        for (u32 i = 0; i < entries_per_sector; i++) {
            fat32_dir_entry_t* e =
                (fat32_dir_entry_t*)(sector_buf + i * sizeof(fat32_dir_entry_t));
            if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                *out_sector = start + s;
                *out_index = i;
                return true;
            }
        }
    }
    return false;
}

// Find directory entry by name (case-insensitive)
static bool find_dir_entry(const char* name, fat32_dir_entry_t* entry,
                            u32* sector, u32* index) {
    u8 target[11];
    name_to_83(name, target);

    u32 sectors = root_dir_sectors();
    u32 entries_per_sector = FAT32_SECTOR_SIZE / sizeof(fat32_dir_entry_t);
    u32 start = root_dir_sector();

    for (u32 s = 0; s < sectors; s++) {
        if (!read_sector(start + s)) return false;
        for (u32 i = 0; i < entries_per_sector; i++) {
            fat32_dir_entry_t* e =
                (fat32_dir_entry_t*)(sector_buf + i * sizeof(fat32_dir_entry_t));
            if (e->name[0] == 0x00 || e->name[0] == 0xE5) continue;
            if (e->attr & FAT32_ATTR_VOLUME_ID) continue;
            if (e->attr & FAT32_ATTR_LFN) continue;

            // Compare 8.3 names
            if (kmemcmp(e->name, target, 11) == 0) {
                if (entry) kmemcpy(entry, e, sizeof(fat32_dir_entry_t));
                if (sector) *sector = start + s;
                if (index) *index = i;
                return true;
            }
        }
    }
    return false;
}

// ============================================================
// Public API
// ============================================================

int fat32_mount(u8 channel, u8 drive) {
    if (!ata_get_drive(channel, drive) ||
        !ata_get_drive(channel, drive)->present) {
        vga_print("[FAT32] Drive not found.\n");
        return -1;
    }

    // Read VBR (first sector of partition)
    if (ata_read_sectors(channel, drive, 0, 1, sector_buf) != 0) {
        vga_print("[FAT32] Cannot read VBR.\n");
        return -2;
    }

    fat32_bpb_t* bpb = (fat32_bpb_t*)sector_buf;

    // Validate
    if (bpb->bytes_per_sector != 512) {
        vga_printf("[FAT32] Unsupported sector size: %u\n", bpb->bytes_per_sector);
        return -3;
    }

    if (bpb->sectors_per_cluster == 0 ||
        bpb->sectors_per_cluster > 128) {
        vga_printf("[FAT32] Invalid sectors per cluster: %u\n", bpb->sectors_per_cluster);
        return -4;
    }

    if (bpb->boot_signature != 0xAA55) {
        vga_print("[FAT32] Invalid boot signature (not 0xAA55).\n");
        return -5;
    }

    // Check if this is FAT32
    bool is_fat32 = (bpb->root_entry_count == 0 && bpb->total_sectors_16 == 0
                     && bpb->sectors_per_fat16 == 0 && bpb->total_sectors_32 > 0);

    if (!is_fat32) {
        // Might be FAT16 or other - check fs_type string
        char fstype[9];
        kmemcpy(fstype, bpb->fs_type, 8);
        fstype[8] = '\0';
        vga_printf("[FAT32] Not FAT32 (type='%s'). Try mounting as FAT16?\n", fstype);
        return -6;
    }

    // Fill volume structure
    kmemset(&vol, 0, sizeof(fat32_vol_t));
    vol.bytes_per_sector = bpb->bytes_per_sector;
    vol.sectors_per_cluster = bpb->sectors_per_cluster;
    vol.cluster_size = bpb->bytes_per_sector * bpb->sectors_per_cluster;
    vol.reserved_sectors = bpb->reserved_sectors;
    vol.fat_offset = bpb->reserved_sectors;
    vol.fat_sectors = bpb->sectors_per_fat;
    vol.data_offset = bpb->reserved_sectors + bpb->sectors_per_fat * bpb->num_fats;
    vol.root_cluster = bpb->root_cluster;
    vol.total_sectors = bpb->total_sectors_32;
    vol.total_clusters = (vol.total_sectors - vol.data_offset)
                         / bpb->sectors_per_cluster;
    vol.sectors_per_fat = bpb->sectors_per_fat;
    vol.active_fat = 0;
    vol.channel = channel;
    vol.drive = drive;
    vol.mounted = true;
    vol.fat_cache_sector = 0xFFFFFFFF;

    // Initialize FD table
    kmemset(fds, 0, sizeof(fds));
    fat32_mounted = true;

    vga_printf("[FAT32] Mounted: %u clusters, %u bytes/cluster\n",
               (u32)vol.total_clusters, (u32)vol.cluster_size);
    vga_printf("[FAT32] Data start: sector %u, Total: %u MB\n",
               (u32)vol.data_offset,
               (u32)(vol.total_sectors * 512 / 1024 / 1024));

    return 0;
}

void fat32_unmount(void) {
    fat32_mounted = false;
    kmemset(&vol, 0, sizeof(fat32_vol_t));
    kmemset(fds, 0, sizeof(fds));
    vga_print("[FAT32] Unmounted.\n");
}

bool fat32_is_mounted(void) {
    return fat32_mounted;
}

const fat32_vol_t* fat32_get_volume(void) {
    return &vol;
}

int fat32_list_root(char names[][FAT32_MAX_FILENAME], int max_entries) {
    if (!fat32_mounted) return -1;

    u32 sectors = root_dir_sectors();
    u32 entries_per_sector = FAT32_SECTOR_SIZE / sizeof(fat32_dir_entry_t);
    u32 start = root_dir_sector();
    int count = 0;

    // LFN reconstruction state
    char lfn_buf[FAT32_MAX_FILENAME];
    int lfn_len = 0;
    u8  lfn_cksum = 0;
    bool has_lfn = false;

    for (u32 s = 0; s < sectors && count < max_entries; s++) {
        if (!read_sector(start + s)) break;
        for (u32 i = 0; i < entries_per_sector && count < max_entries; i++) {
            fat32_dir_entry_t* e =
                (fat32_dir_entry_t*)(sector_buf + i * sizeof(fat32_dir_entry_t));

            if (e->name[0] == 0x00 || e->name[0] == 0xE5) continue;

            // Check for LFN entry
            if (e->attr == FAT32_ATTR_LFN) {
                fat32_lfn_entry_t* lfn = (fat32_lfn_entry_t*)e;
                u8 seq = lfn->ord;

                if (seq & 0x40) {
                    // First LFN entry - reset buffer
                    lfn_len = 0;
                    lfn_cksum = lfn->checksum;
                    has_lfn = true;
                }

                if (lfn_len < FAT32_MAX_FILENAME - 13) {
                    // Decode UTF-16LE characters
                    u16 chars[13];
                    kmemcpy(chars, lfn->name1, 10);
                    kmemcpy(chars + 5, lfn->name2, 12);
                    kmemcpy(chars + 11, lfn->name3, 4);
                    for (int c = 0; c < 13; c++) {
                        u16 ch = chars[c];
                        if (ch == 0 || ch == 0xFFFF) {
                            lfn_buf[lfn_len] = '\0';
                            break;
                        }
                        // Simple ASCII handling (no full Unicode)
                        if (ch < 128) {
                            lfn_buf[lfn_len++] = (char)ch;
                        } else {
                            lfn_buf[lfn_len++] = '?';
                        }
                    }
                }
                continue;
            }

            // Regular 8.3 entry
            if (e->attr & FAT32_ATTR_VOLUME_ID) continue;

            char* name;
            char short_name[13];

            // Copy the 11 raw name bytes first: `e->name` is declared as
            // u8[8] (the base-name field) and GCC cannot see that the
            // extension bytes follow contiguously in the same entry.
            u8 sn83[11];
            kmemcpy(sn83, e->name, 11);

            if (has_lfn && lfn_cksum == lfn_checksum(sn83)) {
                lfn_buf[lfn_len] = '\0';
                name = lfn_buf;
            } else {
                name_from_83(sn83, short_name);
                name = short_name;
            }

            has_lfn = false;

            kstrncpy(names[count], name, FAT32_MAX_FILENAME - 1);
            names[count][FAT32_MAX_FILENAME - 1] = '\0';
            count++;
        }
    }

    return count;
}

int fat32_open(const char* name) {
    if (!fat32_mounted) return -1;

    fat32_dir_entry_t entry;
    u32 sector, index;

    if (!find_dir_entry(name, &entry, &sector, &index)) {
        return -2;  // File not found
    }

    if (entry.attr & FAT32_ATTR_DIRECTORY) {
        return -3;  // Is a directory
    }

    // Find free FD
    for (int i = 0; i < FAT32_MAX_OPEN; i++) {
        if (!fds[i].in_use) {
            fds[i].in_use = true;
            fds[i].first_cluster = ((u32)entry.first_cluster_hi << 16)
                                    | entry.first_cluster_lo;
            fds[i].current_cluster = fds[i].first_cluster;
            fds[i].offset_in_cluster = 0;
            fds[i].cluster_size = vol.cluster_size;
            fds[i].file_offset = 0;
            fds[i].file_size = entry.file_size;
            kmemcpy(fds[i].dir_entry, &entry, 32);
            fds[i].dir_entry_sector = sector;
            fds[i].dir_entry_index = index;
            return i;
        }
    }

    return -4;  // No free FDs
}

s64 fat32_read(int fd, void* buffer, u32 count) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN || !fds[fd].in_use) return -1;

    fat32_fd_t* f = &fds[fd];
    u8* buf = (u8*)buffer;
    u64 remaining = count;
    u64 total_read = 0;

    while (remaining > 0 && f->file_offset < f->file_size) {
        // How much can we read from current cluster?
        u32 available_in_cluster = f->cluster_size - f->offset_in_cluster;
        u32 to_read = (u32)(remaining < available_in_cluster ? remaining : available_in_cluster);

        // Read cluster data sector by sector
        u32 sector = cluster_to_sector(f->current_cluster);
        u32 byte_offset = f->offset_in_cluster;

        for (u32 i = 0; i < to_read; i++) {
            u32 s = sector + (byte_offset + i) / FAT32_SECTOR_SIZE;
            u16 off = (byte_offset + i) % FAT32_SECTOR_SIZE;

            if (!read_sector(s)) return -5;
            buf[total_read + i] = sector_buf[off];
        }

        total_read += to_read;
        f->file_offset += to_read;
        f->offset_in_cluster += to_read;

        if (f->offset_in_cluster >= f->cluster_size) {
            f->offset_in_cluster = 0;
            f->current_cluster = fat_read_cluster(f->current_cluster);
            if (f->current_cluster < 2) break;  // EOC or bad
        }
    }

    return (s64)total_read;
}

s64 fat32_write(int fd, const void* buffer, u32 count) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN || !fds[fd].in_use) return -1;

    fat32_fd_t* f = &fds[fd];
    const u8* buf = (const u8*)buffer;
    u64 remaining = count;
    u64 total_written = 0;

    /* FIX(#fat-eoc-link): after the LAST cluster fills, the advance
     * below sets current_cluster = fat_read_cluster(last) = the EOC
     * SENTINEL (0x0FFFFFF8+). The chain-extension branch then called
     * fat_write_cluster(current_cluster, new) — i.e. a FAT write at
     * index EOC*4 which TRUNCATES to u32 (~4.3GB into the disk): on
     * big volumes it corrupts an arbitrary sector, on small ones the
     * ignored ATA failure silently DROPS the link (new cluster never
     * reachable — data loss + cluster leak). Remember the last VALID
     * cluster and link from THAT. */
    u32 link_from = 0;
    if (f->current_cluster >= 2 && !is_eoc(f->current_cluster))
        link_from = f->current_cluster;
    else if (f->first_cluster >= 2)
        link_from = f->first_cluster;

    while (remaining > 0) {
        // If at end of file chain, allocate new cluster
        if (is_eoc(f->current_cluster) || f->current_cluster < 2) {
            u32 new_cluster = alloc_cluster();
            if (new_cluster == 0) return -6;  // Disk full

            // If file was empty, update first cluster in dir entry
            if (f->file_size == 0) {
                f->first_cluster = new_cluster;
                fat32_dir_entry_t* de = (fat32_dir_entry_t*)f->dir_entry;
                de->first_cluster_hi = (u16)(new_cluster >> 16);
                de->first_cluster_lo = (u16)(new_cluster & 0xFFFF);
                write_dir_entry(f->dir_entry_sector, f->dir_entry_index, de);
            } else {
                // Link the last VALID cluster to the new one
                if (link_from >= 2 && link_from != new_cluster)
                    fat_write_cluster(link_from, new_cluster);
                else
                    return -5;               /* chain state corrupted */
            }

            f->current_cluster = new_cluster;
            f->offset_in_cluster = 0;

            // Zero the new cluster
            u32 sector = cluster_to_sector(new_cluster);
            for (u32 i = 0; i < vol.sectors_per_cluster; i++) {
                kmemset(sector_buf, 0, FAT32_SECTOR_SIZE);
                if (!write_sector(sector + i)) return -7;
            }
        }

        // How much fits in current cluster?
        u32 available = f->cluster_size - f->offset_in_cluster;
        u32 to_write = (u32)(remaining < available ? remaining : available);

        // Write sector by sector
        u32 sector = cluster_to_sector(f->current_cluster);
        u32 byte_offset = f->offset_in_cluster;

        for (u32 i = 0; i < to_write; i++) {
            u32 s = sector + (byte_offset + i) / FAT32_SECTOR_SIZE;
            u16 off = (byte_offset + i) % FAT32_SECTOR_SIZE;

            if (!read_sector(s)) return -8;
            kmemcpy(sector_buf + off, buf + total_written + i, 1);
            if (!write_sector(s)) return -9;
        }

        total_written += to_write;
        f->file_offset += to_write;
        f->offset_in_cluster += to_write;

        if (f->offset_in_cluster >= f->cluster_size) {
            f->offset_in_cluster = 0;
            /* FIX(#fat-eoc-link): remember the valid cluster BEFORE the
             * advance — fat_read_cluster returns the EOC sentinel when
             * the chain is exhausted, and the next allocation must link
             * from this one, not from the sentinel. */
            if (f->current_cluster >= 2 && !is_eoc(f->current_cluster))
                link_from = f->current_cluster;
            f->current_cluster = fat_read_cluster(f->current_cluster);
        }
    }

    // Update file size in directory entry
    if (total_written > 0) {
        /* FIX(#fat-size-truncate): the size used to be overwritten with
         * file_offset UNCONDITIONALLY — a short overwrite of a longer
         * file truncated the directory entry to the new length while
         * the (kept) cluster chain still held the tail => the tail
         * clusters leaked and data silently vanished. Grow-only. */
        if (f->file_offset > f->file_size)
            f->file_size = (u32)f->file_offset;
        fat32_dir_entry_t* de = (fat32_dir_entry_t*)f->dir_entry;
        de->file_size = f->file_size;
        write_dir_entry(f->dir_entry_sector, f->dir_entry_index, de);
        ata_flush_cache(vol.channel, vol.drive);
    }

    return (s64)total_written;
}

void fat32_close(int fd) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN || !fds[fd].in_use) return;
    fds[fd].in_use = false;
}

u32 fat32_get_size(int fd) {
    if (fd < 0 || fd >= FAT32_MAX_OPEN || !fds[fd].in_use) return 0;
    return fds[fd].file_size;
}

// ============================================================
// Shell command: fat32
// ============================================================

// ============================================================
// VFS bridge: expose the mounted FAT32 root directory as ramfs
// nodes under /disk with backend ops (mini-VFS).
// ============================================================

#include "../include/fs.h"
#include "../include/mm.h"

/* Backend context: kmalloc'd copy of the FAT32 file name */
static s64 fbe_read(void* ctx, u64 offset, void* buf, u64 count) {
    const char* name = (const char*)ctx;
    int fd = fat32_open(name);
    if (fd < 0) return -1;

    /* No seek API — skip forward byte-by-chunk to reach offset */
    u8 skip[512];
    u64 skipped = 0;
    while (skipped < offset) {
        u32 chunk = (offset - skipped < sizeof(skip)) ? (u32)(offset - skipped)
                                                      : sizeof(skip);
        s64 r = fat32_read(fd, skip, chunk);
        if (r <= 0) { fat32_close(fd); return 0; }
        skipped += (u64)r;
    }

    s64 total = fat32_read(fd, buf, (u32)count);
    fat32_close(fd);
    return total;
}

static s64 fbe_write(void* ctx, u64 offset, const void* buf, u64 count) {
    const char* name = (const char*)ctx;
    if (offset != 0) return -1;      /* only overwrite-from-start supported */
    int fd = fat32_open(name);
    if (fd < 0) return -1;
    s64 n = fat32_write(fd, buf, (u32)count);
    fat32_close(fd);
    return n;
}

static u64 fbe_size(void* ctx) {
    const char* name = (const char*)ctx;
    int fd = fat32_open(name);
    if (fd < 0) return 0;
    u32 sz = fat32_get_size(fd);
    fat32_close(fd);
    return sz;
}

static const fs_backend_t fat32_backend = {
    "fat32", fbe_read, fbe_write, fbe_size
};

#define FBE_MAX_FILES 32
static char* fbe_names[FBE_MAX_FILES];   /* backend ctx lifetime */

/* Populate /disk with one backend-backed node per root file */
void fat32_mount_vfs(void) {
    fs_mkdir("/disk");                    /* EEXISTS is fine */

    /* Drop old nodes' contexts on re-mount */
    for (int i = 0; i < FBE_MAX_FILES; i++) {
        if (fbe_names[i]) { kfree(fbe_names[i]); fbe_names[i] = NULL; }
    }

    static char names[FBE_MAX_FILES][FAT32_MAX_FILENAME];
    int count = fat32_list_root(names, FBE_MAX_FILES);
    if (count <= 0) {
        vga_print("[VFS] /disk: no files found on FAT volume\n");
        return;
    }

    int attached = 0;
    for (int i = 0; i < count; i++) {
        char path[FAT32_MAX_FILENAME + 8];
        kstrcpy(path, "/disk/");
        kstrcat(path, names[i]);

        fs_create_file(path);
        fbe_names[i] = (char*)kmalloc(FAT32_MAX_FILENAME);
        if (!fbe_names[i]) break;
        kstrncpy(fbe_names[i], names[i], FAT32_MAX_FILENAME - 1);

        if (fs_attach_backend_by_path(path, &fat32_backend, fbe_names[i]) == FS_OK) {
            attached++;
        }
    }
    vga_printf("[VFS] Mounted FAT32 root at /disk (%d files)\n", attached);
}

void fat32_umount_vfs(void) {
    for (int i = 0; i < FBE_MAX_FILES; i++) {
        if (fbe_names[i]) { kfree(fbe_names[i]); fbe_names[i] = NULL; }
    }
}

// ============================================================
// Shell command
// ============================================================

void cmd_fat32(int argc, char** argv) {
    /* "mount" must be reachable from the unmounted state */
    if (argc >= 2 && kstrcmp(argv[1], "mount") == 0) {
        if (argc < 4) {
            vga_print("Usage: fat32 mount <channel> <drive>\n");
            return;
        }
        u8 ch = (u8)katoi(argv[2]);
        u8 dr = (u8)katoi(argv[3]);
        fat32_umount_vfs();
        fat32_unmount();
        int err = fat32_mount(ch, dr);
        if (err != 0) {
            vga_printf("[FAT32] Mount failed (error %d)\n", err);
        } else {
            fat32_mount_vfs();            /* expose root at /disk */
        }
        return;
    }

    if (!fat32_mounted) {
        vga_print("[FAT32] No filesystem mounted.\n");
        vga_print("Usage: fat32 mount <channel> <drive>\n");
        vga_print("  channel: 0=primary, 1=secondary\n");
        vga_print("  drive: 0=master, 1=slave\n");
        vga_print("\nCurrently detected drives (from 'ata info'):\n");
        ata_print_info();
        return;
    }

    if (argc < 2) {
        vga_print("Usage:\n");
        vga_print("  fat32 info          - Show volume info\n");
        vga_print("  fat32 ls            - List root directory\n");
        vga_print("  fat32 cat <file>    - Read file contents\n");
        vga_print("  fat32 write <file> <text> - Write text to file\n");
        vga_print("  fat32 mount <ch> <dr> - Mount FAT32 partition\n");
        vga_print("  fat32 umount        - Unmount\n");
        return;
    }

    if (kstrcmp(argv[1], "info") == 0) {
        const fat32_vol_t* v = fat32_get_volume();
        vga_print("\nFAT32 Volume Information:\n");
        vga_print("-----------------------\n");
        vga_printf("  Cluster size:   %u bytes (%u sectors)\n",
                   (u32)v->cluster_size, (u32)v->sectors_per_cluster);
        vga_printf("  Total clusters: %u\n", (u32)v->total_clusters);
        vga_printf("  Reserved:       %u sectors\n", (u32)v->reserved_sectors);
        vga_printf("  FAT offset:     sector %u (%u sectors)\n",
                   (u32)v->fat_offset, (u32)v->fat_sectors);
        vga_printf("  Data offset:     sector %u\n", (u32)v->data_offset);
        vga_printf("  Root cluster:   %u\n", (u32)v->root_cluster);
        vga_printf("  Total size:     %u MB\n",
                   (u32)(v->total_sectors * 512 / 1024 / 1024));
        vga_printf("  Drive:          ch%u drv%u\n", (u32)v->channel, (u32)v->drive);
        vga_print("\n");
        return;
    }

    if (kstrcmp(argv[1], "ls") == 0) {
        // 64 * 256 bytes is too large for the kernel stack — keep in .bss
        static char names[64][FAT32_MAX_FILENAME];
        int count = fat32_list_root(names, 64);

        if (count <= 0) {
            vga_print("(empty directory or error)\n");
            return;
        }

        for (int i = 0; i < count; i++) {
            vga_printf("  %-20s", names[i]);
            if ((i + 1) % 4 == 0) vga_print("\n");
        }
        if (count % 4 != 0) vga_print("\n");
        vga_printf("  (%d files)\n", count);
        return;
    }

    if (kstrcmp(argv[1], "cat") == 0) {
        if (argc < 3) {
            vga_print("Usage: fat32 cat <filename>\n");
            return;
        }

        int fd = fat32_open(argv[2]);
        if (fd < 0) {
            vga_printf("fat32: cannot open '%s'\n", argv[2]);
            return;
        }

        u32 size = fat32_get_size(fd);
        vga_printf("File: %s (%u bytes)\n\n", argv[2], size);

        u8 buf[256];
        s64 nread;
        while ((nread = fat32_read(fd, buf, sizeof(buf))) > 0) {
            for (s64 j = 0; j < nread; j++) {
                u8 c = buf[j];
                if (c >= 32 && c < 127) {
                    vga_putchar(c);
                } else if (c == '\n') {
                    vga_putchar('\n');
                } else {
                    vga_putchar('.');
                }
            }
        }

        fat32_close(fd);
        vga_print("\n");
        return;
    }

    if (kstrcmp(argv[1], "write") == 0) {
        if (argc < 4) {
            vga_print("Usage: fat32 write <filename> <text...>\n");
            return;
        }

        // Concatenate args 3..n into text
        char text[512];
        text[0] = '\0';
        for (int i = 3; i < argc && i < 10; i++) {
            if (i > 3) kstrcat(text, " ");
            kstrcat(text, argv[i]);
        }

        // Try opening existing file first (for append)
        int fd = fat32_open(argv[2]);
        if (fd >= 0) {
            // Seek to end for append (simple: just write, our write handles chain)
            // Actually our write always appends. For overwrite, close and recreate.
            fat32_close(fd);
        }

        fd = fat32_open(argv[2]);
        if (fd < 0 && fd != -2) {
            vga_printf("fat32: cannot create '%s'\n", argv[2]);
            return;
        }

        // If file exists, we need to truncate by creating new
        // For simplicity, if file exists, write overwrites (not implemented)
        // Just append for now
        if (fd < 0) fd = fat32_open(argv[2]);  // Won't work, already failed

        // Actually, let's handle this: open doesn't create. We need a create function.
        // For now, just report.
        vga_printf("fat32: cannot open '%s' for writing (create not implemented yet)\n", argv[2]);
        vga_print("  Note: fat32 currently only supports reading files from disk.\n");
        vga_print("  Writing requires file creation which is TODO.\n");
        return;
    }

    if (kstrcmp(argv[1], "umount") == 0) {
        fat32_umount_vfs();
        fat32_unmount();
        return;
    }

    vga_printf("fat32: unknown command '%s'\n", argv[1]);
}
