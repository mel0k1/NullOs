/*
 * nsh — NullOs native multi-call busybox-style utility.
 * Raw Linux-ABI syscalls only, no libc. Applet selected by argv[0].
 *
 * applets: echo, ls, cat, uname, pwd, clear
 */

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_lseek   8
#define SYS_uname   63
#define SYS_exit    60
#define SYS_getdents64 217
#define SYS_openat  257

#define AT_FDCWD   -100L
#define O_RDONLY   0L

/* linux_dirent64 */
struct dent64 {
    unsigned long long ino;
    long long          off;
    unsigned short     reclen;
    unsigned char      type;      /* 4=dir 8=reg */
    /* char name[] follows */
};

static long sc3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

static void out(const char* s, long len) {
    if (len < 0) { len = 0; while (s[len]) len++; }
    sc3(SYS_write, 1, (long)s, len);
}

static void puts_(const char* s) { out(s, -1); }

static void put_num(long v) {
    char b[24];
    int i = (int)sizeof(b) - 1;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) b[--i] = '0';
    while (u) { b[--i] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[--i] = '-';
    out(b + i, sizeof(b) - 1 - (unsigned long)i);
}

static const char* base_name(const char* p) {
    const char* r = p;
    while (*p) { if (*p == '/') r = p + 1; p++; }
    return r;
}

/* ------------------------------- applets ------------------------- */

static int app_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) out(" ", 1);
        puts_(argv[i]);
    }
    out("\n", 1);
    return 0;
}

static int app_pwd(int argc, char** argv) {
    (void)argc; (void)argv;
    puts_("/\n");                       /* ramfs root-only cwd for now */
    return 0;
}

static int app_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    static const char seq[] =
        "\033[2J\033[H";                /* ANSI clear — VGA ignores it,
                                           serial terminals honor it   */
    out(seq, (long)sizeof(seq) - 1);
    return 0;
}

struct utsname_s {
    char sysname[65], nodename[65], release[65],
         version[65], machine[65], domain[65];
};

static int app_uname(int argc, char** argv) {
    (void)argc; (void)argv;
    static struct utsname_s u;
    sc3(SYS_uname, (long)&u, 0, 0);
    puts_(u.sysname);  out(" ", 1);
    puts_(u.nodename); out(" ", 1);
    puts_(u.release);  out(" ", 1);
    puts_(u.machine);  out("\n", 1);
    return 0;
}

static int app_cat(const char* path) {
    long fd = sc3(SYS_openat, AT_FDCWD, (long)path, O_RDONLY);
    if (fd < 0) {
        puts_("cat: cannot open "); puts_(path); out("\n", 1);
        return 1;
    }
    static char buf[4096];
    long n;
    while ((n = sc3(SYS_read, fd, (long)buf, (long)sizeof(buf))) > 0)
        out(buf, n);
    sc3(SYS_close, fd, 0, 0);
    return 0;
}

static int app_ls(const char* path) {
    long fd = sc3(SYS_openat, AT_FDCWD, (long)path, O_RDONLY);
    if (fd < 0) {
        puts_("ls: cannot open "); puts_(path); out("\n", 1);
        return 1;
    }

    /* seek back to start in case this fd was used before */
    sc3(SYS_lseek, fd, 0, 0);

    static char buf[4096];
    static char line[300];

    long n;
    while ((n = sc3(SYS_getdents64, fd, (long)buf, (long)sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dent64* d = (struct dent64*)(buf + off);
            const char* name = buf + off + 19;
            int li = 0;
            for (const char* p = name; *p && li < 250; p++)
                line[li++] = *p;
            if (d->type == 4) line[li++] = '/';
            line[li++] = '\n';
            out(line, li);
            off += d->reclen;
        }
    }
    sc3(SYS_close, fd, 0, 0);
    return 0;
}

/* ------------------------------- dispatch ------------------------ */

int main(int argc, char** argv) {
    if (argc < 1) return 1;
    const char* app = base_name(argv[0]);

    if (app[0] == 'e' && app[1] == 'c')  return app_echo(argc, argv);
    if (app[0] == 'l' && app[1] == 's')  {
        if (argc < 2) return app_ls("/");
        for (int i = 1; i < argc; i++) app_ls(argv[i]);
        return 0;
    }
    if (app[0] == 'c' && app[1] == 'a')  {
        if (argc < 2) { puts_("usage: cat FILE\n"); return 1; }
        return app_cat(argv[1]);
    }
    if (app[0] == 'u')                   return app_uname(argc, argv);
    if (app[0] == 'p')                   return app_pwd(argc, argv);
    if (app[0] == 'c' && app[1] == 'l')  return app_clear(argc, argv);

    puts_("nsh: unknown applet: ");
    puts_(app);
    out("\n", 1);
    puts_("applets: echo ls cat uname pwd clear\n");
    return 1;
}
