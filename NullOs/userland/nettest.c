/*
 * nettest — NullOs native network round-trip prober.
 *
 * Pure Linux-x86_64 ABI: socket() -> bind() -> connect() ->
 * write("payload") -> read() echo reply -> exit(status).
 *
 * Isolates the KERNEL net chain from busybox internals:
 *   exit(42) == full UDP round-trip through slirp,
 *   anything else prints diagnostics and exits non-zero.
 */

#define SYS_read    0
#define SYS_write   1
#define SYS_exit    60
#define SYS_socket  41
#define SYS_connect 42
#define SYS_bind    49

#define AF_INET     2
#define SOCK_DGRAM  2

struct sockaddr_in {
    unsigned short family;      /* AF_INET                     */
    unsigned short port_be;     /* network byte order          */
    unsigned char  addr[4];
    unsigned char  zero[8];
};

static long syscall3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

static void wr(const char* s, long n) { syscall3(SYS_write, 1, (long)s, n); }
static void say(const char* s) {
    long n = 0;
    while (s[n]) n++;
    wr(s, n);
}

static void say_hex(unsigned v) {
    char b[9];
    for (int i = 7; i >= 0; i--) {
        int d = v & 0xF;
        b[i] = d < 10 ? ('0' + d) : ('a' + d - 10);
        v >>= 4;
    }
    wr(b, 8);
}

static const char PAY[] = "nettx-native-ok";

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    say("[NETTEST] start\n");

    long fd = syscall3(SYS_socket, AF_INET, SOCK_DGRAM, 0);
    say("[NETTEST] socket=");
    say_hex((unsigned)(fd & 0xFFFFFFFF));
    say("\n");
    if (fd < 0) goto fail;

    /* bind :40001 (bytes big-endian on wire: 0x9C41 -> 9C 41) */
    struct sockaddr_in la;
    la.family  = AF_INET;
    la.port_be = 0x419C;                 /* 40001 swapped once by hand */
    la.addr[0] = 10; la.addr[1] = 0; la.addr[2] = 2; la.addr[3] = 15;
    for (int i = 0; i < 8; i++) la.zero[i] = 0;

    long r = syscall3(SYS_bind, fd, (long)&la, sizeof(la));
    say("[NETTEST] bind=");
    say_hex((unsigned)(r & 0xFFFFFFFF));
    say("\n");
    if (r < 0) goto fail;

    /* connect 10.0.2.2:9000 */
    struct sockaddr_in ra;
    ra.family  = AF_INET;
    ra.port_be = 0x2823;                 /* 9000 -> 23 28 */
    ra.addr[0] = 10; ra.addr[1] = 0; ra.addr[2] = 2; ra.addr[3] = 2;
    for (int i = 0; i < 8; i++) ra.zero[i] = 0;

    r = syscall3(SYS_connect, fd, (long)&ra, sizeof(ra));
    say("[NETTEST] connect=");
    say_hex((unsigned)(r & 0xFFFFFFFF));
    say("\n");
    if (r < 0) goto fail;

    long n = syscall3(SYS_write, (long)fd, (long)PAY, (long)(sizeof(PAY) - 1));
    say("[NETTEST] wrote=");
    say_hex((unsigned)(n & 0xFFFFFFFF));
    say("\n");
    if (n != (long)(sizeof(PAY) - 1)) goto fail;

    /* wait for the echo reply */
    char buf[64];
    long got = -1;
    for (int attempt = 0; attempt < 20 && got <= 0; attempt++) {
        got = syscall3(SYS_read, (long)fd, (long)buf, (long)sizeof(buf));
        say("[NETTEST] try ");
        say_hex((unsigned)attempt);
        say(" got=");
        say_hex((unsigned)(got & 0xFFFFFFFF));
        say("\n");
    }
    if (got > 0 && got == (long)(sizeof(PAY) - 1)) {
        say("[NETTEST] RX=");
        wr(buf, got);
        say("\n[NETTEST] ROUNDTRIP OK\n");
        return 42;
    }

fail:
    say("[NETTEST] FAILED\n");
    return 7;
}
