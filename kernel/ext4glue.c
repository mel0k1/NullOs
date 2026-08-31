/* ============================================================
 * ext4glue.c — lwext4 1.0 integration into NullOs
 * ============================================================
 * The vendored lwext4 sources (kernel/ext4/, PRISTINE — zero source
 * modifications) get everything they need from this glue:
 *
 *   1. libc surface (kernel/ext4stub/*.h on their -I path only):
 *      malloc/calloc/realloc/free -> kernel heap (dlmalloc-backed
 *      kmalloc/kzalloc/krealloc/kfree — the reason the allocator
 *      migration had to land first), and memset/memcpy/... over the
 *      k* string functions.
 *
 *   2. A block device interface bound to the primary ATA disk
 *      (LBA28/LBA48 PIO, 512-byte blocks).
 *
 *   3. Persistence plumbing (#ext4-persist):
 *      - ext4_storage_init(): auto-mount attempt at boot. On success
 *        every file already in the ext4 volume is attached into the
 *        ramfs as a live backend node (same mini-VFS pattern as the
 *        FAT32 /disk mount) — reads AND writes go straight through
 *        to ext4, so guest data survives reboot by construction.
 *      - ext4_storage_sync(): copies ramfs-native (backend==NULL)
 *        files into the ext4 volume so NEW files persist too, then
 *        flushes the block cache (write_back false->true).
 *
 * Volume layout: a DEDICATED ATA disk — primary SLAVE (channel 0,
 * drive 1; QEMU: `-drive file=...,if=ide,index=1`) — is one ext4
 * filesystem (no MBR), mkfs'ed on the host with:
 *   mke2fs -t ext4 -O ^has_journal,^metadata_csum,^64bit -F <img>
 * (no journal: mount-time replay is out of scope for v1; the flag
 * set is the intersection mke2fs/lwext4 agree on bit-for-bit).
 * The primary master stays the raw-storage ramfs backup disk.
 *
 * Locking: all ext4 entry points serialize on a single spinlock
 * (lwext4 is not reentrant; no IRQ context ever touches it).
 * ============================================================ */

#include "../include/types.h"        /* FIRST: kernel bool/typedefs  */
#include "../include/spinlock.h"
#include "ext4/ext4.h"
#include "../include/ext4glue.h"
#include "../include/ata.h"
#include "../include/fs.h"
#include "../include/mm.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/serial.h"

/* ---- libc surface for kernel/ext4/*.c --------------------------- */

void* malloc(size_t n)                { return kmalloc(n); }
void* calloc(size_t a, size_t b)      { return kzalloc(a * b); }
void* realloc(void* p, size_t n)      { return krealloc(p, n); }
void  free(void* p)                   { kfree(p); }

/* memset/memcpy are NOT redefined here — kernel/dlcompat.c already
 * provides them for the whole kernel (dlmalloc needs them too). */

void*  memmove(void* d, const void* s, size_t n)     { return kmemmove(d, s, n); }
int    memcmp(const void* a, const void* b, size_t n){ return kmemcmp(a, b, n); }
size_t strlen(const char* s)                          { return kstrlen(s); }
int    strcmp(const char* a, const char* b)           { return kstrcmp(a, b); }
int    strncmp(const char* a, const char* b, size_t n){ return kstrncmp(a, b, n); }
char*  strcpy(char* d, const char* s)                 { return kstrcpy(d, s); }
char*  strncpy(char* d, const char* s, size_t n)      { return kstrncpy(d, s, n); }

/* insertion sort: lwext4 calls qsort on small element counts only */
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*)) {
    u8* b = (u8*)base;
    static u8 tmp[64];
    if (size > sizeof(tmp)) return;             /* never happens in lwext4 */
    for (size_t i = 1; i < nmemb; i++) {
        kmemcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && compar(b + (j - 1) * size, tmp) > 0) {
            kmemcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        kmemcpy(b + j * size, tmp, size);
    }
}

void abort(void) {
    serial_printf("[EXT4-ABORT] lwext4 called abort() — halting\n");
    vga_print("\n*** lwext4 abort() ***\n");
    cli();
    while (1) { hlt(); }
}

/* ---- ATA block device ------------------------------------------- */

#define EXT4_ATA_CHANNEL 0
#define EXT4_ATA_DRIVE   1     /* primary SLAVE — master is raw storage */
#define EXT4_ATA_BSIZE   512

static int ext4_ata_open(struct ext4_blockdev* bdev);
static int ext4_ata_bread(struct ext4_blockdev* bdev, void* buf,
                          uint64_t blk_id, uint32_t blk_cnt);
static int ext4_ata_bwrite(struct ext4_blockdev* bdev, const void* buf,
                           uint64_t blk_id, uint32_t blk_cnt);
static int ext4_ata_close(struct ext4_blockdev* bdev);

EXT4_BLOCKDEV_STATIC_INSTANCE(ext4_ata_bd, EXT4_ATA_BSIZE, 0,
                              ext4_ata_open, ext4_ata_bread,
                              ext4_ata_bwrite, ext4_ata_close, 0, 0);

static int ext4_ata_open(struct ext4_blockdev* bdev) {
    const ata_drive_t* d = ata_get_drive(EXT4_ATA_CHANNEL, EXT4_ATA_DRIVE);
    if (!d || !d->present) {
        serial_printf("[EXT4BD] drive 0:1 absent\n");
        return ENODEV;
    }
    /* ATA identify parsing in this QEMU setup reports sectors_28=1
     * while sectors_48 holds the true capacity, so take the max of
     * both instead of trusting the lba48 flag. */
    u64 secs = (u64)d->sectors_48;
    if ((u64)d->sectors_28 > secs) secs = d->sectors_28;
    bdev->bdif->ph_bcnt  = secs;
    bdev->part_offset    = 0;
    bdev->part_size      = secs * EXT4_ATA_BSIZE;
    serial_printf("[EXT4BD] open: lba48=%d s28=%u s48=%llu ph_bcnt=%llu "
                  "part_size=%llu\n",
                  (int)d->lba48_supported, d->sectors_28,
                  (unsigned long long)d->sectors_48,
                  (unsigned long long)secs,
                  (unsigned long long)bdev->part_size);
    return EOK;
}

/* ATA PIO transfer budget per call: the ata driver takes a u16 sector
 * count via the ext accessors (LBA48) — chunk to 255 to stay inside
 * the legacy u8 helpers on LBA28-only drives. */
#define ATA_CHUNK 255

static int ext4_ata_bread(struct ext4_blockdev* bdev, void* buf,
                          uint64_t blk_id, uint32_t blk_cnt) {
    (void)bdev;
    u8* out = (u8*)buf;
    while (blk_cnt) {
        u16 n = (blk_cnt > ATA_CHUNK) ? ATA_CHUNK : (u16)blk_cnt;
        int rc = (blk_id + n <= 0xFFFFFFFFULL)
            ? ata_read_sectors(EXT4_ATA_CHANNEL, EXT4_ATA_DRIVE,
                               (u32)blk_id, (u8)n, out)
            : ata_read_sectors_ext(EXT4_ATA_CHANNEL, EXT4_ATA_DRIVE,
                                   blk_id, n, out);
        if (rc != 0) return EIO;
        out     += (u64)n * EXT4_ATA_BSIZE;
        blk_id  += n;
        blk_cnt -= n;
    }
    return EOK;
}

static int ext4_ata_bwrite(struct ext4_blockdev* bdev, const void* buf,
                           uint64_t blk_id, uint32_t blk_cnt) {
    (void)bdev;
    const u8* in = (const u8*)buf;
    while (blk_cnt) {
        u16 n = (blk_cnt > ATA_CHUNK) ? ATA_CHUNK : (u16)blk_cnt;
        int rc = (blk_id + n <= 0xFFFFFFFFULL)
            ? ata_write_sectors(EXT4_ATA_CHANNEL, EXT4_ATA_DRIVE,
                                (u32)blk_id, (u8)n, in)
            : ata_write_sectors_ext(EXT4_ATA_CHANNEL, EXT4_ATA_DRIVE,
                                    blk_id, n, in);
        if (rc != 0) return EIO;
        in      += (u64)n * EXT4_ATA_BSIZE;
        blk_id  += n;
        blk_cnt -= n;
    }
    return EOK;
}

static int ext4_ata_close(struct ext4_blockdev* bdev) {
    (void)bdev;
    return EOK;
}

/* ---- serialization ---------------------------------------------- */

static spinlock_t ext4_lock_storage = SPINLOCK_INIT;
static bool ext4_mounted = false;

/* ---- mount / tree attach ---------------------------------------- */

#define EXT4_MP_NAME   "/mp/"
#define EXT4_DEV_NAME  "ata0disk"

/* backend ctx: ext4 path of the file, allocated at attach time */
#define EXT4BE_MAX_FILES 96
static char* ext4be_names[EXT4BE_MAX_FILES];
static u32   ext4be_count = 0;

static s64 ext4be_read(void* ctx, u64 offset, void* buf, u64 count);
static s64 ext4be_write(void* ctx, u64 offset, const void* buf, u64 count);
static u64 ext4be_size(void* ctx);

static const fs_backend_t ext4_backend = {
    "ext4", ext4be_read, ext4be_write, ext4be_size
};

/* Build "/mp" + ramfs path. ramfs paths start at '/', mp name already
 * has its trailing slash: "/mp" + "/etc/x" = "/mp/etc/x". */
static char* ext4_make_path(const char* rfs_path) {
    /* rfs_path is guaranteed to start with '/' by callers */
    size_t len = kstrlen(rfs_path);
    char* p = (char*)kmalloc(3 + len + 1);
    if (!p) return 0;
    p[0] = '/'; p[1] = 'm'; p[2] = 'p';
    kmemcpy(p + 3, rfs_path, len + 1);
    return p;
}

/* Attach every regular file under ext4 dir `epath` (ramfs path `rpath`)
 * as a live backend node. Returns number of files attached. */
static u32 ext4_attach_tree(const char* epath, const char* rpath) {
    ext4_dir d;
    u32 attached = 0;
    char echild[160];
    char rchild[160];

    u64 irq = spin_lock_irqsave(&ext4_lock_storage);
    int rc = ext4_dir_open(&d, epath);
    spin_unlock_irqrestore(&ext4_lock_storage, irq);
    if (rc != EOK) return 0;

    for (;;) {
        const ext4_direntry* de;
        u32 nlen;
        irq = spin_lock_irqsave(&ext4_lock_storage);
        de = ext4_dir_entry_next(&d);
        if (!de) { spin_unlock_irqrestore(&ext4_lock_storage, irq); break; }
        nlen = de->name_length;
        if (nlen >= sizeof(echild) - 8) { spin_unlock_irqrestore(&ext4_lock_storage, irq); continue; }
        kmemcpy(echild, de->name, nlen); echild[nlen] = 0;
        /* skip "." and ".." */
        if (echild[0] == '.' && (nlen == 1 || (nlen == 2 && echild[1] == '.'))) {
            spin_unlock_irqrestore(&ext4_lock_storage, irq);
            continue;
        }
        u32 etype = de->inode_type;
        spin_unlock_irqrestore(&ext4_lock_storage, irq);

        /* ramfs child path */
        kstrcpy(rchild, rpath);
        {
            size_t rl = kstrlen(rchild);
            if (rl > 0 && rchild[rl - 1] != '/') kstrcat(rchild, "/");
            else if (rl == 0) kstrcpy(rchild, "/");
        }
        kstrcat(rchild, echild);

        if (etype == EXT4_DE_DIR) {
            char ep[160];
            kstrcpy(ep, epath);
            {
                size_t el = kstrlen(ep);
                if (el > 0 && ep[el - 1] != '/') kstrcat(ep, "/");
            }
            kstrcat(ep, de->name);
            fs_mkdir(rchild);
            attached += ext4_attach_tree(ep, rchild);
        } else if (etype == EXT4_DE_REG_FILE) {
            if (ext4be_count >= EXT4BE_MAX_FILES) break;
            char ep[160];
            kstrcpy(ep, epath);
            {
                size_t el = kstrlen(ep);
                if (el > 0 && ep[el - 1] != '/') kstrcat(ep, "/");
            }
            kstrcat(ep, de->name);

            fs_create_file(rchild);
            char* ctx = ext4_make_path(rchild);
            if (!ctx) break;
            ext4be_names[ext4be_count++] = ctx;
            if (fs_attach_backend_by_path(rchild, &ext4_backend, ctx) == FS_OK)
                attached++;
        }
        /* other types (symlink etc.) are skipped in v1 */
    }

    irq = spin_lock_irqsave(&ext4_lock_storage);
    ext4_dir_close(&d);
    spin_unlock_irqrestore(&ext4_lock_storage, irq);
    return attached;
}

int ext4_storage_init(void) {
    const ata_drive_t* d = ata_get_drive(EXT4_ATA_CHANNEL, EXT4_ATA_DRIVE);
    if (!d || !d->present) return -1;

    u64 irq = spin_lock_irqsave(&ext4_lock_storage);
    ext4_device_unregister_all();
    int rc = ext4_device_register(&ext4_ata_bd, EXT4_DEV_NAME);
    serial_printf("[EXT4] register rc=%d\n", rc);
    if (rc != EOK) { spin_unlock_irqrestore(&ext4_lock_storage, irq); return rc; }

    rc = ext4_mount(EXT4_DEV_NAME, EXT4_MP_NAME, false);
    serial_printf("[EXT4] mount rc=%d\n", rc);
    if (rc != EOK) {
        serial_printf("[EXT4] mount failed rc=%d (no ext4 volume?)\n", rc);
        spin_unlock_irqrestore(&ext4_lock_storage, irq);
        return rc;
    }
    /* journal replay: ENOTSUP is fine (no-journal volume) */
    rc = ext4_recover(EXT4_MP_NAME);
    if (rc != EOK && rc != ENOTSUP) {
        serial_printf("[EXT4] recover rc=%d\n", rc);
    }
    ext4_cache_write_back(EXT4_MP_NAME, true);
    ext4_mounted = true;
    spin_unlock_irqrestore(&ext4_lock_storage, irq);

    u32 n = ext4_attach_tree(EXT4_MP_NAME, "");
    vga_printf("[EXT4] mounted %s on %s (%lu live files)\n",
               EXT4_DEV_NAME, EXT4_MP_NAME, (unsigned long)n);
    return EOK;
}

/* ---- backend ops (open per call, like the fat32 glue) ------------ */

static s64 ext4be_read(void* ctx, u64 offset, void* buf, u64 count) {
    if (!ext4_mounted) return -1;
    const char* path = (const char*)ctx;
    ext4_file f;
    s64 total = 0;
    u64 irq = spin_lock_irqsave(&ext4_lock_storage);
    int rc = ext4_fopen2(&f, path, O_RDONLY);
    if (rc == EOK) {
        if (offset) rc = ext4_fseek(&f, (int64_t)offset, SEEK_SET);
        if (rc == EOK) {
            size_t got = 0;
            rc = ext4_fread(&f, buf, (size_t)count, &got);
            if (rc == EOK) total = (s64)got;
        }
        ext4_fclose(&f);
    }
    spin_unlock_irqrestore(&ext4_lock_storage, irq);
    return (rc == EOK) ? total : -1;
}

static s64 ext4be_write(void* ctx, u64 offset, const void* buf, u64 count) {
    if (!ext4_mounted) return -1;
    const char* path = (const char*)ctx;
    ext4_file f;
    s64 total = -1;
    u64 irq = spin_lock_irqsave(&ext4_lock_storage);
    /* write-through: flush the whole mount cache after each write so a
     * reboot (even without ext4sync) keeps guest data on the disk */
    ext4_cache_write_back(EXT4_MP_NAME, false);
    int rc = ext4_fopen2(&f, path, O_RDWR);
    if (rc == EOK) {
        if (offset) rc = ext4_fseek(&f, (int64_t)offset, SEEK_SET);
        if (rc == EOK) {
            size_t put = 0;
            rc = ext4_fwrite(&f, buf, (size_t)count, &put);
            if (rc == EOK) total = (s64)put;
        }
        ext4_fclose(&f);
    }
    ext4_cache_write_back(EXT4_MP_NAME, true);
    spin_unlock_irqrestore(&ext4_lock_storage, irq);
    return total;
}

static u64 ext4be_size(void* ctx) {
    if (!ext4_mounted) return 0;
    const char* path = (const char*)ctx;
    ext4_file f;
    u64 sz = 0;
    u64 irq = spin_lock_irqsave(&ext4_lock_storage);
    if (ext4_fopen2(&f, path, O_RDONLY) == EOK) {
        sz = ext4_fsize(&f);
        ext4_fclose(&f);
    }
    spin_unlock_irqrestore(&ext4_lock_storage, irq);
    return sz;
}

/* ---- ramfs -> ext4 sync ------------------------------------------ */

typedef struct {
    u32 copied;
    u32 failed;
} ext4sync_ctx_t;

static int ext4sync_copy_one(const char* path, const u8* data, u32 size,
                             void* c) {
    ext4sync_ctx_t* ctx = (ext4sync_ctx_t*)c;
    char* ep = ext4_make_path(path);
    if (!ep) { ctx->failed++; return 0; }

    u64 irq = spin_lock_irqsave(&ext4_lock_storage);
    ext4_file f;
    int rc = ext4_fopen2(&f, ep, O_WRONLY | O_CREAT | O_TRUNC);
    if (rc == EOK) {
        if (size) {
            size_t put = 0;
            rc = ext4_fwrite(&f, data, size, &put);
        }
        ext4_fclose(&f);
    }
    spin_unlock_irqrestore(&ext4_lock_storage, irq);
    kfree(ep);

    if (rc == EOK) ctx->copied++;
    else ctx->failed++;
    return 0;
}

int ext4_storage_sync(void) {
    if (!ext4_mounted) return -1;

    /* ramfs-native files only: backend-backed nodes already live in
     * ext4 (fs_iterate_files skips them by contract). */
    ext4sync_ctx_t ctx = { 0, 0 };
    fs_iterate_files(ext4sync_copy_one, &ctx);

    u64 irq = spin_lock_irqsave(&ext4_lock_storage);
    ext4_cache_write_back(EXT4_MP_NAME, false);   /* full flush */
    ext4_cache_write_back(EXT4_MP_NAME, true);
    spin_unlock_irqrestore(&ext4_lock_storage, irq);

    vga_printf("[EXT4] sync: %lu copied, %lu failed\n",
               (unsigned long)ctx.copied, (unsigned long)ctx.failed);
    return 0;
}

bool ext4_is_mounted(void) { return ext4_mounted; }

/* ---- shell commands ---------------------------------------------- */

void cmd_ext4mount(int argc, char** argv) {
    (void)argc; (void)argv;
    if (ext4_mounted) { vga_print("ext4 already mounted\n"); return; }
    int r = ext4_storage_init();
    if (r != EOK) vga_printf("ext4 mount failed: %d\n", r);
}

void cmd_ext4sync(int argc, char** argv) {
    (void)argc; (void)argv;
    int r = ext4_storage_sync();
    if (r != 0) vga_print("ext4 not mounted\n");
}
