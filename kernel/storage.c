/* ============================================================
 * storage.c — Persistent ramfs backup on ATA drive
 * ============================================================
 * Serializes the in-memory ramfs to raw ATA sectors so files
 * survive reboot. On load, reads sectors back into ramfs.
 *
 * Layout on disk (LBA):
 *   LBA+0: Header { magic[8] version u32 node_count u32 data_lba u32 }
 *   LBA+1..N: File entries { path[256] size u32 }
 *   LBA+N+1..: File contents sequentially
 */

#include "../include/storage.h"
#include "../include/ext4glue.h"
#include "../include/ata.h"
#include "../include/fs.h"
#include "../include/vga.h"
#include "../include/string.h"

#define SECTORS_PER_ENTRY  1    /* Each file entry = 512B (path is ~256B) */
#define HEADER_SECTORS     1
#define CHUNK_SECTORS      128  /* Max sectors per ATA r/w call (count=u8) */

/* Scratch sector buffer (static, not on stack) */
static u8 sec_buf[STORAGE_SECTOR];

/* ---- Write all ramfs contents to disk ---------------------------*/

/* State carried across fs_iterate_files callbacks */
typedef struct {
    u32 entry_lba;        /* LBA for the next metadata sector   */
    u32 data_lba_next;    /* next free LBA in the data area     */
} sync_ctx_t;

static int sync_write_entry(const char* path, const u8* data, u32 size, void* c) {
    sync_ctx_t* ctx = (sync_ctx_t*)c;

    /* Metadata sector: path at 0, size at 256, data LBA at 260 */
    kmemset(sec_buf, 0, STORAGE_SECTOR);
    kstrncpy((char*)sec_buf, path, 255);
    *(u32*)(sec_buf + 256) = size;
    *(u32*)(sec_buf + 260) = ctx->data_lba_next;

    ata_write_sectors(0, 0, ctx->entry_lba++, 1, sec_buf);

    /* File data in chunks: ata_write_sectors takes a u8 count, and a
     * 128KB file needs 256 sectors which would overflow a single call. */
    if (size > 0 && data) {
        u32 remaining = size;
        u32 src_off   = 0;
        while (remaining > 0) {
            u32 need  = (remaining + STORAGE_SECTOR - 1) / STORAGE_SECTOR;
            u8  chunk = (need > CHUNK_SECTORS) ? CHUNK_SECTORS : (u8)need;
            ata_write_sectors(0, 0, ctx->data_lba_next, chunk, data + src_off);
            u32 bytes = (u32)chunk * STORAGE_SECTOR;
            src_off           += bytes;
            ctx->data_lba_next += chunk;
            remaining          = (remaining > bytes) ? (remaining - bytes) : 0;
        }
    }
    return 0;
}

int storage_sync(void) {
    u32 file_count = fs_count_files();

    /* Build header sector */
    kmemset(sec_buf, 0, STORAGE_SECTOR);
    kmemcpy(sec_buf, STORAGE_MAGIC, 7);
    *(u32*)(sec_buf + 8)  = 1;              /* version */
    *(u32*)(sec_buf + 12) = file_count;
    *(u32*)(sec_buf + 16) = HEADER_SECTORS + file_count * SECTORS_PER_ENTRY + 1;

    ata_write_sectors(0, 0, STORAGE_START_LBA, 1, sec_buf);

    sync_ctx_t ctx;
    ctx.entry_lba     = STORAGE_START_LBA + HEADER_SECTORS;
    ctx.data_lba_next = STORAGE_START_LBA + HEADER_SECTORS
                        + file_count * SECTORS_PER_ENTRY;

    fs_iterate_files(sync_write_entry, &ctx);

    vga_printf("[SYNC] %lu files saved to disk\n", (unsigned long)file_count);
    return 0;
}

/* ---- Ensure the parent directory chain of a path exists ----------*/

static void storage_ensure_dirs(const char* path) {
    char buf[256];
    for (const char* p = path; *p; p++) {
        if (*p == '/' && p != path) {
            u32 len = (u32)(p - path);
            if (len >= sizeof(buf)) return;
            kmemcpy(buf, path, len);
            buf[len] = '\0';
            fs_mkdir(buf);               /* EEXISTS errors are fine */
        }
    }
}

/* ---- Load ramfs from disk ----------------------------------------*/

int storage_load(void) {
    /* Read header */
    kmemset(sec_buf, 0, STORAGE_SECTOR);
    ata_read_sectors(0, 0, STORAGE_START_LBA, 1, sec_buf);

    /* Check magic */
    if (kmemcmp(sec_buf, STORAGE_MAGIC, 7) != 0) {
        return -1;                           /* No saved filesystem */
    }

    u32 node_count = *(u32*)(sec_buf + 12);
    if (node_count == 0 || node_count > STORAGE_MAX_NODES) return -1;

    vga_printf("[MOUNT] Restoring %lu files from disk...\n",
               (unsigned long)node_count);

    /* Read and restore each file */
    u32 entry_lba = STORAGE_START_LBA + HEADER_SECTORS;

    for (u32 i = 0; i < node_count; i++) {
        kmemset(sec_buf, 0, STORAGE_SECTOR);
        ata_read_sectors(0, 0, entry_lba, 1, sec_buf);
        entry_lba++;

        char* path = (char*)sec_buf;
        u32 fsize = *(u32*)(sec_buf + 256);
        u32 data_lba = *(u32*)(sec_buf + 260);

        if (fsize == 0 || fsize > FS_MAX_FILE_SIZE) continue;   /* sanity */

        storage_ensure_dirs(path);

        /* Create file in ramfs */
        s32 fd = fs_open(path, FS_WRITE | FS_CREATE);
        if (fd < 0) continue;

        /* Read data from disk in chunks (ATA count is u8-limited) and
         * write straight into the ramfs. */
        static u8 chunk_buf[CHUNK_SECTORS * STORAGE_SECTOR];   /* 64KB */
        u32 remaining = fsize;
        while (remaining > 0) {
            u32 need  = (remaining + STORAGE_SECTOR - 1) / STORAGE_SECTOR;
            u8  chunk = (need > CHUNK_SECTORS) ? CHUNK_SECTORS : (u8)need;
            ata_read_sectors(0, 0, data_lba, chunk, chunk_buf);
            u32 bytes = remaining < (u32)chunk * STORAGE_SECTOR
                      ? remaining : (u32)chunk * STORAGE_SECTOR;
            fs_write(fd, chunk_buf, bytes);
            data_lba  += chunk;
            remaining -= bytes;
        }
        fs_close(fd);
    }

    vga_print("[MOUNT] Restore complete.\n");
    return 0;
}

/* ---- Check if disk has saved data ---------------------------------*/

bool storage_has_data(void) {
    kmemset(sec_buf, 0, STORAGE_SECTOR);
    ata_read_sectors(0, 0, STORAGE_START_LBA, 1, sec_buf);
    return (kmemcmp(sec_buf, STORAGE_MAGIC, 7) == 0);
}

/* ---- Shell commands ------------------------------------------------*/

void cmd_sync(int argc, char** argv) {
    (void)argc; (void)argv;
    int r = storage_sync();
    if (r == 0) vga_print("Filesystem synced to disk.\n");
    else vga_printf("Sync failed: %d\n", r);
    /* #ext4-persist: also push ramfs-native files into the ext4
     * volume (backend-backed nodes already live there). */
    if (ext4_is_mounted()) ext4_storage_sync();
}

void cmd_mount(int argc, char** argv) {
    (void)argc; (void)argv;
    int r = storage_load();
    if (r == 0) vga_print("Filesystem restored from disk.\n");
    else vga_print("No saved filesystem found on disk.\n");
}