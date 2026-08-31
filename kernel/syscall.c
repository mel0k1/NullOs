#include "../include/syscall.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/fs.h"
#include "../include/net.h"
#include "../include/timer.h"

/* Internal net.c functions exposed for syscall dispatch */
extern void net_udp_send(const u8* dst_ip, u16 dst_port,
                         u16 src_port, const u8* data, u16 len);
#include "../include/mm.h"
#include "../include/gdt.h"
#include "../include/serial.h"
#include "../include/string.h"
#include "../include/proc.h"
#include "../include/scheduler.h"

// ============================================================
// Linux x86_64 ABI system call implementation.
//
// Numbers follow the standard x86_64 table (see include/syscall.h)
// so that static musl-linked binaries can run in the future.
//
// Entry contract (from syscall_entry.S):
//   syscall_handler(rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4)
//
// User pointer validation: ring-3 code may only touch the user
// address region; anything else is rejected with EFAULT.
//
// NOTE on interrupts: SYSCALL clears RFLAGS.IF (FMASK=0x200), so a
// blocking sys_read must explicitly sti()/cli() around its wait loop
// or the keyboard IRQ would never fire and the read would deadlock.
// ============================================================

volatile u64 sys_exit_pending = 0;

// Top of the dedicated kernel stack for SYSCALL handling.
// SYSCALL does NOT switch stacks in hardware (unlike interrupts), so the
// entry stub loads this value into RSP immediately. Processes get their
// own stack; sched_activate() repoints this global on every switch.
u64 syscall_kstack_top = 0;

/* Default kernel-side syscall stack for non-process contexts */
static u8  syscall_kstack_default_buf[64 * 1024] __attribute__((aligned(16)));
u64 syscall_kstack_default =
    (u64)(syscall_kstack_default_buf + sizeof(syscall_kstack_default_buf));

void* frame_dump_ptr = 0;

extern void syscall_entry(void);

// ---- user pointer validation ------------------------------------

#define USER_ADDR_MIN 0x400000ULL
/* User VA map: image 4-9MB, brk 256MB-1GB, mmap 512-768MB, and the
 * STACK at 0x7FFFF0000 (~32GB) — the old 0x7FFFFF000 ceiling exists
 * precisely to cover the stack. #kernel-halffix adds the KERNEL WINDOW
 * at VAs [1GB, 2GB) mapped in EVERY address space (children clone it),
 * so user pointers must never land there: keep the 32GB ceiling but
 * reject any range that touches the window hole. (A flat 1GB ceiling
 * was tried and broke EVERY stack-pointer syscall: musl's poll struct
 * lives on the 32GB stack -> -EFAULT -> busybox nc died instantly.) */
#define USER_ADDR_MAX 0x7FFFFF000ULL
#define USER_WINDOW_LO 0x40000000ULL   /* 1GB: kernel window start (VA)  */
#define USER_WINDOW_HI 0x80000000ULL   /* 2GB: kernel window end   (VA)  */

static bool user_range_ok(u64 addr, u64 len)
{
    if (addr < USER_ADDR_MIN) return false;
    if (len > USER_ADDR_MAX - USER_ADDR_MIN) return false;
    u64 end = addr + len;
    if (end > USER_ADDR_MAX || end < addr) return false;
    /* reject any range overlapping the kernel window [1GB, 2GB) */
    if (addr < USER_WINDOW_HI && end > USER_WINDOW_LO) return false;
    return true;
}

static inline int fault_if_bad(u64 buf, u64 len)
{
    return user_range_ok(buf, len) ? 0 : -LINUX_EFAULT;
}

// ---- process fd table (Linux fd semantics over our fs layer) ----
//
// Linux fds 0/1/2 are stdin/stdout/stderr — reserved for console.
// Everything opened via open(2) lands in slots >= 3 and maps onto
// our ramfs file descriptors.

#define PROC_FD_MAX 16

typedef struct {
    bool used;
    s32  fs_fd;
} proc_fd_t;

static proc_fd_t proc_fds[PROC_FD_MAX];

void syscall_process_reset(void)
{
    for (int i = 0; i < PROC_FD_MAX; i++) {
        proc_fds[i].used = false;
        proc_fds[i].fs_fd = -1;
    }
    /* reserve console fds — same negative sentinel encoding that
     * fcntl(F_DUPFD*) duplicates of stdio use (fs_fd<0 == console).
     * Distinct values preserve "which stream" for debugging only. */
    proc_fds[0].used = true;   proc_fds[0].fs_fd = -2;  /* stdin  */
    proc_fds[1].used = true;   proc_fds[1].fs_fd = -3;  /* stdout */
    proc_fds[2].used = true;   proc_fds[2].fs_fd = -4;  /* stderr */
}

/* ---- Kernel pipes (#pipe2) ------------------------------------------
 * POSIX anonymous pipes for busybox ash pipelines: a ring buffer per
 * pipe living in kernel .bss, ends encoded as special fs_fd values so
 * read/write/close/dup/poll/fork all dispatch through the fd table.
 * Blocking uses the preemption-friendly hlt loop (the writer is a
 * real task the timer will schedule while the reader sleeps).
 * Encoding: read-end  = FD_PIPE_BASE - 2*idx        (-30, -32, ...)
 *           write-end = FD_PIPE_BASE - 2*idx - 1    (-31, -33, ...) */
#define FD_PIPE_BASE   (-30)
#define MAX_PIPES      16
#define PIPE_BUF_SIZE  8192u                    /* power of two          */
#define PIPE_BUF_MASK  (PIPE_BUF_SIZE - 1u)

static inline bool fd_is_pipe_enc(s32 v) {
    return v <= FD_PIPE_BASE && v > FD_PIPE_BASE - 2 * MAX_PIPES;
}
static inline s32 fd_pipe_index(s32 v) { return (FD_PIPE_BASE - v) >> 1; }
static inline bool fd_pipe_is_write(s32 v) { return ((FD_PIPE_BASE - v) & 1) != 0; }

static inline bool is_pipe_fd(u64 fd) {
    return fd < PROC_FD_MAX && proc_fds[fd].used &&
           fd_is_pipe_enc(proc_fds[fd].fs_fd);
}
static s32 proc_fd_to_pipe(u64 fd, bool* is_write_end) {
    if (!is_pipe_fd(fd)) return -1;
    s32 v = proc_fds[fd].fs_fd;
    if (is_write_end) *is_write_end = fd_pipe_is_write(v);
    return fd_pipe_index(v);
}

typedef struct {
    bool     used;
    u8       buf[PIPE_BUF_SIZE];
    volatile u32 head, tail;                      /* free-running counters */
    volatile int readers, writers;                /* alias refcounts       */
} pipe_t;
static pipe_t g_pipes[MAX_PIPES];

static s32 pipe_alloc(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            g_pipes[i].used    = true;
            g_pipes[i].head    = 0;
            g_pipes[i].tail    = 0;
            g_pipes[i].readers = 0;
            g_pipes[i].writers = 0;
            return i;
        }
    }
    return -1;
}
static void pipe_ref_inc(int idx, bool write_end) {
    if (idx < 0 || idx >= MAX_PIPES || !g_pipes[idx].used) return;
    if (write_end) g_pipes[idx].writers++; else g_pipes[idx].readers++;
}
static void pipe_ref_dec(int idx, bool write_end) {
    if (idx < 0 || idx >= MAX_PIPES || !g_pipes[idx].used) return;
    if (write_end) { if (g_pipes[idx].writers > 0) g_pipes[idx].writers--; }
    else           { if (g_pipes[idx].readers > 0) g_pipes[idx].readers--; }
    if (g_pipes[idx].readers == 0 && g_pipes[idx].writers == 0)
        g_pipes[idx].used = false;                /* last alias frees it */
}

#define LINUX_EPIPE 32

/* unix-socket encodings: forward decls (impl lives below the net
 * section; fork-copy and exit-release dispatch on them too)        */
static inline bool fd_is_unix_enc(s32 v);
static inline bool fd_is_epoll_enc(s32 v);
static inline s32 fd_unix_index(s32 v);
static inline bool fd_unix_is_end0(s32 v);
static void unix_ref_inc(s32 idx, bool end0);
static void unix_ref_dec(s32 idx, bool end0);

/* #wl-substrate forward decls (impl lives in the substrate section):
 * eventfd/timerfd/memfd/signalfd objects ride the sentinel gap
 * -95..-109; readiness and ring-3 I/O route through the helpers.  */
static inline bool fd_is_efd_enc(s32 v);
static inline bool fd_is_tfd_enc(s32 v);
static inline bool fd_is_mfd_enc(s32 v);
static inline bool fd_is_sfd_enc(s32 v);
static inline bool fd_is_wlobj_enc(s32 v);
static void wl_obj_ref_dec(s32 v);
static void wl_obj_ref_inc(s32 v);
bool wl_obj_ready(s32 v, bool in_req, bool out_req,
                  bool* out_in, bool* out_out);
/* later-defined impls reused by the substrate (readv/writev/ppoll) */
static u64 sys_read_impl(u64 fd, u64 buf, u64 count);
static u64 sys_write_impl(u64 fd, u64 buf, u64 count);
static u64 sys_poll_impl(u64 ufds, u64 nfds, u64 timeout_ms);

/* fd-table snapshots for processes (fork copies, activate restores) */
void syscall_fds_save(task_t* t) {
    if (!t) return;
    for (int i = 0; i < PROC_FD_MAX; i++) {
        t->fd_used[i] = proc_fds[i].used ? 1 : 0;
        t->fd_fs[i]   = proc_fds[i].fs_fd;
    }
}

void syscall_fds_load(task_t* t) {
    if (!t) return;
    for (int i = 0; i < PROC_FD_MAX; i++) {
        proc_fds[i].used  = t->fd_used[i] != 0;
        proc_fds[i].fs_fd = t->fd_fs[i];
    }
}

void syscall_fds_copy(task_t* dst, task_t* src) {
    if (!dst || !src) return;
    for (int i = 0; i < PROC_FD_MAX; i++) {
        dst->fd_used[i] = src->fd_used[i];
        dst->fd_fs[i]   = src->fd_fs[i];
        /* POSIX fork(): the child owns an INDEPENDENT reference to
         * every inherited open-file description. Without bumping,
         * the child's pre-execve close() bookkeeping destroyed the
         * description out from under the parent — ash silently saw
         * EOF on its duped script fd (>=10) and ran only the first
         * line of any pasted payload (#ash-script-death). */
        if (dst->fd_used[i] && dst->fd_fs[i] >= 0) {
            extern s32 fs_dup(u32);
            fs_dup((u32)dst->fd_fs[i]);
        }
        /* pipes: forked child owns independent end refs (#pipe2) */
        if (dst->fd_used[i] && fd_is_pipe_enc(dst->fd_fs[i])) {
            pipe_ref_inc(fd_pipe_index(dst->fd_fs[i]),
                         fd_pipe_is_write(dst->fd_fs[i]));
        }
        /* unix channels: same independent-reference rule
         * (#wayland-transport) */
        if (dst->fd_used[i] && fd_is_unix_enc(dst->fd_fs[i])) {
            unix_ref_inc(fd_unix_index(dst->fd_fs[i]),
                         fd_unix_is_end0(dst->fd_fs[i]));
        }
        /* #wl-substrate objects: same independent-reference rule */
        if (dst->fd_used[i]) wl_obj_ref_inc(dst->fd_fs[i]);
    }
}

static s32 proc_fd_alloc_from(s32 fs_fd, s32 floor)
{
    /* POSIX lowest-free-number semantics INCLUDING fds 0..2: a shell
     * may legitimately close stdin and expect the next open("/dev/null")
     * to come back on fd 0 (busybox ash setjobctl does exactly that:
     * close(0); if(open("/dev/null") != 0) -> "can't set tty
     * process group" and interactive job control dies). Real files
     * landing on stdio numbers are equally valid redirections. */
    int start = (floor < 0) ? 0 : floor;
    for (int i = start; i < PROC_FD_MAX; i++) {
        if (!proc_fds[i].used) {
            proc_fds[i].used = true;
            proc_fds[i].fs_fd = fs_fd;
            return i;
        }
    }
    return -1;
}

static s32 proc_fd_alloc(s32 fs_fd)
{
    return proc_fd_alloc_from(fs_fd, 0);
}

static s32 proc_fd_get(u64 fd)               /* → fs fd or -1 */
{
    /* #stdio-redirect-fd FIX: the old `fd < 3` rejection dates from
     * when stdio could only be console sentinels. Redirections now
     * land REAL ramfs descriptors on 0/1/2 (proc_fd_alloc_from), so
     * `wc -l < /etc/hosts` read(0) died with EBADF right here.
     * Console sentinels are still excluded — by the fs_fd<0 check
     * every caller already applies. */
    if (fd >= PROC_FD_MAX) return -1;
    if (!proc_fds[fd].used) return -1;
    s32 v = proc_fds[fd].fs_fd;
    return (v >= 0) ? v : -1;
}

// ---- network sockets as first-class descriptors -------------------
// fs_fd <= -10 encodes a network socket: NET_FD_BASE(-10) minus the
// net-layer index. They ride in the SAME proc fd table as console
// sentinels (-2/-3/-4) and ramfs descriptors (>=0), so dup/close/
// fork-copy/poll all reuse the POSIX machinery for free.
#define FD_NET_BASE (-10)
/* Pipe ends live BELOW the net range: net fds encode as
 * -10-idx (idx<16 -> -10..-25); pipes as -30-2*idx-end (see below). */
static inline bool fd_is_net(s32 v) { return v <= FD_NET_BASE && v > FD_PIPE_BASE; }

/* Encode: fs_fd = -10 - idx   (idx>=0)  -> idx0:-10 idx1:-11 ...
 * Decode: idx     = -10 - fs_fd (valid only inside the net range) */
static inline s32 fd_net_index(s32 v)
{
    return fd_is_net(v) ? (-10 - v) : -1;
}

static inline bool is_net_fd(u64 fd) {
    return fd < PROC_FD_MAX && proc_fds[fd].used &&
           fd_is_net(proc_fds[fd].fs_fd);
}
/* Process exit: release EVERY descriptor this task holds — pipe end
 * aliases, ramfs descriptions and net sockets. Without this a child
 * that dies mid-pipeline leaks its writer count and the reader blocks
 * forever waiting for EOF that never comes (#pipe-eof-leak). */
void syscall_release_all_fds(void)
{
    for (u64 i = 0; i < PROC_FD_MAX; i++) {
        if (!proc_fds[i].used) continue;
        s32 v = proc_fds[i].fs_fd;
        if (fd_is_pipe_enc(v)) {
            pipe_ref_dec(fd_pipe_index(v), fd_pipe_is_write(v));
        } else if (v >= 0) {
            fs_close((u32)v);
        } else if (fd_is_net(v)) {
            s32 nidx = fd_net_index(v);
            if (nidx >= 0) {
                extern void net_socket_close_tcp(int);
                extern int  net_socket_type(int);
                if (net_socket_type(nidx) == SOCK_STREAM)
                    net_socket_close_tcp(nidx);
                extern void net_socket_free(int);
                net_socket_free(nidx);
            }
        } else if (fd_is_unix_enc(v)) {
            unix_ref_dec(fd_unix_index(v), fd_unix_is_end0(v));
        } else if (fd_is_epoll_enc(v)) {
            /* epoll instances have no external refs: nothing to do */
        } else {
            wl_obj_ref_dec(v);             /* efd/tfd/mfd/sfd        */
        }
        proc_fds[i].used = false;
        proc_fds[i].fs_fd = -1;
    }
}

/* Validated fd -> net-layer socket index (-1 if not a net fd). */
static s32 proc_fd_to_net(u64 fd)
{
    if (!is_net_fd(fd)) return -1;
    s32 idx = fd_net_index(proc_fds[fd].fs_fd);
    extern bool net_socket_is_used(int);
    return net_socket_is_used(idx) ? idx : -1;
}

// ---- Unix-domain sockets (#wayland-transport) ----------------------
// AF_UNIX SOCK_STREAM channels: the libwayland wire foundation for
// Tinywl/Labwc. Everything Wayland does on the socket layer:
//   socketpair(53)                    wl_client paired connections
//   socket(AF_UNIX)+connect/bind      display socket ($XDG/wayland-0)
//   listen/accept                     compositor side
//   sendmsg/recvmsg + SCM_RIGHTS      fd passing (wl_shm pools, dmabuf)
//
// fd-table encoding lives BELOW the pipe range (no collision with
// console -2..-4, net -10..-25, pipes -30..-61):
//   end0 = FD_UNIX_BASE - 2*idx       (-70, -72, ...)
//   end1 = FD_UNIX_BASE - 2*idx - 1   (-71, -73, ...)
// One channel = two rings (end0->end1, end1->end0) + per-direction
// pending SCM_RIGHTS queues. In-flight fds hold ONE extra reference:
// bumped at sendmsg, transferred to the receiver at recvmsg, released
// if the message dies undelivered (channel teardown).

#define FD_UNIX_BASE   (-70)
#define MAX_UNICH      12
#define UNIX_BUF_SIZE  8192u
#define UNIX_BUF_MASK  (UNIX_BUF_SIZE - 1u)
#define UNIX_MAX_FDS   4              /* fds per SCM_RIGHTS message */
#define UNIX_MAX_PEND  8              /* queued fd-messages per dir */
#define UNIX_BACKLOG   4

typedef struct {
    s32 enc[UNIX_MAX_FDS];
    int n;
} ufdmsg_t;

typedef struct {
    bool     used;
    u8  ab[UNIX_BUF_SIZE];            /* end0 -> end1 stream */
    volatile u32 ab_head, ab_tail;
    u8  ba[UNIX_BUF_SIZE];            /* end1 -> end0 stream */
    volatile u32 ba_head, ba_tail;
    volatile int refs[2];             /* aliases per end     */
    ufdmsg_t pend01[UNIX_MAX_PEND];   /* fds 0 -> 1 */
    ufdmsg_t pend10[UNIX_MAX_PEND];   /* fds 1 -> 0 */
    int  pend01_head, pend01_n;       /* FIFO per direction    */
    int  pend10_head, pend10_n;
    bool listening;
    char path[64];
    int  backlog[UNIX_BACKLOG];       /* channel indices queued */
    int  backlog_n;
} unich_t;

static unich_t g_unich[MAX_UNICH];

static inline bool fd_is_unix_enc(s32 v) {
    return v <= FD_UNIX_BASE && v > FD_UNIX_BASE - 2 * MAX_UNICH;
}
static inline s32 fd_unix_index(s32 v) { return (FD_UNIX_BASE - v) >> 1; }
/* (FD_UNIX_BASE - v) even => end0, odd => end1 */
static inline bool fd_unix_is_end0(s32 v) { return ((FD_UNIX_BASE - v) & 1) == 0; }

static inline bool is_unix_fd(u64 fd) {
    return fd < PROC_FD_MAX && proc_fds[fd].used &&
           fd_is_unix_enc(proc_fds[fd].fs_fd);
}
static s32 proc_fd_to_unix(u64 fd, bool* is_end0)
{
    if (!is_unix_fd(fd)) return -1;
    s32 v = proc_fds[fd].fs_fd;
    s32 idx = fd_unix_index(v);
    if (idx < 0 || idx >= MAX_UNICH || !g_unich[idx].used) return -1;
    if (is_end0) *is_end0 = fd_unix_is_end0(v);
    return idx;
}

static void unix_ref_inc(s32 idx, bool end0)
{
    if (idx < 0 || idx >= MAX_UNICH || !g_unich[idx].used) return;
    if (end0) g_unich[idx].refs[0]++; else g_unich[idx].refs[1]++;
}

/* forward: defined right below (used by the drop path) */
static void fdref_dec_enc(s32 v);

/* Release one pending SCM_RIGHTS entry without installing it. */
static void unix_drop_ufdmsg(ufdmsg_t* m)
{
    for (int i = 0; i < m->n; i++) {
        s32 v = m->enc[i];
        if (v == -999) continue;          /* slot already handled     */
        fdref_dec_enc(v);
    }
    m->n = 0;
}
static void unix_ref_dec(s32 idx, bool end0)
{
    if (idx < 0 || idx >= MAX_UNICH || !g_unich[idx].used) return;
    unich_t* u = &g_unich[idx];
    if (end0) { if (u->refs[0] > 0) u->refs[0]--; }
    else      { if (u->refs[1] > 0) u->refs[1]--; }
    if (u->refs[0] == 0 && u->refs[1] == 0) {
        /* last alias: drop undelivered fd messages (their in-flight
         * references die with the socket, POSIX close(2) rules)      */
        for (int i = 0; i < UNIX_MAX_PEND; i++) {
            unix_drop_ufdmsg(&u->pend01[i]);
            unix_drop_ufdmsg(&u->pend10[i]);
        }
        u->listening = false;
        u->path[0] = 0;
        u->backlog_n = 0;
        u->used = false;
    }
}

static s32 unix_alloc(void)
{
    for (int i = 0; i < MAX_UNICH; i++) {
        unich_t* u = &g_unich[i];
        if (u->used) continue;
        kmemset(u, 0, sizeof(*u));
        u->used = true;
        return i;
    }
    return -1;
}

/* Ref-count bump for an ENCODED descriptor (SCM_RIGHTS in-flight).
 * fs fds: fs_dup; pipes: pipe_ref_inc; net: net_socket_dup;
 * unix: unix_ref_inc.                                            */
static bool fdref_bump_enc(s32 v)
{
    if (v >= 0) {
        extern s32 fs_dup(u32);
        return fs_dup((u32)v) >= 0;
    }
    if (fd_is_pipe_enc(v)) {
        pipe_ref_inc(fd_pipe_index(v), fd_pipe_is_write(v));
        return true;
    }
    if (fd_is_net(v)) {
        extern int net_socket_dup(int);
        return net_socket_dup(fd_net_index(v)) == 0;
    }
    if (fd_is_unix_enc(v)) {
        s32 idx = fd_unix_index(v);
        if (idx < 0 || idx >= MAX_UNICH || !g_unich[idx].used) return false;
        unix_ref_inc(idx, fd_unix_is_end0(v));
        return true;
    }
    return false;                          /* console sentinels: no  */
}
/* Inverse of fdref_bump_enc for dropped in-flight fds. fs_close is
 * ALREADY a decref (refs>1 -> --, last -> free): one call drops the
 * in-flight reference exactly like fs_dup added it. */
static void wl_obj_ref_dec(s32 v);
static void fdref_dec_enc(s32 v)
{
    if (v >= 0) {
        extern s32 fs_close(u32);
        fs_close((u32)v);
    } else if (fd_is_pipe_enc(v)) {
        pipe_ref_dec(fd_pipe_index(v), fd_pipe_is_write(v));
    } else if (fd_is_net(v)) {
        extern void net_socket_free(int);
        net_socket_free(fd_net_index(v));  /* decrements sk_refs */
    } else if (fd_is_unix_enc(v)) {
        unix_ref_dec(fd_unix_index(v), fd_unix_is_end0(v));
    } else {
        wl_obj_ref_dec(v);                 /* efd/tfd/mfd/sfd    */
    }
}

/* Parse a user msghdr control block: SOL_SOCKET(1)/SCM_RIGHTS(1) with
 * an int[] of process fds. Returns bytes consumed, 0 on absence, -1
 * on malformed control data. */
static s32 unix_parse_cmsg_rights(u64 control, u64 controllen,
                                  s32* out_enc, int* out_n)
{
    *out_n = 0;
    if (!control || controllen < 16) return 0;
    if (!user_range_ok(control, controllen)) return -1;
    u64 off = 0;
    while (off + 16 <= controllen) {
        u64 len  = *(u64*)(control + off);
        s32 lvl  = *(s32*)(control + off + 8);
        s32 type = *(s32*)(control + off + 12);
        if (len < 16 || off + len > controllen) return -1;
        if (lvl == 1 && type == 1) {       /* SOL_SOCKET, SCM_RIGHTS */
            int nf = (int)((len - 16) / 4);
            if (nf > UNIX_MAX_FDS) nf = UNIX_MAX_FDS;
            for (int i = 0; i < nf; i++) {
                s32 pfd = *(s32*)(control + off + 16 + 4 * i);
                if (pfd < 0 || pfd >= PROC_FD_MAX ||
                    !proc_fds[pfd].used) continue;
                out_enc[(*out_n)++] = proc_fds[pfd].fs_fd;
                if (*out_n >= UNIX_MAX_FDS) break;
            }
            return (s32)len;
        }
        off += (len + 7) & ~7ULL;
    }
    return 0;
}

/* Install pending fds into the CURRENT process table; fill the user
 * cmsghdr (len, SOL_SOCKET/SCM_RIGHTS, int fds[]). In-flight refs are
 * MOVED into the freshly allocated table slots. mh_u + 40 is the
 * msghdr controllen field in our user layout (see copy_msghdr_in). */
static s32 unix_emit_cmsg_rights(u64 control, u64 controllen_cap,
                                 ufdmsg_t* m, u64 mh_u)
{
    if (!m->n || !control || controllen_cap < 16) {
        for (int i = 0; i < m->n; i++) fdref_dec_enc(m->enc[i]);
        m->n = 0;
        return 0;
    }
    if (!user_range_ok(control, controllen_cap)) {
        for (int i = 0; i < m->n; i++) fdref_dec_enc(m->enc[i]);
        m->n = 0;
        return -1;
    }
    u64 need = 16 + (u64)m->n * 4;
    if (need > controllen_cap) {           /* receiver ctl too small:
                                              POSIX drops the fds        */
        for (int i = 0; i < m->n; i++) fdref_dec_enc(m->enc[i]);
        m->n = 0;
        return 0;
    }
    *(u64*)control            = need;
    *(s32*)(control + 8)      = 1;         /* SOL_SOCKET  */
    *(s32*)(control + 12)     = 1;         /* SCM_RIGHTS  */
    for (int i = 0; i < m->n; i++) {
        s32 newfd = proc_fd_alloc(m->enc[i]);
        if (newfd < 0) {                   /* table full: drop this fd  */
            fdref_dec_enc(m->enc[i]);
            *(s32*)(control + 16 + 4 * i) = -1;
            continue;
        }
        *(s32*)(control + 16 + 4 * i) = newfd;
    }
    m->n = 0;
    if (user_range_ok(mh_u, 48)) *(u64*)(mh_u + 40) = need;
    return (s32)need;
}

static u64 sys_socketpair_impl(u64 domain, u64 type, u64 proto,
                               u64 sv_u, u64 flags)
{
    (void)proto; (void)flags;
    if (domain != 1) return (u64)(s64)(-93);       /* AF_UNIX(1) only */
    u64 base = type & 0xFFu;
    if (base != 1 && base != 2)
        return (u64)(s64)(-93);                    /* STREAM/DGRAM    */
    if (!user_range_ok(sv_u, 8)) return (u64)(s64)(-LINUX_EFAULT);
    s32 idx = unix_alloc();
    if (idx < 0) return (u64)(s64)(-23);           /* ENFILE          */
    g_unich[idx].refs[0] = 1;
    g_unich[idx].refs[1] = 1;
    s32 fd0 = proc_fd_alloc(FD_UNIX_BASE - 2 * idx);
    s32 fd1 = proc_fd_alloc(FD_UNIX_BASE - 2 * idx - 1);
    if (fd0 < 0 || fd1 < 0) {
        if (fd0 >= 0) { proc_fds[fd0].used = false; proc_fds[fd0].fs_fd = -1; }
        if (fd1 >= 0) { proc_fds[fd1].used = false; proc_fds[fd1].fs_fd = -1; }
        unix_ref_dec(idx, false);
        unix_ref_dec(idx, true);
        return (u64)(s64)(-24);                    /* EMFILE          */
    }
    *(s32*)sv_u       = fd0;
    *(s32*)(sv_u + 4) = fd1;
    serial_printf("[UNIX] socketpair idx=%d sv={%d,%d}\n", idx, fd0, fd1);
    return 0;
}

/* FIFO push/pop for pending SCM_RIGHTS sets. Direction is named by
 * the SENDING end: from_end0 => rides the 0->1 queue. On receive the
 * end pops ITS inbound queue: end0 pops pend10, end1 pops pend01.   */
static bool unix_pend_push(unich_t* u, bool from_end0, const ufdmsg_t* m)
{
    ufdmsg_t* q = from_end0 ? u->pend01 : u->pend10;
    int* n      = from_end0 ? &u->pend01_n : &u->pend10_n;
    int* head   = from_end0 ? &u->pend01_head : &u->pend10_head;
    if (*n >= UNIX_MAX_PEND) return false;
    int slot = (*head + *n) % UNIX_MAX_PEND;
    q[slot] = *m;
    (*n)++;
    (void)head;
    return true;
}
static bool unix_pend_pop(unich_t* u, bool at_end0, ufdmsg_t* out)
{
    ufdmsg_t* q = at_end0 ? u->pend10 : u->pend01;
    int* n      = at_end0 ? &u->pend10_n : &u->pend01_n;
    int* head   = at_end0 ? &u->pend10_head : &u->pend01_head;
    if (*n == 0) return false;
    int slot = *head % UNIX_MAX_PEND;
    *out = q[slot];
    *head = (*head + 1) % UNIX_MAX_PEND;
    (*n)--;
    return true;
}

/* Stream write into the channel ring (blocking; hlt windows let the
 * peer task run — same discipline as the pipe write path). Returns
 * bytes written, -EPIPE when the peer end has no aliases left.      */
static s64 unix_send_stream(s32 uidx, bool end0, const u8* in, u64 count)
{
    unich_t* u = &g_unich[uidx];
    volatile u32* head = end0 ? &u->ab_head : &u->ba_head;
    volatile u32* tail = end0 ? &u->ab_tail : &u->ba_tail;
    u8* ring           = end0 ? u->ab : u->ba;
    int  peer          = end0 ? 1 : 0;
    u64 written = 0;
    while (written < count) {
        if (u->refs[peer] == 0)
            return (written > 0) ? (s64)written : -LINUX_EPIPE;
        u32 used  = *head - *tail;
        u32 space = UNIX_BUF_SIZE - used;
        if (space == 0) {
            /* ring full: let the reader drain (see #unix-accept-yield) */
            extern void task_yield(void);
            task_yield();
            continue;
        }
        u64 n = count - written;
        if (n > space) n = space;
        u32 pos  = (*head) & UNIX_BUF_MASK;
        u32 cont = UNIX_BUF_SIZE - pos;
        if (cont > n) cont = (u32)n;
        kmemcpy(ring + pos, in + written, cont);
        if (n > cont) kmemcpy(ring, in + written + cont, n - cont);
        *head += (u32)n;
        written += n;
    }
    return (s64)written;
}

/* Stream read from the channel ring. dontwait: single sweep, -EAGAIN
 * when empty. Returns bytes, 0 on peer-gone EOF. Does NOT consume
 * pending fd sets — those belong to recvmsg (POSIX cmsg delivery).  */
static s64 unix_recv_stream(s32 uidx, bool end0, u8* out, u64 count,
                            bool dontwait)
{
    unich_t* u = &g_unich[uidx];
    volatile u32* head = end0 ? &u->ba_head : &u->ab_head;
    volatile u32* tail = end0 ? &u->ba_tail : &u->ab_tail;
    u8* ring           = end0 ? u->ba : u->ab;
    int  peer          = end0 ? 1 : 0;
    for (;;) {
        u32 avail = *head - *tail;
        if (avail > 0) {
            u32 n = avail;
            if (n > count) n = (u32)count;
            u32 pos  = (*tail) & UNIX_BUF_MASK;
            u32 cont = UNIX_BUF_SIZE - pos;
            if (cont > n) cont = n;
            kmemcpy(out, ring + pos, cont);
            if (n > cont) kmemcpy(out + cont, ring, n - cont);
            *tail += n;
            return (s64)n;
        }
        if (u->refs[peer] == 0) return 0;      /* EOF: peer side gone */
        if (dontwait) return -LINUX_EAGAIN;
        /* empty ring: let the writer run (see #unix-accept-yield)   */
        {
            extern void task_yield(void);
            task_yield();
        }
    }
}

// ---- epoll (289/290/291): the wl_event_loop heartbeat --------------
// libwayland's event loop is epoll-based (wl_event_loop_epoll). We
// provide create1/ctl/wait over the same readiness model as poll(2):
// console/net/pipe/unix/file classes. Registered records are
// addressless (fd + events + u64 data) like epoll semantics require.
// fd encoding: FD_EPOLL_BASE - instance index.

#define FD_EPOLL_BASE  (-110)
#define MAX_EPINST      4
#define EP_MAX_FDS     32

typedef struct { s32 fd; u32 events; u64 data; bool used; } ep_rec_t;
typedef struct { bool used; ep_rec_t f[EP_MAX_FDS]; } ep_inst_t;
static ep_inst_t g_eps[MAX_EPINST];

static inline bool fd_is_epoll_enc(s32 v) {
    return v <= FD_EPOLL_BASE && v > FD_EPOLL_BASE - MAX_EPINST;
}
static inline s32 fd_epoll_index(s32 v) { return FD_EPOLL_BASE - v; }

/* Readiness for one registered fd (epoll flavor of the poll classes). */
static bool ep_fd_ready(s32 fd, u32 ev, u32* out_revents)
{
    u32 rev = 0;
    if (fd < 0 || fd >= PROC_FD_MAX || !proc_fds[fd].used) {
        *out_revents = 0;
        return false;
    }
    s32 v = proc_fds[fd].fs_fd;
    bool in_req = (ev & 1) != 0;           /* EPOLLIN  */
    bool out_req = (ev & 4) != 0;          /* EPOLLOUT */
    if (fd_is_net(v)) {
        s32 nidx = fd_net_index(v);
        extern bool net_socket_is_used(int);
        if (!net_socket_is_used(nidx)) { *out_revents = 0; return false; }
        extern bool net_socket_has_data(int);
        extern bool net_socket_is_connected(int);
        bool in = net_socket_has_data(nidx);
        bool out = net_socket_is_connected(nidx);
        if (in_req && in) rev |= 1;
        if (out_req && out) rev |= 4;
    } else if (fd_is_efd_enc(v) || fd_is_tfd_enc(v) ||
               fd_is_mfd_enc(v) || fd_is_sfd_enc(v)) {
        bool inr = false, outr = false;
        wl_obj_ready(v, in_req, out_req, &inr, &outr);
        if (in_req && inr) rev |= 1;
        if (out_req && outr) rev |= 4;
    } else if (fd_is_pipe_enc(v)) {
        pipe_t* pp = &g_pipes[fd_pipe_index(v)];
        if (fd_pipe_is_write(v)) {
            if (out_req) rev |= (pp->readers != 0 &&
                                 pp->head - pp->tail < PIPE_BUF_SIZE) ? 4 : 0;
        } else {
            if (in_req) rev |= ((pp->head != pp->tail) ||
                                (pp->writers == 0)) ? 1 : 0;
        }
    } else if (fd_is_unix_enc(v)) {
        bool e0 = fd_unix_is_end0(v);
        unich_t* u = &g_unich[fd_unix_index(v)];
        if (!u->used) { *out_revents = 0; return false; }
        if (e0) {
            if (in_req && ((u->ba_head != u->ba_tail) || u->refs[1] == 0))
                rev |= 1;
            if (out_req && u->refs[1] != 0 &&
                (u->ab_head - u->ab_tail) < UNIX_BUF_SIZE) rev |= 4;
        } else {
            if (in_req && ((u->ab_head != u->ab_tail) || u->refs[0] == 0))
                rev |= 1;
            if (out_req && u->refs[0] != 0 &&
                (u->ba_head - u->ba_tail) < UNIX_BUF_SIZE) rev |= 4;
        }
    } else if (v < 0) {                    /* console sentinels        */
        extern bool keyboard_haschar(void);
        if (in_req && keyboard_haschar()) rev |= 1;
        if (out_req) rev |= 4;
    } else {                               /* regular file: instant    */
        if (in_req) rev |= 1;
        if (out_req) rev |= 4;
    }
    *out_revents = rev;
    return rev != 0;
}

static u64 sys_epoll_create1_impl(u64 flags)
{
    (void)flags;
    for (int i = 0; i < MAX_EPINST; i++) {
        if (g_eps[i].used) continue;
        kmemset(&g_eps[i], 0, sizeof(g_eps[i]));
        g_eps[i].used = true;
        s32 fd = proc_fd_alloc(FD_EPOLL_BASE - i);
        if (fd < 0) { g_eps[i].used = false; return (u64)(s64)(-24); }
        serial_printf("[EPOLL] create inst=%d fd=%d\n", i, fd);
        return (u64)fd;
    }
    return (u64)(s64)(-24);
}

static u64 sys_epoll_ctl_impl(u64 epfd, u64 op, u64 fd, u64 ev_u)
{
    if (epfd >= PROC_FD_MAX || !proc_fds[epfd].used ||
        !fd_is_epoll_enc(proc_fds[epfd].fs_fd))
        return (u64)(s64)(-LINUX_EBADF);
    ep_inst_t* ep = &g_eps[fd_epoll_index(proc_fds[epfd].fs_fd)];
    if (!ep->used) return (u64)(s64)(-LINUX_EBADF);
    /* op: 1=ADD 2=DEL 3=MOD */
    ep_rec_t* rec = NULL;
    for (int i = 0; i < EP_MAX_FDS; i++) {
        if (ep->f[i].used && ep->f[i].fd == (s32)fd) { rec = &ep->f[i]; break; }
    }
    if (op == 2) {                          /* DEL */
        if (!rec) return (u64)(s64)(-2);    /* -ENOENT */
        rec->used = false;
        return 0;
    }
    if (op == 1 || op == 3) {
        if (op == 1 && rec) return (u64)(s64)(-17);   /* EEXIST */
        if (op == 3 && !rec) return (u64)(s64)(-2);   /* -ENOENT */
        if (!rec) {
            for (int i = 0; i < EP_MAX_FDS; i++) {
                if (!ep->f[i].used) { rec = &ep->f[i]; break; }
            }
            if (!rec) return (u64)(s64)(-23);          /* ENOSPC */
        }
        u32 events = 0; u64 data = 0;
        if (ev_u && user_range_ok(ev_u, 16)) {
            events = *(u32*)ev_u;
            data   = *(u64*)(ev_u + 8);
        }
        rec->used = true;
        rec->fd = (s32)fd;
        rec->events = events | 1;      /* EPOLLIN always reported    */
        rec->data = data;
        return 0;
    }
    return (u64)(s64)(-22);                 /* EINVAL */
}

static u64 sys_epoll_wait_impl(u64 epfd, u64 ev_u, u64 maxev, u64 timeout_ms)
{
    if (!proc_fds[epfd].used ||
        !fd_is_epoll_enc(proc_fds[epfd].fs_fd))
        return (u64)(s64)(-LINUX_EBADF);
    ep_inst_t* ep = &g_eps[fd_epoll_index(proc_fds[epfd].fs_fd)];
    if (!ep->used || !maxev) return 0;
    if (maxev > EP_MAX_FDS) maxev = EP_MAX_FDS;
    if (ev_u && !user_range_ok(ev_u, maxev * 16))
        return (u64)(s64)(-LINUX_EFAULT);

    u64 waited = 0;
    for (;;) {
        u32 n_out = 0;
        for (int i = 0; i < EP_MAX_FDS && n_out < maxev; i++) {
            if (!ep->f[i].used) continue;
            u32 rev = 0;
            if (ep_fd_ready(ep->f[i].fd, ep->f[i].events, &rev)) {
                u32* ue = (u32*)(ev_u + n_out * 16);
                *ue = rev;
                *(u64*)(ev_u + n_out * 16 + 8) = ep->f[i].data;
                n_out++;
            }
        }
        if (n_out) return n_out;
        if (timeout_ms == 0) return 0;
        if ((s64)timeout_ms > 0 && waited >= timeout_ms) return 0;
        /* #pipe-fd-switch family: readiness may be produced by another
         * task (pipe writer, unix peer) — yield to it; task_yield()
         * hlt-breathes when nothing is READY, so idle epoll costs the
         * same one tick as the old bare-hlt loop. */
        {
            extern void task_yield(void);
            task_yield();
        }
        waited += 10;
    }
}


// =====================================================================
// #wl-substrate: the syscall surface libwayland / wlroots need.
//
//   eventfd2(290)     wl_event_loop wakeups, drm backend fences
//   timerfd_*         wl_event_loop timers (output repaint scheduling)
//   signalfd4(289)    wlroots signal integration (honest no-op for now)
//   memfd_create(319) wl_shm pool backing + ftruncate + fd-backed mmap
//   readv/writev      libwayland wire output buffering
//   getrandom(318)    wayland socket ids / client seeds
//   futex(202)        musl pthread primitives (single-thread semantics)
//   ppoll(271)        musl poll() lowers to SYS_poll, others use ppoll
//   dup3(292)         O_CLOEXEC flavor of dup2
//   epoll 232/233/281 REAL x86_64 ABI numbers (the old 289/290 mapping
//                     only worked for raw syscall probes, never musl)
//
// fd encodings fill the free sentinel gap below the unix range:
//   eventfd  -95..-100   timerfd -101..-104   memfd -105..-108
//   signalfd -109        (epoll stays at -110..-113)
// =====================================================================

#define FD_EFD_BASE   (-95)
#define MAX_EFD       6
#define FD_TFD_BASE   (-101)
#define MAX_TFD       4
#define FD_MFD_BASE   (-105)
#define MAX_MFD       4
#define FD_SFD_BASE   (-109)
#define MAX_SFD       1

typedef struct {
    bool used;
    int  refs;                       /* process fds holding this object */
    u64  counter;                    /* eventfd value                   */
    bool nonblock;
} efd_t;
static efd_t g_efds[MAX_EFD];

typedef struct {
    bool used;
    int  refs;
    bool armed;
    u64  expire_ms;                  /* absolute uptime deadline        */
    u64  interval_ms;                /* 0 = one-shot                    */
    u64  expired;                    /* pending expiration count        */
    bool nonblock;
} tfd_t;
static tfd_t g_tfds[MAX_TFD];

/* Fold every elapsed deadline into ->expired (readiness side effect).
 * u64 ms clock = ticks*1000/TIMER_FREQ (see uptime_ms_now below). */
static u64 uptime_ms_now(void);
static void tfd_pump(tfd_t* t)
{
    if (!t->armed) return;
    u64 now = uptime_ms_now();
    while (t->expire_ms <= now) {
        t->expired++;
        if (t->interval_ms == 0) { t->armed = false; break; }
        t->expire_ms += t->interval_ms;
    }
}

/* wl_shm pool backing: page-granular physical memory so the SAME
 * frames can be mapped into every sharer's address space (that is
 * the whole point of wl_shm: client writes, compositor reads). */
#define MFD_MAX_PAGES  1024          /* 4 MiB per pool cap              */
typedef struct {
    bool        used;
    int         refs;
    u64         size;
    u32         npages;
    phys_addr_t pages[MFD_MAX_PAGES];
    u64         cursor;              /* read/write position             */
} mfd_t;
static mfd_t g_mfds[MAX_MFD];

typedef struct {
    bool used;
    int  refs;
} sfd_t;
static sfd_t g_sfds[MAX_SFD];

bool fd_is_efd_enc(s32 v) {
    return v <= FD_EFD_BASE && v > FD_EFD_BASE - MAX_EFD;
}
static inline s32 fd_efd_index(s32 v) { return FD_EFD_BASE - v; }
bool fd_is_tfd_enc(s32 v) {
    return v <= FD_TFD_BASE && v > FD_TFD_BASE - MAX_TFD;
}
static inline s32 fd_tfd_index(s32 v) { return FD_TFD_BASE - v; }
bool fd_is_mfd_enc(s32 v) {
    return v <= FD_MFD_BASE && v > FD_MFD_BASE - MAX_MFD;
}
static inline s32 fd_mfd_index(s32 v) { return FD_MFD_BASE - v; }
bool fd_is_sfd_enc(s32 v) {
    return v <= FD_SFD_BASE && v > FD_SFD_BASE - MAX_SFD;
}
static inline s32 fd_sfd_index(s32 v) { return FD_SFD_BASE - v; }
bool fd_is_wlobj_enc(s32 v) {
    return fd_is_efd_enc(v) || fd_is_tfd_enc(v) ||
           fd_is_mfd_enc(v) || fd_is_sfd_enc(v);
}

/* Readiness for poll/epoll across the wl object classes. */
bool wl_obj_ready(s32 v, bool in_req, bool out_req,
                  bool* out_in, bool* out_out)
{
    *out_in = false; *out_out = false;
    if (fd_is_efd_enc(v)) {
        efd_t* e = &g_efds[fd_efd_index(v)];
        if (!e->used) return false;
        *out_in  = e->counter > 0;
        *out_out = true;
    } else if (fd_is_tfd_enc(v)) {
        tfd_t* t = &g_tfds[fd_tfd_index(v)];
        if (!t->used) return false;
        tfd_pump(t);
        *out_in  = t->expired > 0;
        *out_out = false;
    } else if (fd_is_mfd_enc(v)) {
        mfd_t* m = &g_mfds[fd_mfd_index(v)];
        if (!m->used) return false;
        *out_in  = m->cursor < m->size;
        *out_out = true;
    } else {
        /* signalfd: never ready until user signals exist */
        return g_sfds[fd_sfd_index(v)].used;
    }
    (void)in_req; (void)out_req;
    return true;
}

/* Ring-3 I/O into the wl objects (read/write dispatch). */
s64 wl_obj_read(s32 v, u8* buf, u64 count, bool nonblock_override)
{
    if (fd_is_efd_enc(v)) {
        efd_t* e = &g_efds[fd_efd_index(v)];
        for (;;) {
            u64 c = e->counter;
            if (c > 0) {
                if (count < 8) return -22;
                *(u64*)buf = c;
                e->counter = 0;
                return 8;
            }
            if (e->nonblock || nonblock_override) return -11; /*EAGAIN*/
            extern void task_yield(void);
            task_yield();
        }
    }
    if (fd_is_tfd_enc(v)) {
        tfd_t* t = &g_tfds[fd_tfd_index(v)];
        for (;;) {
            tfd_pump(t);
            if (t->expired > 0) {
                if (count < 8) return -22;
                *(u64*)buf = t->expired;
                t->expired = 0;
                return 8;
            }
            if (t->nonblock || nonblock_override) return -11;
            extern void task_yield(void);
            task_yield();
        }
    }
    if (fd_is_mfd_enc(v)) {
        mfd_t* m = &g_mfds[fd_mfd_index(v)];
        u64 avail = (m->cursor < m->size) ? m->size - m->cursor : 0;
        u64 n = (avail < count) ? avail : count;
        {
            extern u64 vmm_pa_read_begin(void);
            extern void vmm_pa_read_end(u64);
            u64 sv = vmm_pa_read_begin();
            u64 done = 0;
            while (done < n) {
                u32 pg = (u32)(m->cursor / 0x1000);
                u32 off = (u32)(m->cursor & 0xFFFULL);
                u32 chunk = 0x1000 - off;
                if (chunk > n - done) chunk = (u32)(n - done);
                kmemcpy((u8*)buf + done,
                        (const u8*)PHYS_TO_VIRT(m->pages[pg]) + off,
                        chunk);
                done += chunk; m->cursor += chunk;
            }
            vmm_pa_read_end(sv);
        }
        return (s64)n;
    }
    /* signalfd */
    return -11;                                /* EAGAIN             */
}

s64 wl_obj_write(s32 v, const u8* buf, u64 count)
{
    if (fd_is_efd_enc(v)) {
        efd_t* e = &g_efds[fd_efd_index(v)];
        if (count < 8) return -22;
        u64 add = *(const u64*)buf;
        if (add == 0xFFFFFFFFFFFFFFFFULL) return -22;
        if (e->counter > 0xFFFFFFFFFFFFFFFFULL - add) return -11;
        e->counter += add;
        return 8;
    }
    if (fd_is_tfd_enc(v)) return -22;          /* EINVAL             */
    if (fd_is_mfd_enc(v)) {
        mfd_t* m = &g_mfds[fd_mfd_index(v)];
        u64 avail = (m->cursor < m->size) ? m->size - m->cursor : 0;
        u64 n = (avail < count) ? avail : count;
        {
            extern u64 vmm_pa_read_begin(void);
            extern void vmm_pa_read_end(u64);
            u64 sv = vmm_pa_read_begin();
            u64 done = 0;
            while (done < n) {
                u32 pg = (u32)(m->cursor / 0x1000);
                u32 off = (u32)(m->cursor & 0xFFFULL);
                u32 chunk = 0x1000 - off;
                if (chunk > n - done) chunk = (u32)(n - done);
                kmemcpy((u8*)PHYS_TO_VIRT(m->pages[pg]) + off,
                        (const u8*)buf + done, chunk);
                done += chunk; m->cursor += chunk;
            }
            vmm_pa_read_end(sv);
        }
        return (s64)n;
    }
    return -22;                                /* signalfd: EINVAL   */
}

/* Per-object ref bookkeeping shared by close/dup/fork/exit paths. */
void wl_obj_ref_dec(s32 v)
{
    if (fd_is_efd_enc(v)) {
        s32 i = fd_efd_index(v);
        if (i >= 0 && i < MAX_EFD && g_efds[i].used &&
            g_efds[i].refs > 0 && --g_efds[i].refs == 0)
            g_efds[i].used = false;
    } else if (fd_is_tfd_enc(v)) {
        s32 i = fd_tfd_index(v);
        if (i >= 0 && i < MAX_TFD && g_tfds[i].used &&
            g_tfds[i].refs > 0 && --g_tfds[i].refs == 0)
            g_tfds[i].used = false;
    } else if (fd_is_mfd_enc(v)) {
        s32 i = fd_mfd_index(v);
        if (i >= 0 && i < MAX_MFD && g_mfds[i].used &&
            g_mfds[i].refs > 0 && --g_mfds[i].refs == 0) {
            for (u32 pg = 0; pg < g_mfds[i].npages; pg++)
                pmm_free_page(g_mfds[i].pages[pg]);
            g_mfds[i].npages = 0;
            g_mfds[i].size   = 0;
            g_mfds[i].used   = false;
        }
    } else if (fd_is_sfd_enc(v)) {
        s32 i = fd_sfd_index(v);
        if (i >= 0 && i < MAX_SFD && g_sfds[i].used &&
            g_sfds[i].refs > 0 && --g_sfds[i].refs == 0)
            g_sfds[i].used = false;
    }
}

void wl_obj_ref_inc(s32 v)
{
    if (fd_is_efd_enc(v)) {
        s32 i = fd_efd_index(v);
        if (i >= 0 && i < MAX_EFD && g_efds[i].used) g_efds[i].refs++;
    } else if (fd_is_tfd_enc(v)) {
        s32 i = fd_tfd_index(v);
        if (i >= 0 && i < MAX_TFD && g_tfds[i].used) g_tfds[i].refs++;
    } else if (fd_is_mfd_enc(v)) {
        s32 i = fd_mfd_index(v);
        if (i >= 0 && i < MAX_MFD && g_mfds[i].used) g_mfds[i].refs++;
    } else if (fd_is_sfd_enc(v)) {
        s32 i = fd_sfd_index(v);
        if (i >= 0 && i < MAX_SFD && g_sfds[i].used) g_sfds[i].refs++;
    }
}

static u64 uptime_ms_now(void)
{
    return (u64)timer_get_ticks() * 1000ULL / (u64)TIMER_FREQ;
}

/* ---- eventfd2(290) ------------------------------------------------ */
static u64 sys_eventfd2_impl(u64 initval, u64 flags)
{
    bool nonblock = (flags & 0x800) != 0;      /* EFD_NONBLOCK       */
    for (int i = 0; i < MAX_EFD; i++) {
        if (g_efds[i].used) continue;
        kmemset(&g_efds[i], 0, sizeof(g_efds[i]));
        g_efds[i].used     = true;
        g_efds[i].refs     = 1;
        g_efds[i].counter  = initval;
        g_efds[i].nonblock = nonblock;
        s32 fd = proc_fd_alloc(FD_EFD_BASE - i);
        if (fd < 0) { g_efds[i].used = false; return (u64)(s64)(-24); }
        return (u64)fd;
    }
    return (u64)(s64)(-24);                    /* EMFILE             */
}

/* ---- timerfd_create(253) / settime(254) / gettime(255) ------------ */
static u64 sys_timerfd_create_impl(u64 clockid, u64 flags)
{
    if (clockid != 0 && clockid != 1) return (u64)(s64)(-22);
    bool nonblock = (flags & 0x800) != 0;      /* TFD_NONBLOCK       */
    for (int i = 0; i < MAX_TFD; i++) {
        if (g_tfds[i].used) continue;
        kmemset(&g_tfds[i], 0, sizeof(g_tfds[i]));
        g_tfds[i].used     = true;
        g_tfds[i].refs     = 1;
        g_tfds[i].nonblock = nonblock;
        s32 fd = proc_fd_alloc(FD_TFD_BASE - i);
        if (fd < 0) { g_tfds[i].used = false; return (u64)(s64)(-24); }
        return (u64)fd;
    }
    return (u64)(s64)(-24);
}

static u64 sys_timerfd_settime_impl(u64 fd, u64 flags, u64 newval_u,
                                    u64 oldval_u)
{
    if (!fd_is_tfd_enc(proc_fds[fd].fs_fd)) return (u64)(s64)(-LINUX_EBADF);
    tfd_t* t = &g_tfds[fd_tfd_index(proc_fds[fd].fs_fd)];
    if (!user_range_ok(newval_u, 32)) return (u64)(s64)(-LINUX_EFAULT);
    u64 nsec_iv = *(u64*)(newval_u + 8);       /* it_interval.tv_nsec */
    u64 sec_iv  = *(u64*)(newval_u + 0);
    u64 nsec_v  = *(u64*)(newval_u + 24);      /* it_value.tv_nsec    */
    u64 sec_v   = *(u64*)(newval_u + 16);
    if (oldval_u && user_range_ok(oldval_u, 32)) {
        /* report the old arm state (interval + remaining)          */
        tfd_pump(t);
        *(u64*)(oldval_u + 0)  = t->interval_ms / 1000;
        *(u64*)(oldval_u + 8)  = (t->interval_ms % 1000) * 1000000ULL;
        u64 rem = (t->armed && t->expire_ms > uptime_ms_now())
                  ? (t->expire_ms - uptime_ms_now()) : 0;
        *(u64*)(oldval_u + 16) = rem / 1000;
        *(u64*)(oldval_u + 24) = (rem % 1000) * 1000000ULL;
    }
    if (sec_v == 0 && nsec_v == 0) {           /* disarm              */
        t->armed = false; t->expired = 0;
        t->interval_ms = 0;
        return 0;
    }
    u64 vms  = sec_v * 1000ULL + nsec_v / 1000000ULL;
    u64 ivms = sec_iv * 1000ULL + nsec_iv / 1000000ULL;
    t->interval_ms = ivms;
    t->expire_ms   = (flags & 1) ? vms            /* TFD_TIMER_ABSTIME */
                                 : uptime_ms_now() + vms;
    t->armed  = true;
    t->expired = 0;
    return 0;
}

static u64 sys_timerfd_gettime_impl(u64 fd, u64 curval_u)
{
    if (!fd_is_tfd_enc(proc_fds[fd].fs_fd)) return (u64)(s64)(-LINUX_EBADF);
    tfd_t* t = &g_tfds[fd_tfd_index(proc_fds[fd].fs_fd)];
    if (!user_range_ok(curval_u, 32)) return (u64)(s64)(-LINUX_EFAULT);
    tfd_pump(t);
    *(u64*)(curval_u + 0)  = t->interval_ms / 1000;
    *(u64*)(curval_u + 8)  = (t->interval_ms % 1000) * 1000000ULL;
    u64 rem = (t->armed && t->expire_ms > uptime_ms_now())
              ? (t->expire_ms - uptime_ms_now()) : 0;
    *(u64*)(curval_u + 16) = rem / 1000;
    *(u64*)(curval_u + 24) = (rem % 1000) * 1000000ULL;
    return 0;
}

/* ---- memfd_create(319) + ftruncate(77) ----------------------------- */
static u64 sys_memfd_create_impl(u64 name_u, u64 flags)
{
    (void)name_u; (void)flags;                 /* MFD_* flags fine    */
    for (int i = 0; i < MAX_MFD; i++) {
        if (g_mfds[i].used) continue;
        kmemset(&g_mfds[i], 0, sizeof(g_mfds[i]));
        g_mfds[i].used = true;
        g_mfds[i].refs = 1;
        s32 fd = proc_fd_alloc(FD_MFD_BASE - i);
        if (fd < 0) { g_mfds[i].used = false; return (u64)(s64)(-24); }
        return (u64)fd;
    }
    return (u64)(s64)(-24);
}

static u64 sys_ftruncate_impl(u64 fd, s64 len)
{
    if (len < 0) return (u64)(s64)(-22);
    if (!fd_is_mfd_enc(proc_fds[fd].fs_fd))
        return (u64)(s64)(-22);                /* fs resize: TODO     */
    mfd_t* m = &g_mfds[fd_mfd_index(proc_fds[fd].fs_fd)];
    u64 want = ((u64)len + 0xFFFULL) & ~0xFFFULL;
    u32 np = (u32)(want / 0x1000);
    if (np > MFD_MAX_PAGES) return (u64)(s64)(-27);   /* EFBIG      */
    if (np < m->npages) {
        for (u32 pg = np; pg < m->npages; pg++)
            pmm_free_page(m->pages[pg]);
    } else if (np > m->npages) {
        for (u32 pg = m->npages; pg < np; pg++) {
            phys_addr_t pa = pmm_alloc_page();
            if (pa == 0) {
                /* roll back the pages we got */
                for (u32 p2 = m->npages; p2 < pg; p2++)
                    pmm_free_page(m->pages[p2]);
                return (u64)(s64)(-12);        /* ENOMEM             */
            }
            m->pages[pg] = pa;
            {
                extern u64 vmm_pa_read_begin(void);
                extern void vmm_pa_read_end(u64);
                u64 sv = vmm_pa_read_begin();
                kmemset((void*)PHYS_TO_VIRT(pa), 0, 0x1000);
                vmm_pa_read_end(sv);
            }
        }
    }
    m->npages = np;
    m->size   = (u64)len;
    return 0;
}

/* ---- signalfd4(289): honest minimal -------------------------------
 * No user-space signal delivery exists yet (kill(2) only drives job
 * control stop/cont). libwayland registers the signalfd in its epoll
 * set; it must simply never report readable. Read: EAGAIN (the
 * NONBLOCK flavor libwayland uses). Update path accepts any mask.  */
static u64 sys_signalfd4_impl(s64 ufd, u64 mask_u, u64 sigsetsize,
                              u64 flags)
{
    (void)mask_u;
    if (sigsetsize != 8 && sigsetsize != 0) return (u64)(s64)(-22);
    if (ufd == -1) {
        for (int i = 0; i < MAX_SFD; i++) {
            if (g_sfds[i].used) continue;
            g_sfds[i].used = true;
            g_sfds[i].refs = 1;
            s32 fd = proc_fd_alloc(FD_SFD_BASE - i);
            if (fd < 0) { g_sfds[i].used = false; return (u64)(s64)(-24); }
            return (u64)fd;
        }
        return (u64)(s64)(-24);
    }
    if (ufd < 0 || ufd >= (s64)PROC_FD_MAX ||
        !fd_is_sfd_enc(proc_fds[ufd].fs_fd))
        return (u64)(s64)(-LINUX_EBADF);
    return (u64)ufd;                           /* mask update: ok    */
}

/* ---- getrandom(318): real bytes (musl loops on 0!) ------------------
 * xorshift64 seeded from uptime + RTC mix. Not crypto, but the wayland
 * socket name and client seeds only need unpredictability vs. the boot
 * image, not an adversary. Cap per call: Linux caps at 32 MB; we cap
 * at 1 MiB (buffer is filled in-kernel in one go).                    */
static u64 sys_getrandom_impl(u64 buf, u64 len, u64 flags)
{
    (void)flags;
    if (!user_range_ok(buf, len ? len : 1)) return (u64)(s64)(-LINUX_EFAULT);
    if (len == 0) return 0;
    if (len > 1024 * 1024) len = 1024 * 1024;
    static u64 gr_spin;                    /* per-call advance: two calls
                                            * inside one ms must differ */
    u64 st = uptime_ms_now() * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
    st ^= (u64)timer_get_ticks() << 17;
    st ^= ++gr_spin * 0x2545F4914F6CDD1DULL;
    u8* out = (u8*)buf;
    for (u64 i = 0; i < len; i++) {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        out[i] = (u8)(st >> 24);
    }
    return len;
}

/* ---- futex(202): single-thread honest semantics --------------------
 * No threads exist yet, so WAIT-with-matching-value would deadlock by
 * definition. musl's mutex paths treat any return as "retry" (spurious
 * wake), WAKE reports zero waiters. Both are POSIX-tolerable here.   */
static u64 sys_futex_impl(u64 uaddr, u64 op, u64 val, u64 timeout_u)
{
    (void)timeout_u;
    if (!user_range_ok(uaddr, 4)) return (u64)(s64)(-LINUX_EFAULT);
    u64 opc = op & 0x7Fu;
    if (opc == 0 /*WAIT*/ || opc == 9 /*WAIT_BITSET*/) {
        if (*(volatile u32*)uaddr != (u32)val)
            return (u64)(s64)(-11);            /* -EAGAIN            */
        {
            extern void task_yield(void);
            task_yield();                      /* spurious-wake path */
        }
        return 0;
    }
    if (opc == 1 /*WAKE*/ || opc == 10 /*WAKE_BITSET*/) return 0;
    if (opc == 5 /*WAIT_REQUEUE*/ || opc == 4 /*CMP_REQUEUE*/) return 0;
    return (u64)(s64)(-22);                    /* -EINVAL            */
}

/* ---- ppoll(271): timespec timeout flavor of poll(7) ---------------- */
static u64 sys_ppoll_impl(u64 ufds, u64 nfds, u64 tmo_u, u64 sigmask_u,
                          u64 sigsetsize)
{
    (void)sigmask_u; (void)sigsetsize;         /* no user signals yet */
    s64 ms = -1;                               /* NULL => infinite    */
    if (tmo_u && user_range_ok(tmo_u, 16)) {
        s64 sec = *(s64*)tmo_u;
        s64 nsc = *(s64*)(tmo_u + 8);
        ms = sec * 1000 + nsc / 1000000;
        if (sec < 0) ms = 0;
    }
    return sys_poll_impl(ufds, nfds, (u64)ms);
}

// ---- Linux write(2) ---------------------------------------------
// ssize_t write(int fd, const void* buf, size_t count);
// fd 1 (stdout) and 2 (stderr) go to the VGA console.
// fds >= 3 (from open(2)) are dispatched into the ramfs.

static u64 sys_write_impl(u64 fd, u64 buf, u64 count)
{
    if (count == 0) return 0;             /* zero-length writes always ok */
    if (fault_if_bad(buf, count)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }

    /* Table-driven dispatch: sentinel slots ARE the console; real
     * descriptors ("cmd > file") go to the fs layer; NETWORK sockets
     * route into the UDP layer as connected-socket writes; closed fds
     * fail with EBADF instead of silently drawing on the screen. */
    {
        static int wr_seen_fd;
        if ((int)fd != wr_seen_fd) {
            s32 v = (fd < PROC_FD_MAX && proc_fds[fd].used)
                    ? proc_fds[fd].fs_fd : -999;
            serial_printf("[WR] fd=%lu fs_fd=%d\n",
                          (unsigned long)fd, v);
            wr_seen_fd = (int)fd;
        }
    }
    if (is_pipe_fd(fd)) {
        bool wr_end = false;
        s32 pidx = proc_fd_to_pipe(fd, &wr_end);
        if (pidx < 0) return (u64)(s64)(-LINUX_EBADF);
        if (!wr_end) return (u64)(s64)(-LINUX_EBADF);   /* write to read end */
        pipe_t* pp = &g_pipes[pidx];
        const u8* in = (const u8*)buf;
        u64 written = 0;
        while (written < count) {
            if (pp->readers == 0) {
                /* POSIX: reader gone => SIGPIPE/EPIPE. No user signal
                 * delivery yet, so report EPIPE (busybox prints
                 * "write error" and exits — pipelines still work). */
                return (written > 0) ? (u64)written
                                     : (u64)(s64)(-LINUX_EPIPE);
            }
            u32 used = pp->head - pp->tail;
            u32 space = PIPE_BUF_SIZE - used;
            if (space == 0) {
                /* #pipe-fd-switch FIX: the reader is another task —
                 * yield to it instead of hlt-spinning (preemption is
                 * OFF by default; see the read-side comment). */
                extern void task_yield(void);
                task_yield();
                continue;                             /* reader catches up */
            }
            u64 n = count - written;
            if (n > space) n = space;
            u32 pos  = pp->head & PIPE_BUF_MASK;
            u32 cont = PIPE_BUF_SIZE - pos;
            if (cont > n) cont = (u32)n;
            kmemcpy(pp->buf + pos, in + written, cont);
            if (n > cont) kmemcpy(pp->buf, in + written + cont, n - cont);
            pp->head += (u32)n;
            written += n;
        }
        return written;
    }
    if (fd < PROC_FD_MAX && proc_fds[fd].used &&
        fd_is_wlobj_enc(proc_fds[fd].fs_fd)) {
        extern s64 wl_obj_write(s32, const u8*, u64);
        s64 n = wl_obj_write(proc_fds[fd].fs_fd, (const u8*)buf, count);
        return (n < 0) ? (u64)(s64)n : (u64)n;
    }
    if (is_unix_fd(fd)) {
        /* #wayland-transport: AF_UNIX channel write (full duplex:
         * every end writes its own ring) */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0) return (u64)(s64)(-LINUX_EBADF);
        s64 n = unix_send_stream(uidx, end0, (const u8*)buf, count);
        return (u64)(s64)n;
    }
    if (is_net_fd(fd)) {
        /* write(2) on a connected UDP socket == sendto(remote).        */
        s32 idx = proc_fd_to_net(fd);
        {
            static bool wr_dbg;
            if (!wr_dbg) {
                wr_dbg = true;
                serial_printf("[WRNET] enter idx=%d remote=%u.%u.%u.%u:%u "
                              "lp=%u count=%lu\n", idx,
                              0u,0u,0u,0u, 0u,
                              (idx>=0)?net_socket_local_port(idx):0,
                              (unsigned long)count);
            }
        }
        if (idx < 0) return (u64)(s64)(-LINUX_EBADF);
        u8 rip[4]; u16 rport;
        int gr = -1;
        {
            extern int net_socket_get_remote(int, u8*, u16*);
            gr = net_socket_get_remote(idx, rip, &rport);
        }
        if (gr != 0) return (u64)(s64)(-89);      /* EDESTADDRREQ      */
        /* TCP stream socket: connected-write == push segments; the
         * handshake already resolved ARP, slirp is reliable, so this
         * is a straight split into <=1400B PSH|ACK segments.       */
        {
            extern int net_socket_type(int);
            extern int net_socket_stream_send(int, const void*, u32);
            if ((net_socket_type(idx) & 0xFF) == SOCK_STREAM) {
                s64 sn = net_socket_stream_send(idx,
                                                (const void*)buf,
                                                (u32)count);
                serial_printf("[WSND] idx=%d cnt=%ld n=%ld\n",
                              (unsigned)idx, (s64)count, sn);
                return (sn < 0) ? (u64)(s64)-104 : (u64)sn;
            }
        }
        if (count > 1500) count = 1500;
        /* Wait until the destination MAC is resolvable before handing
         * the datagram down: net_udp_send() drops on unresolved ARP,
         * and a one-shot write() has NO retry — historically this was
         * where every "udp-ok-nulos" payload silently died.
         * Poll-drain first (reply may already sit in the NIC ring),
         * then hlt-wait, re-kicking a fresh ARP request occasionally. */
        bool ar = false;
        {
            extern bool net_arp_ready_route(const u8*);
            extern void net_poll(void);
            extern void net_arp_kick_route(const u8*);
            net_poll();
            ar = net_arp_ready_route(rip);
            for (int i = 0; i < 300 && !ar; i++) {
                if ((i & 31) == 31) net_arp_kick_route(rip);
                __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
                net_poll();
                ar = net_arp_ready_route(rip);
            }
        }
        {
            static bool wr2_dbg;
            if (!wr2_dbg) {
                wr2_dbg = true;
                serial_printf("[WRNET2] dst=%u.%u.%u.%u:%u count=%lu arp=%d\n",
                              rip[0],rip[1],rip[2],rip[3], rport,
                              (unsigned long)count, ar ? 1 : 0);
            }
        }
        if (ar) {
            {
                extern bool net_socket_is_icmp(int);
                if (net_socket_is_icmp(idx)) {
                    /* User-built ICMP echo request: forward verbatim
                     * under an IP header (checksum done by caller). */
                    extern void net_icmp_send(const u8*, const u8*, u16);
                    extern void net_socket_icmp_arm(int, const u8*,
                                                    const u8*, u16);
                    u16 l2 = (count > 1480) ? 1480 : (u16)count;
                    net_icmp_send(rip, (const u8*)buf, l2);
                    net_socket_icmp_arm(idx, rip, (const u8*)buf, l2);
                    serial_printf("[WRICMP] idx=%d dst=%u.%u.%u.%u len=%lu\n",
                                  idx, rip[0],rip[1],rip[2],rip[3],
                                  (unsigned long)l2);
                    return l2;
                }
            }
            extern void net_udp_send(const u8*, u16, u16, const u8*, u16);
            net_udp_send(rip, rport,
                         net_socket_local_port(idx),
                         (const u8*)buf, (u16)count);
        } else {
            serial_printf("[WRDROP] dst=%u.%u.%u.%u count=%lu "
                          "no ARP — datagram dropped\n",
                          rip[0],rip[1],rip[2],rip[3],
                          (unsigned long)count);
        }
        return count;
    }
    bool cons_sink;
    if (fd < PROC_FD_MAX && proc_fds[fd].used) {
        if (proc_fds[fd].fs_fd < 0) {
            cons_sink = true;
        } else {
            s64 n = fs_write((u32)proc_fds[fd].fs_fd,
                             (const void*)buf, count);
            return (n < 0) ? (u64)(s64)(-1) : (u64)n;
        }
    } else {
        return (u64)(s64)(-LINUX_EBADF);
    }
    if (!cons_sink) return (u64)(s64)(-LINUX_EBADF);

    const char* p = (const char*)buf;
    for (u64 i = 0; i < count; i++) {
        vga_putchar(p[i]);
    }
    return count;
}

// ---- Linux read(2) ----------------------------------------------
// ssize_t read(int fd, void* buf, size_t count);
//
// fd 0 reads the PS/2 keyboard: line-buffered console input with
// local echo and backspace handling. Blocks until at least one
// character is available. Returns byte count; -EBADF/-EFAULT on error.

static u64 sys_read_impl(u64 fd, u64 buf, u64 count)
{
    /* count==0 must succeed BEFORE buffer validation: apk probes
     * empty files with read(fd, NULL/"", 0) and a -EFAULT here
     * surfaces downstream as adb "IO ERROR" (#apk-empty-read). */
    if (count == 0) return 0;
    if (!user_range_ok(buf, count)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }

    /* Table-driven dispatch (POSIX fd semantics): a slot can hold a
     * console sentinel (negative), a real ramfs descriptor (redirect
     * "cmd < file" lands real files on fd 0!), a NETWORK socket or be
     * CLOSED (EBADF). */
    {
        static int rd_seen_fd;
        if ((int)fd != rd_seen_fd) {
            s32 v = (fd < PROC_FD_MAX && proc_fds[fd].used)
                    ? proc_fds[fd].fs_fd : -999;
            serial_printf("[RD] fd=%lu fs_fd=%d\n",
                          (unsigned long)fd, v);
            rd_seen_fd = (int)fd;
        }
    }
    if (is_pipe_fd(fd)) {
        /* Pipe read end: block until data or writer-side EOF. The
         * writer is a real task; the timer preempts our hlt loop so
         * it makes progress while we sleep. */
        bool wr_end = false;
        s32 pidx = proc_fd_to_pipe(fd, &wr_end);
        if (pidx < 0 || wr_end) return (u64)(s64)(-LINUX_EBADF);
        pipe_t* pp = &g_pipes[pidx];
        u8* out = (u8*)buf;
        for (;;) {
            u32 avail = pp->head - pp->tail;
            if (avail > 0) {
                u32 n = avail;
                if (n > count) n = count;
                u32 pos  = pp->tail & PIPE_BUF_MASK;
                u32 cont = PIPE_BUF_SIZE - pos;
                if (cont > n) cont = n;
                kmemcpy(out, pp->buf + pos, cont);
                if (n > cont) kmemcpy(out + cont, pp->buf, n - cont);
                pp->tail += n;
                return (u64)n;
            }
            if (pp->writers == 0) return 0;       /* EOF: all writers gone */
            if (count == 0) return 0;
            /* #pipe-fd-switch FIX (real root): the writer is a REAL
             * task, and IRQ preemption is OFF by default — a bare
             * hlt loop here never reschedules, so a fork-v2 child
             * that should produce data (or close the pipe) never
             * gets its first slice and the reader blocks forever.
             * task_yield() breathes one hlt when nothing is READY
             * (same power profile) and actually hands the CPU to
             * the writer when one exists — the established pattern
             * of proc_do_wait4 / #unix-accept-yield. */
            {
                extern void task_yield(void);
                task_yield();
            }
        }
    }
    if (fd < PROC_FD_MAX && proc_fds[fd].used &&
        fd_is_wlobj_enc(proc_fds[fd].fs_fd)) {
        extern s64 wl_obj_read(s32, u8*, u64, bool);
        s64 n = wl_obj_read(proc_fds[fd].fs_fd, (u8*)buf, count,
                            false);
        return (n < 0) ? (u64)(s64)n : (u64)n;
    }
    if (is_unix_fd(fd)) {
        /* #wayland-transport: AF_UNIX channel read (full duplex).
         * Pending SCM_RIGHTS sets stay queued — POSIX delivers them
         * only through recvmsg control data. */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0) return (u64)(s64)(-LINUX_EBADF);
        s64 n = unix_recv_stream(uidx, end0, (u8*)buf, count, false);
        return (u64)(s64)n;
    }
    if (is_net_fd(fd)) {
        s32 idx = proc_fd_to_net(fd);
        if (idx < 0) return (u64)(s64)(-LINUX_EBADF);

        {
            extern int net_socket_type(int);
            if ((net_socket_type(idx) & 0xFF) == SOCK_STREAM) {
                /* TCP read: blocks briefly for the first byte, returns
                 * exactly 0 on peer-FIN EOF (musl relies on 0-EOF to
                 * terminate relay loops / HTTP bodies).             */
                extern int net_socket_stream_recv(int, void*, u32);
                extern bool net_socket_has_eof(int);
                s64 n = -1;
                /* #tcp-read-eagain: a BLOCKING fd must never answer
                 * -EAGAIN — apk's istream turns it into EIO and aborts
                 * the download mid-stream (stalled at ~128KB on every
                 * big fetch). Block until data or EOF; the hlt window
                 * still gets timer IRQs so net_poll() keeps flowing. */
                for (;;) {
                    static u32 rd_last_data_ms, rd_dumps;
                    n = net_socket_stream_recv(idx, (void*)buf,
                                               (u32)count);
                    u32 rd_now = (u32)timer_get_uptime_ms();
                    if (n != -1) { rd_last_data_ms = rd_now; break; } /* data/EOF */
                    if (net_socket_has_eof(idx)) { n = 0; break; }
                    /* #tcp-bigfetch-stall forensics: when the reader
                     * starves for seconds mid-transfer, dump the NIC's
                     * hardware view (CAPR/ISR/ring head) so a silent
                     * NIC is distinguishable from an unread ring. */
                    if (rd_last_data_ms && rd_now - rd_last_data_ms > 5000 &&
                        rd_dumps < 6) {
                        rd_dumps++;
                        extern void rtl8139_rx_diag(void);
                        serial_printf("[RDSTALL t=%lu] waiting for socket "
                                      "data %lu ms\n",
                                      (unsigned long)rd_now,
                                      (unsigned long)(rd_now - rd_last_data_ms));
                        rtl8139_rx_diag();
                    }
                    /* #pipe-fd-switch family: yield so peer tasks keep
                     * flowing while we wait on the wire; net_poll() is
                     * still ours — the NIC is NOT a task, nobody else
                     * will drain it for us (#poll-deport-irq). */
                    {
                        extern void task_yield(void);
                        task_yield();
                    }
                    {
                        extern void net_poll(void);
                        net_poll();
                    }
                }
                {
                    static int rds_full;
                    if (rds_full < 200) {
                        rds_full++;
                        serial_printf("[RDSTREAM t=%lu] idx=%d cnt=%ld n=%ld\n",
                                      (unsigned long)timer_get_uptime_ms(),
                                      idx, (s64)count, (long)n);
                    }
                }
                if (n > 0) return (u64)n;
                if (n == 0 ||
                    (n == -1 && net_socket_has_eof(idx))) return 0;
                return (u64)(s64)(-LINUX_EAGAIN);
            }
        }

        /* UDP read as before */
        s64 n = 0;
        {
            s32 idx = proc_fd_to_net(fd);
            if (idx < 0) return (u64)(s64)(-LINUX_EBADF);
            {
                static bool rdnet_dbg;
                if (!rdnet_dbg) {
                    rdnet_dbg = true;
                    serial_printf("[RDNET] enter idx=%d\n", idx);
                }
            }
            u8 sip[4]; u16 sport;
            for (int tries = 0; tries < 400; tries++) {
                n = net_socket_recvfrom(idx, (void*)buf,
                                        (u32)count, sip, &sport);
                if (n >= 0) break;
                __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
                {
                    extern void net_poll(void);
                    net_poll();
                }
            }
            {
                static bool rdnet2_dbg;
                if (!rdnet2_dbg) {
                    rdnet2_dbg = true;
                    serial_printf("[RDNET2] result=%ld idx=%d\n",
                                  (long)n, idx);
                }
            }
            if (n < 0) n = -LINUX_EAGAIN;
        }
        return (n < 0) ? (u64)(s64)n : (u64)n;
    }
    bool is_console;
    if (fd < PROC_FD_MAX && proc_fds[fd].used) {
        is_console = proc_fds[fd].fs_fd < 0;
    } else {
        return (u64)(s64)(-LINUX_EBADF);
    }
    if (!is_console) {
        s32 fs_fd = proc_fd_get(fd);
        if (fs_fd < 0) return (u64)(s64)(-LINUX_EBADF);
        s64 n = fs_read((u32)fs_fd, (void*)buf, count);
        serial_printf("[RDF] fd=%lu fsfd=%d cnt=%lu n=%ld\n",
                      (unsigned long)fd, fs_fd, (unsigned long)count, n);
        return (n < 0) ? (u64)(s64)(-LINUX_EBADF) : (u64)n;
    }

    /* Keyboard — line-buffered console input with local echo and
     * backspace handling. Blocks until at least one character.
     * FORK-V2: while nobody typed anything we COOPERATIVELY YIELD —
     * concurrent children keep running past a parent blocked on
     * stdin. The hlt window costs at most one timer tick (~10 ms)
     * when idle and returns INSTANTLY on the keyboard IRQ, so
     * single-task interactive latency is unchanged. */
    char* out = (char*)buf;
    u64 got = 0;
    /* Kernel threads (the built-in shell, pid==0) rely on this echo.
     * Ring-3 processes own their line editing: busybox ash echoes and
     * redraws by itself, so kernel-side echo appeared TWICE per key
     * ("ccaatt"). POSIX-ish: tty driver echoes only when the process
     * did not take the terminal into its own hands. */
    task_t* cur_task = task_get_current();
    bool local_echo = (!cur_task || cur_task->pid == 0);
    for (;;) {
        /* Drain NON-DATA events (key releases, bare modifiers) tightly,
         * exactly like the pre-v2 loop did — they must never delay the
         * delivery of an already-buffered real character.             */
        char c = 0;
        while (keyboard_haschar()) {
            c = keyboard_getchar();
            if (c != 0) break;
            c = 0;
        }
        if (c == 0) {
            /* [BISECT-D] idle breath only */
            __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
            continue;
        }
        /* NOTE: zero events (releases/modifiers) were drained above,
         * so reaching this point guarantees a real data byte — feeding
         * release padding to libc used to fill ash's lines with NULs
         * ("ccaatt"). */
        if (c == 8) {
            if (got > 0) {
                got--;
                if (local_echo) { vga_putchar(8); vga_putchar(32); vga_putchar(8); }
            }
            continue;
        }
        if (c == 13) c = 10;
        if (local_echo) vga_putchar(c);        /* local echo       */
        out[got++] = c;
        if (c == 10 || got >= count) break;  /* line-buffered    */
    }
    return got;
}

// ---- Linux exit(2) ----------------------------------------------

#include "../include/scheduler.h"

static u64 sys_exit_impl(u64 code)
{
    /* Processes terminate through the process layer: mark zombie and
     * switch away to the runner. Never returns. */
    task_t* t = task_get_current();
    if (t && t->pid > 0) {
        proc_exit_current((s32)(s64)code);
    }

    serial_printf("[EXIT] code=%ld\n", (s64)code);
    vga_printf("\n[SYS] Process exited with code: %ld\n", (s64)code);

    // Tell syscall_entry.S to abandon IRETQ-to-user and re-enter shell
    sys_exit_pending = 1;
    return code;                               /* not consumed */
}

// ---- Linux brk(12) ----------------------------------------------
// Program break for the current process (musl malloc grows the heap
// through brk increments). Cursor is PER-PROCESS.

static u64 sys_brk_impl(u64 addr)
{
    u64 cur = proc_get_brk();
    serial_printf("[BRK] cur=%lx req=%lx\n", (unsigned long)cur, (unsigned long)addr);

    if (addr == 0 || addr < cur) {
        return cur;                   /* query / shrink-noop */
    }
    if (addr > PROC_BRK_MAX) {
        return cur;                   /* refuse to grow      */
    }

    u64 new_end = (addr + 0xFFFULL) & ~0xFFFULL;
    /* #brk-skip-page: the OLD rounding `(cur + 0xFFF) & ~0xFFF` SKIPPED
     * the page that STARTS at an already-aligned break — and mallocng
     * grows brk in exactly-aligned single-page steps, so every growth
     * after the first mapped NOTHING yet reported success. The meta
     * pages mallocng then wrote were served by the demand pager with
     * no bookkeeping, and the NEXT brk growth replaced their PTEs with
     * fresh pages — mallocng's metadata vanished mid-life and its
     * a_crash() traps fired at exit (#apk-exit-malcheck). The break
     * line itself is unmapped memory: map from the page CONTAINING cur. */
    u64 cur_page = cur & ~0xFFFULL;

    for (u64 page = cur_page; page < new_end; page += 0x1000) {
        phys_addr_t phys = pmm_alloc_page();
        if (phys == 0) {
            return cur;               /* OOM: keep old break */
        }
        vmm_map(page, phys, VMM_PRESENT | VMM_WRITE | VMM_USER);
        /* zero via a kernel-CR3 window: identity VA writes under a
         * user CR3 whose low windows are split alias USER seg frames
         * (#ident-alias hazard — same family as #pfdump-alias) */
        {
            extern u64 vmm_pa_read_begin(void);
            extern void vmm_pa_read_end(u64);
            u64 s3 = vmm_pa_read_begin();
            kmemset((void*)PHYS_TO_VIRT(phys), 0, 0x1000);  /* anon zeroed */
            vmm_pa_read_end(s3);
        }
    }

    proc_set_brk(new_end);
    return new_end;
}

// ---- Linux open(2) ----------------------------------------------
// int open(const char* path, int flags, int mode);
//
// Flag translation (Linux → our ramfs):
//   low 2 bits: 0=O_RDONLY → FS_READ, 1=O_WRONLY → FS_WRITE,
//               2=O_RDWR → both
//   O_CREAT (0100) → FS_CREATE

#define LIN_O_ACCMODE 3
#define LIN_O_CREAT   0100

static s32 proc_fd_console_open(void)
{
    /* Virtual /dev/tty: a slot whose fs_fd sentinel says "console".
     * Reads/writes/ioctls on it route through the tty layer. */
    for (int i = 3; i < PROC_FD_MAX; i++) {
        if (!proc_fds[i].used) {
            proc_fds[i].used = true;
            proc_fds[i].fs_fd = -2;       /* console(0) sentinel  */
            serial_printf("[TTYOPEN] slot=%d\n", i);
            return i;
        }
    }
    return -1;
}

/* forward decl (definition lives in the tty/job-control section) */
static void tty_note_session_leader(s32 pid);

static bool fd_is_console(u64 fd);

/* The fd on which tcsetpgrp/tcgetpgrp most recently SUCCEEDED.
 * busybox ash opens /dev/tty, dups it for ioctl probing, then its
 * redirection dance may close/recycle those slots — but 'fg' still
 * calls tcsetpgrp on the remembered handle number afterwards.
 * ([#fg-notty] jc-run11: [TIOCSPGRP] fd=10 OK at boot, later
 * "[IOCNOTTY] fd=10 used=0" aborted the SIGCONT delivery.)         */
static s32 tty_ctl_fd = -1;

/* True when the path names our virtual terminal device. Compares
 * without trusting NUL beyond user range (path was pre-validated). */
static bool path_is_dev_tty(const char* p)
{
    if (!p) return false;
    if (p[0]=='/' && p[1]=='d' && p[2]=='e' && p[3]=='v' && p[4]=='/') {
        const char* n = p + 5;
        return (n[0]=='t' && n[1]=='t' && n[2]=='y' && !n[3]) ||
               (n[0]=='c' && n[1]=='o' && n[2]=='n' && n[3]=='s' &&
                n[4]=='o' && n[5]=='l' && n[6]=='e' && !n[7]);
    }
    return false;
}

/* ── dirfd → path table (#apk-dirfd) ──────────────────────────────
 * apk-tools opens "/" once and routes ALL its I/O through *at()
 * syscalls with that dirfd (openat/mkdirat/fstatat). Our ramfs only
 * knows full paths, so remember the path each fs_fd was opened with
 * and rebuild the full path for *at() calls. */
#define FSFD_PATH_MAX 128
static char g_fsfd_path[FSFD_PATH_MAX][160];
static bool g_fsfd_path_used[FSFD_PATH_MAX];

static void fsfd_path_record(s32 fs_fd, const char* path) {
    if (fs_fd < 0 || fs_fd >= FSFD_PATH_MAX || !path) return;
    u64 i = 0;
    while (path[i] && i < sizeof(g_fsfd_path[0]) - 1) {
        g_fsfd_path[fs_fd][i] = path[i]; i++;
    }
    g_fsfd_path[fs_fd][i] = 0;
    g_fsfd_path_used[fs_fd] = true;
}

static void fsfd_path_clear(s32 fs_fd) {
    if (fs_fd < 0 || fs_fd >= FSFD_PATH_MAX) return;
    g_fsfd_path_used[fs_fd] = false;
}

/* dirfd + relative user path → full kernel path in `out`.
 * Returns false (caller answers -EBADF/-EFAULT). */
static bool fsfd_path_join(s32 dir_fd, u64 rel_u, char* out, u64 cap) {
    const char* rel = (const char*)rel_u;
    if (!user_range_ok(rel_u, 1)) return false;
    u64 rl = 0;
    while (rel[rl] && rl < 256) rl++;
    if (!user_range_ok(rel_u, rl + 1)) return false;
    s32 fs_fd = proc_fd_get((u64)dir_fd);
    if (fs_fd < 0 || fs_fd >= FSFD_PATH_MAX ||
        !g_fsfd_path_used[fs_fd]) return false;
    u64 o = 0;
    const char* base = g_fsfd_path[fs_fd];
    while (base[o] && o < cap - 1) { out[o] = base[o]; o++; }
    if (o > 0 && out[o - 1] != '/') { if (o < cap - 1) out[o++] = '/'; }
    u64 j = (rel[0] == '/') ? 1 : 0;
    while (rel[j] && o < cap - 1) out[o++] = rel[j++];
    out[o] = 0;
    return o > 0;
}

/* open with a kernel-readable path (no user validation) */
static u64 open_full(const char* path, u64 flags)
{
    /* /dev/tty and /dev/console are THE console (no ramfs entry):
     * BusyBox ash refuses interactive job control when this open
     * fails, so it must succeed as a first-class device node. */
    if (path_is_dev_tty(path)) {
        s32 kfd = proc_fd_console_open();
        if (kfd < 0) return (u64)(s64)(-24);          /* EMFILE   */
        {
            /* Opening the controlling terminal claims our session
             * leader pid (used by ^Z delivery to skip the shell). */
            task_t* cur = task_get_current();
            if (cur && cur->pid > 0)
                tty_note_session_leader(cur->pid);
        }
        return (u64)kfd;
    }

    u32 fs_flags = FS_READ;
    u64 acc = flags & LIN_O_ACCMODE;
    if (acc == 1)      { fs_flags = FS_WRITE; }
    else if (acc == 2) { fs_flags = FS_READ | FS_WRITE; }
    if (flags & LIN_O_CREAT) {
        fs_flags |= FS_CREATE | FS_WRITE;
    }

    s32 fs_fd = fs_open(path, fs_flags);
    if (fs_fd < 0) {
        serial_printf("[OPEN] '%s' FAIL fd=%d\n", path, fs_fd);
        /* honest errno: (u64)-1 would decode as errno=1 (EPERM) in
         * musl and mislead apk ("lock database: Operation not
         * permitted"); the common failure here is a missing path. */
        return (u64)(s64)(-LINUX_ENOENT);
    }
    serial_printf("[OPEN] '%s' fd=%d fl=%lx\n", path, fs_fd,
                  (unsigned long)flags);
    fsfd_path_record(fs_fd, path);

    s32 kfd = proc_fd_alloc(fs_fd);
    if (kfd < 0) {
        fs_close((u32)fs_fd);
        fsfd_path_clear(fs_fd);
        return (u64)(s64)(-1);
    }
    return (u64)kfd;
}

static u64 sys_open_impl(u64 path_u, u64 flags, u64 mode)
{
    (void)mode;
    const char* path = (const char*)path_u;

    if (!user_range_ok(path_u, 1)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }
    u64 plen = 0;
    while (path[plen] && plen < 256) plen++;
    if (!user_range_ok(path_u, plen + 1)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }
    return open_full(path, flags);
}

// ---- Linux close(3) ---------------------------------------------

static u64 sys_close_impl(u64 fd)
{
    /* POSIX: ANY used slot may be closed, stdio included. The shell
     * shuffles stdio constantly (close(0)+open("/dev/null") dance in
     * ash setjobctl); refusing it broke interactive job control. */
    if (fd >= PROC_FD_MAX || !proc_fds[fd].used) {
        return (u64)(s64)(-LINUX_EBADF);
    }
    if (is_pipe_fd(fd)) {
        bool wr_end = false;
        s32 pidx = proc_fd_to_pipe(fd, &wr_end);
        pipe_ref_dec(pidx, wr_end);
        proc_fds[fd].used = false;
        proc_fds[fd].fs_fd = -1;
        return 0;
    }
    if (is_unix_fd(fd)) {
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx >= 0) unix_ref_dec(uidx, end0);
        proc_fds[fd].used = false;
        proc_fds[fd].fs_fd = -1;
        return 0;
    }
    if (fd_is_efd_enc(proc_fds[fd].fs_fd) ||
        fd_is_tfd_enc(proc_fds[fd].fs_fd) ||
        fd_is_mfd_enc(proc_fds[fd].fs_fd) ||
        fd_is_sfd_enc(proc_fds[fd].fs_fd)) {
        wl_obj_ref_dec(proc_fds[fd].fs_fd);
        proc_fds[fd].used = false;
        proc_fds[fd].fs_fd = -1;
        return 0;
    }
    if (proc_fds[fd].fs_fd <= FD_EPOLL_BASE) {
        /* closing the epoll fd frees the instance (POSIX) */
        s32 ei = fd_epoll_index(proc_fds[fd].fs_fd);
        if (ei >= 0 && ei < MAX_EPINST) g_eps[ei].used = false;
        proc_fds[fd].used = false;
        proc_fds[fd].fs_fd = -1;
        return 0;
    }
    if (is_net_fd(fd)) {
        s32 idx = fd_net_index(proc_fds[fd].fs_fd);
        if (idx >= 0) {
            extern void net_socket_close_tcp(int);
            extern int  net_socket_type(int);
            if ((net_socket_type(idx) & 0xFF) == SOCK_STREAM) {
                net_socket_close_tcp(idx);     /* graceful FIN         */
            }
            extern void net_socket_free(int);
            net_socket_free(idx);
        }
    }
    if (proc_fds[fd].fs_fd >= 0) {
        fs_close((u32)proc_fds[fd].fs_fd);
        fsfd_path_clear(proc_fds[fd].fs_fd);
    }
    proc_fds[fd].used = false;
    proc_fds[fd].fs_fd = -1;
    return 0;
}

// ---- Linux mmap(9) stub -----------------------------------------
// Anonymous-memory-only: carves zeroed pages from a bump region at
// 512 MB. Enough for musl's early TLS. Cursor is PER-PROCESS.

#define MMAP_REGION_BASE PROC_MMAP_BASE
#define MMAP_REGION_MAX  PROC_MMAP_MAX

static u64 sys_mmap_impl(u64 addr, u64 len, u64 prot, u64 flags, u64 fd_arg, u64 offset)
{
    serial_printf("[MMAP] addr=%lx len=%lx prot=%lx flags=%lx\n",
                  (unsigned long)addr, (unsigned long)len,
                  (unsigned long)prot, (unsigned long)flags);
    if (len == 0) {
        return (u64)(s64)(-22);               /* -EINVAL */
    }
    u64 size = (len + 0xFFF) & ~0xFFFULL;
    u64 mmap_next = proc_get_mmap();

    #define LIN_MAP_FIXED  0x10
    #define LIN_MAP_SHARED 0x01
    #define LIN_PROT_WRITE 2

    u64 map_flags = VMM_PRESENT | VMM_USER;
    if (prot & 2) map_flags |= VMM_WRITE;     /* PROT_WRITE */

    /* #wl-substrate: fd-backed mapping for memfd pools. The SAME
     * physical frames are mapped into every mapper's address space —
     * that is what makes wl_shm work (client writes pixels, the
     * compositor reads them through its own mapping). */
    if (fd_is_mfd_enc(proc_fds[fd_arg].fs_fd)) {
        mfd_t* m = &g_mfds[fd_mfd_index(proc_fds[fd_arg].fs_fd)];
        if (offset & 0xFFFULL) return (u64)(s64)(-22);   /* page-align */
        u32 first = (u32)(offset / 0x1000);
        u32 np = (u32)(size / 0x1000);
        if ((u64)first + np > m->npages) return (u64)(s64)(-12);
        u64 base = addr;
        if (!(flags & LIN_MAP_FIXED) || base == 0) {
            base = mmap_next;
            mmap_next += size;
            proc_set_mmap(mmap_next);
        } else {
            base &= ~0xFFFULL;
        }
        if (!user_range_ok(base, size)) return (u64)(s64)(-12);
        for (u32 k = 0; k < np; k++) {
            u64 va = base + (u64)k * 0x1000;
            if (flags & LIN_MAP_FIXED) {
                phys_addr_t oldp = vmm_get_phys(va);
                if (oldp) { vmm_unmap(va); pmm_free_page(oldp); }
            }
            vmm_map(va, m->pages[first + k], map_flags);
        }
        serial_printf("[MMAP] memfd off=%lu np=%u ret=%lx\n",
                      (unsigned long)offset, np, (unsigned long)base);
        return base;
    }

    if ((flags & LIN_MAP_FIXED) && addr != 0) {
        /* MAP_FIXED: caller demands a specific address */
        u64 aligned = addr & ~0xFFFULL;
        if (!user_range_ok(aligned, size)) {
            return (u64)(s64)(-12);           /* -ENOMEM */
        }
        for (u64 page = aligned; page < aligned + size; page += 0x1000) {
            /* release any page already backing this VA (POSIX MAP_FIXED
             * semantics: the new mapping REPLACES the old one) */
            phys_addr_t old = vmm_get_phys(page);
            if (old) {
                vmm_unmap(page);
                pmm_free_page(old);
            }
            phys_addr_t phys = pmm_alloc_page();
            if (phys == 0) return (u64)(s64)(-12);
            vmm_map(page, phys, map_flags);
            {
                extern u64 vmm_pa_read_begin(void);
                extern void vmm_pa_read_end(u64);
                u64 s3 = vmm_pa_read_begin();
                kmemset((void*)PHYS_TO_VIRT(phys), 0, 0x1000);
                vmm_pa_read_end(s3);
            }
        }
        serial_printf("[MMAP] fixed ret=%lx\n", (unsigned long)aligned);
        return aligned;
    }

    /* Anonymous bump allocation from mmap region */
    if (mmap_next + size > MMAP_REGION_MAX) {
        return (u64)(s64)(-12);               /* -ENOMEM */
    }

    u64 base = mmap_next;
    for (u64 page = base; page < base + size; page += 0x1000) {
        phys_addr_t phys = pmm_alloc_page();
        if (phys == 0) {
            return (u64)(s64)(-12);           /* -ENOMEM */
        }
        vmm_map(page, phys, map_flags);
        {
            extern u64 vmm_pa_read_begin(void);
            extern void vmm_pa_read_end(u64);
            u64 s3 = vmm_pa_read_begin();
            kmemset((void*)PHYS_TO_VIRT(phys), 0, 0x1000);
            vmm_pa_read_end(s3);
        }
    }

    mmap_next += size;
    proc_set_mmap(mmap_next);
    serial_printf("[MMAP] ret=%lx\n", (unsigned long)base);
    return base;
}

// ---- Linux munmap(11) -------------------------------------------

static u64 sys_munmap_impl(u64 addr, u64 len)
{
    if (addr == 0 || len == 0) return (u64)(s64)(-22);
    if (!user_range_ok(addr, len)) return (u64)(s64)(-LINUX_EFAULT);

    u64 start = addr & ~0xFFFULL;
    u64 end   = (addr + len + 0xFFF) & ~0xFFFULL;

    for (u64 page = start; page < end; page += 0x1000) {
        phys_addr_t phys = vmm_get_phys(page);
        if (phys != 0) {
            vmm_unmap(page);
            pmm_free_page(phys);
        }
    }
    return 0;
}

// ---- Linux readv(19) ---------------------------------------------

static u64 sys_readv_impl(u64 fd, u64 iov_u, u64 iovcnt)
{
    if (iovcnt == 0 || iovcnt > 1024)
        return (u64)(s64)(-22);
    if (!user_range_ok(iov_u, (u64)iovcnt * 16))
        return (u64)(s64)(-LINUX_EFAULT);

    u64 total = 0;
    for (int i = 0; i < (int)iovcnt; i++) {
        u64 base = *(u64*)(iov_u + (u64)i * 16);
        u64 len  = *(u64*)(iov_u + (u64)i * 16 + 8);
        if (len == 0) continue;

        u64 r = sys_read_impl(fd, base, len);
        if ((s64)r < 0) {
            return (total > 0) ? total : r;
        }
        total += r;
        if ((s64)r < (s64)len) break;          /* short read → done */
    }
    return total;
}

// ---- Linux getpid(39) ---------------------------------------------

static u64 sys_getpid_impl(void)
{
    s32 pid = sched_current_pid();
    return (pid > 0) ? (u64)pid : 1;
}

// ---- Linux clock_gettime(228) -------------------------------------

typedef struct __attribute__((packed)) {
    s64 tv_sec;
    s64 tv_nsec;
} timespec_t;

static u64 sys_clock_gettime_impl(u64 clk_id, u64 tp_user)
{
    if (clk_id != 0 && clk_id != 1) return (u64)(s64)(-22);
    if (!user_range_ok(tp_user, sizeof(timespec_t))) return (u64)(s64)(-LINUX_EFAULT);

    timespec_t* tp = (timespec_t*)tp_user;
    u64 ticks = timer_get_ticks();
    tp->tv_sec  = (s64)(ticks / TIMER_FREQ);
    tp->tv_nsec = (s64)(((ticks % TIMER_FREQ) * 1000000000ULL) / TIMER_FREQ);
    return 0;
}

/* ---- nanosleep(35): relative and absolute; only CLOCK_REALTIME(0) /
 * CLOCK_MONOTONIC(1) matter here. Blocking uses the cooperative
 * scheduler sleep (same mechanism task_sleep_ms gives kernel code):
 * the task is parked, its stop_sig state is untouched, so a ^Z'd child
 * that is currently sleeping still reports through wait4(WUNTRACED)
 * exactly like a running one. --------------------------------------*/
static u64 sys_nanosleep_impl(u64 req_user, u64 rem_user)
{
    if (!user_range_ok(req_user, sizeof(timespec_t)))
        return (u64)(s64)(-LINUX_EFAULT);
    timespec_t* rq = (timespec_t*)req_user;
    s64 sec = rq->tv_sec;
    s64 nsec = rq->tv_nsec;
    if (sec < 0 || nsec < 0 || nsec >= 1000000000LL) {
        return (u64)(s64)(-22);               /* -EINVAL           */
    }
    u64 ms = (u64)sec * 1000ULL + (u64)nsec / 1000000ULL;
    if (ms == 0) ms = 1;                      /* granularity floor */
    if (ms > 3600000ULL) ms = 3600000ULL;     /* clamp: 1h         */
    task_sleep_ms(ms);
    if (rem_user && user_range_ok(rem_user, sizeof(timespec_t))) {
        timespec_t* rr = (timespec_t*)rem_user;
        rr->tv_sec = 0; rr->tv_nsec = 0;      /* full sleep done   */
    }
    return 0;
}

/* ---- Linux dup(32) / dup2(33) ------------------------------------*/

static u64 sys_dup_impl(u64 oldfd)
{
    if (oldfd >= PROC_FD_MAX || !proc_fds[oldfd].used)
        return (u64)(s64)(-LINUX_EBADF);
    s32 val = proc_fds[oldfd].fs_fd;
    if (val >= 0) {
        extern s32 fs_dup(u32);                /* shared description   */
        val = fs_dup((u32)val);
        if (val < 0) return (u64)(s64)(-LINUX_EBADF);
    } else if (fd_is_pipe_enc(val)) {
        pipe_ref_inc(fd_pipe_index(val), fd_pipe_is_write(val));
    } else if (val <= FD_NET_BASE) {
        extern int net_socket_dup(int);
        if (net_socket_dup(fd_net_index(val)) != 0)
            return (u64)(s64)(-LINUX_EBADF);
    }
    s32 newfd = proc_fd_alloc(proc_fds[oldfd].fs_fd);
    if (newfd < 0) {
        s32 orig = proc_fds[oldfd].fs_fd;      /* rollback the +ref    */
        if (orig >= 0) {
            extern s32 fs_close(u32);
            fs_close((u32)orig);
        } else if (fd_is_pipe_enc(orig)) {
            pipe_ref_dec(fd_pipe_index(orig), fd_pipe_is_write(orig));
        } else if (orig <= FD_NET_BASE) {
            extern void net_socket_free(int);
            net_socket_free(fd_net_index(orig));
        }
        return (u64)(s64)(-LINUX_EBADF);
    }
    return (u64)newfd;
}

static u64 sys_dup2_impl(u64 oldfd, u64 newfd)
{
    if (oldfd >= PROC_FD_MAX || !proc_fds[oldfd].used)
        return (u64)(s64)(-LINUX_EBADF);
    if (newfd >= PROC_FD_MAX) return (u64)(s64)(-LINUX_EBADF);
    if (oldfd == newfd) return newfd;
    /* #dup2-oldslot-leak FIX: the target slot's EXISTING reference
     * must be released for EVERY class, not only regular files. The
     * old code fs_close()d fs_fd>=0 but silently DROPPED pipe/net/
     * unix encodings — busybox ash's `dup2(pipeW, 1)` inside a $( )
     * substitution overwrote fd1 that still held the substitution
     * pipe's writer ref, so writers never reached 0, the parent's
     * read never saw EOF and every command substitution hung the
     * shell forever (seen as the final w=1 in [PINC]/[PDEC]). */
    if (proc_fds[newfd].used)
        fdref_dec_enc(proc_fds[newfd].fs_fd);
    /* alias WITH refcount: file handles via fs, sockets/pipes via own layers */
    s32 val = proc_fds[oldfd].fs_fd;
    if (val >= 0) {
        extern s32 fs_dup(u32);
        val = fs_dup((u32)val);
        if (val < 0) return (u64)(s64)(-LINUX_EBADF);
    } else if (fd_is_pipe_enc(val)) {
        pipe_ref_inc(fd_pipe_index(val), fd_pipe_is_write(val));
    } else if (val <= FD_NET_BASE) {
        extern int net_socket_dup(int);
        if (net_socket_dup(fd_net_index(val)) != 0)
            return (u64)(s64)(-LINUX_EBADF);
    }
    proc_fds[newfd].used = true;
    proc_fds[newfd].fs_fd = val;
    return newfd;
}

/* ---- Linux pipe(22) / pipe2(293) -----------------------------------
 * int pipe2(int pipefd[2], int flags);
 * pipefd[0] = read end, pipefd[1] = write end. flags ignored
 * (O_CLOEXEC has nothing to strip from — no execve fd closing yet). */
static u64 sys_pipe2_impl(u64 fds_u, u64 flags)
{
    (void)flags;
    if (!user_range_ok(fds_u, 8)) return (u64)(s64)(-LINUX_EFAULT);

    s32 idx = pipe_alloc();
    if (idx < 0) return (u64)(s64)(-23);          /* ENFILE             */

    s32 rfd = proc_fd_alloc(FD_PIPE_BASE - 2 * idx);            /* read  */
    s32 wfd = (rfd >= 0) ? proc_fd_alloc(FD_PIPE_BASE - 2 * idx - 1)
                         : -1;                                  /* write */
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) { proc_fds[rfd].used = false; proc_fds[rfd].fs_fd = -1; }
        if (wfd >= 0) { proc_fds[wfd].used = false; proc_fds[wfd].fs_fd = -1; }
        g_pipes[idx].used = false;
        return (u64)(s64)(-24);                   /* EMFILE             */
    }
    g_pipes[idx].readers = 1;
    g_pipes[idx].writers = 1;

    s32 pair[2] = { rfd, wfd };
    kmemcpy((void*)fds_u, pair, sizeof(pair));
    serial_printf("[PIPE] rfd=%d wfd=%d\n", rfd, wfd);
    return 0;
}

/* ---- Linux fork(57)/execve(59)/wait4(61) --------------------------*/
/* Full implementations on top of the process layer (proc.c):
 *   fork:   COW-clone of the address space + copied trap frame;
 *           the child is sync-run to completion, parent gets its pid.
 *   execve: fresh address space + ELF; the live trap frame is patched
 *           so this syscall "returns" into the new program.
 *   wait4:  reaps zombie children (status = code << 8).                */

static u64 sys_fork_impl(void) { return (u64)(s64)proc_do_fork(); }

static u64 sys_execve_impl(u64 path_u, u64 argv_u, u64 envp_u)
{
    (void)envp_u;
    return (u64)(s64)proc_do_execve((const char*)path_u,
                                    (char* const*)argv_u);
}

static u64 sys_wait4_impl(u64 pid, u64 wstatus, u64 options, u64 rusage)
{
    (void)rusage;
    bool nohang    = (options & 1) != 0;  /* WNOHANG                 */
    bool wuntraced = (options & 2) != 0;  /* WUNTRACED: job control  */
    return (u64)(s64)proc_do_wait4((s32)(s64)pid, wstatus,
                                   nohang, wuntraced);
}

/* ---- Linux setsockopt(54) / getsockopt(55) stubs --------------------*/

static u64 sys_setsockopt_impl(u64 fd, u64 level, u64 optname,
                               u64 optval, u64 optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

/* ---- BusyBox/musl support syscalls ---------------------------------*/

/* arch_prctl(158): musl sets FS base for TLS (stack canary etc.) */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

static u64 sys_arch_prctl_impl(u64 code, u64 addr)
{
    extern void* scheduler_task_table(void);
    switch (code) {
    case ARCH_SET_FS:
        wrmsr(0xC0000100, addr);          /* MSR_FS_BASE            */
        /* record for per-task restore at every switch-in
         * (#fs-base-no-switch: the MSR persists across processes and
         * a stale base points into the PREVIOUS image's TLS) */
        {
            extern task_t* task_get_current(void);
            task_t* t = task_get_current();
            if (t) t->fs_base = addr;
        }
        return 0;
    case ARCH_SET_GS:
        wrmsr(0xC0000102, addr);          /* MSR_GS_BASE            */
        return 0;
    case ARCH_GET_FS:
        if (!user_range_ok(addr, 8)) return (u64)(s64)(-LINUX_EFAULT);
        *(u64*)addr = rdmsr(0xC0000100);
        return 0;
    case ARCH_GET_GS:
        if (!user_range_ok(addr, 8)) return (u64)(s64)(-LINUX_EFAULT);
        *(u64*)addr = rdmsr(0xC0000102);
        return 0;
    default:
        return (u64)(s64)(-22);           /* -EINVAL                */
    }
}

/* openat(257): musl routes open() here. AT_FDCWD or a real dirfd
 * (#apk-dirfd: apk opens "/" once and does everything via openat). */
#define AT_FDCWD -100
static u64 sys_openat_impl(u64 dirfd, u64 path_u, u64 flags, u64 mode)
{
    static char full[256];
    if ((s64)dirfd == AT_FDCWD)
        return sys_open_impl(path_u, flags, mode);
    if (!fsfd_path_join((s32)(s64)dirfd, path_u, full, sizeof(full)))
        return (u64)(s64)(-9);            /* -EBADF */
    return open_full(full, flags);
}

/* mkdir(83) / mkdirat(258): ramfs directory creation. apk --initdb
 * builds /etc/apk/{keys,db,installed.db}... via mkdirat(AT_FDCWD,...). */
static u64 mkdir_full(const char* path)
{
    s32 r = fs_mkdir(path);
    if (r != FS_OK)
        return (u64)(s64)((r == FS_ERR_EXISTS) ? -17 : -2); /* EEXIST/ENOENT */
    return 0;
}

static u64 sys_mkdir_impl(u64 path_u, u64 mode)
{
    (void)mode;
    const char* path = (const char*)path_u;
    if (!user_range_ok(path_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
    u64 plen = 0;
    while (path[plen] && plen < 256) plen++;
    if (!user_range_ok(path_u, plen + 1)) return (u64)(s64)(-LINUX_EFAULT);
    return mkdir_full(path);
}

static u64 sys_mkdirat_impl(u64 dirfd, u64 path_u, u64 mode)
{
    static char full[256];
    if ((s64)dirfd == AT_FDCWD)
        return sys_mkdir_impl(path_u, mode);
    if (!fsfd_path_join((s32)(s64)dirfd, path_u, full, sizeof(full)))
        return (u64)(s64)(-9);            /* -EBADF */
    return mkdir_full(full);
}

/* mknod(133) / mknodat(259): apk_db_create() seeds /dev/{null,zero,
 * tty,random,urandom} through mknodat. We materialize them as EMPTY
 * regular files — the console paths get their device semantics via
 * path_is_dev_tty in open/stat, the rest are good enough as files. */
static u64 mknod_full(const char* path)
{
    s32 fd = fs_open(path, FS_CREATE | FS_WRITE);
    if (fd < 0) return (u64)(s64)(-LINUX_ENOENT);
    fsfd_path_record(fd, path);
    fs_close((u32)fd);
    fsfd_path_clear(fd);
    return 0;
}

static u64 sys_mknodat_impl(u64 dirfd, u64 path_u, u64 mode, u64 dev)
{
    static char full[256];
    (void)mode; (void)dev;
    if ((s64)dirfd == AT_FDCWD) {
        const char* path = (const char*)path_u;
        if (!user_range_ok(path_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
        u64 plen = 0;
        while (path[plen] && plen < 256) plen++;
        if (!user_range_ok(path_u, plen + 1)) return (u64)(s64)(-LINUX_EFAULT);
        return mknod_full(path);
    }
    if (!fsfd_path_join((s32)(s64)dirfd, path_u, full, sizeof(full)))
        return (u64)(s64)(-9);
    return mknod_full(full);
}

/* unlink(87) / unlinkat(263) / rename(82) / renameat(264): the cache
 * commit dance of apk (write .apknew.tmp -> rename over the final
 * name) needs real unlink/rename on the ramfs. */
static u64 sys_unlinkat_impl(u64 dirfd, u64 path_u, u64 flags)
{
    static char full[256];
    const char* path;
    if ((s64)dirfd == AT_FDCWD) {
        path = (const char*)path_u;
        if (!user_range_ok(path_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
        u64 plen = 0;
        while (path[plen] && plen < 256) plen++;
        if (!user_range_ok(path_u, plen + 1))
            return (u64)(s64)(-LINUX_EFAULT);
    } else {
        if (!fsfd_path_join((s32)(s64)dirfd, path_u, full, sizeof(full)))
            return (u64)(s64)(-9);
        path = full;
    }
    s32 r = (flags & 0x200) ? fs_rmdir(path) : fs_delete_file(path);
    return (r == FS_OK) ? 0 : (u64)(s64)(-LINUX_ENOENT);
}

static u64 sys_renameat_impl(u64 oldfd, u64 old_u, u64 newfd, u64 new_u)
{
    static char oldfull[256], newfull[256];
    const char* op; const char* np;
    if ((s64)oldfd == AT_FDCWD) {
        op = (const char*)old_u;
        if (!user_range_ok(old_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
        u64 plen = 0;
        while (op[plen] && plen < 256) plen++;
        if (!user_range_ok(old_u, plen + 1))
            return (u64)(s64)(-LINUX_EFAULT);
    } else {
        if (!fsfd_path_join((s32)(s64)oldfd, old_u, oldfull,
                            sizeof(oldfull)))
            return (u64)(s64)(-9);
        op = oldfull;
    }
    if ((s64)newfd == AT_FDCWD) {
        np = (const char*)new_u;
        if (!user_range_ok(new_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
        u64 plen = 0;
        while (np[plen] && plen < 256) plen++;
        if (!user_range_ok(new_u, plen + 1))
            return (u64)(s64)(-LINUX_EFAULT);
    } else {
        if (!fsfd_path_join((s32)(s64)newfd, new_u, newfull,
                            sizeof(newfull)))
            return (u64)(s64)(-9);
        np = newfull;
    }
    s32 r = fs_rename(op, np);
    return (r == FS_OK) ? 0 : (u64)(s64)(-LINUX_ENOENT);
}

/* symlinkat(266): #fs-symlink — apk packages install shared-library
 * soname links (libz.so.1 -> libz.so.1.3.1) through this. Args:
 * (target, newdirfd, linkpath). */
static u64 sys_symlinkat_impl(u64 target_u, u64 newfd, u64 linkpath_u)
{
    static char full[256];
    char target[160];
    if (!user_range_ok(target_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
    {
        const char* t = (const char*)target_u;
        u64 plen = 0;
        while (t[plen] && plen < sizeof(target) - 1) plen++;
        if (t[plen]) return (u64)(s64)(-LINUX_ENAMETOOLONG);
        if (!user_range_ok(target_u, plen + 1))
            return (u64)(s64)(-LINUX_EFAULT);
        kmemcpy(target, t, plen + 1);
    }
    const char* linkpath;
    if ((s64)newfd == AT_FDCWD) {
        linkpath = (const char*)linkpath_u;
        if (!user_range_ok(linkpath_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
        u64 plen = 0;
        while (linkpath[plen] && plen < 256) plen++;
        if (!user_range_ok(linkpath_u, plen + 1))
            return (u64)(s64)(-LINUX_EFAULT);
    } else {
        if (!fsfd_path_join((s32)(s64)newfd, linkpath_u, full,
                            sizeof(full)))
            return (u64)(s64)(-9);
        linkpath = full;
    }
    s32 r = fs_symlink(target, linkpath);
    switch (r) {
        case FS_OK:            return 0;
        case FS_ERR_EXISTS:    return (u64)(s64)(-LINUX_EEXIST);
        case FS_ERR_NOTDIR:    return (u64)(s64)(-LINUX_ENOTDIR);
        default:               return (u64)(s64)(-LINUX_ENOENT);
    }
}

/* readlinkat(267) / readlink(89): copy the raw target text out */
static u64 sys_readlinkat_impl(u64 dirfd, u64 path_u, u64 buf, u64 bufsiz)
{
    static char full[256];
    const char* path;
    if ((s64)dirfd == AT_FDCWD) {
        path = (const char*)path_u;
        if (!user_range_ok(path_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
        u64 plen = 0;
        while (path[plen] && plen < 256) plen++;
        if (!user_range_ok(path_u, plen + 1))
            return (u64)(s64)(-LINUX_EFAULT);
    } else {
        if (!fsfd_path_join((s32)(s64)dirfd, path_u, full, sizeof(full)))
            return (u64)(s64)(-9);
        path = full;
    }
    fs_node_t* n = fs_resolve_path(path);        /* NO follow */
    if (!n) return (u64)(s64)(-LINUX_ENOENT);
    if (n->type != FS_FILE_TYPE_SYMLINK)
        return (u64)(s64)(-LINUX_EINVAL);
    u64 tl = kstrlen(n->link_target);
    u64 copy = (bufsiz < tl) ? bufsiz : tl;
    if (copy && !user_range_ok(buf, copy))
        return (u64)(s64)(-LINUX_EFAULT);
    if (copy) kmemcpy((void*)buf, n->link_target, copy);
    return copy;
}

/* gettimeofday(96): boot-relative clock (PIT ticks). Honest zeroes
 * base; consumers here only need monotonic deltas (libfetch timeouts). */
static u64 sys_gettimeofday_impl(u64 tv_user, u64 tz_user)
{
    (void)tz_user;
    if (!tv_user) return 0;
    if (!user_range_ok(tv_user, 16)) return (u64)(s64)(-LINUX_EFAULT);
    u64 ticks = timer_get_ticks();
    *(s64*)tv_user       = (s64)(ticks / TIMER_FREQ);
    *(s64*)(tv_user + 8) = (s64)(((ticks % TIMER_FREQ) * 1000000ULL)
                                 / TIMER_FREQ);
    return 0;
}

/* lseek(8) */
static u64 sys_lseek_impl(u64 fd, s64 off, u64 whence)
{
    s32 fs_fd = proc_fd_get(fd);
    if (fs_fd < 0) {
        /* console sentinels: historical noop-seek, keep succeeding */
        if (fd <= 2) return 0;
        return (u64)(s64)(-LINUX_EBADF);
    }
    s64 r = fs_seek((u32)fs_fd, off, (u32)whence);
    return (r < 0) ? (u64)(s64)-1 : (u64)r;
}

/* forward decl (defined below) */
static u64 sys_stat_impl(u64 path_u, u64 statbuf);
static u64 stat_full_ex(const char* path, u64 statbuf, bool follow);

/* newfstatat(262): stat via path (AT_FDCWD or dirfd + relative) */
static u64 sys_newfstatat_impl(u64 dirfd, u64 path_u, u64 statbuf, u64 flags)
{
    static char full[256];
    bool nofollow = (flags & 0x100) != 0;   /* AT_SYMLINK_NOFOLLOW */
    if ((s64)dirfd == AT_FDCWD) {
        if (!user_range_ok(path_u, 1)) return (u64)(s64)(-LINUX_EFAULT);
        const char* path = (const char*)path_u;
        u64 plen = 0;
        while (path[plen] && plen < 256) plen++;
        if (!user_range_ok(path_u, plen + 1))
            return (u64)(s64)(-LINUX_EFAULT);
        return stat_full_ex(path, statbuf, !nofollow);
    }
    if (!fsfd_path_join((s32)(s64)dirfd, path_u, full, sizeof(full)))
        return (u64)(s64)(-9);            /* -EBADF */
    return stat_full_ex(full, statbuf, !nofollow);
}

/* uname(63) */
struct utsname_fake { char sysname[65], nodename[65], release[65],
                      version[65], machine[65], domain[65]; };

static u64 sys_uname_impl(u64 buf_u)
{
    if (!user_range_ok(buf_u, sizeof(struct utsname_fake)))
        return (u64)(s64)(-LINUX_EFAULT);
    struct utsname_fake* u = (struct utsname_fake*)buf_u;
    kmemset(u, 0, sizeof(*u));
    kstrcpy(u->sysname,  "NullOs");
    kstrcpy(u->nodename, "nullos");
    kstrcpy(u->release,  "6.1.0-nullos");
    kstrcpy(u->version,  "#1 SMP NullOs");
    kstrcpy(u->machine,  "x86_64");
    return 0;
}

/* getdents64(217) over ramfs directories */
static u64 sys_getdents64_impl(u64 fd, u64 buf, u64 len)
{
    s32 fs_fd = proc_fd_get(fd);
    if (fs_fd < 0) return (u64)(s64)(-LINUX_EBADF);
    if (!user_range_ok(buf, len)) return (u64)(s64)(-LINUX_EFAULT);
    s64 r = fs_getdents64((u32)fs_fd, (void*)buf, len);
    return (r < 0) ? (u64)(s64)-1 : (u64)r;
}

/* getppid(110) */
static u64 sys_getppid_impl(void)
{
    task_t* t = task_get_current();
    return (t && t->pid > 0) ? (u64)t->ppid : 0;
}

static u64 sys_getsockopt_impl(u64 fd, u64 level, u64 optname,
                               u64 optval, u64 optlen_ptr)
{
    (void)fd; (void)level; (void)optname;
    if (optval && user_range_ok(optval, 4)) *(u32*)optval = 0;
    if (optlen_ptr && user_range_ok(optlen_ptr, 4)) *(u32*)optlen_ptr = 4;
    return 0;
}

/* ---- musl startup stubs -------------------------------------------*/

static u64 sys_set_tid_address_impl(u64 tidptr) { (void)tidptr; return 1; }

// ---- Linux stat(4) / fstat(5) / access(21) ------------------------
// struct stat layout for x86_64 musl (~144 bytes)

static void fill_fake_stat(void* statbuf, u32 size, u32 mode)
{
    kmemset(statbuf, 0, 144);
    *(u64*)((u8*)statbuf + 0)  = 1;              /* st_dev     */
    *(u64*)((u8*)statbuf + 8)  = 1;              /* st_ino     */
    *(u64*)(statbuf + 16)       = 1;             /* st_nlink   */
    *(u32*)(statbuf + 24)       = mode;           /* st_mode    */
    *(u32*)(statbuf + 28)       = 0;              /* st_uid     */
    *(u32*)(statbuf + 32)       = 0;              /* st_gid     */
    *(u64*)(statbuf + 48)       = size;           /* st_size    */
    *(u64*)(statbuf + 56)       = 512;             /* st_blksize */
    *(u64*)(statbuf + 64)       = (size + 511) / 512; /* st_blocks */
}

/* musl/LSB st_mode fragments we report through the fake-stat shim */
#define STAT_MODE_REG 0x81A4u   /* S_IFREG | 0644 */
#define STAT_MODE_DIR 0x41EDu   /* S_IFDIR | 0755 */
#define STAT_MODE_CHR 0x2192u   /* S_IFCHR | 0620 (console/tty)   */
#define STAT_MODE_LNK 0xA1FFu   /* S_IFLNK | 0777 (#fs-symlink)   */

static u64 stat_full(const char* path, u64 statbuf)
{
    return stat_full_ex(path, statbuf, true);   /* follow by default */
}

/* #fs-symlink: nofollow variant for lstat()/newfstatat(AT_SYMLINK_NOFOLLOW).
 * A symlink reports S_IFLNK and st_size = target length (POSIX). */
static u64 stat_full_ex(const char* path, u64 statbuf, bool follow)
{
    if (!user_range_ok(statbuf, 144)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }

    /* Virtual console devices live outside the ramfs node table:
     * before this branch existed they stat'ed as fake regular files
     * (always-success shim); with the honest -ENOENT below BusyBox
     * ash degrades at startup (its tty probes rely on stat first)
     * and interactive job control breaks. Report a real character
     * device, matching the open() side of the same device. */
    if (path_is_dev_tty(path)) {
        fill_fake_stat((void*)statbuf, 0, STAT_MODE_CHR);
        return 0;
    }

    fs_node_t* n = follow ? fs_resolve_path_follow(path)
                          : fs_resolve_path(path);
    if (!n) return (u64)(s64)(-LINUX_ENOENT);

    u32 mode; u32 sz;
    if (n->type == FS_FILE_TYPE_DIR) {
        mode = STAT_MODE_DIR;                 /* dirs report canonical size */
        sz   = 4096;
    } else if (n->type == FS_FILE_TYPE_SYMLINK) {
        mode = STAT_MODE_LNK;
        sz   = kstrlen(n->link_target);
    } else {
        mode = STAT_MODE_REG;
        /* size probe via open/get_size honours backend nodes (devfs/fat) */
        s32 fd = fs_open(path, FS_READ);
        if (fd >= 0) {
            extern u32 fs_get_size_by_fd(u32 fd);
            sz = fs_get_size_by_fd(fd);
            fs_close(fd);
        } else sz = n->size;
    }
    fill_fake_stat((void*)statbuf, sz, mode);
    return 0;
}

static u64 sys_stat_impl(u64 path_u, u64 statbuf)
{
    if (!user_range_ok(path_u, 1)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }
    u64 plen = 0;
    const char* path = (const char*)path_u;
    while (path[plen] && plen < 256) plen++;
    if (!user_range_ok(path_u, plen + 1)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }
    return stat_full(path, statbuf);
}

static u64 sys_fstat_impl(u64 fd, u64 statbuf)
{
    if (!user_range_ok(statbuf, 144)) return (u64)(s64)(-LINUX_EFAULT);
    s32 kfd = proc_fd_get(fd);
    if (kfd < 0) {
        /* console stdio / sentinel tty fds are character devices;
         * blksize stays the historical 4096 (musl sizes its stdout
         * buffer from this: 512 correlated with a mid-flood crash
         * inside vfprintf's movaps path, kept as bisect anchor).
         * #stdio-redirect-fd: REAL files redirected onto stdio take
         * the fs path below now — the old `fd <= 2` force-field
         * reported char-device for redirected regular files. */
        fill_fake_stat((void*)statbuf, 4096, STAT_MODE_CHR);
        return 0;
    }
    fs_node_t* n = fs_get_node_by_fd((u32)kfd);
    if (!n) { fill_fake_stat((void*)statbuf, 4096, STAT_MODE_CHR); return 0; }

    u32 mode; u32 sz;
    if (n->type == FS_FILE_TYPE_DIR) {
        mode = STAT_MODE_DIR; sz = 4096;
    } else {
        extern u32 fs_get_size_by_fd(u32 fd);
        mode = STAT_MODE_REG; sz = fs_get_size_by_fd((u32)kfd);
    }
    /* HONEST size: the old "round up to 4096" shim made apk's adb
     * reader see st_size=4096 on an EMPTY /etc/apk/world, then the
     * short read hit EOF -> apk IO ERROR -> abort (#apk-empty-read).
     * Consumers must see the real byte count. */
    fill_fake_stat((void*)statbuf, sz, mode);
    return 0;
}

static u64 sys_access_impl(u64 path_u, u64 mode)
{
    (void)path_u; (void)mode;
    return 0;                                     /* always accessible */
}

// ---- Linux writev(20) -------------------------------------------
// ssize_t writev(int fd, const struct iovec* iov, int iovcnt);
// struct iovec { void* base; size_t len; };

static u64 sys_writev_impl(u64 fd, u64 iov_u, u64 iovcnt)
{
    if (iovcnt == 0 || iovcnt > 1024) {
        return (u64)(s64)(-22);               /* -EINVAL */
    }
    if (!user_range_ok(iov_u, (u64)iovcnt * 16)) {
        return (u64)(s64)(-LINUX_EFAULT);
    }

    u64 total = 0;
    for (int i = 0; i < (int)iovcnt; i++) {
        u64 base = *(u64*)(iov_u + (u64)i * 16);
        u64 len  = *(u64*)(iov_u + (u64)i * 16 + 8);
        if (len == 0) continue;
        if (!user_range_ok(base, len)) {
            return (total > 0) ? total : (u64)(s64)(-LINUX_EFAULT);
        }
        u64 r = sys_write_impl(fd, base, len);
        if ((s64)r < 0) {
            return (total > 0) ? total : r;
        }
        total += r;
    }
    return total;
}

// ---- tty state (console = PS/2 keyboard + VGA) --------------------
// Single controlling terminal for the whole system: fd 0/1/2 are the
// console for every process. BusyBox ash's setjobctl() probes exactly
// these ioctls before enabling interactive job control.

#define LINUX_ENOTTY      25
#define LINUX_EPIPE       32

#define LINUX_TCGETS      0x5401
#define LINUX_TCSETS      0x5402
#define LINUX_TCSETSW     0x5403
#define LINUX_TCSETSF     0x5404
#define LINUX_TIOCSCTTY   0x540E
#define LINUX_TIOCGPGRP   0x540F
#define LINUX_TIOCSPGRP   0x5410
#define LINUX_TIOCGWINSZ  0x5413
#define LINUX_TIOCSWINSZ  0x5414
#define LINUX_FIONREAD    0x541B

/* musl struct termios: c_iflag,c_oflag,c_cflag,c_lflag (u32 x4),
 * c_line (u8), c_cc[32], __c_ispeed,__c_ospeed (u32 x2) = 60 bytes */
typedef struct {
    u32 c_iflag, c_oflag, c_cflag, c_lflag;
    u8  c_line;
    u8  c_cc[32];
    u32 c_ispeed, c_ospeed;
} kernel_termios_t;

/* Classic cooked-mode defaults (BRKINT|ICRNL|IMAXBEL|IXON etc.) */
static kernel_termios_t tty_termios = {
    .c_iflag = 0x4500u | 0x0002u,                 /* ICRNL|IMAXBEL|IXON|BRKINT */
    .c_oflag = 0x0005u,                            /* OPOST|ONLCR       */
    .c_cflag = 0x00BFu,                            /* CREAD|CS8|B38400-ish */
    .c_lflag = 0x803Bu,                            /* ISIG|ICANON|ECHO.. */
    .c_line  = 0,
    .c_cc    = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0 }, /* VINTR..VSUSP */
    .c_ispeed = 0x1001u, .c_ospeed = 0x1001u,
};

struct winsize_st { u16 rows, cols, xpix, ypix; };
static struct winsize_st tty_winsize = { 25, 80, 0, 0 };
static s32 tty_fg_pgrp = 0;   /* 0 => "current process's group"     */
static s32 tty_sid    = 0;    /* controlling session leader (the sh)*/

/* --- Partial signal heart (job-control trio only, scheduler-level):
 * SIGSTOP(19)/SIGTSTP(20) park the target by setting task_t.stop_sig;
 * every scheduler pick site skips flagged tasks until SIGCONT(18)
 * clears the flag again. No user handler frames yet. ----------------*/

/* Deliver SIGTSTP to the foreground process group of the console,
 * POSIX-style excluding the session leader itself (^Z at the shell's
 * own prompt must not freeze the shell). Called from keyboard_getchar
 * right after '^Z' (0x1A) was translated; the char is swallowed and
 * never reaches any reader. No-op while job control is disarmed
 * (ash never tcsetpgrp'd anything) or fg group is unknown. */
void tty_on_ctrl_z(void)
{
    if (tty_fg_pgrp <= 0) return;
    extern void* scheduler_task_table(void);
    task_t* tbl = (task_t*)scheduler_task_table();
    if (!tbl) return;
    for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
        task_t* t = &tbl[i];
        if (!t->active || t->pid <= 0) continue;
        s32 eff = (t->pgrp > 0) ? t->pgrp : t->pid;
        if (eff != tty_fg_pgrp) continue;
        if (tty_sid > 0 && t->pid == tty_sid) continue;   /* don't stop sh */
        if (t->stop_sig) continue;                        /* already parked */
        t->stop_sig = 20;
        t->stop_reported = false;
        serial_printf("[TSTP] ctrl-z -> stop pid=%d pgrp=%d\n",
                      t->pid, eff);
    }
}

static void tty_note_session_leader(s32 pid)
{
    /* First claimant wins: busybox ash opens /dev/tty early. */
    if (tty_sid == 0 && pid > 0) {
        tty_sid = pid;
        serial_printf("[TTY] sid=%d\n", pid);
    }
}

static bool fd_is_console(u64 fd)
{
    if (fd <= 2) return true;      /* stdio reserved for console */
    /* Console-sentinel family: dup'd stdio slots (-2..-4), virtual
     * /dev/tty handles. Values <= -10 are NETWORK descriptors and
     * must never masquerade as console. */
    if (fd < PROC_FD_MAX && proc_fds[fd].used &&
        proc_fds[fd].fs_fd < 0 && proc_fds[fd].fs_fd > -10)
        return true;
    /* [#fg-notty fix] the controlling-terminal HANDLE lives on in
     * ash's memory even after fd-table churn closed or recycled its
     * slot: a tcsetpgrp/tcgetpgrp through that exact number still
     * addresses THE one system console (no per-fd state involved).
     * Without this, job resume aborts with ENOTTY right here.      */
    if ((s64)fd == (s64)tty_ctl_fd) {
        serial_printf("[TTYHEAL] ctl_fd=%d survives slot churn "
                      "(used=%d fs_fd=%d)\n", tty_ctl_fd,
                      (fd < PROC_FD_MAX && proc_fds[fd].used) ? 1 : 0,
                      (fd < PROC_FD_MAX ? proc_fds[fd].fs_fd : -99));
        return true;
    }
    serial_printf("[IOCNOTTY] fd=%llu used=%d fs_fd=%d\n",
                  (unsigned long long)fd,
                  (fd < PROC_FD_MAX && proc_fds[fd].used),
                  (fd < PROC_FD_MAX ? proc_fds[fd].fs_fd : -99));
    return false;
}

// ---- Linux ioctl(16): real tty requests ---------------------------

static u64 sys_ioctl_impl(u64 fd, u64 request, u64 arg)
{
    switch (request) {
    case LINUX_TCGETS:
        if (!user_range_ok(arg, sizeof(kernel_termios_t)))
            return (u64)(s64)(-LINUX_EFAULT);
        kmemcpy((void*)arg, &tty_termios, sizeof(kernel_termios_t));
        return 0;

    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF:
        if (!user_range_ok(arg, sizeof(kernel_termios_t)))
            return (u64)(s64)(-LINUX_EFAULT);
        kmemcpy(&tty_termios, (const void*)arg,
                sizeof(kernel_termios_t));
        return 0;                    /* no queue flush needed yet  */

    case LINUX_TIOCGWINSZ: {
        if (!user_range_ok(arg, 8))
            return (u64)(s64)(-LINUX_EFAULT);
        u16* ws = (u16*)arg;
        ws[0] = tty_winsize.rows;
        ws[1] = tty_winsize.cols;
        ws[2] = tty_winsize.xpix;
        ws[3] = tty_winsize.ypix;
        return 0;
    }

    case LINUX_TIOCSWINSZ: {
        if (!user_range_ok(arg, 8))
            return (u64)(s64)(-LINUX_EFAULT);
        u16* ws = (u16*)arg;
        if (ws[0]) tty_winsize.rows = ws[0];
        if (ws[1]) tty_winsize.cols = ws[1];
        tty_winsize.xpix  = ws[2];
        tty_winsize.ypix  = ws[3];
        return 0;
    }

    case LINUX_TIOCGPGRP: {
        if (!fd_is_console(fd)) return (u64)(s64)(-LINUX_ENOTTY);
        if (!user_range_ok(arg, 4))
            return (u64)(s64)(-LINUX_EFAULT);
        s32 fg = tty_fg_pgrp;
        if (fg == 0) {
            task_t* cur = task_get_current();
            fg = (cur && cur->pgrp > 0) ? cur->pgrp : (cur ? cur->pid : 0);
        }
        *(s32*)arg = fg;
        return 0;
    }

    case LINUX_TIOCSPGRP: {
        serial_printf("[TIOCSPGRP] fd=%llu\n", (unsigned long long)fd);
        if (!fd_is_console(fd)) return (u64)(s64)(-LINUX_ENOTTY);
        if (!user_range_ok(arg, 4))
            return (u64)(s64)(-LINUX_EFAULT);
        s32 p = *(s32*)arg;
        if (p < 0) return (u64)(s64)(-22);    /* -EINVAL          */
        tty_fg_pgrp = p;
        tty_ctl_fd = (s32)fd;                 /* remember the handle */
        return 0;
    }

    case LINUX_TIOCSCTTY:
        /* Only one console: becoming its controller always succeeds.
         * The claimant becomes our recorded controlling session leader */
        {
            task_t* cur = task_get_current();
            if (cur && cur->pid > 0)
                tty_note_session_leader(cur->pid);
        }
        return 0;

    case LINUX_FIONREAD:
        if (!user_range_ok(arg, 4))
            return (u64)(s64)(-LINUX_EFAULT);
        *(s32*)arg = 0;              /* bytes immediately readable  */
        return 0;

    default:
        serial_printf("[IOCTL] unsupported req=%lx fd=%lu\n",
                      (unsigned long)request, (unsigned long)fd);
        return (u64)(s64)(-LINUX_ENOTTY);
    }
}

// ---- Linux getpgrp(111) / setpgid(109) / setsid(112) --------------

static task_t* job_find_task(s32 pid)
{
    extern void* scheduler_task_table(void);
    task_t* table = (task_t*)scheduler_task_table();
    if (!table || pid <= 0) return NULL;
    for (s32 i = 0; i < TASK_MAX_TASKS; i++)
        if (table[i].active && table[i].pid == pid) return &table[i];
    return NULL;
}

static u64 sys_getpgrp_impl(void)
{
    task_t* cur = task_get_current();
    if (!cur || cur->pid <= 0) return 0;
    return (u64)(cur->pgrp > 0 ? cur->pgrp : cur->pid);
}

static u64 sys_setpgid_impl(u64 pid_u, u64 pgid_u)
{
    task_t* cur = task_get_current();
    if (!cur || cur->pid <= 0) return (u64)(s64)(-LINUX_EPERM);

    s32 pid  = (pid_u  == 0) ? cur->pid  : (s32)pid_u;
    s32 pgid = (pgid_u == 0) ? -1        : (s32)pgid_u;

    task_t* target = (pid == cur->pid) ? cur : job_find_task(pid);
    if (!target) return (u64)(s64)(-3);             /* -ESRCH     */

    s32 newgrp = (pgid == -1) ? target->pid : pgid;
    if (newgrp <= 0) return (u64)(s64)(-22);        /* -EINVAL    */

    target->pgrp = newgrp;
    return 0;
}

static u64 sys_setsid_impl(void)
{
    task_t* cur = task_get_current();
    if (!cur || cur->pid <= 0) return (u64)(s64)(-LINUX_EPERM);
    cur->pgrp = cur->pid;           /* session+pgrp leader           */
    return (u64)cur->pid;
}

/* kill(62): the job-control trio is delivered for real at scheduler
 * level (task_t.stop_sig park + wait4(WUNTRACED) report + SIGCONT
 * resume). Everything else stays a success no-op: user handlers are
 * not wired into syscall return paths yet, and erroring kill() sends
 * interactive ash into endless SIGTTIN-style retry loops. */
static u64 sys_kill_impl(s32 pid, s32 sig)
{
    if (sig == 18 || sig == 19 || sig == 20) {     /* CONT/STOP/TSTP   */
        extern void* scheduler_task_table(void);
        task_t* tbl = (task_t*)scheduler_task_table();
        task_t* cur = task_get_current();
        s32 me = (cur && cur->pid > 0) ? cur->pid : 0;
        s32 my_pgrp = 0;
        if (pid == 0) {
            /* POSIX: pid==0 targets the caller's process group */
            my_pgrp = (cur && cur->pgrp > 0) ? cur->pgrp : me;
            if (my_pgrp <= 0 && me > 0) my_pgrp = me;
        }
        bool hit = false, self_stop = false;
        if (tbl) {
            for (s32 i = 0; i < TASK_MAX_TASKS; i++) {
                task_t* t = &tbl[i];
                if (!t->active || t->pid <= 0) continue;
                bool match;
                if (pid > 0)      match = (t->pid == pid);
                else if (pid < 0) {
                    s32 eff = (t->pgrp > 0) ? t->pgrp : t->pid;
                    match = (eff == -pid);
                } else {
                    match = ((t->pgrp > 0 ? t->pgrp : t->pid)
                             == my_pgrp);
                }
                if (!match) continue;
                hit = true;
                if (sig == 18) {                   /* SIGCONT          */
                    t->stop_sig = 0;
                    t->stop_reported = true;       /* consume stale rpt*/
                } else {                           /* STOP/TSTP        */
                    if (!t->stop_sig) {
                        t->stop_sig      = (u32)sig;
                        t->stop_reported = false;
                    }
                    if (t->pid == me) self_stop = true;
                }
            }
        }
        serial_printf("[KILL] sig=%d pid=%d hit=%d self_stop=%d\n",
                      sig, pid, hit ? 1 : 0, self_stop ? 1 : 0);
        if (!hit) return (u64)(s64)(-3);           /* -ESRCH           */
        if (self_stop) {
            /* Own STOP must not linger: give up CPU immediately.
             * task_yield() from inside a blocking syscall is an
             * established pattern here (proc_do_wait4 uses it). */
            task_yield();
        }
        return 0;
    }
    return 0;                                      /* delivered-noop   */
}


// ---- Linux poll(7): console-focused minimal -----------------------
// struct pollfd { int fd; short events; short revents; } = 8 bytes.
// Only terminal-input waiting matters here (BusyBox ash line editing):
// console fds become POLLIN as soon as the keyboard buffer holds a
// character; other fds are POLLNVAL (never ready). Polling honours
// timeout: 0 => pure probe; -1 => indefinite; else milliseconds.

#define LINUX_POLLIN    0x0001
#define LINUX_POLLOUT   0x0004
#define LINUX_POLLERR   0x0008
#define LINUX_POLLNVAL  0x0020

static u64 sys_poll_impl(u64 ufds, u64 nfds, u64 timeout_ms)
{
    struct pfd_s { s32 fd; s16 events; s16 revents; };
    if (nfds == 0 || nfds > 64) return (u64)(s64)(-22);
    if (!user_range_ok(ufds, nfds * 8))
        return (u64)(s64)(-LINUX_EFAULT);

    u64 waited_ms = 0;
    for (;;) {
        u32 n_ready = 0;
        /* Drain the NIC RX ring once per sweep so UDP datagrams that
         * arrived during hlt windows become visible to socket entries.
         */
        {
            extern void net_poll(void);
            net_poll();
        }
        for (u64 i = 0; i < nfds; i++) {
            struct pfd_s* p = (struct pfd_s*)(ufds + i * 8);
            p->revents = 0;
            if (p->fd == -1) continue;            /* POSIX: ignored    */
            {
                static s32 poll_dbg = 0;
                if (poll_dbg < 6) {
                    poll_dbg++;
                    serial_printf("[POLL] call=%d fd=%d ev=%04x\n",
                                  poll_dbg, p->fd, (u32)(u16)p->events);
                }
            }

            enum { PCLS_NONE, PCLS_CONS, PCLS_FILE, PCLS_NET, PCLS_PIPE,
                   PCLS_UNIX, PCLS_WLOBJ } cls;
            s32 nidx = -1;
            bool pipe_wr = false;
            bool uni_e0 = false;
            cls = PCLS_NONE;
            if (p->fd >= 0 && p->fd < PROC_FD_MAX && proc_fds[p->fd].used) {
                s32 v = proc_fds[p->fd].fs_fd;
                if (fd_is_net(v)) {
                    extern bool net_socket_is_used(int);
                    nidx = fd_net_index(v);
                    if (net_socket_is_used(nidx)) {
                        cls = PCLS_NET;
                    }
                } else if (fd_is_pipe_enc(v)) {
                    nidx = fd_pipe_index(v);
                    pipe_wr = fd_pipe_is_write(v);
                    cls = PCLS_PIPE;
                } else if (fd_is_unix_enc(v)) {
                    nidx = fd_unix_index(v);
                    uni_e0 = fd_unix_is_end0(v);
                    cls = PCLS_UNIX;
                } else if (fd_is_efd_enc(v) || fd_is_tfd_enc(v) ||
                           fd_is_mfd_enc(v) || fd_is_sfd_enc(v)) {
                    cls = PCLS_WLOBJ;
                } else if (v < 0) {
                    cls = PCLS_CONS;
                } else {
                    cls = PCLS_FILE;
                }
            }
            if (cls == PCLS_NONE) {
                p->revents = LINUX_POLLNVAL;
                continue;
            }

            bool in_ready  = false;
            bool out_ready = true;   /* console/VGA & ramfs writes block-free */
            switch (cls) {
            case PCLS_CONS:
                in_ready = keyboard_haschar();
                break;
            case PCLS_FILE:
                in_ready = true;              /* regular files are instant */
                break;
            case PCLS_NET: {
                in_ready = net_socket_has_data(nidx);
                out_ready = net_socket_is_connected(nidx);
                break;
            }
            case PCLS_PIPE: {
                pipe_t* pp = &g_pipes[nidx];
                if (pipe_wr) {
                    out_ready = (pp->readers != 0) &&
                                (pp->head - pp->tail < PIPE_BUF_SIZE);
                    in_ready = false;
                } else {
                    in_ready = (pp->head != pp->tail) || (pp->writers == 0);
                    out_ready = false;
                }
                break;
            }
            case PCLS_WLOBJ: {
                s32 v = proc_fds[p->fd].fs_fd;
                extern bool wl_obj_ready(s32, bool, bool, bool*, bool*);
                wl_obj_ready(v,
                             (p->events & LINUX_POLLIN) != 0,
                             (p->events & LINUX_POLLOUT) != 0,
                             &in_ready, &out_ready);
                break;
            }
            case PCLS_UNIX: {
                unich_t* uu = &g_unich[nidx];
                if (uni_e0) {
                    in_ready = (uu->ba_head != uu->ba_tail) ||
                               (uu->refs[1] == 0);
                    out_ready = (uu->refs[1] != 0) &&
                                ((uu->ab_head - uu->ab_tail) < UNIX_BUF_SIZE);
                } else {
                    in_ready = (uu->ab_head != uu->ab_tail) ||
                               (uu->refs[0] == 0);
                    out_ready = (uu->refs[0] != 0) &&
                                ((uu->ba_head - uu->ba_tail) < UNIX_BUF_SIZE);
                }
                break;
            }
            default:
                break;
            }
            if ((p->events & LINUX_POLLIN) && in_ready) {
                p->revents |= LINUX_POLLIN;
                n_ready++;
            }
            if ((p->events & LINUX_POLLOUT) && out_ready) {
                p->revents |= LINUX_POLLOUT;
                n_ready++;
            }
        }
        if (n_ready > 0) return n_ready;
        if (timeout_ms == 0) return 0;
        if ((s64)timeout_ms >= 0 && waited_ms >= timeout_ms) return 0;

        /* #pipe-fd-switch family: yield instead of bare breath —
         * readiness for ash's poll(tty,-1) may be produced by a
         * concurrent task (keyboard IRQ needs no yield, pipe/unix
         * peers DO need the switch). */
        {
            extern void task_yield(void);
            task_yield();
        }
        waited_ms += 10;
    }
}

/* select(23): musl's libfetch drives connected sockets through
 * select(WRITE_SET) before write() — without it every HTTP request
 * died as -EPERM and apk fetched nothing (#apk-fetch-select).
 * Translate the three fd_sets into one poll sweep. */
struct timeval_k { s64 tv_sec, tv_usec; };

static u64 sys_select_impl(u64 nfds, u64 rd_u, u64 wr_u, u64 ex_u, u64 tv_u)
{
    if (nfds > 1024) return (u64)(s64)(-22);

    struct pfd_s { s32 fd; s16 events; s16 revents; };
    struct pfd_s pfds[64];
    u64 n = 0;
    for (u64 fd = 0; fd < nfds && n < 64; fd++) {
        u16 ev = 0;
        if (rd_u) {
            if (!user_range_ok(rd_u + (fd >> 3), 1))
                return (u64)(s64)(-LINUX_EFAULT);
            if ((*(const u8*)(rd_u + (fd >> 3))) & (1u << (fd & 7)))
                ev |= LINUX_POLLIN;
        }
        if (wr_u) {
            if (!user_range_ok(wr_u + (fd >> 3), 1))
                return (u64)(s64)(-LINUX_EFAULT);
            if ((*(const u8*)(wr_u + (fd >> 3))) & (1u << (fd & 7)))
                ev |= LINUX_POLLOUT;
        }
        if (ev) {
            pfds[n].fd = (s32)fd;
            pfds[n].events = (s16)ev;
            pfds[n].revents = 0;
            n++;
        }
    }
    if (n == 0) return 0;                 /* nothing requested: no block */

    u64 timeout_ms = 0xFFFFFFFFULL;       /* NULL timeout: infinite      */
    if (tv_u) {
        if (!user_range_ok(tv_u, sizeof(struct timeval_k)))
            return (u64)(s64)(-LINUX_EFAULT);
        struct timeval_k* tv = (struct timeval_k*)tv_u;
        s64 ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
        if (ms < 0) ms = 0;
        timeout_ms = (u64)ms;
    }

    /* BLOCK until something is ready or the caller's timeout expires.
     * #select-no-block: a single sweep returning 0 reads as "select
     * timeout" to libfetch — it then aborts perfectly healthy
     * downloads after the first 128KB block. */
    u64 waited_ms = 0;
    u32 n_ready = 0;
    for (;;) {
        {
            extern void net_poll(void);
            net_poll();
        }
        n_ready = 0;
        for (u64 i = 0; i < n; i++) {
            pfds[i].revents = 0;
            s32 fd = pfds[i].fd;
            bool in_ready = false, out_ready = false;
            if (fd >= 0 && fd < PROC_FD_MAX && proc_fds[fd].used) {
                s32 v = proc_fds[fd].fs_fd;
                if (fd_is_net(v)) {
                    extern bool net_socket_is_used(int);
                    s32 nidx = fd_net_index(v);
                    if (net_socket_is_used(nidx)) {
                        extern bool net_socket_has_data(int);
                        extern bool net_socket_is_connected(int);
                        in_ready  = net_socket_has_data(nidx);
                        out_ready = net_socket_is_connected(nidx);
                    }
                } else if (fd_is_pipe_enc(v)) {
                    extern bool fd_pipe_is_write(s32);
                    s32 pidx = fd_pipe_index(v);
                    pipe_t* pp = &g_pipes[pidx];
                    if (fd_pipe_is_write(v)) {
                        out_ready = (pp->readers != 0) &&
                                    (pp->head - pp->tail < PIPE_BUF_SIZE);
                    } else {
                        in_ready = (pp->head != pp->tail) ||
                                   (pp->writers == 0);
                    }
                } else if (v < 0) {
                    in_ready = keyboard_haschar(); /* console            */
                    out_ready = true;
                } else {
                    in_ready = out_ready = true;   /* regular file       */
                }
            } else {
                return (u64)(s64)(-LINUX_EBADF);   /* fd not open        */
            }
            if ((pfds[i].events & LINUX_POLLIN) && in_ready) {
                pfds[i].revents |= LINUX_POLLIN;
                n_ready++;
            }
            if ((pfds[i].events & LINUX_POLLOUT) && out_ready) {
                pfds[i].revents |= LINUX_POLLOUT;
                n_ready++;
            }
        }
        if (n_ready > 0) break;
        if (timeout_ms == 0) break;
        if ((s64)timeout_ms >= 0 && waited_ms >= timeout_ms) break;
        /* #pipe-fd-switch family: yield to peer tasks (same rule as
         * poll/pipe-read above). */
        {
            extern void task_yield(void);
            task_yield();
        }
        waited_ms += 10;
    }
    return n_ready;
}

// ---- Linux socket(41) -------------------------------------------
// int socket(int domain, int type, int protocol);
// AF_INET(2) + SOCK_DGRAM(2); protocol 17 (UDP) OR 0 (default):
// BusyBox/musl pass proto=0 for "udp" (getprotobyname resolves AFTER
// creation, the raw syscall carries 0), so both are accepted here.
// SOCK_STREAM cleanly fails -EPROTONOSUPPORT (TCP is future work).

static u64 sys_socket_impl(u64 domain, u64 type, u64 proto)
{
    /* POSIX: SOCK_CLOEXEC(0x80000)/SOCK_NONBLOCK(0x800) ride in the
     * type argument on Linux. Mask to the base type — musl's resolver
     * opens its socket WITH the flags (#gai-socket-flags) and used to
     * fall back after our -EPROTONOSUPPORT. Flags are accepted and
     * tracked: CLOEXEC is a no-op without execve semantics needing
     * it; NONBLOCK is emulated by bounded blocking loops. */
    u64 base = type & 0xFFu;
    bool icmp_ok = (proto == IPPROTO_ICMP_NUM) &&
                   (base == SOCK_RAW || base == SOCK_DGRAM);
    bool stream_ok = (base == SOCK_STREAM) &&
                     (proto == 0 || proto == IPPROTO_TCP_NUM);
    bool dgram_ok  = (base == SOCK_DGRAM) &&
                     (proto == 0 || proto == IPPROTO_UDP_NUM);

    if (domain == 1) {
        /* #wayland-transport: AF_UNIX(1) socket() — unbound end0 of a
         * fresh channel; connect()/bind() take it from here.       */
        if (base != 1 && base != 2) return (u64)(s64)(-93);
        s32 idx = unix_alloc();
        if (idx < 0) return (u64)(s64)(-23);
        g_unich[idx].refs[0] = 1;
        s32 fd = proc_fd_alloc(FD_UNIX_BASE - 2 * idx);
        if (fd < 0) {
            unix_ref_dec(idx, true);
            unix_ref_dec(idx, false);
            return (u64)(s64)(-24);
        }
        serial_printf("[UNIX] socket idx=%d fd=%d\n", idx, fd);
        return (u64)fd;
    }

    if (domain != 2 || (!icmp_ok && !stream_ok && !dgram_ok)) {
        return (u64)(s64)(-93);               /* -EPROTONOSUPPORT */
    }

    s32 r = net_socket_alloc();
    if (r < 0) return (u64)(s64)(-23);        /* ENFILE  */

    {
        extern int net_socket_set_type(int, int);
        net_socket_set_type(r, (int)base);    /* remember DGRAM vs STREAM */
    }
    if (icmp_ok) {
        extern void net_socket_set_proto(int, int);
        net_socket_set_proto(r, IPPROTO_ICMP_NUM);   /* ping flavor      */
    }

    s32 fd = proc_fd_alloc(FD_NET_BASE - r);
    if (fd < 0) {
        net_socket_free(r);
        return (u64)(s64)(-24);               /* EMFILE  */
    }
    serial_printf("[SOCK] netidx=%d -> fd=%d (slot fs_fd=%d) type=%lu\n",
                  r, fd, proc_fds[fd].fs_fd,
                  (unsigned long)type);
    {
        extern void net_socket_set_local_port(int, u16);
        net_socket_set_local_port(r, 0);      /* port assigned by bind/connect */
    }
    return (u64)fd;
}

// ---- Linux bind(49) ---------------------------------------------
// int bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
// Claims local_port on the net-layer socket (wildcard IP only).

static u64 sys_bind_impl(u64 fd, u64 addr_u, u64 addrlen)
{
    serial_printf("[BIND] fd=%lu\n", (unsigned long)fd);
    if (is_unix_fd(fd)) {
        /* AF_UNIX bind: register the display name. No fs node is
         * created — connect() resolves through the channel path
         * table (documented simplification).                  */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0) return (u64)(s64)(-LINUX_EBADF);
        if (!user_range_ok(addr_u, addrlen < 16 ? addrlen : 16))
            return (u64)(s64)(-LINUX_EFAULT);
        const u8* sa = (const u8*)addr_u;
        u16 fam = (u16)(sa[0] | (sa[1] << 8));
        if (fam != 1) return (u64)(s64)(-22);       /* EINVAL */
        char path[64];
        u64 plen = (addrlen > 2) ? (addrlen - 2) : 0;
        if (plen > 63) plen = 63;
        u64 i2 = 0;
        for (; i2 < plen; i2++) {
            path[i2] = (char)sa[2 + i2];
            if (!path[i2]) break;
        }
        path[63] = 0;
        if (!path[0]) return (u64)(s64)(-22);
        for (int q = 0; q < MAX_UNICH; q++) {
            if (g_unich[q].used && g_unich[q].listening &&
                !kstrncmp(g_unich[q].path, path, 64))
                return (u64)(s64)(-98);             /* EADDRINUSE */
        }
        kmemcpy(g_unich[uidx].path, path, 64);
        g_unich[uidx].listening = true;
        serial_printf("[UNIX] bind idx=%d path=%s\n", uidx, path);
        return 0;
    }
    if (!user_range_ok(addr_u, addrlen < 16 ? addrlen : 16) || addrlen < 8)
        return (u64)(s64)(-LINUX_EFAULT);
    s32 idx = proc_fd_to_net(fd);
    if (idx < 0) return (u64)(s64)(-LINUX_EBADF);

    const u8* sa = (const u8*)addr_u;
    /* sin_family is NATIVE-endian (host), unlike port/addr (BE).
     * Read it as a little-endian u16 on x86_64. */
    if ((u16)(sa[0] | ((u16)sa[1] << 8)) != 2)
        return (u64)(s64)(-22);
    u16 port = ((u16)sa[2] << 8) | sa[3];
    int rc = net_socket_bind(idx, &sa[4], port);
    if (rc != 0) return (u64)(s64)-rc;        /* -98 EADDRINUSE etc   */
    return 0;
}

// ---- Linux connect(42) ------------------------------------------
// int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
// struct sockaddr_in: family(2) port(2BE) ip(4) zero(8) = 16 bytes
// UDP connect only stores the peer endpoint AND auto-binds an
// ephemeral local port when none was set — otherwise replies have no
// return address (Linux kernel does exactly this for unbound UDP).

static u64 sys_connect_impl(u64 fd, u64 addr_u, u64 addrlen)
{
    if (is_unix_fd(fd)) {
        /* #wayland-transport: connect to a bound display name.
         * A NEW channel carries the connection; the client fd is
         * rewired to its end1, end0 waits on the listener's backlog
         * for accept(). */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0) return (u64)(s64)(-LINUX_EBADF);
        if (!user_range_ok(addr_u, addrlen < 16 ? addrlen : 16))
            return (u64)(s64)(-LINUX_EFAULT);
        const u8* sa = (const u8*)addr_u;
        u16 fam = (u16)(sa[0] | (sa[1] << 8));
        if (fam != 1) return (u64)(s64)(-22);
        char path[64];
        u64 plen = (addrlen > 2) ? (addrlen - 2) : 0;
        if (plen > 63) plen = 63;
        u64 i2 = 0;
        for (; i2 < plen; i2++) {
            path[i2] = (char)sa[2 + i2];
            if (!path[i2]) break;
        }
        path[63] = 0;
        s32 lst = -1;
        for (int q = 0; q < MAX_UNICH; q++) {
            if (g_unich[q].used && g_unich[q].listening &&
                !kstrncmp(g_unich[q].path, path, 64)) { lst = q; break; }
        }
        if (lst < 0) return (u64)(s64)(-111);       /* ECONNREFUSED */
        if (g_unich[lst].backlog_n >= UNIX_BACKLOG)
            return (u64)(s64)(-105);                /* ENOBUFS */
        s32 cidx = unix_alloc();
        if (cidx < 0) return (u64)(s64)(-23);
        g_unich[cidx].refs[0] = 1;    /* adopted by accept() */
        g_unich[cidx].refs[1] = 1;    /* this fd             */
        g_unich[lst].backlog[g_unich[lst].backlog_n++] = cidx;
        unix_ref_dec(uidx, end0);     /* drop the unbound shell channel */
        proc_fds[fd].fs_fd = FD_UNIX_BASE - 2 * cidx - 1;   /* end1 */
        serial_printf("[UNIX] connect fd=%d lst=%d chan=%d path=%s\n",
                      (int)fd, lst, cidx, path);
        return 0;
    }
    if (!user_range_ok(addr_u, addrlen < 16 ? addrlen : 16) || addrlen < 8)
        return (u64)(s64)(-LINUX_EFAULT);
    s32 idx = proc_fd_to_net(fd);
    if (idx < 0) return (u64)(s64)(-LINUX_EBADF);

    u8* sa = (u8*)addr_u;
    u8 rip[4] = {sa[4], sa[5], sa[6], sa[7]};
    u16 rport = ((u16)sa[2] << 8) | (u16)sa[3];
    serial_printf("[CONNECT] fd=%ld idx=%d ip=%u.%u.%u.%u:%u type=%x\n",
                  (s64)fd, idx, rip[0], rip[1], rip[2], rip[3], rport,
                  (unsigned)net_socket_type(idx));
    net_socket_connect(idx, rip, rport);
    if (!net_socket_is_bound(idx)) {
        net_socket_bind(idx, rip, 0);          /* ephemeral            */
    }

    /* TCP stream: run the full blocking three-way handshake now —
     * musl connect(2) is synchronous and callers expect a live
     * connection on return. UDP keeps the store-endpoint semantics. */
    {
        extern int net_socket_type(int);
        extern int net_socket_connect_tcp(int);
        if ((net_socket_type(idx) & 0xFF) == SOCK_STREAM) {
            s32 hs = net_socket_connect_tcp(idx);
            serial_printf("[CONNECT] hs=%d\n", hs);
            if (hs != 0) return (u64)(s64)hs;  /* -110 ETIMEDOUT etc   */
            serial_printf("[CONN] tcp established idx=%d\n", idx);
        }
        return 0;
    }
    return 0;
}

// ---- Linux listen(50) / accept(43) / accept4(288) ----------------
// int listen(int fd, int backlog);
//   Marks the bound STREAM socket passive. bind() must precede it
//   (POSIX order — ash/python always do that anyway).
// int accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
//   Blocks (bounded hlt loop like recvfrom) until a fully
//   ESTABLISHED child waits, then returns a NEW process fd of type
//   SOCK_STREAM describing that connection. addr gets the peer.

static u64 sys_listen_impl(u64 fd, u64 backlog)
{
    if (is_unix_fd(fd)) {
        /* bind() already marked the channel listening; listen() is a
         * POSIX formality here (backlog is fixed at UNIX_BACKLOG).  */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0) return (u64)(s64)(-LINUX_EBADF);
        g_unich[uidx].listening = true;
        return 0;
    }
    (void)backlog;
    s32 idx = proc_fd_to_net(fd);
    if (idx < 0) return (u64)(s64)(-LINUX_EBADF);
    {
        extern int net_socket_type(int);
        if (net_socket_type(idx) != SOCK_STREAM)
            return (u64)(s64)(-95);               /* -EOPNOTSUPP */
    }
    {
        extern int net_socket_listen(int, int);
        int rc = net_socket_listen(idx, (int)(s64)backlog);
        if (rc != 0) return (u64)(s64)-rc;
    }
    return 0;
}

/* fill sockaddr_in {family=AF_INET BE, port BE, ip, zeros} */
static void sockaddr_fill_out(u8* sa16, const u8* ip4, u16 port)
{
    sa16[0] = 0; sa16[1] = 2;                     /* AF_INET BE */
    sa16[2] = (u8)(port >> 8);
    sa16[3] = (u8)(port & 0xFF);
    sa16[4] = ip4[0]; sa16[5] = ip4[1];
    sa16[6] = ip4[2]; sa16[7] = ip4[3];
    kmemset(sa16 + 8, 0, 8);
}

static u64 sys_accept_impl(u64 fd, u64 addr_u, u64 addrlen_ptr)
{
    if (is_unix_fd(fd)) {
        /* #wayland-transport: pop the next queued connection channel
         * and return its end0 as a NEW process fd. Blocking hlt loop
         * like the net accept (preemption-friendly).               */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0 || !g_unich[uidx].listening)
            return (u64)(s64)(-LINUX_EBADF);
        for (;;) {
            if (g_unich[uidx].backlog_n > 0) {
                s32 cidx = g_unich[uidx].backlog[0];
                for (int k = 1; k < g_unich[uidx].backlog_n; k++)
                    g_unich[uidx].backlog[k - 1] = g_unich[uidx].backlog[k];
                g_unich[uidx].backlog_n--;
                s32 nfd = proc_fd_alloc(FD_UNIX_BASE - 2 * cidx);
                if (nfd < 0) return (u64)(s64)(-24);
                serial_printf("[UNIX] accept lst=%d -> chan=%d fd=%d\n",
                              uidx, cidx, nfd);
                return (u64)nfd;
            }
            /* #unix-accept-yield: preemption is OFF by default — a hlt
             * loop never schedules the connecting client task. Yield
             * explicitly (same discipline as wait4's reap loop).     */
            {
                extern void task_yield(void);
                task_yield();
            }
        }
    }
    if (!is_net_fd(fd)) return (u64)(s64)(-LINUX_EBADF);
    s32 idx = proc_fd_to_net(fd);
    if (idx < 0) return (u64)(s64)(-LINUX_EBADF);
    if (addr_u && !user_range_ok(addr_u, 16))
        return (u64)(s64)(-LINUX_EFAULT);
    if (addrlen_ptr && !user_range_ok(addrlen_ptr, 4))
        return (u64)(s64)(-LINUX_EFAULT);

    /* bounded blocking drain: SYNs land inside net_poll() which the
     * wait loop pumps via timer wakeups (same pattern as recvfrom) */
    s32 child = -1;
    {
        extern s32 net_socket_accept_pop(int);
        extern void net_poll(void);
        for (int tries = 0; tries < 900; tries++) {
            child = net_socket_accept_pop(idx);
            if (child >= 0) break;
            __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
            net_poll();
        }
    }
    if (child < 0) {
        serial_printf("[ACCFAIL] lp=%u timeout\n",
                      net_socket_local_port(idx));
        return (u64)(s64)(-11);                   /* -EAGAIN after cap */
    }

    s32 nfd = proc_fd_alloc(FD_NET_BASE - child);
    if (nfd < 0) {
        extern void net_socket_free(int);
        net_socket_free(child);                   /* drop conn instead  */
        return (u64)(s64)(-24);                   /* -EMFILE */
    }

    u8 rip[4] = {0};
    u16 rport = 0;
    {
        extern int net_socket_get_remote(int, u8*, u16*);
        net_socket_get_remote(child, rip, &rport);
    }
    if (addr_u && addrlen_ptr && user_range_ok(addr_u, 16)) {
        sockaddr_fill_out((u8*)addr_u, rip, rport);
        if (user_range_ok(addrlen_ptr, 4)) *(u32*)addrlen_ptr = 16;
    }
    serial_printf("[ACCEPT] lp=%u -> nfd=%d child_idx=%d "
                  "peer=%u.%u.%u.%u:%u\n",
                  net_socket_local_port(idx), nfd, child,
                  rip[0], rip[1], rip[2], rip[3], rport);
    return (u64)nfd;
}

// ---- Linux sendto(44) -------------------------------------------
// ssize_t sendto(fd, buf, len, flags, dest_addr, dest_addrlen);
// send(2) = sendto with dest_addr == NULL → uses the connected peer.
// MSG_DONTWAIT is honored; without it a short bounded retry warms ARP
// so the first datagram of a cold cache is not silently dropped.

static u64 sys_sendto_impl(u64 fd, u64 buf, u64 len,
                           u64 flags, u64 dest_addr)
{
    serial_printf("[SENDTO] fd=%ld len=%ld\n", (s64)fd, (s64)len);
    if (!is_net_fd(fd)) return (u64)(s64)(-LINUX_EBADF);
    s32 idx = proc_fd_to_net(fd);
    if (idx < 0) return (u64)(s64)(-LINUX_EBADF);
    if (!user_range_ok(buf, len)) return (u64)(s64)(-LINUX_EFAULT);
    if ((s64)len <= 0) return 0;
    if (len > NET_MTU) len = NET_MTU;

    /* UDP auto-bind: an unbound socket sending a datagram needs an
     * ephemeral local port or every reply has nowhere to demux to.
     * Linux does this implicitly; musl's getaddrinfo() resolver
     * relies on it (DNS query from a never-bound socket). */
    {
        extern int net_socket_type(int);
        if (net_socket_type(idx) == SOCK_DGRAM &&
            !net_socket_is_bound(idx)) {
            /* ICMP ping sockets skip ephemeral binding entirely */
            extern bool net_socket_is_icmp(int);
            if (!net_socket_is_icmp(idx)) {
                net_socket_bind(idx, 0, 0);       /* ephemeral port    */
            }
        }
    }

    u16 dst_port;
    u8  dst_ip[4];

    if (dest_addr && user_range_ok(dest_addr, 16)) {
        /* sendto with explicit destination sockaddr_in */
        u8* sa = (u8*)dest_addr;
        dst_ip[0] = sa[4]; dst_ip[1] = sa[5];
        dst_ip[2] = sa[6]; dst_ip[3] = sa[7];
        dst_port = ((u16)sa[2] << 8) | (u16)sa[3];
    } else if (!dest_addr || !user_range_ok(dest_addr, 1)) {
        /* NULL destination: connected remote endpoint */
        int gr = net_socket_get_remote(idx, dst_ip, &dst_port);
        if (gr != 0) {
            return (u64)(s64)(-89);           /* -EDESTADDRREQ */
        }
    } else {
        return (u64)(s64)(-LINUX_EFAULT);
    }

    /* Bounded ARP warm-up: net_udp_send DROPS when the MAC for the
     * next-hop IP is unresolved; the QEMU slirp gateway answers ARP
     * within ~a few ms of its request. For blocking callers extend
     * the wait substantially — the first frame must not race away
     * unresolved (#ping-first-shot). */
    if (!(flags & 0x40)) {                    /* LIN_MSG_DONTWAIT */
        extern bool net_arp_ready_route(const u8*);
        int budget = 40;
        while (budget < 260 && !net_arp_ready_route(dst_ip))
            budget++;                         /* cheap grow for blocking */
        for (int i = 0; i < budget && !net_arp_ready_route(dst_ip); i++) {
            /* FIX(#gai-arp-kick): the rewritten budget loop lost the
             * periodic ARP re-kick — it waited for an answer to a
             * request nobody had sent, so the FIRST of two
             * back-to-back sendto calls always lost its datagram
             * (net_udp_send's silent drop), musl saw a reply for
             * only one of its A/AAAA queries and returned EAI_AGAIN
             * ("bad address"). Kick at entry and every 32 iters. */
            if ((i & 31) == 0) {
                extern void net_arp_kick_route(const u8*);
                net_arp_kick_route(dst_ip);
            }
            __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
            net_poll();
        }
    }

    /* Raw/ping flavor: verbatim ICMP send after the shared ARP warm-up */
    {
        extern bool net_socket_is_icmp(int);
        if (net_socket_is_icmp(idx)) {
            extern void net_icmp_send(const u8*, const u8*, u16);
            extern void net_socket_icmp_arm(int, const u8*, const u8*, u16);
            u16 l2 = (len > 1480) ? 1480 : (u16)len;
            net_icmp_send(dst_ip, (const u8*)buf, l2);
            net_socket_icmp_arm(idx, dst_ip, (const u8*)buf, l2);
            serial_printf("[ICMPSND] sk=%u -> %u.%u.%u.%u len=%u\n",
                          (unsigned)idx, dst_ip[0], dst_ip[1],
                          dst_ip[2], dst_ip[3], l2);
            return len;
        }
    }

    /* #apk-fetch: STREAM sockets must NOT fall through to the UDP
     * builder — libfetch's send() on a connected TCP socket used to
     * leave as a bogus UDP datagram (silent drop -> "operation timed
     * out"). Route through the TCP stream sender. */
    {
        extern int net_socket_type(int);
        if ((net_socket_type(idx) & 0xFF) == SOCK_STREAM) {
            extern int net_socket_stream_send(int, const void*, u32);
            s64 sn = net_socket_stream_send(idx, (const void*)buf,
                                            (u32)len);
            serial_printf("[SSND] idx=%d len=%ld n=%ld\n",
                          (unsigned)idx, (s64)len, sn);
            if (sn < 0) return (u64)(s64)sn;
            return (u64)sn;
        }
    }

    net_udp_send(dst_ip, dst_port,
                 net_socket_local_port(idx), (const u8*)buf, (u16)len);
    {
        /* once-per-socket breadcrumb */
        static bool snd_seen[MAX_SOCKETS];
        if (!snd_seen[idx]) {
            snd_seen[idx] = true;
            serial_printf("[UDPSND] sk=%u -> %u.%u.%u.%u:%u lp=%u len=%ld\n",
                          (unsigned)idx, dst_ip[0], dst_ip[1], dst_ip[2],
                          dst_ip[3], dst_port,
                          net_socket_local_port(idx), (s64)len);
        }
    }
    return len;
}

// ---- Linux recvfrom(45) -----------------------------------------
// ssize_t recvfrom(fd, buf, len, flags, src_addr, src_addrlen_ptr);

static u64 sys_recvfrom_impl(u64 fd, u64 buf, u64 count,
                             u64 flags, u64 src_addr, u64 src_addrlen_ptr)
{
    if (!is_net_fd(fd)) return (u64)(s64)(-LINUX_EBADF);
    s32 idx = proc_fd_to_net(fd);
    if (idx < 0) return (u64)(s64)(-LINUX_EBADF);
    if (!user_range_ok(buf, count)) return (u64)(s64)(-LINUX_EFAULT);
    u8 src_ip[4];
    u16 src_port;
    s64 n;
    /* #ping-a6-mystery: some ring-3 callers arrive with a6 holding a
     * tiny value (e.g. 17) instead of the &addrlen pointer — treat
     * anything we cannot validate as "caller does not want the source
     * endpoint written" instead of faulting out. */
    bool have_alen = false;
    if (src_addrlen_ptr) {
        have_alen = user_range_ok(src_addrlen_ptr, 4);
        static bool rfn_warned;
        if (!rfn_warned && !have_alen) {
            rfn_warned = true;
            serial_printf("[RFN-WARN] a6=%lx not a pointer; ignoring\n",
                          src_addrlen_ptr);
        }
    }
    if (flags & 0x40) {                       /* MSG_DONTWAIT: drain+probe */
        n = net_socket_recvfrom(idx, (void*)buf, (u32)count,
                                src_ip, &src_port);
    } else {
        n = -1;
        for (int tries = 0; tries < 500; tries++) {
            n = net_socket_recvfrom(idx, (void*)buf, (u32)count,
                                    src_ip, &src_port);
            if (n >= 0) break;
            __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
            net_poll();
        }
    }

    if (n < 0) return (u64)(s64)(-LINUX_EAGAIN);

    {
        /* once-per-socket breadcrumb */
        static bool rcv_seen[MAX_SOCKETS];
        if (!rcv_seen[idx]) {
            rcv_seen[idx] = true;
            serial_printf("[UDPRCV] sk=%u <- %u.%u.%u.%u:%u len=%ld\n",
                          (unsigned)idx, src_ip[0], src_ip[1],
                          src_ip[2], src_ip[3], src_port, n);
        }
    }

    if (src_addr && user_range_ok(src_addr, 16)) {
        u8* sa = (u8*)src_addr;
        /* sin_family is NATIVE-endian (LE on x86_64): value 2 == {02,00}.
         * Writing the BE pair {00,02} made musl's resolver reject every
         * genuine DNS reply as a foreign address (#gai-drop). Port stays
         * network order; the address copies verbatim. */
        sa[0] = 2; sa[1] = 0;                 /* AF_INET native       */
        sa[2] = (u8)(src_port >> 8);
        sa[3] = (u8)(src_port & 0xFF);
        sa[4] = src_ip[0]; sa[5] = src_ip[1];
        sa[6] = src_ip[2]; sa[7] = src_ip[3];
        kmemset(sa + 8, 0, 8);
        if (have_alen) {
            *(u32*)src_addrlen_ptr = 16;
        }
    }
    return (u64)n;
}

// ---- Linux sendmsg(46) / recvmsg(47) ------------------------------
// struct msghdr (x86_64): name@0, namelen@8, iov@16, iovlen@24,
// control@32, controllen@40, flags@48. We honor the FIRST iovec only
// and zero the ancillary section — enough for busybox ping's
// msg-dialects and later for musl paths, without touching the
// recvfrom/sendto code above.

struct msghdr_k {
    u64    name;             /* void*                            */
    u32    namelen;
    u32    pad0;
    u64    iov;              /* struct iovec* {base,len}         */
    u64    iovlen;
    u64    control;
    u64    controllen;
    s32    flags;
    u32    pad1;
};

static bool copy_msghdr_in(u64 umh, struct msghdr_k* mh)
{
    if (!user_range_ok(umh, sizeof(struct msghdr_k))) return false;
    kmemcpy((void*)mh, (const void*)umh, sizeof(*mh));
    return true;
}

/* Send the first iovec's bytes; destination = msg_name if present,
 * else the connected peer. Same ARP-warm + icmp/udp branch as sendto
 * via direct reuse: we simply route through sys_sendto_impl with the
 * parsed buffer. */
static u64 sys_sendmsg_impl(u64 fd, u64 mh_u, u64 flags)
{
    struct msghdr_k mh;
    if (!copy_msghdr_in(mh_u, &mh)) return (u64)(s64)(-LINUX_EFAULT);
    if (is_unix_fd(fd)) {
        /* #wayland-transport: unix sendmsg with SCM_RIGHTS fd passing.
         * Data goes first, then the fd set joins the pending queue —
         * the receiver pops one set per recvmsg that returns bytes
         * (documented simplification of the stream/cmsg pairing). */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0) return (u64)(s64)(-LINUX_EBADF);
        if (!mh.iov || !user_range_ok(mh.iov, 16) || mh.iovlen < 1)
            return (u64)(s64)(-LINUX_EFAULT);
        u64 base = *(u64*)mh.iov;
        u64 blen = *(u64*)(mh.iov + 8);
        s32 enc[UNIX_MAX_FDS];
        int nenc = 0;
        s32 clen = unix_parse_cmsg_rights(mh.control, mh.controllen,
                                          enc, &nenc);
        if (clen < 0) return (u64)(s64)(-LINUX_EFAULT);
        if (blen && !user_range_ok(base, blen))
            return (u64)(s64)(-LINUX_EFAULT);
        ufdmsg_t m;
        m.n = 0;
        for (int i2 = 0; i2 < nenc; i2++) {
            if (!fdref_bump_enc(enc[i2])) continue;   /* in-flight ref */
            m.enc[m.n++] = enc[i2];
        }
        if (blen) {
            s64 sn = unix_send_stream(uidx, end0,
                                      (const u8*)base, blen);
            if (sn < 0) {
                for (int i2 = 0; i2 < m.n; i2++)
                    fdref_dec_enc(m.enc[i2]);
                return (u64)(s64)sn;
            }
        }
        if (m.n && !unix_pend_push(&g_unich[uidx], end0, &m)) {
            for (int i2 = 0; i2 < m.n; i2++)
                fdref_dec_enc(m.enc[i2]);
            return (u64)(s64)(-105);                  /* ENOBUFS */
        }
        serial_printf("[UNIX] sendmsg idx=%d end0=%d bytes=%lu fds=%d\n",
                      uidx, (int)end0, (unsigned long)blen, m.n);
        return (u64)blen;
    }
    if (!is_net_fd(fd)) return (u64)(s64)(-LINUX_EBADF);
    if (!mh.iov || !user_range_ok(mh.iov, 16) || mh.iovlen < 1)
        return (u64)(s64)(-LINUX_EFAULT);

    u64 base = *(u64*)mh.iov;                 /* iovec.iov_base   */
    u64 blen = *(u64*)(mh.iov + 8);           /* iovec.iov_len    */
    if (!blen) return 0;

    u64 dest = 0;
    if (mh.name && mh.namelen >= 8 && user_range_ok(mh.name, 16)) {
        dest = mh.name;
    }
    u64 rc = sys_sendto_impl(fd, base, blen,
                             (u64)(s32)mh.flags ? (u64)(s32)mh.flags
                                                : flags,
                             dest);
    /* Honesty pass: zero out what callers inspect after a send. */
    if ((s64)rc >= 0 && user_range_ok(mh_u, sizeof(struct msghdr_k))) {
        struct msghdr_k* mhw = (struct msghdr_k*)mh_u;
        mhw->controllen = 0;
        mhw->control    = 0;
        mhw->flags      = 0;
    }
    return rc;
}

/* Receive into as many iovecs as the datagram fills; source endpoint
 * (UDP) or loopback-zero (ICMP) goes into msg_name. Blocking loop is
 * intentionally identical to sys_recvfrom_impl. */
static u64 sys_recvmsg_impl(u64 fd, u64 mh_u, u64 flags)
{
    struct msghdr_k mh;
    int rmc_ret = -999;
    if (!copy_msghdr_in(mh_u, &mh)) return (u64)(s64)(-LINUX_EFAULT);
    if (is_unix_fd(fd)) {
        /* #wayland-transport: unix recvmsg — data from the channel
         * ring, one pending SCM_RIGHTS set delivered as cmsg.     */
        bool end0 = false;
        s32 uidx = proc_fd_to_unix(fd, &end0);
        if (uidx < 0) { rmc_ret = -LINUX_EBADF; goto rmsg_out; }
        if (!mh.iov || !user_range_ok(mh.iov, 16) || mh.iovlen < 1) {
            rmc_ret = -LINUX_EFAULT; goto rmsg_out;
        }
        u64 base = *(u64*)mh.iov;
        u64 blen = *(u64*)(mh.iov + 8);
        if (!base || blen > 0x10000 || !user_range_ok(base, blen)) {
            rmc_ret = -LINUX_EFAULT; goto rmsg_out;
        }
        bool dw = ((flags | (u64)mh.flags) & 0x40) != 0;
        s64 n = unix_recv_stream(uidx, end0, (u8*)base, blen, dw);
        if (n < 0) { rmc_ret = (int)n; goto rmsg_out; }
        bool delivered = false;
        if (n > 0) {
            ufdmsg_t fm;
            if (unix_pend_pop(&g_unich[uidx], end0, &fm)) {
                s32 got = unix_emit_cmsg_rights(mh.control, mh.controllen,
                                                &fm, mh_u);
                delivered = (got > 0);
            }
        }
        {
            struct msghdr_k* mhw = (struct msghdr_k*)mh_u;
            mhw->flags = 0;
            if (!delivered) mhw->controllen = 0;
        }
        serial_printf("[UNIX] recvmsg idx=%d end0=%d bytes=%ld fds=%d\n",
                      uidx, (int)end0, n, delivered ? 1 : 0);
        rmc_ret = (int)n;
        goto rmsg_out;
    }
    if (!is_net_fd(fd)) { rmc_ret = -LINUX_EBADF; goto rmsg_out; }
    /* FIX(#rmsg-wrong-slot): translate process fd -> NET socket index
     * exactly like recvfrom does; passing the raw fd indexed the WRONG
     * slot of the socket table (always empty => phantom EAGAIN). */
    s32 rmsg_idx = proc_fd_to_net(fd);
    if (!mh.iov || !user_range_ok(mh.iov, 16) ||
        (mh.iovlen < 1 || mh.iovlen > 4)) {
        rmc_ret = -LINUX_EFAULT; goto rmsg_out;
    }

    /* First iovec carries the same receiving duty as recvfrom:
     * datagram semantics mean ONE ring entry per call anyway. */
    u64 base = *(u64*)mh.iov;
    u64 blen = *(u64*)(mh.iov + 8);
    if (!base || blen > 0x10000 || !user_range_ok(base, blen)) {
        rmc_ret = -LINUX_EFAULT; goto rmsg_out;
    }

    u8 src_ip[4]; u16 src_port = 0;
    s64 n;
    bool is_stream;
    {
        extern int net_socket_type(int);
        is_stream = ((net_socket_type(rmsg_idx) & 0xFF) == SOCK_STREAM);
    }
    if (is_stream) {
        /* #apk-fetch: libfetch reads the HTTP response through
         * recvmsg(); a STREAM socket must take the TCP path (ring
         * recvfrom never sees stream data). 0 = peer FIN EOF. */
        extern int net_socket_stream_recv(int, void*, u32);
        extern bool net_socket_has_eof(int);
        n = -1;
        int cap = ((flags | (u64)mh.flags) & 0x40) ? 1 : 500;
        for (int tries = 0; tries < cap; tries++) {
            n = net_socket_stream_recv(rmsg_idx, (void*)base,
                                       (u32)blen);
            if (n != -1) break;
            __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
            net_poll();
        }
        serial_printf("[RMSG] idx=%d blen=%ld n=%ld tries=%d\n",
                      (unsigned)rmsg_idx, (s64)blen, n, cap);
        if (n < 0 && net_socket_has_eof(rmsg_idx)) n = 0;
        if (n < 0) {
            rmc_ret = -LINUX_EAGAIN; goto rmsg_out;
        }
    } else {
    if ((flags | (u64)mh.flags) & 0x40) {     /* MSG_DONTWAIT     */
        n = net_socket_recvfrom(rmsg_idx, (void*)base, (u32)blen,
                                src_ip, &src_port);
    } else {
        n = -1;
        for (int tries = 0; tries < 500; tries++) {
            n = net_socket_recvfrom(rmsg_idx, (void*)base, (u32)blen,
                                    src_ip, &src_port);
            if (n >= 0) break;
            __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
            net_poll();
        }
    }
    if (n < 0) {
        rmc_ret = -LINUX_EAGAIN; goto rmsg_out;
    }
    }
    if (!is_stream && mh.name && mh.namelen >= 8 &&
        user_range_ok(mh.name, 16)) {
        u8* sa = (u8*)mh.name;
        sa[0] = 2; sa[1] = 0;                 /* AF_INET native       */
        sa[2] = (u8)(src_port >> 8);
        sa[3] = (u8)(src_port & 0xFF);
        sa[4] = src_ip[0]; sa[5] = src_ip[1];
        sa[6] = src_ip[2]; sa[7] = src_ip[3];
        kmemset(sa + 8, 0, 8);
        if (user_range_ok(mh_u + 8, 4)) *(u32*)(mh_u + 8) = 16;
    }
    {
        /* no cmsg support: controllen=0, flags=0 */
        struct msghdr_k* mhw = (struct msghdr_k*)mh_u;
        mhw->controllen = 0;
        mhw->flags      = 0;
    }
    rmc_ret = (int)n;

rmsg_out:
    return (u64)(s64)rmc_ret;
}


// ---- table -------------------------------------------------------

static s32 sched_current_pid_for_syscall(void)
{
    extern s32 sched_current_pid(void);
    return sched_current_pid();
}

/* last-syscall breadcrumbs for PF forensics (#pfdump attribution) */
volatile u64 g_last_sc_nr  = 0;
volatile u64 g_last_sc_pid = 0;

u64 syscall_handler(u64 num, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    {
        extern s32 sched_current_pid(void);
        serial_printf("[SC] pid=%ld nr=%ld\n", (s64)sched_current_pid(), (s64)num);
        g_last_sc_pid = (u64)(s64)sched_current_pid();
        g_last_sc_nr  = num;
    }
    switch (num) {
    case LINUX_NR_read:   return sys_read_impl  (a1, a2, a3);
    case LINUX_NR_write:  return sys_write_impl (a1, a2, a3);
    case LINUX_NR_open:   return sys_open_impl  (a1, a2, a3);
    case LINUX_NR_close:  return sys_close_impl (a1);
    case LINUX_NR_mmap:   return sys_mmap_impl  (a1, a2, a3, a4, a5, a6);
    case LINUX_NR_munmap: return sys_munmap_impl(a1, a2);
    case LINUX_NR_ioctl:  return sys_ioctl_impl (a1, a2, a3);
    case LINUX_NR_poll:   return sys_poll_impl  (a1, a2, a3);
    case 23:              return sys_select_impl(a1, a2, a3, a4, a5);
    case LINUX_NR_writev: return sys_writev_impl(a1, a2, a3);
    case LINUX_NR_readv:  return sys_readv_impl (a1, a2, a3);
    case LINUX_NR_brk:    return sys_brk_impl   (a1);
    case 4:              return sys_stat_impl   (a1, a2);
    case 5:              return sys_fstat_impl  (a1, a2);
    case 21:             return sys_access_impl (a1, a2);
    case 39:             return sys_getpid_impl ();
    case 228:            return sys_clock_gettime_impl(a1, a2);
    case 96:             return sys_gettimeofday_impl(a1, a2);
    case 83:             return sys_mkdir_impl(a1, a2);
    case 258:            return sys_mkdirat_impl(a1, a2, a3);
    case 133:            return sys_mknodat_impl((u64)(s64)AT_FDCWD, a1, a2, a3);
    case 259:            return sys_mknodat_impl(a1, a2, a3, a4);
    case 87:             return sys_unlinkat_impl((u64)(s64)AT_FDCWD, a1, 0);
    case 263:            return sys_unlinkat_impl(a1, a2, a3);
    case 82:             return sys_renameat_impl((u64)(s64)AT_FDCWD, a1,
                                                  (u64)(s64)AT_FDCWD, a2);
    case 264:            return sys_renameat_impl(a1, a2, a3, a4);
    case 266:            return sys_symlinkat_impl(a1, a2, a3);
    case 267:            return sys_readlinkat_impl(a1, a2, a3, a4);
    case 89:             return sys_readlinkat_impl((u64)(s64)AT_FDCWD, a1,
                                                    a2, a3);
    case 260:            return 0;               /* fchownat: single-user root */
    case 268:            return 0;               /* fchmodat: mode not enforced */
    case 280:            return 0;               /* utimensat: no wall-clock yet */
    case 138:            return 0;               /* setfsuid: no-op     */
    case 139:            return 0;               /* setfsgid: no-op     */
    case 73: {                                    /* flock: no contention */
        if (a1 >= PROC_FD_MAX || !proc_fds[a1].used)
            return (u64)(s64)(-LINUX_EBADF);
        return 0;                                 /* LOCK_SH/EX/UN all ok */
    }
    case 137: {                                   /* statfs(path, buf)   */
        /* #statfs-wrong-buf-arg (#apk-exit-malcheck ROOT CAUSE):
         * the x86_64 Linux ABI is statfs(const char* path in rdi,
         * struct statfs* buf in rsi) — the OUTPUT struct is a2.
         * The old code wrote the whole struct to a1 — the PATH STRING
         * POINTER — annihilating 128 bytes around a live mallocng
         * chunk (apk's "/cache" path string): the chunk's extended
         * reserved field at end-4 was overwritten with f_bsize
         * (0x1000), and the exit-time free() of that chunk tripped
         * mallocng's assert -> a_crash -> #GP 0x6e372c, minutes after
         * a fully successful "OK: 0 MiB in 2 packages".
         *
         * Layout note: musl x86_64 struct statfs is exactly 120 bytes
         * (f_fsid@56, f_namelen@64, f_frsize@72, f_flags@80,
         * f_spare[4]@88). The old 128-byte statfs_k had an extra
         * f_favail shifting every field after f_ffree AND overwrote 8
         * bytes past musl's struct. Field offsets below now match
         * musl bit-for-bit and exactly 120 bytes are written. */
        struct statfs_musl {
            u64 f_type, f_bsize, f_blocks, f_bfree, f_bavail;
            u64 f_files, f_ffree;
            u64 f_fsid;                /* fsid_t = 2×u32, 8 bytes    */
            u64 f_namelen, f_frsize, f_flags;
            u64 f_spare[4];
        };
        if (!user_range_ok(a1, 1)) return (u64)(s64)(-LINUX_EFAULT);
        if (!user_range_ok(a2, sizeof(struct statfs_musl)))
            return (u64)(s64)(-LINUX_EFAULT);
        /* honest -ENOENT for bogus paths: also guarantees we never
         * scribble over caller memory for a failed lookup */
        {
            const char* path = (const char*)a1;
            u64 plen = 0;
            while (path[plen] && plen < 256) plen++;
            if (!user_range_ok(a1, plen + 1))
                return (u64)(s64)(-LINUX_EFAULT);
            fs_node_t* n = fs_resolve_path_follow(path);
            if (!n) return (u64)(s64)(-LINUX_ENOENT);
        }
        struct statfs_musl* s = (struct statfs_musl*)a2;
        kmemset(s, 0, sizeof(*s));
        s->f_type   = 0xEF53;                     /* "ext2-like" enough   */
        s->f_bsize  = 4096;
        s->f_blocks = 1 << 20;
        s->f_bfree  = s->f_blocks - 1024;
        s->f_bavail = s->f_bfree;
        s->f_files  = 1 << 16;
        s->f_ffree  = s->f_files - 128;
        s->f_namelen = 255;
        s->f_frsize = 4096;
        return 0;
    }
    case 14:             return 0;               /* rt_sigprocmask  */
    case 95:             return 0x12u;           /* umask: fixed 022 */
    case 102:            return 0;               /* getuid          */
    case 107:            return 0;               /* geteuid         */
    case 32:             return sys_dup_impl     (a1);
    case 33:             return sys_dup2_impl    (a1, a2);
    case 57:             return sys_fork_impl    ();
    case 59:             return sys_execve_impl  (a1, a2, a3);
    case 61:             return sys_wait4_impl   (a1, a2, a3, 0);
    case 54:             return sys_setsockopt_impl(a1,a2,a3,a4,a5);
    case 55:             return sys_getsockopt_impl(a1,a2,a3,a4,a5);
    case 41:             return sys_socket_impl (a1, a2, a3);
    case 42:             return sys_connect_impl(a1, a2, a3);
    case 49:             return sys_bind_impl   (a1, a2, a3);   /* bind  */
    case 50:             return sys_listen_impl (a1, a2);       /* listen*/
    case 43:             return sys_accept_impl (a1, a2, a3);   /* accept*/
    case 288:            return sys_accept_impl (a1, a2, a3);   /* accept4*/
    case 44:             return sys_sendto_impl (a1, a2, a3, a4, a5);
    case 45:             return sys_recvfrom_impl(a1, a2, a3, a4, a5, a6);
    case 46:             return sys_sendmsg_impl(a1, a2, a3);   /* sendmsg*/
    case 47:             return sys_recvmsg_impl(a1, a2, a3);   /* recvmsg*/
    case 22:             return sys_pipe2_impl  (a1, 0);        /* pipe   */
    case 293:            return sys_pipe2_impl  (a1, a2);       /* pipe2  */
    case 53:             return sys_socketpair_impl(a1, a2, a3, a4, a5);
    /* epoll: REAL x86_64 numbers (232 wait, 233 ctl, 281 pwait).
     * The old 289/290 mapping was probe-only — musl always issues
     * 233/232 and would have gotten -EPERM. */
    case 232:            return sys_epoll_wait_impl(a1, a2, a3, a4);
    case 233:            return sys_epoll_ctl_impl(a1, a2, a3, a4);
    case 281:            return sys_epoll_wait_impl(a1, a2, a3, a4);
    case 291:            return sys_epoll_create1_impl(a1);
    /* #wl-substrate additions (readv/writev already wired above via
     * LINUX_NR_* constants — implementations predate this block) */
    case 77:             return sys_ftruncate_impl(a1, (s64)a2);
    case 202:            return sys_futex_impl(a1, a2, a3, a4);
    case 253:            return sys_timerfd_create_impl(a1, a2);
    case 254:            return sys_timerfd_settime_impl(a1, a2, a3, a4);
    case 255:            return sys_timerfd_gettime_impl(a1, a2);
    case 271:            return sys_ppoll_impl(a1, a2, a3, a4, a5);
    case 289:            return sys_signalfd4_impl((s64)(s64)a1,
                                                   a2, a3, a4);
    case 290:            return sys_eventfd2_impl(a1, a2);
    case 292:            return sys_dup2_impl(a1, a2);   /* dup3     */
    case 318:            return sys_getrandom_impl(a1, a2, a3);
    case 319:            return sys_memfd_create_impl(a1, a2);
    /* ---- BusyBox / musl support ------------------------------------*/
    case LINUX_NR_exit + 171:                                   /* 231 */
        return sys_exit_impl(a1);         /* exit_group == exit here   */
    case 8:              return sys_lseek_impl(a1, (s64)a2, a3);
    case 63:             return sys_uname_impl(a1);
    case 110:            return sys_getppid_impl();
    case 158:            return sys_arch_prctl_impl(a1, a2);
    case 217:            return sys_getdents64_impl(a1, a2, a3);
    case 257:            return sys_openat_impl(a1, a2, a3, a4);
    case 262:            return sys_newfstatat_impl(a1, a2, a3, a4);
    /* Legacy stat family: BusyBox ls probes every dirent through
     * lstat(=nr 6; previously fell to default -> -EPERM -> "Operation
     * not permitted" per entry). stat(4)/fstat(5) were already wired
     * above; lstat(6) joins them — no symlinks exist, so lstat==stat. */
    case 6:              return sys_stat_impl(a1, a2);
    case 10:             return 0;               /* mprotect          */
    case 28:             return 0;               /* madvise           */
    case 35:             return sys_nanosleep_impl(a1, a2);
    /* futex(202): real impl — see #wl-substrate block above */
    case 218:            return 1;               /* set_tid_address   */
    case 111:            return sys_getpgrp_impl();  /* getpgrp       */
    case 62:             return sys_kill_impl((s32)(s64)a1,
                                              (s32)(s64)a2);
                                  /* kill(2): CONT/STOP/TSTP really
                                   * delivered (scheduler-level park);
                                   * other signals remain success no-ops
                                   * until handlers reach syscall return
                                   * paths. History: a plain error or a
                                   * plain success mattered to ash's
                                   * fg-pgrp retry loop (see job code). */
    case 98: {                                   /* getrusage          */
        /* Zero-filled rusage is honest here: no CPU accounting yet.
         * Layout: int who; struct rusage* usage (144 bytes, 18 u64s). */
        if (!user_range_ok(a2, 144)) return (u64)(s64)(-LINUX_EFAULT);
        u64* r = (u64*)a2;
        for (int i = 0; i < 18; i++) r[i] = 0;
        return 0;
    }
    case 121: {                                  /* getpgid(pid)
         * FIX(#ash-tty): BusyBox ash enables job control only after
         * `while (getpgrp() != fg_pgrp) kill(0,SIGTTIN)`, and musl maps
         * getpgrp() onto SYS_getpgid (=121 on x86_64). Returning an
         * error left ash in the SIGTTIN retry loop forever ("can't
         * access tty" / spin trio nr=16+62+121). */
        s32 qpid = (s32)a1;
        extern void* scheduler_task_table(void);
        task_t* tbl = (task_t*)scheduler_task_table();
        s32 me = sched_current_pid_for_syscall();
        if (qpid == 0) {
            task_t* cur = NULL;
            if (tbl) for (s32 i = 0; i < TASK_MAX_TASKS; i++)
                if (tbl[i].active && tbl[i].pid == me) cur = &tbl[i];
            if (!cur) return 0;
            return (cur->pgrp > 0) ? (u64)cur->pgrp : (u64)cur->pid;
        }
        if (tbl) {
            for (s32 i = 0; i < TASK_MAX_TASKS; i++)
                if (tbl[i].active && tbl[i].pid == qpid) {
                    task_t* t = &tbl[i];
                    return (t->pgrp > 0) ? (u64)t->pgrp : (u64)t->pid;
                }
        }
        return (u64)(s64)(-3);                    /* -ESRCH          */
    }
    case 109:            return sys_setpgid_impl(a1, a2); /* setpgid  */
    case 112:            return sys_setsid_impl();   /* setsid        */
    /* ---- busybox sh needs a REAL fcntl(F_DUPFD/F_DUPFD_CLOEXEC): the
     * old success-stub returned 0, so ash's F_DUPFD_CLOEXEC(1030) probe
     * "allocated" fd 0 — closing it later destroyed STDIN. ----------*/
    case 72: {                                    /* fcntl             */
        if (a1 >= PROC_FD_MAX || !proc_fds[a1].used)
            return (u64)(s64)(-LINUX_EBADF);
        switch ((u64)a2) {
        case 0:                                   /* F_DUPFD           */
        case 1030: {                              /* F_DUPFD_CLOEXEC   */
            /* FIX(#ash-tty): stdin/stdout/stderr are console fds the
             * ramfs table cannot see. ash dups fd 0 to gain a tty fd
             * for ioctl probing — a dup of stdio only needs A SLOT,
             * not an fs backend (marked fs_fd<0 sentinel). File fds
             * duplicate their real ramfs descriptor. */
            s32 nd;
            if (a1 <= 2) {
                nd = proc_fd_alloc_from((s32)(-(s32)a1 - 2), (s32)a3);
            } else {
                /* Sentinel fds (-2..) duplicate as sentinels too —
                 * ash re-dups its /dev/tty handle. Real FILE fds must
                 * alias with an incremented refcount (#ash-script):
                 * busybox ash moves script fds to >=10 via F_DUPFD
                 * and immediately closes the low fd, expecting the
                 * description to survive (POSIX dup semantics).
                 * NET-encoded fds (fs_fd<=-10) alias through the
                 * socket layer's own refcount (#ping-dup2).          */
                s32 back = proc_fds[a1].fs_fd;
                bool rolled = false;
                if (back >= 0) {
                    extern s32 fs_dup(u32);
                    back = fs_dup((u32)back);
                    if (back < 0) return (u64)(s64)(-LINUX_EBADF);
                    rolled = true;
                } else if (fd_is_pipe_enc(back)) {
                    pipe_ref_inc(fd_pipe_index(back), fd_pipe_is_write(back));
                    rolled = true;
                } else if (fd_is_unix_enc(back)) {
                    /* #wayland-transport: unix channels alias through
                     * their per-end refcount like pipes do.          */
                    unix_ref_inc(fd_unix_index(back),
                                 fd_unix_is_end0(back));
                    rolled = true;
                } else if (back <= FD_NET_BASE) {
                    extern int net_socket_dup(int);
                    if (net_socket_dup(fd_net_index(
                            proc_fds[a1].fs_fd)) != 0)
                        return (u64)(s64)(-LINUX_EBADF);
                    rolled = true;
                }
                nd = proc_fd_alloc_from(proc_fds[a1].fs_fd, (s32)a3);
                if (nd < 0 && rolled) {
                    extern void net_socket_free(int);
                    extern s32 fs_close(u32);   /* roll the ref back   */
                    s32 origv = proc_fds[a1].fs_fd;
                    fdref_dec_enc(origv);       /* all classes        */
                }
            }
            return (nd < 0) ? (u64)(s64)(-24)     /* -EMFILE           */
                            : (u64)nd;
        }
        case 1:  return 0;                        /* F_GETFD           */
        case 2:  return 0;                        /* F_SETFD (ignored) */
        case 3:  return 2;                        /* F_GETFL: O_RDWR   */
        case 4:  return 0;                        /* F_SETFL (ignored) */
        default: return 0;
        }
    }
    /* ---- BusyBox `sh` support (minimal real implementations) -------*/
    case 79: {                                    /* getcwd(buf,size)  */
        /* No per-process cwd yet: the shell root is "/". */
        const char* cwd = "/";
        u64 len = 2;
        if (!a1 || a2 < len) return (u64)(s64)(-34); /* -ERANGE */
        kmemcpy((void*)a1, cwd, len);
        return a2 ? len : 0;
    }
    case 13:             return 0;               /* rt_sigaction: ok  */
    case 302:            return -38;              /* prlimit64stub    */
    case 269:            return 0;               /* faccessat         */
    /* getrandom(318): real impl — see #wl-substrate block above */
    case 99:             return 0;               /* sysinfo           */
    case 200:            return 0;               /* tkill: delivered  */
    case LINUX_NR_exit:   return sys_exit_impl  (a1);
    default:
        serial_printf("[SYSCALL] unimplemented nr=%ld\n", (s64)num);
        return (u64)(s64)(-LINUX_EPERM);
    }
}

void syscall_init(void)
{
    syscall_kstack_top = syscall_kstack_default;

    // Enable EFER.SCE — without this bit the SYSCALL instruction
    // raises #UD in user mode.
    {
        u64 efer = rdmsr(0xC0000080);           /* MSR_EFER */
        efer |= (1ULL << 0);                    /* SCE      */
        wrmsr(0xC0000080, efer);
    }

    // Configure MSR for syscall/sysret
    //
    // STAR MSR layout (per AMD/Intel SDM):
    //   SYSCALL:  CS = STAR[47:32], SS = STAR[47:32] + 8
    //   SYSRETQ: CS = STAR[63:48] + 16, SS = STAR[63:48] + 8
    //
    // We set STAR[47:32] = kernel CS so SYSCALL enters with CS=kcode,
    // SS=kdata. The return path in syscall_entry.S always uses IRETQ
    // with explicit user CS/SS — SYSRET is never executed, so the
    // known "SYSRET returns kernel SS" limitation does not apply.
    u64 star = ((u64)GDT_KCODE_SEL << 32) | ((u64)GDT_KCODE_SEL << 48);
    wrmsr(0xC0000081, star);

    // LSTAR: syscall entry point
    wrmsr(0xC0000082, (u64)syscall_entry);

    // FMASK: clear IF on syscall entry (interrupts off in kernel)
    wrmsr(0xC0000084, 0x200);

    vga_print("[SYSCALL] Linux x86_64 ABI table ready "
              "(read=0 write=1 exit=60)\n");
}
