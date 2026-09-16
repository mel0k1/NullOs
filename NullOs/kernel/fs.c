#include "../include/fs.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/string.h"
#include "../include/mm.h"

// ============================================================
// Global state
// ============================================================

static fs_node_t fs_nodes[FS_MAX_NODES];
static u32      fs_node_count = 0;
static fs_node_t* fs_root = NULL;
static fs_node_t* fs_cwd  = NULL;   // current working directory
static fs_fd_t   fs_fds[FS_MAX_FDS];

// ============================================================
// Internal helpers
// ============================================================

// Allocate a node slot
static fs_node_t* fs_alloc_node(void) {
    for (u32 i = 0; i < FS_MAX_NODES; i++) {
        if (!fs_nodes[i].active) {
            kmemset(&fs_nodes[i], 0, sizeof(fs_node_t));
            fs_nodes[i].active = true;
            return &fs_nodes[i];
        }
    }
    return NULL;
}

// Free a node slot (does NOT free children or data)
static void fs_free_node(fs_node_t* node) {
    if (!node) return;
    if (node->data) {
        kfree(node->data);
        node->data = NULL;
    }
    node->active = false;
}

// Find a child by name in a directory
static fs_node_t* fs_find_child(fs_node_t* dir, const char* name) {
    if (!dir || dir->type != FS_FILE_TYPE_DIR) return NULL;
    for (u32 i = 0; i < dir->child_count; i++) {
        if (dir->children[i] && kstrcmp(dir->children[i]->name, name) == 0) {
            return dir->children[i];
        }
    }
    return NULL;
}

// Add a child to a directory
static s32 fs_add_child(fs_node_t* dir, fs_node_t* child) {
    if (!dir || dir->type != FS_FILE_TYPE_DIR) return FS_ERR_NOTDIR;
    if (dir->child_count >= FS_MAX_CHILDREN) return FS_ERR_GENERAL;
    dir->children[dir->child_count++] = child;
    child->parent = dir;
    return FS_OK;
}

// Remove a child from a directory (does NOT free the child node)
static void fs_remove_child(fs_node_t* dir, fs_node_t* child) {
    if (!dir) return;
    for (u32 i = 0; i < dir->child_count; i++) {
        if (dir->children[i] == child) {
            // Shift remaining children
            for (u32 j = i; j < dir->child_count - 1; j++) {
                dir->children[j] = dir->children[j + 1];
            }
            dir->child_count--;
            dir->children[dir->child_count] = NULL;
            child->parent = NULL;
            return;
        }
    }
}

// Get the last component of a path (e.g. "/a/b/c" -> "c")
static const char* fs_basename(const char* path) {
    const char* last_slash = kstrrchr(path, '/');
    if (!last_slash) return path;
    if (last_slash[1] == '\0') {
        // Trailing slash — go back further
        // For simplicity, just return the part before
        while (last_slash > path && *last_slash == '/') last_slash--;
        const char* prev = kstrrchr(path, '/');
        return prev ? prev + 1 : path;
    }
    return last_slash + 1;
}

// ============================================================
// Public API
// ============================================================

static void devfs_init(void);

void fs_init(void) {
    kmemset(fs_nodes, 0, sizeof(fs_nodes));
    kmemset(fs_fds, 0, sizeof(fs_fds));
    fs_node_count = 0;

    // Create root directory
    fs_root = fs_alloc_node();
    if (!fs_root) {
        vga_print("[FS] ERROR: Cannot allocate root node\n");
        return;
    }
    kstrcpy(fs_root->name, "/");
    fs_root->type = FS_FILE_TYPE_DIR;
    fs_root->parent = fs_root;  // root's parent is itself
    fs_cwd = fs_root;

    // Create some default directories
    fs_mkdir("/bin");
    fs_mkdir("/dev");
    fs_mkdir("/tmp");

    devfs_init();

    vga_print("[FS] RAM-backed filesystem initialized (NULFS v2 + devfs)\n");
}

// ============================================================
// devfs: /dev/null and /dev/zero as backend-backed nodes
// ============================================================

static s64 devnull_read(void* ctx, u64 offset, void* buf, u64 count) {
    (void)ctx; (void)offset; (void)buf; (void)count;
    return 0;                                    /* immediate EOF */
}

static s64 devnull_write(void* ctx, u64 offset, const void* buf, u64 count) {
    (void)ctx; (void)offset; (void)buf;
    return (s64)count;                           /* black hole */
}

static s64 devzero_read(void* ctx, u64 offset, void* buf, u64 count) {
    (void)ctx; (void)offset;
    kmemset(buf, 0, count);
    return (s64)count;
}

static s64 devzero_write(void* ctx, u64 offset, const void* buf, u64 count) {
    (void)ctx; (void)offset; (void)buf;
    return (s64)count;                           /* black hole */
}

static const fs_backend_t dev_null_backend = {
    "devnull", devnull_read, devnull_write, NULL
};
static const fs_backend_t dev_zero_backend = {
    "devzero", devzero_read, devzero_write, NULL
};

static void devfs_init(void) {
    if (fs_create_file("/dev/null") == FS_OK || fs_resolve_path("/dev/null")) {
        fs_attach_backend_by_path("/dev/null", &dev_null_backend, NULL);
    }
    if (fs_create_file("/dev/zero") == FS_OK || fs_resolve_path("/dev/zero")) {
        fs_attach_backend_by_path("/dev/zero", &dev_zero_backend, NULL);
    }
    /* placeholder nodes for the syscall-level char devices: open() on
     * these paths is intercepted BEFORE fs_open (same pattern as the
     * virtual /dev/tty), the nodes exist so ls/stat see the topology */
    fs_mkdir("/dev/input");
    fs_create_file("/dev/input/event0");
    fs_create_file("/dev/input/event1");
    fs_mkdir("/dev/dri");
    fs_create_file("/dev/dri/card0");
}

static fs_node_t* fs_walk_path(const char* path_in, bool follow_final)
{
    char bufa[400], bufb[400];
    char* cur = bufa;
    char* nxt = bufb;
    fs_node_t* carry_start = NULL;   /* start node for a relative splice */
    int hops = 0;
    bool spliced;

    if (!path_in) return NULL;
    kstrncpy(cur, path_in, sizeof(bufa) - 1);
    cur[sizeof(bufa) - 1] = '\0';

    for (;;) {
        if (++hops > 8) return NULL;            /* symlink loop → ELOOP */
        if (!fs_root)  return NULL;

        fs_node_t* current;
        const char* p = cur;
        if (carry_start) {
            current = carry_start;
            carry_start = NULL;
        } else if (p[0] == '/') {
            current = fs_root;
            while (*p == '/') p++;
        } else {
            current = fs_cwd;
        }
        if (!p[0]) return current;

        spliced = false;
        while (*p && !spliced) {
            char component[MAX_FILENAME];
            u32  ci = 0;
            while (*p && *p != '/') {
                if (ci < MAX_FILENAME - 1) component[ci++] = *p;
                p++;
            }
            /* #walk-separator-skip: consume the '/' separator — the
             * original rewrite left p AT the slash, so every
             * multi-component path spun forever on an empty component */
            if (*p == '/') p++;
            component[ci] = '\0';
            bool final = (*p == '\0');

            if (component[0] == '\0' || kstrcmp(component, ".") == 0) {
                if (final) return current;
                continue;                       /* '//' or '.' mid-path  */
            }
            if (kstrcmp(component, "..") == 0) {
                if (current != fs_root) current = current->parent;
                if (final) return current;
                continue;
            }

            fs_node_t* child = fs_find_child(current, component);
            if (!child) return NULL;

            if (child->type == FS_FILE_TYPE_SYMLINK &&
                (!final || follow_final)) {
                /* splice: remaining path becomes <target>[/<rest>] */
                const char* target = child->link_target;
                const char* rest   = (*p == '/') ? p + 1 : p;
                u32 w = 0;
                if (target[0] != '/')
                    carry_start = current;      /* relative to link dir */
                while (target[w] && w < 399) {
                    nxt[w] = target[w];
                    w++;
                }
                if (*rest && w < 399) {
                    if (w && nxt[w - 1] != '/') nxt[w++] = '/';
                    while (*rest && w < 399) {
                        nxt[w++] = *rest++;
                    }
                }
                nxt[w] = '\0';
                char* tmp = cur; cur = nxt; nxt = tmp;
                spliced = true;
                break;
            }

            current = child;
            if (final) return current;
        }
        if (!spliced) return current;   /* walked to a directory prefix end */
    }
}

fs_node_t* fs_resolve_path(const char* path) {
    return fs_walk_path(path, false);   /* no-follow: unlink/rename/readlink */
}

fs_node_t* fs_resolve_path_follow(const char* path) {
    return fs_walk_path(path, true);    /* follow: open/stat semantics */
}

static s32 fs_split_path(const char* path, fs_node_t** out_dir, char* out_name);

/* #fs-symlink: create a symlink node holding the raw target text. */
s32 fs_symlink(const char* target, const char* linkpath) {
    if (!target || !linkpath) return FS_ERR_INVAL;
    if (!target[0]) return FS_ERR_INVAL;

    char name[MAX_FILENAME];
    fs_node_t* dir = NULL;
    s32 err = fs_split_path(linkpath, &dir, name);
    if (err != FS_OK) return err;
    if (!dir || dir->type != FS_FILE_TYPE_DIR) return FS_ERR_NOTDIR;
    if (!name[0]) return FS_ERR_INVAL;
    if (fs_find_child(dir, name)) return FS_ERR_EXISTS;

    fs_node_t* node = fs_alloc_node();
    if (!node) return FS_ERR_NOMEM;

    kstrncpy(node->name, name, MAX_FILENAME - 1);
    node->type = FS_FILE_TYPE_SYMLINK;
    node->size = 0;
    node->data = NULL;
    node->data_cap = 0;
    kstrncpy(node->link_target, target, sizeof(node->link_target) - 1);
    node->link_target[sizeof(node->link_target) - 1] = '\0';

    return fs_add_child(dir, node);
}

// Get the parent directory and the last component of a path
static s32 fs_split_path(const char* path, fs_node_t** out_dir, char* out_name) {
    if (!path || !path[0]) return FS_ERR_INVAL;

    // Copy path to mutable buffer
    char buf[MAX_PATH];
    kstrncpy(buf, path, MAX_PATH - 1);
    buf[MAX_PATH - 1] = '\0';

    // Remove trailing slashes
    size_t len = kstrlen(buf);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

    // Find last slash
    char* last_slash = kstrrchr(buf, '/');

    // Compute the basename first and make sure it fits into out_name
    // (MAX_FILENAME bytes). Without this check a long final component
    // overflows the caller's fixed-size buffer on the stack.
    const char* base = last_slash ? last_slash + 1 : buf;
    if (kstrlen(base) >= MAX_FILENAME) return FS_ERR_INVAL;

    if (!last_slash) {
        // No slash — relative path, parent is CWD
        kstrcpy(out_name, buf);
        *out_dir = fs_cwd;
        return FS_OK;
    }

    if (last_slash == buf) {
        // "/filename" — parent is root
        kstrcpy(out_name, last_slash + 1);
        *out_dir = fs_root;
        return FS_OK;
    }

    // Split: dir part is buf[0..last_slash], name is last_slash+1
    *last_slash = '\0';
    kstrcpy(out_name, last_slash + 1);

    // If name is empty, the path referred to a directory
    if (out_name[0] == '\0') {
        // Re-attach the slash and resolve
        *last_slash = '/';
        *out_dir = fs_resolve_path_follow(buf);
        if (!*out_dir) return FS_ERR_NOTFOUND;
        return FS_OK;
    }

    *out_dir = fs_resolve_path_follow(buf);
    if (!*out_dir) return FS_ERR_NOTFOUND;
    return FS_OK;
}

s32 fs_open(const char* path, u32 flags) {
    if (!path) return FS_ERR_INVAL;

    /* #fs-symlink: open() follows a final symlink to its target */
    fs_node_t* node = fs_resolve_path_follow(path);

    // If FS_CREATE and file doesn't exist, create it
    if (!node && (flags & FS_CREATE)) {
        fs_create_file(path);
        node = fs_resolve_path_follow(path);
    }

    if (!node) return FS_ERR_NOTFOUND;

    /* O_TRUNC: zero the size up front (backend nodes manage own size) */
    if ((flags & FS_TRUNC) && node->type == FS_FILE_TYPE_REGULAR && !node->backend) {
        node->size = 0;
    }

    // Find a free fd
    for (u32 i = 0; i < FS_MAX_FDS; i++) {
        if (!fs_fds[i].is_open) {
            fs_fds[i].node    = node;
            fs_fds[i].offset  = (flags & FS_APPEND) ? node->size : 0;
            fs_fds[i].flags   = flags;
            fs_fds[i].is_open = true;
            fs_fds[i].refs    = 1;
            return (s32)i;
        }
    }

    return FS_ERR_NOFDS;
}

s32 fs_close(u32 fd) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return FS_ERR_INVAL;
    /* POSIX dup(): several process fds may alias ONE open-file
     * description. Only the LAST close really frees it, otherwise
     * busybox ash's setinputfile dance (F_DUPFD to >=10, then close
     * of the low fd) destroyed its own script descriptor before the
     * first read — a silent EOF + exit(0). */
    if (fs_fds[fd].refs > 1) {
        fs_fds[fd].refs--;
        return FS_OK;
    }
    fs_fds[fd].is_open = false;
    fs_fds[fd].node    = NULL;
    return FS_OK;
}

/* fcntl(F_DUPFD)/dup(): create another reference to the SAME open
 * file description (shared cursor), per POSIX. */
s32 fs_dup(u32 fd) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return FS_ERR_INVAL;
    fs_fds[fd].refs++;
    return (s32)fd;
}

fs_node_t* fs_get_node_by_fd(u32 fd) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return NULL;
    return fs_fds[fd].node;
}

s64 fs_read(u32 fd, void* buf, u64 count) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return FS_ERR_INVAL;
    fs_node_t* node = fs_fds[fd].node;
    if (!node || node->type != FS_FILE_TYPE_REGULAR) return FS_ERR_INVAL;

    /* Backend-backed node (devfs, mounted volumes): dispatch out */
    if (node->backend && node->backend->read) {
        s64 n = node->backend->read(node->backend_ctx, fs_fds[fd].offset,
                                    buf, count);
        if (n > 0) fs_fds[fd].offset += (u64)n;
        return n;
    }
    if (node->backend) return FS_ERR_PERM;

    // Guard against offset beyond EOF (possible via fs_seek): without this
    // check `avail` underflows and kmemcpy reads far past node->data.
    if (fs_fds[fd].offset >= node->size) return 0;

    u64 avail = node->size - fs_fds[fd].offset;
    if (count > avail) count = avail;
    if (count == 0) return 0;

    kmemcpy(buf, node->data + fs_fds[fd].offset, count);
    fs_fds[fd].offset += count;
    return (s64)count;
}

s64 fs_write(u32 fd, const void* buf, u64 count) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return FS_ERR_INVAL;
    fs_node_t* node = fs_fds[fd].node;
    if (!node || node->type != FS_FILE_TYPE_REGULAR) return FS_ERR_INVAL;
    if (!(fs_fds[fd].flags & (FS_WRITE | FS_APPEND))) return FS_ERR_PERM;

    // APPEND mode: always write at end
    if (fs_fds[fd].flags & FS_APPEND) {
        fs_fds[fd].offset = node->size;
    }

    /* Backend-backed node: dispatch out (offset handled by caller side) */
    if (node->backend) {
        if (!node->backend->write) return FS_ERR_PERM;
        s64 n = node->backend->write(node->backend_ctx, fs_fds[fd].offset,
                                     buf, count);
        if (n > 0) {
            fs_fds[fd].offset += (u64)n;
            if (node->backend->size)
                node->size = (u32)node->backend->size(node->backend_ctx);
        }
        return n;
    }

    u64 off = fs_fds[fd].offset;
    // Reject offsets beyond the max file size: otherwise `needed` can
    // overflow and the truncate path computes a huge negative count,
    // producing an out-of-bounds kmemcpy below.
    if (off >= FS_MAX_FILE_SIZE) return FS_ERR_INVAL;
    u64 needed = off + count;
    u64 dbg_count = count;

    // Grow buffer if necessary
    if (needed > node->data_cap) {
        u32 new_cap = node->data_cap ? node->data_cap * 2 : 256;
        while (new_cap < needed && new_cap < FS_MAX_FILE_SIZE) new_cap *= 2;
        if (new_cap < needed) {
            count = FS_MAX_FILE_SIZE - off;  // truncate
            if (count == 0) return 0;
            new_cap = FS_MAX_FILE_SIZE;
        }

        u8* new_data = (u8*)kmalloc(new_cap);
        if (!new_data) return FS_ERR_NOMEM;

        if (node->data) {
            kmemcpy(new_data, node->data, node->size);
            kfree(node->data);
        }
        node->data     = new_data;
        node->data_cap = new_cap;
    }

    /* FIX(#fs-sparse-leak): a sparse write (lseek past EOF + write)
     * left the gap [node->size, off) as UNINITIALIZED kernel heap —
     * stale dlmalloc contents of previously-freed kernel objects were
     * then readable by any userland reader (kernel info leak). Zero
     * the gap on every write that extends past the current size. */
    if (off > node->size)
        kmemset(node->data + node->size, 0, off - node->size);

    kmemcpy(node->data + off, buf, count);
    fs_fds[fd].offset = off + count;
    if (node->size < off + count) node->size = off + count;

    if (dbg_count > 4096)
        serial_printf("[FSW] fd=%u cnt=%lu ret=%lu size=%u\n",
                      fd, dbg_count, count, node->size);
    return (s64)count;
}

s64 fs_seek(u32 fd, s64 offset, u32 whence) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return FS_ERR_INVAL;

    fs_node_t* node = fs_fds[fd].node;

    switch (whence) {
        case 0:  // SEEK_SET
            fs_fds[fd].offset = (offset < 0) ? 0 : (u64)offset;
            break;
        case 1:  // SEEK_CUR
            if (offset < 0 && (u64)(-offset) > fs_fds[fd].offset)
                fs_fds[fd].offset = 0;
            else
                fs_fds[fd].offset += offset;
            break;
        case 2:  // SEEK_END
            if (node) {
                s64 base = (s64)node->size;
                s64 new_off = base + offset;
                fs_fds[fd].offset = (new_off < 0) ? 0 : (u64)new_off;
            }
            break;
        default:
            return FS_ERR_INVAL;
    }

    return (s64)fs_fds[fd].offset;
}

s32 fs_create_file(const char* path) {
    if (!path) return FS_ERR_INVAL;

    char name[MAX_FILENAME];
    fs_node_t* dir = NULL;
    s32 err = fs_split_path(path, &dir, name);
    if (err != FS_OK) return err;
    if (!dir || dir->type != FS_FILE_TYPE_DIR) return FS_ERR_NOTDIR;
    if (!name[0]) return FS_ERR_INVAL;

    // Check if already exists
    if (fs_find_child(dir, name)) return FS_ERR_EXISTS;

    fs_node_t* node = fs_alloc_node();
    if (!node) return FS_ERR_NOMEM;

    kstrncpy(node->name, name, MAX_FILENAME - 1);
    node->type     = FS_FILE_TYPE_REGULAR;
    node->size     = 0;
    node->data     = NULL;
    node->data_cap = 0;

    return fs_add_child(dir, node);
}

s32 fs_mkdir(const char* path) {
    if (!path) return FS_ERR_INVAL;

    char name[MAX_FILENAME];
    fs_node_t* dir = NULL;
    s32 err = fs_split_path(path, &dir, name);
    if (err != FS_OK) return err;
    if (!dir || dir->type != FS_FILE_TYPE_DIR) return FS_ERR_NOTDIR;
    if (!name[0]) return FS_ERR_INVAL;

    // Check if already exists
    if (fs_find_child(dir, name)) return FS_ERR_EXISTS;

    fs_node_t* node = fs_alloc_node();
    if (!node) return FS_ERR_NOMEM;

    kstrncpy(node->name, name, MAX_FILENAME - 1);
    node->type = FS_FILE_TYPE_DIR;

    return fs_add_child(dir, node);
}

s32 fs_delete_file(const char* path) {
    if (!path) return FS_ERR_INVAL;

    fs_node_t* node = fs_resolve_path(path);
    if (!node) return FS_ERR_NOTFOUND;
    if (node == fs_root) return FS_ERR_PERM;
    if (node->type == FS_FILE_TYPE_DIR && node->child_count > 0)
        return FS_ERR_NOTEMPTY;

    fs_node_t* parent = node->parent;
    if (parent) fs_remove_child(parent, node);

    // Close any file descriptors still referencing this node.
    // Otherwise the fds keep a dangling pointer: after the slot is
    // reallocated by fs_alloc_node, reads through the old fd return
    // another file's data (use-after-free).
    for (u32 i = 0; i < FS_MAX_FDS; i++) {
        if (fs_fds[i].is_open && fs_fds[i].node == node) {
            fs_fds[i].is_open = false;
            fs_fds[i].node = NULL;
        }
    }

    // Recursively free children if directory (shouldn't happen due to check above)
    if (node->type == FS_FILE_TYPE_DIR) {
        for (u32 i = 0; i < node->child_count; i++) {
            if (node->children[i]) {
                fs_free_node(node->children[i]);
            }
        }
    }

    fs_free_node(node);
    return FS_OK;
}

s32 fs_rmdir(const char* path) {
    if (!path) return FS_ERR_INVAL;

    fs_node_t* node = fs_resolve_path(path);
    if (!node) return FS_ERR_NOTFOUND;
    if (node->type != FS_FILE_TYPE_DIR) return FS_ERR_INVAL;
    if (node == fs_root) return FS_ERR_PERM;
    if (node->child_count > 0) return FS_ERR_NOTEMPTY;

    fs_node_t* parent = node->parent;
    if (parent) fs_remove_child(parent, node);
    fs_free_node(node);
    return FS_OK;
}

s32 fs_rename(const char* oldp, const char* newp) {
    if (!oldp || !newp) return FS_ERR_INVAL;

    fs_node_t* node = fs_resolve_path(oldp);
    if (!node) return FS_ERR_NOTFOUND;
    if (node == fs_root) return FS_ERR_PERM;

    /* destination split: parent dir + final component */
    fs_node_t* ndir = NULL;
    char nname[MAX_FILENAME];
    s32 err = fs_split_path(newp, &ndir, nname);
    if (err != FS_OK) return err;
    if (!ndir || ndir->type != FS_FILE_TYPE_DIR) return FS_ERR_NOTDIR;
    if (!nname[0]) return FS_ERR_INVAL;

    /* FIX(#rename-cycle): no ancestor/cycle check existed.
     * rename("/a", "/a/b/x") made /a's parent /a/b and /a/b's parent
     * /a — fs_get_path() then looped FOREVER (hard hang at the next
     * pwd/sync/shell prompt). rename("/a/b", "/a") freed the source's
     * own parent and then used the freed slot. Reject: destination
     * directory IS the node, or lies INSIDE the node's subtree. */
    if (ndir == node) return FS_ERR_INVAL;
    for (fs_node_t* p = ndir; p; p = p->parent) {
        if (p == node) return FS_ERR_INVAL;   /* dst inside src subtree */
        if (p == fs_root) break;
    }

    /* replace the target if it exists (POSIX rename semantics) */
    fs_node_t* existing = fs_find_child(ndir, nname);
    if (existing && existing != node) {
        if (existing == fs_root) return FS_ERR_PERM;
        fs_remove_child(ndir, existing);
        for (u32 i = 0; i < FS_MAX_FDS; i++) {
            if (fs_fds[i].is_open && fs_fds[i].node == existing) {
                fs_fds[i].is_open = false;
                fs_fds[i].node = NULL;
            }
        }
        fs_free_node(existing);
    }

    /* detach from the old parent, rename, re-add.
     * FIX(#rename-detach-order): detach BEFORE fs_add_child — the old
     * same-directory path re-added without removing first, leaving a
     * duplicate entry in the children array. Capacity is checked
     * FIRST so a full destination dir can not orphan the node (the
     * old code detached, failed fs_add_child, and the file vanished
     * from the namespace despite the error return). */
    fs_node_t* oparent = node->parent;
    if (oparent != ndir && ndir->child_count >= FS_MAX_CHILDREN)
        return FS_ERR_INVAL;                 /* dst full: keep old name  */
    if (oparent) fs_remove_child(oparent, node);
    kstrncpy(node->name, nname, MAX_FILENAME - 1);
    node->name[MAX_FILENAME - 1] = '\0';
    s32 r = fs_add_child(ndir, node);
    return (r == FS_OK) ? FS_OK : r;
}

const char* fs_get_error(s32 err) {
    switch (err) {
        case FS_OK:          return "Success";
        case FS_ERR_GENERAL:  return "General error";
        case FS_ERR_NOTFOUND: return "File not found";
        case FS_ERR_PERM:     return "Permission denied";
        case FS_ERR_NOMEM:    return "Out of memory";
        case FS_ERR_INVAL:    return "Invalid argument";
        case FS_ERR_EXISTS:   return "Already exists";
        case FS_ERR_NOTDIR:   return "Not a directory";
        case FS_ERR_NOTEMPTY: return "Directory not empty";
        case FS_ERR_NOFDS:    return "No free file descriptors";
        default:             return "Unknown error";
    }
}

// ============================================================
// Extended API
// ============================================================

fs_node_t* fs_get_cwd(void) {
    return fs_cwd;
}

s32 fs_set_cwd(const char* path) {
    if (!path) return FS_ERR_INVAL;
    fs_node_t* node = fs_resolve_path(path);
    if (!node) return FS_ERR_NOTFOUND;
    if (node->type != FS_FILE_TYPE_DIR) return FS_ERR_NOTDIR;
    fs_cwd = node;
    return FS_OK;
}

void fs_get_path(fs_node_t* node, char* buf, size_t bufsize) {
    if (!node || !buf || bufsize == 0) return;

    // Build path by walking up to root
    // We need to count the depth first
    u32 depth = 0;
    fs_node_t* n = node;
    while (n != fs_root && n->parent != n) {
        depth++;
        n = n->parent;
    }

    // Now fill from the end
    buf[bufsize - 1] = '\0';
    size_t pos = bufsize - 1;

    n = node;
    while (n != fs_root && n->parent != n && pos > 0) {
        size_t namelen = kstrlen(n->name);
        if (pos < namelen + 1) break;  // not enough space
        pos -= namelen;
        kmemcpy(buf + pos, n->name, namelen);
        if (pos > 0) buf[--pos] = '/';
        n = n->parent;
    }

    if (pos == bufsize - 1) {
        // node was root
        if (bufsize >= 2) {
            buf[0] = '/';
            buf[1] = '\0';
        }
        return;
    }

    // Shift to start of buffer
    size_t len = bufsize - 1 - pos;
    kmemmove(buf, buf + pos, len);
    buf[len] = '\0';
}

int fs_list_dir(const char* path, fs_node_t** out_nodes, int max) {
    if (!path || !out_nodes || max <= 0) return 0;

    fs_node_t* dir = fs_resolve_path(path);
    if (!dir) return FS_ERR_NOTFOUND;
    if (dir->type != FS_FILE_TYPE_DIR) return FS_ERR_NOTDIR;

    int count = 0;
    for (u32 i = 0; i < dir->child_count && count < max; i++) {
        out_nodes[count++] = dir->children[i];
    }
    return count;
}

s32 fs_write_file(const char* path, const void* data, u64 len) {
    s32 fd = fs_open(path, FS_WRITE | FS_CREATE);
    if (fd < 0) return fd;

    // Truncate to 0 first by seeking to start
    fs_seek(fd, 0, 0);

    s64 written = fs_write(fd, data, len);
    fs_close(fd);
    return (written >= 0) ? FS_OK : (s32)written;
}

/* Get file size by fd (for stat/fstat syscalls) */
u32 fs_get_size_by_fd(u32 fd) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open || !fs_fds[fd].node) return 0;
    const fs_node_t* node = fs_fds[fd].node;
    if (node->backend && node->backend->size)
        return (u32)node->backend->size(node->backend_ctx);
    return node->size;
}

/* Shrink a file opened on fd to new_size bytes (truncate tail).
 * Backend-backed nodes manage their own size — ignored there. */
void fs_resize_fd(u32 fd, u64 new_size) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open || !fs_fds[fd].node) return;
    fs_node_t* node = fs_fds[fd].node;
    if (node->backend) return;
    if (new_size < (u64)node->size) node->size = (u32)new_size;
}

/* Attach a backend to an existing regular file (mini-VFS mount) */
s32 fs_attach_backend_by_path(const char* path, const fs_backend_t* be, void* ctx) {
    fs_node_t* node = fs_resolve_path(path);
    if (!node || node->type != FS_FILE_TYPE_REGULAR) return FS_ERR_NOTFOUND;
    node->backend     = be;
    node->backend_ctx = ctx;
    if (be && be->size) node->size = (u32)be->size(ctx);
    return FS_OK;
}


/* Accessors for shell output redirection */
s32 proc_fds_get_fsfd(u64 fd) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return -1;
    return (s32)fd;
}

void proc_fds_set_fsfd(u64 fd, s32 fsfd) {
    /* For stdout redirection: swap the underlying file node */
    if (fd < FS_MAX_FDS && fsfd >= 0) {
        fs_fds[fd].node = fs_nodes[fsfd % FS_MAX_NODES].active
            ? &fs_nodes[fsfd % FS_MAX_NODES] : 0;
        fs_fds[fd].offset = 0;
    }
}

/* Walk every active regular file (persistence layer).
 * Backend-backed nodes (devfs, mounted volumes) are skipped: their
 * data does not live in the ramfs and must not be serialized. */
u32 fs_count_files(void) {
    u32 n = 0;
    for (u32 i = 0; i < FS_MAX_NODES; i++) {
        if (fs_nodes[i].active && !fs_nodes[i].backend &&
            fs_nodes[i].type == FS_FILE_TYPE_REGULAR) n++;
    }
    return n;
}

void fs_iterate_files(fs_file_walk_cb cb, void* ctx) {
    char path[300];
    for (u32 i = 0; i < FS_MAX_NODES; i++) {
        if (!fs_nodes[i].active || fs_nodes[i].backend ||
            fs_nodes[i].type != FS_FILE_TYPE_REGULAR)
            continue;
        fs_get_path(&fs_nodes[i], path, sizeof(path));
        if (cb(path, fs_nodes[i].data, fs_nodes[i].size, ctx)) break;
    }
}

/* File size by path (for exec loader buffer allocation) */
s64 fs_file_size(const char* path) {
    /* FIX(#fs-size-symlink): size lookup used the NO-follow resolver,
     * so any path ending in a symlink (the /bin applet links seeded by
     * #busybox-applet-links — /bin/sh -> busybox et al.) reported
     * FS_ERR_NOTFOUND even though fs_open() happily opened the very
     * same path (open() follows the final symlink). process_spawn()
     * gates on fs_file_size() and returned a bare -1 with zero
     * diagnostics: `elf /bin/sh script` from the native shell failed
     * as "Failed to create process" while `elf /bin/busybox ...`
     * worked — the exact trap this prober fell into. Size is a stat-
     * class operation: use the follow resolver like open/stat do. */
    fs_node_t* n = fs_resolve_path_follow(path);
    if (!n || n->type != FS_FILE_TYPE_REGULAR) return FS_ERR_NOTFOUND;
    return (s64)n->size;
}

/* Linux getdents64 over a ramfs directory. The fd's seek offset is
 * reused as the child-cursor so successive calls walk the directory.
 * Emits struct linux_dirent64 {u64 ino; s64 off; u16 reclen; u8 type;
 * char name[];} entries, 8-byte aligned, until buf is full or EOF. */
s64 fs_getdents64(u32 fd, void* buf, u64 len) {
    if (fd >= FS_MAX_FDS || !fs_fds[fd].is_open) return FS_ERR_INVAL;
    fs_node_t* node = fs_fds[fd].node;
    if (!node || node->type != FS_FILE_TYPE_DIR) return -FS_ERR_NOTDIR;

    u32 cursor = (u32)(fs_fds[fd].offset & 0xFFFFFFFF);
    u64 produced = 0;
    u8* out = (u8*)buf;

    while (cursor < node->child_count && produced < len) {
        fs_node_t* ch = node->children[cursor];
        cursor++;

        if (!ch || !ch->active) continue;

        const char* name = ch->name;
        u32 nlen = (u32)kstrlen(name);
        u16 reclen = (u16)((19 + nlen + 1 + 7) & ~7);

        if (produced + reclen > len) { cursor--; break; }

        u64 ino = 2 + cursor;
        *(u64*)(out + produced)      = ino;
        *(s64*)(out + produced + 8)  = (s64)cursor;
        *(u16*)(out + produced + 16) = reclen;
        out[produced + 18] = (ch->type == FS_FILE_TYPE_DIR)    ? 4
                           : (ch->type == FS_FILE_TYPE_SYMLINK) ? 10  /* DT_LNK */
                           : 8;
        kmemcpy(out + produced + 19, name, nlen + 1);
        kmemset(out + produced + 19 + nlen + 1, 0,
                reclen - (u16)(19 + nlen + 1));

        produced += reclen;
    }

    fs_fds[fd].offset = cursor;
    return (s64)produced;
}
