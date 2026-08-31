/*
 * httpget — NullOs native minimal HTTP client over the kernel TCP stack.
 *
 * Usage (from native shell):
 *   elf /bin/httpget <dotted-ip> <port> <path> <outfile>
 * Example:
 *   elf /bin/httpget 10.0.2.2 8080 f.txt /tmp/got.txt
 *
 * Sends a plain HTTP/1.0 GET, streams the response to <outfile>,
 * prints received bytes and body preview. exit(42) on success.
 */

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_exit   60
#define SYS_socket 41
#define SYS_connect 42

#define AF_INET     2
#define SOCK_STREAM 1

#define O_WRONLY    1
#define O_CREAT     0100
#define O_TRUNC     01000

struct sockaddr_in {
    unsigned short family;
    unsigned short port_be;
    unsigned char  addr[4];
    unsigned char  zero[8];
};

static long sc3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

static void wr(const char* s, long n) { sc3(SYS_write, 1, (long)s, n); }
static void say(const char* s) { long n=0; while (s[n]) n++; wr(s,n); }

static void say_num(long v) {
    char b[24]; int i=(int)sizeof(b)-1;
    unsigned long u = v<0 ? (unsigned long)-v : (unsigned long)v;
    if (!u) b[--i]='0';
    while (u){ b[--i]='0'+(u%10); u/=10; }
    if (v<0) b[--i]='-';
    wr(b+i, sizeof(b)-1-(unsigned long)i);
}

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }

/* dotted-ip -> 4 bytes */
static int parse_ip(const char* s, unsigned char out[4]) {
    int oct=0, val=0;
    while (*s && oct<4) {
        if (*s>='0' && *s<='9') val=val*10+(*s-'0');
        else if (*s=='.') { out[oct++]=(unsigned char)val; val=0; }
        s++;
    }
    out[oct]=(unsigned char)val;
    return (oct==3)?0:-1;
}

static char reqbuf[256];
static char rspbuf[16384];

int main(int argc, char** argv)
{
    if (argc < 5) {
        say("[HTTPGET] usage: <ip> <port> <path> <outfile>\n");
        return 2;
    }
    const char* ip_s   = argv[1];
    int port           = 0;
    for (const char* p=argv[2]; *p; p++) port = port*10 + (*p-'0');
    const char* path   = argv[3];
    const char* outp   = argv[4];

    unsigned char ip[4];
    if (parse_ip(ip_s, ip)<0) { say("[HTTPGET] bad ip\n"); return 2; }

    say("[HTTPGET] ip=");
    say(ip_s); say(" port="); say_num(port); say(" path=/"); say(path);
    say("\n");

    /* NOTE: argv strings arrive NUL-separated by our loader (verified
     * in proc.c stack layout), so slen() stops at each argument end. */

    long fd = sc3(SYS_socket, AF_INET, SOCK_STREAM, 0);
    say("[HTTPGET] socket="); say_num(fd); say("\n");
    if (fd<0) return 7;

    struct sockaddr_in ra;
    ra.family  = AF_INET;
    ra.port_be = (unsigned short)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
    ra.addr[0]=ip[0]; ra.addr[1]=ip[1]; ra.addr[2]=ip[2]; ra.addr[3]=ip[3];
    for (int i=0;i<8;i++) ra.zero[i]=0;

    /* Retry connect a few times: the first SYN can race the ARP
     * warm-up for the gateway (#ping-first-shot family), which made
     * paste-harness boots flaky (connect=-110 on the first try). */
    long rc = -110;
    for (int attempt = 0; attempt < 4 && rc < 0; attempt++) {
        if (attempt > 0) {
            sc3(SYS_close, fd, 0, 0);
            fd = sc3(SYS_socket, AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return 7;
            /* crude ~300ms pause via read on a bad fd is not portable;
             * burn time with a dummy short connect attempt instead of
             * sleeping (no nanosleep here). */
            for (volatile long spin=0; spin<3000000; spin++) { }
        }
        rc = sc3(SYS_connect, fd, (long)&ra, sizeof(ra));
        say("[HTTPGET] connect#"); say_num(attempt+1); say("=");
        say_num(rc); say("\n");
    }
    if (rc<0) return 7;

    /* build request */
    int q=0;
    const char* l1="GET /";  for(int i=0;l1[i];i++) reqbuf[q++]=l1[i];
    for(const char* p=path;*p;p++) reqbuf[q++]=*p;
    const char* l2=" HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    for(int i=0;l2[i];i++) reqbuf[q++]=l2[i];

    long wn = sc3(SYS_write, fd, (long)reqbuf, q);
    say("[HTTPGET] request sent bytes="); say_num(wn); say("\n");
    if (wn != q) return 7;

    /* stream the whole response into rspbuf */
    long total=0;
    for (;;) {
        if ((unsigned long)total >= sizeof(rspbuf)-1) break;
        long n = sc3(SYS_read, fd, (long)(rspbuf+total),
                     (long)(sizeof(rspbuf)-1-total));
        if (n <= 0) {
            if (n == 0) { say("[HTTPGET] peer closed (EOF)\n"); }
            else        { say("[HTTPGET] read err="); say_num(n); say("\n"); }
            break;
        }
        total += n;
    }
    sc3(SYS_close, fd, 0, 0);

    say("[HTTPGET] got total bytes="); say_num(total); say("\n");
    if (total<=0) return 7;

    /* split header/body at CRLFCRLF */
    long hdr_end=-1;
    for (long i=0;i+3<total;i++) {
        if (rspbuf[i]==13 && rspbuf[i+1]==10 &&
            rspbuf[i+2]==13 && rspbuf[i+3]==10) { hdr_end=i; break; }
    }
    if (hdr_end<0) { say("[HTTPGET] no header terminator\n"); return 7; }

    say("[HTTPGET] header:\n----------\n");
    wr(rspbuf, hdr_end);
    say("\n----------\n");

    long body_len = total - (hdr_end+4);
    say("[HTTPGET] body bytes="); say_num(body_len); say("\n");

    long ofd = sc3(SYS_open, (long)outp,
                   O_WRONLY|O_CREAT|O_TRUNC, 0644);
    say("[HTTPGET] open("); say(outp); say(")="); say_num(ofd); say("\n");
    if (ofd<0) return 7;

    long wn2 = sc3(SYS_write, ofd, (long)(rspbuf+hdr_end+4), body_len);
    sc3(SYS_close, ofd, 0, 0);
    say("[HTTPGET] wrote-to-file="); say_num(wn2); say("\n");

    say("[HTTPGET] body preview: ");
    long prev = body_len<80 ? body_len : 80;
    wr(rspbuf+hdr_end+4, prev);
    say("\n[HTTPGET] OK\n");
    return 42;
}
