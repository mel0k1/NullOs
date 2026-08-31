/*
 * tcp_srv (srvtest) — NullOs native TCP echo-server smoke test.
 *
 * Usage: elf /bin/srvtest <port>
 *
 * Binds INADDR_ANY:<port>, listen(backlog=4), blocks in accept(),
 * reads the peer's stream until FIN, echoes everything back, closes
 * both fds and exits 42. The host-side harness connects through
 * slirp hostfwd, half-closes after its payload and compares bytes —
 * this exercises listen()/accept()/TCP-server RX+TX end-to-end.
 */

#define SYS_read   0
#define SYS_write  1
#define SYS_close  3
#define SYS_exit   60
#define SYS_socket 41
#define SYS_accept 43
#define SYS_bind   49
#define SYS_listen 50

#define AF_INET     2
#define SOCK_STREAM 1

struct sockaddr_in {
    unsigned short family;
    unsigned short port_be;
    unsigned char  addr[4];
    unsigned char  zero[8];
};

static long sc(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

static void wr(const char* s, long n) { sc(SYS_write, 1, (long)s, n); }
static void say(const char* s) { long n = 0; while (s[n]) n++; wr(s, n); }

static void say_num(long v) {
    char b[24]; int i = (int)sizeof(b) - 1;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    if (!u) b[--i] = '0';
    while (u) { b[--i] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[--i] = '-';
    wr(b + i, sizeof(b) - 1 - (unsigned long)i);
}

static char buf[2048];

int main(int argc, char** argv)
{
    if (argc < 2) { say("[SRV] usage: <port>\n"); return 2; }

    int port = 0;
    for (const char* p = argv[1]; *p; p++) port = port * 10 + (*p - '0');

    long s = sc(SYS_socket, AF_INET, SOCK_STREAM, 0);
    say("[SRV] sock="); say_num(s); say("\n");
    if (s < 0) return 7;

    struct sockaddr_in sa;
    sa.family  = AF_INET;
    sa.port_be = (unsigned short)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
    for (int i = 0; i < 4; i++) sa.addr[i] = 0;
    for (int i = 0; i < 8; i++) sa.zero[i] = 0;

    long rc = sc(SYS_bind, s, (long)&sa, sizeof(sa));
    say("[SRV] bind="); say_num(rc); say("\n");
    if (rc != 0) return 7;

    rc = sc(SYS_listen, s, 4, 0);
    say("[SRV] listen="); say_num(rc); say(" port=");
    say_num(port); say(" READY\n");
    if (rc != 0) return 7;

    struct sockaddr_in peer;
    unsigned int plen = 16;
    long c = sc(SYS_accept, s, (long)&peer, (long)&plen);
    if (c < 0) {
        say("[SRV] accept err="); say_num(c); say("\n");
        return 7;
    }
    say("[SRV] accept fd="); say_num(c);
    say(" peer="); say_num(peer.addr[0]); wr(".", 1);
    say_num(peer.addr[1]); wr(".", 1);
    say_num(peer.addr[2]); wr(".", 1);
    say_num(peer.addr[3]);
    /* port arrives big-endian in the sockaddr the kernel filled */
    say(":"); say_num((long)(((peer.port_be & 0xFF) << 8)
                             | (peer.port_be >> 8)));
    say("\n");

    long tot = 0;
    int eager = 0;                       /* bounded EAGAIN retries      */
    for (;;) {
        if (tot >= (long)sizeof(buf)) break;
        long n = sc(SYS_read, c, (long)(buf + tot),
                    (long)(sizeof(buf) - tot));
        if (n == 0) { say("[SRV] peer fin (EOF)\n"); break; }
        if (n < 0) {
            if (n == -11 && ++eager < 5) continue;   /* EAGAIN */
            say("[SRV] read err="); say_num(n); say("\n");
            break;
        }
        tot += n;
        say("[SRV] rx_total="); say_num(tot); say("\n");
    }

    long wn = tot > 0 ? sc(SYS_write, c, (long)buf, tot) : 0;
    say("[SRV] echoed="); say_num(wn); say(" of ");
    say_num(tot); say("\n");

    sc(SYS_close, c, 0, 0);
    sc(SYS_close, s, 0, 0);
    say("[SRV] done\n");
    return 42;
}
