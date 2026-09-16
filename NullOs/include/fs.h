#ifndef FS_H
#define FS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// File types
#define FS_FILE_TYPE_REGULAR  0
#define FS_FILE_TYPE_DIR      1
#define FS_FILE_TYPE_SYMLINK  2   /* #fs-symlink: apk packages ship links
                                   * (libz.so.1 -> libz.so.1.3.1 etc.) */

// Open flags
#define FS_READ   0x01
#define FS_WRITE  0x02
#define FS_APPEND 0x04
#define FS_CREATE 0x08
#define FS_TRUNC  0x10   /* truncate to zero on open */

// Limits
#define FS_MAX_NODES       512  /* was 256: applet symlinks + apk trees outgrew it */
#define FS_MAX_CHILDREN    96   /* was 32: /bin holds busybox + nsh applets + 50+ symlinks */
#define FS_MAX_FILE_SIZE   (16 * 1024 * 1024)  // 16 MB per file (BusyBox!)
#define FS_MAX_FDS         64

// Error codes
#define FS_OK              0
#define FS_ERR_GENERAL    -1
#define FS_ERR_NOTFOUND   -2
#define FS_ERR_PERM       -3
#define FS_ERR_NOMEM      -4
#define FS_ERR_INVAL      -5
#define FS_ERR_EXISTS     -6
#define FS_ERR_NOTDIR     -7
#define FS_ERR_NOTEMPTY   -8
#define FS_ERR_NOFDS      -9

// ============================================================
// In-memory filesystem node
// ============================================================

/* Backend ops (mini-VFS): when set on a node, data lives outside the
 * ramfs buffer — e.g. /dev/null, /dev/zero or files of a mounted
 * FAT32 volume under /disk. All callbacks may be omitted (NULL). */
typedef struct fs_backend {
    const char* name;
    s64  (*read) (void* ctx, u64 offset, void* buf, u64 count);
    s64  (*write)(void* ctx, u64 offset, const void* buf, u64 count);
    u64  (*size) (void* ctx);
} fs_backend_t;

typedef struct fs_node {
    char              name[MAX_FILENAME];
    u32               type;        // REGULAR, DIR or SYMLINK
    u32               size;        // Data length (files only)
    u8*               data;        // File data buffer (kmalloc'd, NULL for dirs)
    u32               data_cap;    // Capacity of data buffer
    struct fs_node*   parent;
    struct fs_node*   children[FS_MAX_CHILDREN];
    u32               child_count;
    bool              active;      // Slot in use
    const fs_backend_t* backend;   // NULL = plain ramfs file
    void*             backend_ctx;
    char              link_target[160]; /* SYMLINK only: raw target text */
} fs_node_t;

// ============================================================
// File descriptor
// ============================================================

typedef struct {
    fs_node_t* node;
    u64        offset;
    u32        flags;
    bool       is_open;
    u32        refs;         /* POSIX dup() shares one description */
} fs_fd_t;

// ============================================================
// Public API (same signatures as before, now functional)
// ============================================================

void  fs_init(void);
s32   fs_open(const char* path, u32 flags);
s32   fs_close(u32 fd);
s32   fs_dup(u32 fd);             /* alias: +ref on shared description */
s64   fs_read(u32 fd, void* buf, u64 count);
s64   fs_write(u32 fd, const void* buf, u64 count);
s64 fs_seek(u32 fd, s64 offset, u32 whence);
void fs_resize_fd(u32 fd, u64 new_size);
s32   fs_mkdir(const char* path);
s32   fs_rmdir(const char* path);
s32   fs_rename(const char* oldp, const char* newp);
s32   fs_create_file(const char* path);
s32   fs_delete_file(const char* path);
const char* fs_get_error(s32 err);

// ============================================================
// Extended API for shell and other consumers
// ============================================================

// Resolve a path to a node (e.g. "/dir/file" or "relative/path")
fs_node_t* fs_resolve_path(const char* path);
/* #fs-symlink: follow the FINAL component if it is a symlink (and any
 * intermediate link). Use for open()/stat(); plain fs_resolve_path()
 * stays no-follow for readlink/unlink/rename/lstat semantics. */
fs_node_t* fs_resolve_path_follow(const char* path);
/* Create a symlink node at linkpath pointing at target (raw text). */
s32 fs_symlink(const char* target, const char* linkpath);

// Node behind an open fs fd (NULL if fd invalid or not open)
fs_node_t* fs_get_node_by_fd(u32 fd);

// Get/set current working directory
fs_node_t* fs_get_cwd(void);
s32       fs_set_cwd(const char* path);

// Build an absolute path string for a node
void fs_get_path(fs_node_t* node, char* buf, size_t bufsize);

// List directory contents: fills `names` with pointers into node->name
// Returns number of entries, or negative error
int fs_list_dir(const char* path, fs_node_t** out_nodes, int max);

// Write to a file by path (creates or truncates)
s32 fs_write_file(const char* path, const void* data, u64 len);
u32 fs_get_size_by_fd(u32 fd);
s32  proc_fds_get_fsfd(u64 fd);
void proc_fds_set_fsfd(u64 fd, s32 fsfd);

// Walk every active regular file (persistence layer). The callback gets
// the full absolute path, data pointer and size. Non-zero return stops.
typedef int (*fs_file_walk_cb)(const char* path, const u8* data, u32 size, void* ctx);
u32 fs_count_files(void);
void fs_iterate_files(fs_file_walk_cb cb, void* ctx);

// Attach a backend to an existing regular file (mini-VFS mount)
s32 fs_attach_backend_by_path(const char* path, const fs_backend_t* be, void* ctx);

// File size by path (-ERR on missing)
s64 fs_file_size(const char* path);

// Linux getdents64(2) over a ramfs directory fd. Uses the fd's seek
// offset as the child-cursor. Returns bytes emitted, 0 at EOF.
s64 fs_getdents64(u32 fd, void* buf, u64 len);

#ifdef __cplusplus
}
#endif

#endif // FS_H