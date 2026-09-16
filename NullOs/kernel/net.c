/* ============================================================
 * net.c — NullOs Network Stack (Ethernet, ARP, IPv4, ICMP)
 * ============================================================
 *
 * Minimal polling-based network stack:
 *   - ARP request/reply for layer-2 address resolution
 *   - ICMP echo reply for ping
 *   - Designed to be called from shell command context
 *
 * QEMU user-net: slirp stack responds to ARP automatically,
 * and echoes ICMP through the virtual gateway.
 */

#include "../include/net.h"
#include "../include/rtl8139.h"
#include "../include/vga.h"
#include "../include/serial.h"
#include "../include/string.h"
#include "../include/timer.h"

/* Forward declarations for internal functions used by public API */
static void arp_send_request(const u8* target_ip);
static const u8* arp_resolve(const u8* ip);
static u16 checksum(const u8* data, u32 len);
static void sock_init(void);
void net_udp_send(const u8* dst_ip, u16 dst_port,
                         u16 src_port, const u8* data, u16 len);
u16 net_htons(u16 v);

/* Our identity */
static u8 our_ip[4]     = {10, 0, 2, 15};

static u8 our_mac[6]    = {0, 0, 0, 0, 0, 0};   /* filled from NIC */

/* ARP cache: simple table, linear scan */
static arp_entry_t arp_table[ARP_TABLE_SIZE];
static u32 arp_entries = 0;

/* Stats */
static u64 rx_pkts = 0;
static u64 tx_pkts = 0;

/* Socket table */
#define MAX_SOCKETS       16

static net_socket_t sockets[MAX_SOCKETS];
static bool sockets_initialized = false;
static u32 urx_logs = 0;
static u32 udpdrop_logs = 0;
static u32 tcprx_logs = 0;
static u32 streamtx_logs = 0;

static void sock_init(void) {
    if (!sockets_initialized) {
        kmemset(sockets, 0, sizeof(sockets));
        sockets_initialized = true;
    }
}

/* TX frame buffer: was ONE shared static buffer — the heart of
 * #tcp-bigfetch-stall. A task preempted mid-build (IF=1 in every
 * mainline net path) let the successor build+transmit ITS frame
 * over the same bytes; the resumed task then transmitted a chimera
 * (foreign payload, own length) that slirp silently dropped — the
 * peer retransmitted forever and big fetches stalled at ~131KB.
 * Every sender now builds into a LOCAL frame buffer on its own
 * kernel stack; the only shared TX state left is the NIC's
 * descriptor ring, serialized inside rtl8139_send (TX-LOCK). */

/* ---- Helpers ------------------------------------------------------*/

static u16 checksum(const u8* data, u32 len)
{
    u32 sum = 0;
    for (u32 i = 0; i + 1 < len; i += 2) {
        sum += (u16)(data[i] << 8 | data[i + 1]);
    }
    if (len & 1) {
        sum += (u16)(data[len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    /* one-shot forensic breadcrumb */
    {
        static bool cs_dbg;
        if (!cs_dbg && len == 20) {
            cs_dbg = true;
            serial_printf("[CSUM] raw_sum=%04x folded=%04x ret=%04x "
                          "in=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
                          "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                          (unsigned)(sum), (unsigned)(~sum & 0xFFFF),
                          (unsigned)(u16)(~sum),
                          data[0],data[1],data[2],data[3],data[4],
                          data[5],data[6],data[7],data[8],data[9],
                          data[10],data[11],data[12],data[13],data[14],
                          data[15],data[16],data[17],data[18],data[19]);
        }
    }
    return (u16)(~sum);
}

static void dump_mac(const u8* mac) {
    vga_printf("%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- ARP -----------------------------------------------------------*/

static arp_entry_t* arp_lookup(const u8* ip) {
    for (u32 i = 0; i < arp_entries; i++) {
        if (arp_table[i].valid &&
            kmemcmp(arp_table[i].ip, ip, 4) == 0) {
            return &arp_table[i];
        }
    }
    return 0;
}

static void arp_cache_add(const u8* ip, const u8* mac) {
    if (arp_entries >= ARP_TABLE_SIZE) {
        /* Evict oldest (index 0), shift down */
        for (u32 i = 0; i < ARP_TABLE_SIZE - 1; i++) {
            arp_table[i] = arp_table[i + 1];
        }
        arp_entries--;
    }
    arp_entry_t* e = &arp_table[arp_entries++];
    e->valid = true;
    kmemcpy(e->ip, ip, 4);
    kmemcpy(e->mac, mac, 6);
}

static void arp_send_request(const u8* target_ip) {
    /* local frame (#tcp-bigfetch-stall): no cross-task aliasing */
    u8 tx_frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    arp_pkt_t* arp = (arp_pkt_t*)(tx_frame + sizeof(eth_hdr_t));

    /* Broadcast destination MAC */
    kmemset(eth->dst_mac, 0xFF, 6);

    const u8* src_mac = rtl8139_get_mac();
    kmemcpy(eth->src_mac, src_mac, 6);
    eth->ethertype = 0x0608;               /* 0x0806 BE */

    arp->hw_type    = 0x0100;              /* hw=1, BE */
    arp->proto_type = 0x0008;              /* 0x0800, BE */
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->opcode     = 0x0100;              /* request=1, BE */
    kmemcpy(arp->sender_mac, src_mac, 6);
    kmemcpy(arp->sender_ip, our_ip, 4);
    kmemset(arp->target_mac, 0, 6);
    kmemcpy(arp->target_ip, target_ip, 4);

    rtl8139_send(tx_frame, sizeof(eth_hdr_t) + sizeof(arp_pkt_t));
}

static void arp_send_reply(const arp_pkt_t* request) {
    /* local frame (#tcp-bigfetch-stall) */
    u8 tx_frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    arp_pkt_t* arp = (arp_pkt_t*)(tx_frame + sizeof(eth_hdr_t));

    /* Reply goes back to the sender's MAC */
    kmemcpy(eth->dst_mac, request->sender_mac, 6);
    kmemcpy(eth->src_mac, rtl8139_get_mac(), 6);
    eth->ethertype = 0x0608;               /* 0x0806, BE */

    arp->hw_type    = (1 >> 8) | (1 << 8);
    arp->proto_type = 0x0008;              /* 0x0800, BE */
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->opcode     = 0x0200;              /* reply=2, BE */
    kmemcpy(arp->sender_mac, rtl8139_get_mac(), 6);
    kmemcpy(arp->sender_ip, our_ip, 4);
    kmemcpy(arp->target_mac, request->sender_mac, 6);
    kmemcpy(arp->target_ip, request->sender_ip, 4);

    rtl8139_send(tx_frame, sizeof(eth_hdr_t) + sizeof(arp_pkt_t));
}

/* Resolve IP → MAC. Returns MAC pointer or NULL if unknown.
 * Sends ARP request if not in cache. Caller should retry after
 * a short delay or use the returned entry on next call.          */
static const u8* arp_resolve(const u8* ip) {
    arp_entry_t* e = arp_lookup(ip);
    if (e) return e->mac;

    /* Not in cache — send ARP request */
    arp_send_request(ip);
    return 0;
}

/* ---- ICMP ----------------------------------------------------------*/

static void icmp_echo_reply(const u8* incoming_ip_hdr,
                            const u8* icmp_data, u16 icmp_len) {
    const ip_hdr_t* req_ip = (const ip_hdr_t*)incoming_ip_hdr;

    /* local frame (#tcp-bigfetch-stall) */
    u8 tx_frame[NET_MAX_FRAME];
    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    ip_hdr_t*  ip  = (ip_hdr_t*)(tx_frame + sizeof(eth_hdr_t));
    u8*        icmp = tx_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t);

    /* Resolve destination MAC (should already be cached from ARP exchange) */
    const u8* dst_mac = arp_resolve(req_ip->src_ip);
    if (!dst_mac) {
        /* Force resolve by sending ARP request first */
        arp_send_request(req_ip->src_ip);
        /* For now, skip — caller can retry */
        return;
    }

    /* Ethernet header */
    kmemcpy(eth->dst_mac, dst_mac, 6);
    kmemcpy(eth->src_mac, rtl8139_get_mac(), 6);
    eth->ethertype = 0x0008;               /* 0x0800 BE */

    /* IP header: swap src/dst from request */
    u16 total_len = sizeof(ip_hdr_t) + icmp_len;
    ip->ver_ihl    = 0x45;                        /* IPv4, IHL=5 (20 bytes) */
    ip->tos        = 0;
    ip->total_len  = (u16)((total_len >> 8) | ((total_len & 0xFF) << 8));
    ip->id         = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IPPROTO_ICMP;
    ip->checksum   = 0;
    kmemcpy(ip->src_ip, our_ip, 4);
    kmemcpy(ip->dst_ip, req_ip->src_ip, 4);

    /* IP checksum */
    /* internet checksum is a NUMBER; the wire needs it big-endian:
     * storing the raw value little-endian byte-swapped it and slirp
     * silently dropped every frame we sent (ICMP pings too). */
    ip->checksum = net_htons(checksum((const u8*)ip, sizeof(ip_hdr_t)));

    /* ICMP echo reply: copy request data, change type to REPLY */
    icmp[0] = ICMP_ECHO_REPLY;
    icmp[1] = 0;
    /* Copy id+seq+data from original request (starting at offset 4) */
    for (u16 i = 4; i < icmp_len; i++) {
        icmp[i] = icmp_data[i];
    }

    /* Recalculate ICMP checksum over entire ICMP message */
    icmp[2] = 0;
    icmp[3] = 0;
    u16 csum = checksum(icmp, icmp_len);
    icmp[2] = (csum >> 8);
    icmp[3] = (csum & 0xFF);

    /* Send */
    rtl8139_send(tx_frame, sizeof(eth_hdr_t) + total_len);
}

/* ---- Main packet dispatcher ----------------------------------------*/
/* Called periodically (or manually) to check for incoming frames      */

static void net_udp_receive(const u8* src_ip, const u8* udp_data, u16 udp_len);
static void net_icmp_pace_walk(void);          /* forward: user-icmp pace */

/* ---- User-space ping sockets (SOCK_RAW / SOCK_DGRAM + IPPROTO_ICMP) - */
/* A single busybox `ping` owns the only raw socket in a session;       */
/* replies are delivered to ANY open icmp-flavor socket, oldest first.  */

/* user-ICMP pace/retry machinery (#ping-no-sigalrm) — state first,    */
/* logic lives next to the TX core further below.                       */
typedef struct {
    bool armed;
    u32  t_sent_ms;
    u16  len;
    u16  tries;
    u8   dst[4];
    u8   data[1480];
} icmp_pace_t;
static icmp_pace_t g_icmp_pace[MAX_SOCKETS];
#define ICMP_PACE_INTERVAL_MS  1000u
#define ICMP_PACE_MAX_TRIES    30u

static bool net_has_icmp_socket(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].ip_proto == IPPROTO_ICMP) return true;
    }
    return false;
}

/* Queue an inbound ICMP message (full header+data, what raw readers
 * expect) into the first icmp-flavor socket. Mirrors UDP ring rules:
 * 512B cap per entry, drop when full. */
static void net_icmp_deliver(const u8* src_ip, const u8* msg, u16 len) {
    if (!sockets_initialized || len == 0) return;
    sock_init();
    for (int i = 0; i < MAX_SOCKETS; i++) {
        net_socket_t* sk = &sockets[i];
        if (!sk->used || sk->ip_proto != IPPROTO_ICMP) continue;
        if ((u32)(sk->rx_head - sk->rx_tail) >= SOCK_RX_DEPTH) continue;
        u32 q   = sk->rx_head % SOCK_RX_DEPTH;
        u16 n   = (len > 512) ? 512 : len;
        kmemcpy(sk->rx[q].data, msg, n);
        kmemcpy(sk->rx[q].src_ip, src_ip, 4);
        sk->rx[q].src_port = 0;
        sk->rx[q].len      = n;
        sk->rx_head++;
        serial_printf("[ICMPRX] idx=%d type=%u len=%u\n", i, msg[0], n);
        /* NOTE (#ping-no-sigalrm): do NOT disarm the pacer on this
         * delivery — busybox ping has no working ITIMER/SIGALRM here
         * to schedule its next request, so the kernel keeps sending
         * a fresh sequence each second until the count is satisfied.
         * The 30-try GIVEUP cap bounds the flow when nobody reads. */
        return;
    }
}
void net_receive_frame(const u8* data, u32 len) {
    if (len < sizeof(eth_hdr_t)) return;

    const eth_hdr_t* eth = (const eth_hdr_t*)data;
    u16 etype = (eth->ethertype >> 8) | (eth->ethertype << 8); /* LE→BE */

    rx_pkts++;
    {
        /* one-shot breadcrumb: prove the wire lives */
        static bool rxf_dbg;
        if (!rxf_dbg) {
            rxf_dbg = true;
            serial_printf("[RXF] first frame type=%04x len=%u\n",
                          etype, len);
        }
    }

    if (etype == ETHERTYPE_ARP && len >= sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
        const arp_pkt_t* arp = (const arp_pkt_t*)(data + sizeof(eth_hdr_t));
        u16 opcode = (arp->opcode >> 8) | (arp->opcode << 8);

        /* Cache sender's MAC/IP mapping */
        arp_cache_add(arp->sender_ip, arp->sender_mac);

        if (opcode == ARP_OP_REQUEST) {
            /* Is it asking for OUR IP? */
            if (kmemcmp(arp->target_ip, our_ip, 4) == 0) {
                arp_send_reply(arp);
            }
        }
    }
    else if (etype == ETHERTYPE_IPV4 && len >= sizeof(eth_hdr_t) + sizeof(ip_hdr_t)) {
        const ip_hdr_t* ip = (const ip_hdr_t*)(data + sizeof(eth_hdr_t));
        u16 ihl = (ip->ver_ihl & 0x0F) * 4;
        u16 total_len = (ip->total_len >> 8) | (ip->total_len << 8);

        /* FIX(#icmp-total-len-smash): total_len is a RAW big-endian
         * field from the received frame — it was never checked
         * against the ACTUAL frame length, so a crafted packet with
         * total_len=0xFFFF made icmp_echo_reply copy ~64KB through a
         * 1480-byte stack buffer (kernel stack smash; matching ~64KB
         * overread of the poll buffer). Clamp to the received bytes
         * and reject nonsense IHL values (RFC: >= 20). */
        u16 frame_payload = (u16)(len - sizeof(eth_hdr_t));
        if (ihl < 20 || ihl > frame_payload) return;
        if (total_len > frame_payload) total_len = frame_payload;
        if (total_len < sizeof(ip_hdr_t)) return;

        /* Only process packets addressed to us */
        if (kmemcmp(ip->dst_ip, our_ip, 4) != 0) return;

        if (ip->protocol == IPPROTO_ICMP && total_len >= ihl + 8) {
            const u8* icmp_data = data + sizeof(eth_hdr_t) + ihl;
            u16 icmp_len = total_len - ihl;
            u8 icmp_type = icmp_data[0];

            if (icmp_type == ICMP_ECHO_REQ && !net_has_icmp_socket()) {
                /* Classic kernel responder: nobody in userland owns
                 * ICMP right now, so we answer external pings the
                 * old way. When a ping socket exists, requests get
                 * forwarded instead of answered by both of us. */
                icmp_echo_reply(data + sizeof(eth_hdr_t), icmp_data,
                                icmp_len);
            } else {
                /* Any ICMP message when a raw reader exists: echo
                 * replies (busybox ping consumes type 0), and other
                 * types like dest-unreachable surface to the app
                 * instead of being swallowed. */
                net_icmp_deliver(ip->src_ip, icmp_data, icmp_len);
            }
        }
        else if (ip->protocol == IPPROTO_UDP && total_len >= ihl + 8) {
            const u8* udp_data = data + sizeof(eth_hdr_t) + ihl;
            net_udp_receive(ip->src_ip, udp_data, total_len - ihl);
        }
        else if (ip->protocol == IPPROTO_TCP && total_len >= ihl + NET_TCP_HEADER_LEN) {
            const u8* seg = data + sizeof(eth_hdr_t) + ihl;
            net_tcp_receive(ip->src_ip, seg, (u32)(total_len - ihl));
        }
    }
}

/* Poll all pending RX frames */

/* #tcp-bigfetch-stall poll-guard: net_poll mutates shared state —
 * the drain buffer, socket RX rings, RTX queues, TCP state machines.
 * Two tasks draining concurrently (fetch task preempted mid-dispatch
 * while the shell's poll loop drained in its place) corrupt both.
 * Concurrent callers now SKIP the drain: the holder runs to
 * completion in bounded time (every path inside is non-blocking)
 * and the skipper's outer wait loop retries on its next pass. */
static volatile int poll_active = 0;

void net_poll(void) {
    if (__sync_lock_test_and_set(&poll_active, 1)) return;  /* already draining */
    u8 poll_buf[NET_MAX_FRAME];   /* local: no cross-task aliasing */
    int n;
    while ((n = rtl8139_poll(poll_buf, NET_MAX_FRAME)) > 0) {
        net_receive_frame(poll_buf, n);
    }
    /* Retransmission timers run at poll cadence: every blocking
     * syscall loop (read/connect/poll/accept) drains the NIC here,
     * which doubles as the re-arm site for unacked TCP segments. */
    {
        extern void net_tcp_rtx_walk(void);
        net_tcp_rtx_walk();
    }
    /* user-raw-ICMP pacing (#ping-no-sigalrm): replay overdue echo
     * requests with bumped seq so busybox ping counts arrivals */
    net_icmp_pace_walk();
    __sync_lock_release(&poll_active);
}

/* ---- Public API -----------------------------------------------------*/

void net_init(void) {
    kmemset(arp_table, 0, sizeof(arp_table));
    arp_entries = 0;
    rx_pkts = 0;
    tx_pkts = 0;

    const u8* mac = rtl8139_get_mac();
    kmemcpy(our_mac, mac, 6);
}

u64 net_get_rx_packets(void) { return rx_pkts; }
u64 net_get_tx_packets(void) { return tx_pkts; }


/* ---- Public socket API (for syscall.c) ---------------------------*/

int net_socket_alloc(void) {
    sock_init();
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            kmemset(&sockets[i], 0, sizeof(net_socket_t));
            sockets[i].used   = true;
            sockets[i].sk_refs = 1;
            return i;
        }
    }
    return -1;
}

/* POSIX: an aliased socket survives until its LAST alias closes */
int net_socket_dup(int fd) {
    if (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used) {
        if (sockets[fd].sk_refs < 255) sockets[fd].sk_refs++;
        return 0;
    }
    return -1;
}

void net_socket_free(int fd) {
    if (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used) {
        if (sockets[fd].sk_refs > 1) {
            sockets[fd].sk_refs--;             /* other aliases alive */
            return;
        }
        kmemset(&sockets[fd], 0, sizeof(net_socket_t));
    }
}

bool net_socket_has_data(int fd) {
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return false;
    /* A TCP socket with peer-FIN pending must surface readable: the
     * consumer's read() then returns 0 (EOF) instead of hanging. */
    if (sockets[fd].sock_type == SOCK_STREAM && sockets[fd].tcp_eof) {
        return true;
    }
    return sockets[fd].rx_head != sockets[fd].rx_tail;
}

bool net_arp_ready(const u8* ip) {
    return arp_lookup(ip) != 0;
}

/* Public ARP re-request entry for syscall-layer retries */
void net_arp_kick(const u8* ip) {
    arp_send_request(ip);
}

/* ---- default-route aware next-hop (#route-fix) ----------------------
 * slirp answers ARP only for the virtual 10.0.2.0/24 hosts; frames to
 * INTERNET destinations must be addressed to the GATEWAY's MAC while
 * keeping the original destination in the IP header. Without this,
 * every off-subnet connect/send silently died waiting for an ARP
 * reply nobody was going to give (busybox wget info.cern.ch: "can't
 * connect to remote host: Operation timed out"). */
static const u8* net_next_hop(const u8* dst_ip) {
    if (dst_ip[0] == 10 && dst_ip[1] == 0 && dst_ip[2] == 2)
        return dst_ip;                        /* on virtual subnet    */
    return (const u8*)GATEWAY_IP;             /* default gateway      */
}

bool net_arp_ready_route(const u8* dst_ip) {
    return arp_lookup(net_next_hop(dst_ip)) != 0;
}

void net_arp_kick_route(const u8* dst_ip) {
    arp_send_request(net_next_hop(dst_ip));
}

/* First free port in ephemeral range; caller may retry next value.
 * Single-direction counter keeps multiple sockets distinct. */
static u16 sock_ephemeral_next = 32768u;
static u16 sock_ephemeral_alloc(void) {
    for (int tries = 0; tries < (65536 - 32768); tries++) {
        u16 p = sock_ephemeral_next++;
        bool taken = false;
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (sockets[i].used && sockets[i].local_port == p) {
                taken = true;
                break;
            }
        }
        if (!taken) return p;
    }
    return 0;
}

/* bind(2): claim local port (wildcard IP semantics are implicit).
 * Port 0 => kernel assigns an ephemeral port. Refuses collisions with
 * another bound socket (POSIX EADDRINUSE for UDP without SO_REUSEADDR). */
int net_socket_bind(int fd, const u8* ip, u16 port) {
    (void)ip;                                    /* wildcard binding only */
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return -8;
    if (port == 0) {
        port = sock_ephemeral_alloc();
        if (!port) return -11;                   /* allocation exhausted   */
    } else {
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (i != fd && sockets[i].used &&
                sockets[i].local_port == port) {
                return -98;                      /* EADDRINUSE             */
            }
        }
    }
    sockets[fd].local_port = port;
    return 0;
}

bool net_socket_is_bound(int fd) {
    return (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used &&
            sockets[fd].local_port != 0);
}

int net_socket_connect(int fd, const u8* ip, u16 port) {
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return -1;
    kmemcpy(sockets[fd].remote_ip, ip, 4);
    sockets[fd].remote_port = port;
    sockets[fd].connected = true;
    return 0;
}

int net_socket_sendto(int fd, const u8* ip, u16 port,
                      const u8* data, u16 len)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return -1;
    net_udp_send(ip, port, sockets[fd].local_port, data, len);
    return len;
}

int net_socket_recvfrom(int fd, void* buf, u32 count,
                        u8* out_src_ip, u16* out_src_port)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return -1;

    /* Drain pending NIC packets into socket queues */
    net_poll();

    net_socket_t* sk = &sockets[fd];
    if (sk->rx_head == sk->rx_tail) return -1;  /* EAGAIN */

    u32 idx = sk->rx_tail % SOCK_RX_DEPTH;
    u32 n = sk->rx[idx].len;
    if (n > count) n = count;
    kmemcpy(buf, sk->rx[idx].data, n);

    if (out_src_ip) kmemcpy(out_src_ip, sk->rx[idx].src_ip, 4);
    if (out_src_port) *out_src_port = sk->rx[idx].src_port;

    sk->rx_tail++;
    return (int)n;
}

bool net_socket_is_used(int fd) {
    return (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used);
}

bool net_socket_is_connected(int fd) {
    return (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used && sockets[fd].connected);
}

int net_socket_get_remote(int fd, u8* ip, u16* port) {
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return -1;
    kmemcpy(ip, sockets[fd].remote_ip, 4);
    *port = sockets[fd].remote_port;
    return 0;
}

u16 net_socket_local_port(int fd) {
    return (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used)
           ? sockets[fd].local_port : 0;
}

void net_socket_set_local_port(int fd, u16 port) {
    if (fd >= 0 && fd < MAX_SOCKETS) sockets[fd].local_port = port;
}


/* ---- Shell command: ping --------------------------------------------*/

void cmd_ping(int argc, char** argv) {
    /* local frame (#tcp-bigfetch-stall) */
    u8 tx_frame[NET_MAX_FRAME];
    if (argc < 2 || !rtl8139_is_ready()) {
        vga_print("Usage: ping <dotted-ip>\n");
        vga_print("Example: ping 10.0.2.2\n");
        if (!rtl8139_is_ready()) {
            vga_print("(NIC not initialized — run 'drivers init N' first)\n");
        }
        return;
    }

    /* Parse dotted decimal IP into 4-byte array */
    u8 target_ip[4] = {0};
    const char* s = argv[1];
    int octet = 0, val = 0;
    while (*s && octet < 4) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) val = 255;      /* FIX(#ping-octet): no silent wrap */
        } else if (*s == '.') {
            target_ip[octet++] = (u8)val;
            val = 0;
        }
        s++;
    }
    /* FIX(#ping-octet): after 4 dots octet==4 and the final store
     * wrote one byte past the 4-byte array (stack OOB write). */
    if (octet < 4) target_ip[octet] = (u8)val;

    vga_print("PING ");
    for (int i = 0; i < 4; i++) {
        vga_printf("%d%s", target_ip[i], i < 3 ? "." : "\n");
    }

    /* Step 1: ARP resolve */
    vga_print("Resolving ");
    vga_printf("%d.%d.%d.%d ...\n",
               target_ip[0], target_ip[1], target_ip[2], target_ip[3]);

    arp_send_request(target_ip);

    /* Poll for ARP reply (up to ~100 attempts) */
    bool resolved = false;
    for (int retry = 0; retry < 200 && !resolved; retry++) {
        net_poll();
        if (arp_lookup(target_ip)) resolved = true;
        /* busy-wait delay */
        for (volatile int d = 0; d < 500000; d++);
    }

    if (!resolved) {
        vga_print("ARP timeout — host unreachable\n");
        return;
    }

    const arp_entry_t* ae = arp_lookup(target_ip);
    vga_print("MAC: ");
    dump_mac(ae->mac);
    vga_print("\n");

    /* Step 2: Build ICMP echo request */
    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    ip_hdr_t*  ip  = (ip_hdr_t*)(tx_frame + sizeof(eth_hdr_t));
    u8*        icmp = tx_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t);

    u16 icmp_data_len = 64;                       /* payload: id+seq+56B data */

    kmemcpy(eth->dst_mac, ae->mac, 6);
    kmemcpy(eth->src_mac, rtl8139_get_mac(), 6);
    eth->ethertype = 0x0008;               /* 0x0800 BE */

    u16 total_len = sizeof(ip_hdr_t) + icmp_data_len;
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = (u16)((total_len >> 8) | (total_len << 8));
    ip->id         = 0x1234;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IPPROTO_ICMP;
    ip->checksum   = 0;
    kmemcpy(ip->src_ip, our_ip, 4);
    kmemcpy(ip->dst_ip, target_ip, 4);
    /* internet checksum is a NUMBER; the wire needs it big-endian:
     * storing the raw value little-endian byte-swapped it and slirp
     * silently dropped every frame we sent (ICMP pings too). */
    ip->checksum = net_htons(checksum((const u8*)ip, sizeof(ip_hdr_t)));

    /* ICMP echo request */
    icmp[0] = ICMP_ECHO_REQ;
    icmp[1] = 0;
    icmp[2] = 0;
    icmp[3] = 0;
    icmp[4] = 0x12;                               /* id */
    icmp[5] = 0x34;
    icmp[6] = 0;                                  /* seq hi */
    icmp[7] = 1;                                  /* seq lo */
    for (int i = 8; i < 64; i++) {
        icmp[i] = (u8)('A' + (i % 26));           /* fill with pattern */
    }

    u16 csum = checksum(icmp, icmp_data_len);
    icmp[2] = (csum >> 8);
    icmp[3] = (csum & 0xFF);

    /* Send */
    rtl8139_send(tx_frame, sizeof(eth_hdr_t) + total_len);
    tx_pkts++;
    vga_print("Sent 64-byte echo request.\n");

    /* Wait for reply — baseline captured NOW, after both the ARP
     * exchange and this send; anything that only ever counted the
     * earlier ARP reply as "our echo answer" was a false positive. */
    {
        u64 base_rx = rx_pkts;
        for (int retry = 0; retry < 3000; retry++) {
            net_poll();
            if (rx_pkts > base_rx) {
                vga_printf("Reply received! (%lu packets RX)\n",
                           (unsigned long)net_get_rx_packets());
                return;
            }
            for (volatile int d = 0; d < 10000; d++);
        }
    }

    vga_print("Request timed out.\n");
}


/* ============================================================
 * UDP — User Datagram Protocol
 * ============================================================ */
u16 net_htons(u16 v) { return (v >> 8) | (v << 8); }
u16 net_ntohs(u16 v) { return (v >> 8) | (v << 8); }

/* Compute UDP checksum over pseudo-header + header + data */
static u16 udp_checksum(const u8* src_ip, const u8* dst_ip,
                        const udp_hdr_t* hdr, u16 total_udp_len)
{
    /* Pseudo-header sum */
    u32 sum = ((src_ip[0] << 8 | src_ip[1]) +
               (src_ip[2] << 8 | src_ip[3]) +
               (dst_ip[0] << 8 | dst_ip[1]) +
               (dst_ip[2] << 8 | dst_ip[3]));
    sum += IPPROTO_UDP;
    sum += total_udp_len;

    /* Header words */
    const u16* h = (const u16*)hdr;
    for (int i = 0; i < 4; i++) {
        sum += (h[i] >> 8) | (h[i] << 8);
    }

    /* Data words (pad odd byte) */
    u32 data_len = total_udp_len - NET_UDP_HEADER_LEN;
    const u8* data = (const u8*)hdr + NET_UDP_HEADER_LEN;
    for (u32 i = 0; i + 1 < data_len; i += 2) {
        sum += (data[i] << 8) | data[i+1];
    }
    if (data_len & 1) {
        sum += (data[data_len-1] << 8);
    }

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

/* Send a UDP datagram: build UDP → IP → Ethernet → NIC */
void net_udp_send(const u8* dst_ip, u16 dst_port,
                  u16 src_port, const u8* data, u16 len)
{
    /* FIX(#udp-send-overflow): defensive cap — the frame buffer holds
     * exactly 1472 payload bytes; the syscall layer now caps too, but
     * any OTHER future caller must not be able to smash the stack. */
    if (len > (NET_MTU - 20 - 8)) len = (NET_MTU - 20 - 8);

    /* local frame (#tcp-bigfetch-stall) */
    u8 tx_frame[NET_MAX_FRAME];
    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    ip_hdr_t*  ip  = (ip_hdr_t*)(tx_frame + sizeof(eth_hdr_t));
    udp_hdr_t* udp = (udp_hdr_t*)(tx_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t));

    /* Resolve destination MAC */
    const u8* dst_mac = arp_resolve(net_next_hop(dst_ip));
    if (!dst_mac) {
        arp_send_request(net_next_hop(dst_ip));
        return;                                /* caller retries later */
    }

    kmemcpy(eth->dst_mac, dst_mac, 6);
    kmemcpy(eth->src_mac, rtl8139_get_mac(), 6);
    eth->ethertype = 0x0008;                    /* 0x0800 BE */

    u16 udp_total = NET_UDP_HEADER_LEN + len;

    /* UDP header */
    udp->src_port = net_htons(src_port);
    udp->dst_port = net_htons(dst_port);
    udp->length   = net_htons(udp_total);
    udp->checksum = 0;                          /* optional in IPv4 */

    /* Copy payload */
    u8* payload = tx_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + NET_UDP_HEADER_LEN;
    kmemcpy(payload, data, len);

    /* IP header */
    u16 ip_total = sizeof(ip_hdr_t) + udp_total;
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = (u16)((ip_total >> 8) | ((ip_total & 0xFF) << 8));
    ip->id         = 0x0001;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IPPROTO_UDP;
    ip->checksum   = 0;
    kmemcpy(ip->src_ip, our_ip, 4);
    kmemcpy(ip->dst_ip, dst_ip, 4);
    /* internet checksum is a NUMBER; the wire needs it big-endian:
     * storing the raw value little-endian byte-swapped it and slirp
     * silently dropped every frame we sent (ICMP pings too). */
    ip->checksum = net_htons(checksum((const u8*)ip, sizeof(ip_hdr_t)));

    rtl8139_send(tx_frame, sizeof(eth_hdr_t) + ip_total);
}

/* Send a user-built ICMP message verbatim: the raw-socket caller
 * (busybox ping) supplies the complete ICMP header incl. checksum,
 * exactly like Linux SOCK_RAW+IPPROTO_ICMP with IP_HDRINCL off —
 * we only wrap it in an IP header. Length cap 1480 = MTU-20. */

/* ---- user-ICMP pace/retry machinery (#ping-no-sigalrm) --------------
 * BusyBox ping schedules its per-second resends through
 * ITIMER_REAL/SIGALRM; our kernel cannot deliver signals to ring-3
 * handlers yet, so without help a single lost first request stalls
 * the whole run. The raw path records the latest echo request and
 * net_poll() replays it on a 1s cadence, BUMPING the ICMP sequence
 * (with a fresh checksum) so the consumer counts every arrival.
 * Any inbound delivery to the socket disarms the pacing.
 * (State declared near the top of this file for visibility.)          */

/* TX core shared by first-shot and paced replays */
static void icmp_tx_core(const u8* dst_ip, const u8* data, u16 len)
{
    /* local frame (#tcp-bigfetch-stall) */
    u8 tx_frame[NET_MAX_FRAME];
    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    ip_hdr_t*  ip  = (ip_hdr_t*)(tx_frame + sizeof(eth_hdr_t));

    /* Resolve destination MAC */
    const u8* dst_mac = arp_resolve(net_next_hop(dst_ip));
    if (!dst_mac) {
        arp_send_request(net_next_hop(dst_ip));
        return;                                /* next walk retries     */
    }

    kmemcpy(eth->dst_mac, dst_mac, 6);
    kmemcpy(eth->src_mac, rtl8139_get_mac(), 6);
    eth->ethertype = 0x0008;                    /* 0x0800 BE            */

    /* Payload after IP header */
    u8* payload = tx_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t);
    kmemcpy(payload, data, len);

    /* IP header: proto=ICMP, standard TTL */
    u16 ip_total = sizeof(ip_hdr_t) + len;
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = (u16)((ip_total >> 8) | ((ip_total & 0xFF) << 8));
    ip->id         = 0x0001;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = IPPROTO_ICMP;
    ip->checksum   = 0;
    kmemcpy(ip->src_ip, our_ip, 4);
    kmemcpy(ip->dst_ip, dst_ip, 4);
    ip->checksum = net_htons(checksum((const u8*)ip, sizeof(ip_hdr_t)));

    rtl8139_send(tx_frame, sizeof(eth_hdr_t) + ip_total);
}

void net_icmp_send(const u8* dst_ip, const u8* data, u16 len)
{
    if (!dst_ip || !data || len == 0) return;
    if (len > NET_MTU - sizeof(ip_hdr_t)) len = NET_MTU - sizeof(ip_hdr_t);

    /* TEMP DIAG kept small (#ping): surface missing-MAC moments */
    if (!arp_resolve(dst_ip)) {
        static u32 nomac_logs;
        if (nomac_logs < 3) {
            nomac_logs++;
            serial_printf("[ICMPNOMAC] want=%u.%u.%u.%u\n",
                          dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
        }
    }
    icmp_tx_core(dst_ip, data, len);
    {
        static bool icmpsnd_dbg;
        if (!icmpsnd_dbg) {
            icmpsnd_dbg = true;
            serial_printf("[ICMPTX] dst=%u.%u.%u.%u type=%u len=%u\n",
                          dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3],
                          data[0], len);
        }
    }
}

/* Arm (re-arm) the 1s pacer for this socket's raw flow */
void net_socket_icmp_arm(int fd, const u8* dst_ip,
                         const u8* data, u16 len)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !dst_ip || !data ||
        len == 0 || len > 1480) return;
    icmp_pace_t* p = &g_icmp_pace[fd];
    kmemcpy(p->dst, dst_ip, 4);
    kmemcpy(p->data, data, len);
    p->len       = len;
    p->t_sent_ms = (u32)timer_get_uptime_ms();
    p->tries     = 0;
    p->armed     = true;
}

/* Called from net_poll(): replay overdue echo requests, advancing the
 * ICMP sequence so the consuming app counts distinct replies. */
static void net_icmp_pace_walk(void)
{
    if (!sockets_initialized) return;
    u32 now = (u32)timer_get_uptime_ms();

    for (int i = 0; i < MAX_SOCKETS; i++) {
        icmp_pace_t* p = &g_icmp_pace[i];
        if (!sockets[i].used || sockets[i].ip_proto != IPPROTO_ICMP) {
            p->armed = false;
            continue;
        }
        if (!p->armed) continue;
        if ((u32)(now - p->t_sent_ms) < ICMP_PACE_INTERVAL_MS) continue;

        if (++p->tries > ICMP_PACE_MAX_TRIES) {
            p->armed = false;
            serial_printf("[ICMPPACE-GIVEUP] sk=%d tries=%u\n", i, p->tries);
            continue;
        }

        /* advance 16-bit seq at ICMP offset 6, then refresh checksum */
        u8* d   = p->data;
        u16 seq = (u16)((d[6] << 8) | d[7]);
        seq++;
        d[6] = (u8)(seq >> 8);
        d[7] = (u8)(seq & 0xFF);
        d[2] = 0; d[3] = 0;
        u16 csum = checksum(d, p->len);
        d[2] = (u8)(csum >> 8);
        d[3] = (u8)(csum & 0xFF);

        icmp_tx_core(p->dst, d, p->len);
        p->t_sent_ms = now;
        static u32 pace_logs;
        if (pace_logs < 8) {
            pace_logs++;
            serial_printf("[ICMPPACE] sk=%d try=%u seq=%u len=%u\n",
                          i, p->tries, seq, p->len);
        }
    }
}

/* ---- User-ping socket flavor helpers (syscall.c side) ----------------*/

void net_socket_set_proto(int fd, int proto) {
    if (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used)
        sockets[fd].ip_proto = (u8)proto;
}

bool net_socket_is_icmp(int fd) {
    return (fd >= 0 && fd < MAX_SOCKETS && sockets[fd].used &&
            sockets[fd].ip_proto == IPPROTO_ICMP);
}

/* Handle incoming UDP packet: dispatch to bound socket receive queue */
static void net_udp_receive(const u8* src_ip,
                            const u8* udp_data, u16 udp_len)
{
    if (!sockets_initialized || udp_len < NET_UDP_HEADER_LEN) return;

    const udp_hdr_t* hdr = (const udp_hdr_t*)udp_data;
    u16 dst_port = net_ntohs(hdr->dst_port);
    u16 src_port = net_ntohs(hdr->src_port); (void)src_port;

    sock_init();

    /* Find bound socket */
    bool matched = false;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].local_port == dst_port) {
            matched = true;
            net_socket_t* sk = &sockets[i];

            /* one-shot breadcrumb: UDP demux actually hit a socket */
            if (urx_logs < 8) urx_logs++;
            serial_printf("[UDPRX#%u] idx=%d dport=%u len=%u\n",
                          urx_logs, i, dst_port, udp_len);

            /* Enqueue into ring buffer */
            /* FIX(#rx-ring-guard-udp): the old guard compared a mod'ed
             * index `(head+1)%DEPTH` against the RAW monotonic tail —
             * once the socket had consumed >= 16 datagrams the test
             * could never fire again, and unread entries got silently
             * overwritten (same class as the already-fixed tcp_enqueue
             * / icmp guards). Fill-level form, like everywhere else. */
            if (sk->rx_head - sk->rx_tail >= SOCK_RX_DEPTH) return;

            u32 idx = sk->rx_head % SOCK_RX_DEPTH;
            u16 copy_len = udp_len - NET_UDP_HEADER_LEN;
            if (copy_len > 512) copy_len = 512;

            /* TEMP DIAG (#gai-parse): first 12 bytes of the DNS
             * payload (id, flags, qd/ancount...) once per socket. */
            {
                static bool udpdns_dbg[MAX_SOCKETS];
                /* FIX(#dnsrx-overread): only with a payload big enough
                 * — a bare 8-byte UDP header used to print 12 bytes of
                 * stale poll_buf stack contents to the serial console. */
                if (!udpdns_dbg[i] && src_port == 53 &&
                    udp_len >= NET_UDP_HEADER_LEN + 12) {
                    udpdns_dbg[i] = true;
                    serial_printf("[DNSRX] id=%02x%02x fl=%02x%02x "
                                  "qd=%02x%02x an=%02x%02x ns=%02x%02x "
                                  "ar=%02x%02x len=%u\n",
                                  udp_data[8], udp_data[9],
                                  udp_data[10], udp_data[11],
                                  udp_data[12], udp_data[13],
                                  udp_data[14], udp_data[15],
                                  udp_data[16], udp_data[17],
                                  udp_data[18], udp_data[19],
                                  copy_len);
                }
            }

            kmemcpy(sk->rx[idx].data,
                    udp_data + NET_UDP_HEADER_LEN, copy_len);
            sk->rx[idx].len = copy_len;
            kmemcpy(sk->rx[idx].src_ip, src_ip, 4);
            sk->rx[idx].src_port = net_ntohs(hdr->src_port);

            sk->rx_head++;
            return;
        }
    }
    if (!matched && udpdrop_logs < 8) {
        udpdrop_logs++;
        serial_printf("[UDPDROP#%u] dport=%u len=%u\n",
                      udpdrop_logs, dst_port, udp_len);
    }
}


/* ============================================================
 * TCP — minimal reliable client (active open, in-order RX)
 * ============================================================
 *
 * Scope decision: implement the OUTBOUND path that real programs
 * need first (busybox nc <host> <port>, wget): SYN handshake,
 * ESTABLISHED data exchange with per-segment ACKs, peer-FIN → EOF,
 * graceful FIN on close. LISTEN/accept is the natural next step.
 * Reliability model: slirp is loss-free on the user-net path, so
 * the only retransmission is the initial SYN inside the connect
 * loop; data segments rely on inline TX-complete waiting and the
 * host ACK clock. The 4KB rx window matches the socket ring
 * exactly (SOCK_RX_DEPTH * 1024).
 */

u32 net_htonl(u32 v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

int net_socket_set_type(int fd, int sock_type)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return -1;
    sockets[fd].sock_type = (u8)sock_type;
    return 0;
}

int net_socket_type(int fd)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return 0;
    return sockets[fd].sock_type;
}

bool net_socket_has_eof(int fd)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].used) return false;
    return sockets[fd].tcp_eof;
}

/* internet checksum over TCP segment + pseudo-header */
static u16 tcp_checksum(const u8* src_ip, const u8* dst_ip,
                        const u8* seg, u32 total_len)
{
    u32 sum = ((src_ip[0] << 8 | src_ip[1]) +
               (src_ip[2] << 8 | src_ip[3]) +
               (dst_ip[0] << 8 | dst_ip[1]) +
               (dst_ip[2] << 8 | dst_ip[3]));
    sum += IPPROTO_TCP;
    sum += total_len;

    for (u32 i = 0; i + 1 < total_len; i += 2) {
        sum += (seg[i] << 8) | seg[i + 1];
    }
    if (total_len & 1) {
        sum += (seg[total_len - 1] << 8);
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum);
}

/* Emit one TCP segment inside eth+ip with an EXPLICIT sequence
 * number. Drops silently when ARP is unresolved — the connect()
 * loop pre-warms it and slirp keeps it cached for the whole
 * connection lifetime afterwards. The seq parameter lets the
 * retransmitter replay a segment exactly as it went out first. */
static void tcp_emit(net_socket_t* sk, u32 wire_seq, u8 flags,
                     const u8* payload, u16 len)
{
    /* local frame (#tcp-bigfetch-stall): the ACK/data frame lives on
     * the sender's own stack — a preempted half-built ACK can no
     * longer be overwritten by another task's transmission. */
    u8 tx_frame[NET_MAX_FRAME];
    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    ip_hdr_t*  ip  = (ip_hdr_t*)(tx_frame + sizeof(eth_hdr_t));
    tcp_hdr_t* tcp = (tcp_hdr_t*)(tx_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t));

    const u8* dst_mac = arp_resolve(net_next_hop(sk->remote_ip));
    if (!dst_mac) {
        arp_send_request(net_next_hop(sk->remote_ip));
        return;
    }

    kmemcpy(eth->dst_mac, dst_mac, 6);
    kmemcpy(eth->src_mac, rtl8139_get_mac(), 6);
    eth->ethertype = 0x0008;                       /* IPv4 BE */

    u16 ip_total = sizeof(ip_hdr_t) + NET_TCP_HEADER_LEN + len;
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = net_htons(ip_total);
    ip->id        = net_htons(0x5343);
    ip->flags_frag= 0;
    ip->ttl       = 64;
    ip->protocol  = IPPROTO_TCP;
    ip->checksum  = 0;
    kmemcpy(ip->src_ip, our_ip, 4);
    kmemcpy(ip->dst_ip, sk->remote_ip, 4);
    ip->checksum  = net_htons(checksum((const u8*)ip, sizeof(ip_hdr_t)));

    kmemset(tcp, 0, NET_TCP_HEADER_LEN);
    tcp->src_port = net_htons(sk->local_port);
    tcp->dst_port = net_htons(sk->remote_port);
    tcp->seq      = net_htonl(wire_seq);
    tcp->ack      = net_htonl(sk->rcv_seq);
    tcp->doff_res = 0x50;                          /* 20 bytes, no opts */
    tcp->flags    = flags;
    /* #tcp-rx-drop-silent-ack companion: advertise the ring's FREE
     * space, not a fixed capacity — a stalled reader must shrink the
     * window so the peer pauses instead of filling a full ring. */
    {
        u32 used = (u32)(sk->rx_head - sk->rx_tail);
        u32 free_bytes = (SOCK_RX_DEPTH > used)
                         ? (u32)(SOCK_RX_DEPTH - used) *
                               (u32)sizeof(sk->rx[0].data)
                         : 0;
        tcp->window = net_htons((u16)free_bytes);
    }
    tcp->checksum = 0;
    tcp->urgent   = 0;

    u8* pl = tx_frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + NET_TCP_HEADER_LEN;
    if (len && payload) kmemcpy(pl, payload, len);

    tcp->checksum = net_htons(tcp_checksum(our_ip, sk->remote_ip,
                                           (const u8*)tcp,
                                           NET_TCP_HEADER_LEN + len));
    rtl8139_send(tx_frame, sizeof(eth_hdr_t) + ip_total);
}

/* convenience wrapper: normal transmit uses snd_seq as-is */
static void tcp_send_segment(net_socket_t* sk, u8 flags,
                             const u8* payload, u16 len)
{
    tcp_emit(sk, sk->snd_seq, flags, payload, len);
}

static void tcp_log(const char* tag, net_socket_t* sk, u8 flags)
{
    serial_printf("[%s] lp=%u rp=%u st=%u fl=%02x\n",
                  tag, sk->local_port, sk->remote_port,
                  sk->tcp_state, flags);
}

/* ---- TCP retransmission queue --------------------------------------
 * Every reliable-bytes-carrying segment is recorded verbatim before
 * its first transmission and replayed (same wire sequence!) by the
 * RTO timer until the peer's cumulative ACK covers it. Pure ACKs are
 * never recorded; a FIN entry occupies one sequence byte (len==0). */

static inline s32 seq_diff(u32 a, u32 b) { return (s32)(a - b); }

/* seq-space footprint of one queued entry (data bytes + FIN slot) */
static inline u16 ent_cover(u16 len, u8 flags)
{
    return (u16)(len + ((flags & TCPF_FIN) ? 1 : 0));
}

#define TCP_RTO_BASE   300u
#define TCP_RTO_MAX   2400u
#define TCP_RTX_TRIES   5

/* shared ISN source for client and server sides alike */
static u32 g_isn_seed = 0x51D37E11u;

/* remember a freshly emitted segment so the RTX walk can replay it */
static void tcp_txq_record(net_socket_t* sk, u32 seq, u8 flags,
                           const u8* payload, u16 len)
{
    for (int i = 0; i < SK_TXQ_DEPTH; i++) {
        if (!sk->txq[i].used) {
            sk->txq[i].used      = true;
            sk->txq[i].seq       = seq;
            sk->txq[i].len       = len;
            sk->txq[i].flags     = flags;
            sk->txq[i].retries   = 0;
            sk->txq[i].t_sent_ms = (u32)timer_get_uptime_ms();
            sk->txq[i].rto_ms    = TCP_RTO_BASE;
            if (len && payload) kmemcpy(sk->txq[i].data, payload, len);
            sk->inflight = (u16)(sk->inflight + ent_cover(len, flags));
            return;
        }
    }
    /* no slot: pace_window upstream prevents this for stream writes;
     * the segment still goes out but cannot be re-transmitted. */
}

/* process a cumulative ACK: pop fully-covered entries */
static void tcp_txq_ack(net_socket_t* sk, u32 ackn)
{
    for (int i = 0; i < SK_TXQ_DEPTH; i++) {
        if (!sk->txq[i].used) continue;
        u16 cov = ent_cover(sk->txq[i].len, sk->txq[i].flags);
        if (cov == 0) cov = 1;
        if (seq_diff(ackn, sk->txq[i].seq) >= (s32)cov) {
            sk->txq[i].used = false;
            sk->inflight = (u16)(sk->inflight >= cov
                                 ? sk->inflight - cov : 0);
        }
    }
}

/* RTO-driven re-send of all overdue unacked segments */
void net_tcp_rtx_walk(void)
{
    if (!sockets_initialized) return;
    u32 now = (u32)timer_get_uptime_ms();

    for (int i = 0; i < MAX_SOCKETS; i++) {
        net_socket_t* sk = &sockets[i];
        if (!sk->used || sk->sock_type != SOCK_STREAM) continue;
        if (sk->inflight == 0 || !sk->connected) continue;
        if (sk->tcp_state != TS_ESTABLISHED &&
            sk->tcp_state != TS_FIN_SENT) continue;

        bool dead = false;
        for (int e = 0; e < SK_TXQ_DEPTH && !dead; e++) {
            if (!sk->txq[e].used) continue;
            u32 elapsed = now - sk->txq[e].t_sent_ms;
            if (elapsed < sk->txq[e].rto_ms) continue;

            if (sk->txq[e].retries >= TCP_RTX_TRIES) {
                static u32 giveup_logs;
                if (giveup_logs < 8) {
                    giveup_logs++;
                    serial_printf("[TCPTXFAIL] lp=%u rp=%u "
                                  "no ACK after %u retries\n",
                                  sk->local_port, sk->remote_port,
                                  sk->txq[e].retries);
                }
                dead = true;
                break;
            }
            sk->txq[e].retries++;
            sk->txq[e].t_sent_ms = now;
            u32 nrto = (u32)sk->txq[e].rto_ms * 2;
            if (nrto > TCP_RTO_MAX) nrto = TCP_RTO_MAX;
            sk->txq[e].rto_ms = nrto;

            static u32 rt_logs;
            if (rt_logs < 12) {
                rt_logs++;
                serial_printf("[TCPRTX#%u] lp=%u rp=%u seq=%08x "
                              "len=%u tries=%u\n",
                              rt_logs, sk->local_port, sk->remote_port,
                              sk->txq[e].seq, sk->txq[e].len,
                              sk->txq[e].retries);
            }
            if (sk->txq[e].len)
                tcp_emit(sk, sk->txq[e].seq, sk->txq[e].flags,
                         sk->txq[e].data, sk->txq[e].len);
            else
                tcp_emit(sk, sk->txq[e].seq, sk->txq[e].flags, 0, 0);
        }

        if (dead) {
            for (int e = 0; e < SK_TXQ_DEPTH; e++) sk->txq[e].used=false;
            sk->inflight  = 0;
            sk->tcp_eof   = true;
            sk->connected = false;
            sk->tcp_state = TS_CLOSED_WAIT;   /* readers surface EOF */
        }
    }
}

/* enqueue stream payload into the socket ring (chunked to entry size).
 * Returns the number of bytes actually queued — the caller MUST only
 * advance rcv_seq / ACK by this amount (#tcp-rx-drop-silent-ack: the
 * old void version silently dropped the tail of a segment while the
 * dispatcher ACKed the whole thing, losing stream bytes forever). */
static u16 tcp_enqueue(net_socket_t* sk, const u8* p, u16 n)
{
    u16 queued = 0;
    while (n > 0) {
        /* #rx-ring-guard-raw-tail: head/tail are MONOTONIC u32 counters
         * (slot = counter % DEPTH). The old guard compared a mod'ed
         * next against the RAW tail — once tail passed DEPTH the
         * comparison could never fire again, the ring silently
         * overwrote unread entries (observed head-tail=22 at DEPTH=16),
         * and the kernel ACKed bytes whose data was gone. Compare the
         * fill LEVEL instead — wrap-safe monotonic subtraction. */
        if (sk->rx_head - sk->rx_tail >= SOCK_RX_DEPTH) break;  /* full */

        u16 chunk = n;
        if (chunk > sizeof(sk->rx[0].data)) chunk = sizeof(sk->rx[0].data);
        u32 idx = sk->rx_head % SOCK_RX_DEPTH;

        kmemset(sk->rx[idx].data, 0, sizeof(sk->rx[idx].data));
        kmemcpy(sk->rx[idx].data, p, chunk);
        sk->rx[idx].len = chunk;

        sk->rx_head++;
        p += chunk;
        n -= chunk;
        queued += chunk;
    }
    return queued;
}

/* passive-open: a fresh SYN aimed at a LISTENING socket spawns a
 * child socket that completes the 3WHS; once ESTABLISHED it queues
 * on the parent for accept() to hand out as a brand-new fd.
 * Backlog admission control refuses beyond the parent's backlog —
 * the peer's SYN retransmission will return once slots free up.  */
static void tcp_listener_syn(const u8* src_ip, net_socket_t* lst,
                             u16 sport, u32 seq)
{
    /* count pending children (half-open SYN_RCVD included) toward
     * the backlog so one flood cannot exhaust the socket table */
    u32 pend = lst->acc_head - lst->acc_tail;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].used || i == (int)(lst - sockets)) continue;
        if (sockets[i].sock_type != SOCK_STREAM) continue;
        if (sockets[i].local_port != lst->local_port) continue;
        if (sockets[i].tcp_state == TS_SYN_RCVD ||
            sockets[i].tcp_state == TS_ESTABLISHED) pend++;
    }
    u8 backlog = lst->backlog ? lst->backlog : 1;
    if (pend >= backlog) {
        serial_printf("[TCPBACK] lp=%u pend=%lu backlog=%u drop SYN\n",
                      lst->local_port, (unsigned long)pend, backlog);
        return;
    }

    int ci = net_socket_alloc();
    if (ci < 0) return;                       /* table full: peer retries */
    net_socket_t* c = &sockets[ci];

    c->sock_type   = SOCK_STREAM;
    c->local_port  = lst->local_port;
    kmemcpy(c->remote_ip, src_ip, 4);
    c->remote_port = sport;
    g_isn_seed += 640007u;
    c->snd_seq     = g_isn_seed + (u32)ci * 7919u;
    c->snd_una     = c->snd_seq;
    c->rcv_seq     = seq + 1;
    c->connected   = true;      /* exact-match finds this conn later  */
    c->tcp_state   = TS_SYN_RCVD;

    tcp_log("TCPLSN", c, TCPF_SYN);
    tcp_emit(c, c->snd_seq, TCPF_SYN | TCPF_ACK, 0, 0);
}

void net_tcp_receive(const u8* src_ip, const u8* seg_in, u32 seg_len)
{
    (void)src_ip;
    if (!sockets_initialized || seg_len < NET_TCP_HEADER_LEN) return;

    const tcp_hdr_t* th = (const tcp_hdr_t*)seg_in;
    u16 dport = net_ntohs(th->dst_port);
    u16 sport = net_ntohs(th->src_port);
    u32 seq   = net_htonl(th->seq);
    u32 ackn  = net_htonl(th->ack);
    u8  flags = th->flags;
    u16 hlen  = (th->doff_res >> 4) * 4;
    if (hlen < NET_TCP_HEADER_LEN || hlen > seg_len) return;

    const u8* payload = seg_in + hlen;
    u16 plen = (u16)(seg_len - hlen);

    /* socket match: prefer the exact 4-tuple connection, then route
     * virgin SYNs to LISTENING parents, and only fall back to the
     * legacy unconnected client-style socket last (never stealing a
     * listening port or mid-handshake server child).                */
    net_socket_t* sk = 0;
    s32 ski = -1;
    for (int i = 0; i < MAX_SOCKETS && !sk; i++) {
        if (!sockets[i].used || sockets[i].sock_type != SOCK_STREAM) continue;
        if (sockets[i].local_port != dport) continue;
        /* FIX(#tcp-demux-no-ip): the established-socket match compared
         * only ports — two peers reusing a source port demuxed to the
         * same socket. Full 4-tuple, like any real stack. */
        if (sockets[i].connected && sockets[i].remote_port == sport &&
            kmemcmp(sockets[i].remote_ip, src_ip, 4) == 0) {
            sk = &sockets[i];
            ski = i;
        } else if (!sk && !sockets[i].connected &&
                   sockets[i].tcp_state == TS_CLOSED) {
            sk = &sockets[i];
            ski = i;
            sk->remote_port = sport;
            kmemcpy(sk->remote_ip, src_ip, 4);
        }
    }

    /* passive open: SYN for a port someone listens on */
    if (!sk && (flags & TCPF_SYN) && !(flags & TCPF_ACK)) {
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (!sockets[i].used || sockets[i].sock_type != SOCK_STREAM)
                continue;
            if (sockets[i].tcp_state != TS_LISTEN) continue;
            if (sockets[i].local_port != dport) continue;
            tcp_listener_syn(src_ip, &sockets[i], sport, seq);
            return;
        }
    }

    if (!sk) {
        static u32 ship_logs;
        if (ship_logs < 4) {
            ship_logs++;
            serial_printf("[TCPSHIP] dport=%u sport=%u fl=%02x "
                          "(no stream socket)\n", dport, sport, flags);
        }
        return;
    }

    /* remember the peer's advertised receive window: send pacing
     * below uses it instead of a fixed guess. */
    if (th->window) sk->peer_window = net_ntohs(th->window);

    switch (sk->tcp_state) {

    case TS_SYN_SENT:
        /* The SYN itself consumed one sequence number: the peer's
         * ACK points at ISN+1, so the comparison is against
         * snd_seq+1 and snd_seq advances past the SYN byte. */
        if ((flags & (TCPF_SYN | TCPF_ACK)) == (TCPF_SYN | TCPF_ACK) &&
            ackn == sk->snd_seq + 1) {
            sk->snd_seq    += 1;
            sk->snd_una     = sk->snd_seq;
            sk->rcv_seq     = seq + 1;
            sk->connected   = true;
            sk->tcp_state   = TS_ESTABLISHED;
            tcp_send_segment(sk, TCPF_ACK, 0, 0);
            tcp_log("TCPOK", sk, flags);
        }
        break;

    case TS_SYN_RCVD:
        /* Server-side handshake completion: the child's SYN+ACK is
         * being answered by the final ACK of the three-way dance. */
        if ((flags & TCPF_ACK) && !(flags & TCPF_RST) &&
            ackn == sk->snd_seq + 1) {
            int child_idx = ski;
            sk->snd_seq    += 1;
            sk->snd_una     = sk->snd_seq;
            sk->tcp_state   = TS_ESTABLISHED;
            tcp_send_segment(sk, TCPF_ACK, 0, 0);
            tcp_log("TCPSRVOK", sk, flags);

            /* enqueue onto the LISTENING parent's accept queue */
            for (int i = 0; i < MAX_SOCKETS; i++) {
                if (!sockets[i].used || sockets[i].sock_type != SOCK_STREAM)
                    continue;
                if (sockets[i].tcp_state != TS_LISTEN) continue;
                if (sockets[i].local_port != sk->local_port) continue;
                u32 next = (sockets[i].acc_head + 1) % SK_ACC_DEPTH;
                if (next == sockets[i].acc_tail) break;  /* ring full */
                sockets[i].acc_q[sockets[i].acc_head % SK_ACC_DEPTH] =
                    (u8)child_idx;
                sockets[i].acc_head++;
                serial_printf("[TCPACCQ] listen_lp=%u child=%d "
                              "pending=%lu\n",
                              sockets[i].local_port, child_idx,
                              (unsigned long)(sockets[i].acc_head -
                                              sockets[i].acc_tail));
                break;
            }
        } else if ((flags & TCPF_SYN) && !(flags & TCPF_ACK)) {
            /* our SYN|ACK was lost: the client re-sent its SYN */
            sk->rcv_seq = seq + 1;
            tcp_emit(sk, sk->snd_seq, TCPF_SYN | TCPF_ACK, 0, 0);
            tcp_log("TCPSYNRX", sk, flags);
        }
        break;

    case TS_ESTABLISHED: {
        bool want_ack = false;

        if (flags & TCPF_RST) {
            sk->tcp_eof   = true;
            sk->connected = false;
            sk->tcp_state = TS_CLOSED_WAIT;
            tcp_log("TCPRST", sk, flags);
            break;
        }

        /* cumulative ACK first: frees RTX entries / send window so
         * pacing sees fresh state even mid-burst */
        if ((flags & TCPF_ACK) && seq_diff(ackn, sk->snd_una) > 0) {
            tcp_txq_ack(sk, ackn);
            sk->snd_una = ackn;
        }

        /* silent-ACK our outstanding data: any valid ACK confirms
         * everything sent so far (slirp is strict stop-and-go here) */
        if ((flags & TCPF_ACK) && ackn == sk->snd_seq) {
            want_ack = false;                      /* pure data flow     */
        }

        if (plen > 0 && seq == sk->rcv_seq) {
            /* #tcp-rx-drop-silent-ack: ACK and rcv_seq advance ONLY by
             * what actually entered the ring; the peer retransmits the
             * remainder once the reader drains space. */
            u16 took = tcp_enqueue(sk, payload, plen);
            if (took > 0) {
                sk->rcv_seq += took;
                /* pace the breadcrumb: first 8 then every 32nd so big
                 * transfers stay diagnosable without serial flooding */
                if (tcprx_logs < 8) tcprx_logs++;
                else if (sk->rx_head & 31) { /* skip the log, still count */ }
                else serial_printf("[TCPRX t=%lu] lp=%u len=%u ring=%u\n",
                                   (unsigned long)timer_get_uptime_ms(),
                                   sk->local_port, took,
                                   (unsigned)(sk->rx_head - sk->rx_tail));
                if (tcprx_logs <= 8)
                    serial_printf("[TCPRX#%u t=%lu] lp=%u len=%u ring=%u\n",
                                  tcprx_logs,
                                  (unsigned long)timer_get_uptime_ms(),
                                  sk->local_port, took,
                                  (unsigned)(sk->rx_head - sk->rx_tail));
            }
            /* ACK unconditionally — even with took==0 (ring full).
             * #tcp-zero-window-deadlock: a ring-full ACK carries
             * window=0, the peer goes into persist mode and probes;
             * each probe MUST be answered so the CURRENT (possibly
             * reopened) window reaches the peer as soon as the reader
             * drains the ring. Silencing the probe froze the stream
             * mid-download forever. */
            want_ack = true;
        } else if (plen > 0 && seq != sk->rcv_seq) {
            /* Old data / retransmit of ACKed ranges: pure ACK with the
             * current window. */
            want_ack = true;
        }

        if ((flags & TCPF_FIN) && seq + plen == sk->rcv_seq) {
            sk->rcv_seq   += 1;                    /* FIN eats one seq  */
            sk->tcp_eof    = true;
            want_ack = true;
            tcp_log("TCPFIN", sk, flags);
        }

        if (want_ack) {
            tcp_send_segment(sk, TCPF_ACK, 0, 0);
            /* #tcp-bigfetch-stall forensics: log every win=0 reply
             * (rate-limited) — persist-mode dances show up here */
            {
                static u32 win0_logs;
                u32 used_now = (u32)(sk->rx_head - sk->rx_tail);
                u32 free_now = (SOCK_RX_DEPTH > used_now)
                    ? (u32)(SOCK_RX_DEPTH - used_now) *
                          (u32)sizeof(sk->rx[0].data) : 0;
                if (free_now == 0 && win0_logs < 40) {
                    win0_logs++;
                    serial_printf("[TCPWIN0 t=%lu] lp=%u ring_full\n",
                                  (unsigned long)timer_get_uptime_ms(),
                                  sk->local_port);
                }
            }
        }
        break;
    }

    case TS_FIN_SENT:
        if ((flags & TCPF_ACK) && seq_diff(ackn, sk->snd_una) > 0) {
            tcp_txq_ack(sk, ackn);                 /* FIN entry pops here */
            sk->snd_una   = ackn;
            sk->tcp_state = TS_CLOSED_WAIT;
            tcp_log("TCPFINACK", sk, flags);
        }
        if (flags & TCPF_FIN) {
            sk->rcv_seq   = seq + 1;
            sk->tcp_eof   = true;
            tcp_send_segment(sk, TCPF_ACK, 0, 0);
            sk->tcp_state = TS_CLOSED_WAIT;
        }
        break;

    default:
        break;
    }
}

/* Blocking three-way handshake; returns 0 or -110 (ETIMEDOUT). */
int net_socket_connect_tcp(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS || !sockets[idx].used) return -9;
    net_socket_t* sk = &sockets[idx];

    g_isn_seed += 640007u;
    u32 isn = g_isn_seed + (u32)idx * 7919u;
    sk->sock_type  = SOCK_STREAM;
    sk->tcp_eof    = false;
    sk->snd_seq    = isn;
    sk->snd_una    = isn;
    sk->rcv_seq    = 0;
    sk->peer_window= 0;
    sk->inflight   = 0;
    sk->tcp_state  = TS_SYN_SENT;
    /* NOTE: do NOT wipe sk->connected here. connect() already
     * stamped the peer tuple; clearing it used to make net_tcp_receive
     * miss our OWN SYN|ACK replies ([TCPSHIP] "no stream socket"),
     * because the exact 4-tuple branch requires connected=true while
     * the legacy fallback demands TS_CLOSED. Handshakes died on both
     * doors until this was left intact. */

    /* warm ARP for the NEXT-HOP (gateway for internet destinations) */
    {
        extern bool net_arp_ready_route(const u8*);
        extern void net_arp_kick_route(const u8*);
        net_poll();
        for (int i = 0; i < 100 && !net_arp_ready_route(sk->remote_ip); i++) {
            if ((i & 31) == 31) net_arp_kick_route(sk->remote_ip);
            hlt_irq_restore();   /* #tls-if-leak: was sti\nhlt\ncli */
            net_poll();
        }
    }

    for (int tries = 0; tries < 600; tries++) {
        if (sk->tcp_state == TS_ESTABLISHED) return 0;
        if (sk->tcp_state == TS_CLOSED_WAIT) return -104;/* ECONNRESET */
        if ((tries & 31) == 0) {                       /* retransmit SYN */
            sk->snd_seq = isn;
            tcp_send_segment(sk, TCPF_SYN, 0, 0);
        }
        hlt_irq_restore();   /* #tls-if-leak: was sti\nhlt\ncli */
        net_poll();
    }
    sk->tcp_state = TS_CLOSED;
    serial_printf("[TCPFAIL] connect timeout lp=%u\n", sk->local_port);
    return -110;                                       /* ETIMEDOUT     */
}

int net_socket_stream_send(int idx, const void* buf, u32 len)
{
    serial_printf("[STRSND-ENTER] idx=%d len=%u st=%d\n",
                  idx, len,
                  (int)(idx >= 0 && idx < MAX_SOCKETS
                        ? (int)sockets[idx].tcp_state : -1));
    if (idx < 0 || idx >= MAX_SOCKETS || !sockets[idx].used) return -9;
    net_socket_t* sk = &sockets[idx];
    if (sk->tcp_state != TS_ESTABLISHED) return -104;/* ECONNRESET    */

    const u8* p = (const u8*)buf;
    u32 off = 0;
    /* pace against the peer's advertised window (fresh from the last
     * inbound segment) or a conservative default early on */
    u32 win = sk->peer_window ? sk->peer_window
                              : (u32)(SOCK_RX_DEPTH * 1024);
    if (win > 8192) win = 8192;      /* never outrun our own TXQ depth */

    while (off < len) {
        u16 chunk = (u16)(len - off);
        if (chunk > 1400) chunk = 1400;

        /* Wait until the window admits this segment AND a free RTX
         * slot exists — every transmitted byte must stay replayable
         * until its cumulative ACK lands. */
        int guard = 0;
        for (;;) {
            bool have_slot = false;
            for (int q = 0; q < SK_TXQ_DEPTH; q++) {
                if (!sk->txq[q].used) { have_slot = true; break; }
            }
            if (have_slot && (u32)sk->inflight + chunk <= win) break;
            if (++guard > 600 || sk->tcp_state != TS_ESTABLISHED) {
                return off ? (int)off : -104;   /* honest partial write */
            }
            hlt_irq_restore();   /* #tls-if-leak: was sti\nhlt\ncli */
            net_poll();                          /* ACKs pop entries   */
        }

        tcp_txq_record(sk, sk->snd_seq, TCPF_PSH | TCPF_ACK,
                       p + off, chunk);
        tcp_send_segment(sk, TCPF_PSH | TCPF_ACK, p + off, chunk);
        sk->snd_seq += chunk;
        off += chunk;
    }
    if (streamtx_logs < 4) {
        streamtx_logs++;
        serial_printf("[TCPTX#%u] lp=%u bytes=%lu seq->%08x\n",
                      streamtx_logs, sk->local_port,
                      (unsigned long)len, sk->snd_seq);
    }
    return (int)off;
}

/* -1 = empty (EAGAIN), 0 = EOF, >0 bytes copied */
int net_socket_stream_recv(int idx, void* buf, u32 count)
{
    if (idx < 0 || idx >= MAX_SOCKETS || !sockets[idx].used) return -9;
    net_socket_t* sk = &sockets[idx];

    net_poll();

    u32 copied = 0;
    u8* out = (u8*)buf;
    u32 used_before = (u32)(sk->rx_head - sk->rx_tail);
    while (copied < count && sk->rx_tail != sk->rx_head) {
        u32 idx_r = sk->rx_tail % SOCK_RX_DEPTH;
        u32 n = sk->rx[idx_r].len;
        if (n > count - copied) {
            /* #rx-partial-read-drop: the consumer wants fewer bytes
             * than the entry holds. The old code copied the prefix and
             * did tail++ — the REMAINDER of the entry was thrown away
             * (a few hundred bytes lost on every non-multiple read;
             * big fetches came up 3KB short with md5 mismatches).
             * Keep the tail of the entry for the next read. */
            u32 take = count - copied;
            kmemcpy(out + copied, sk->rx[idx_r].data, take);
            u32 rest = sk->rx[idx_r].len - (u16)take;
            kmemmove(sk->rx[idx_r].data,
                     sk->rx[idx_r].data + take, rest);
            sk->rx[idx_r].len = (u16)rest;
            copied += take;
            break;                                  /* count filled   */
        }
        kmemcpy(out + copied, sk->rx[idx_r].data, n);
        sk->rx_tail++;
        copied += n;
    }

    /* #tcp-window-update-on-drain: the advertised window only travels
     * inside ACKs, and we ACK only in reaction to inbound segments.
     * A reader that drains a ring the peer had filled with window=0
     * must ANNOUNCE the reopened window or the sender stays in
     * persist mode forever (downloads stalled at ~ring size). */
    if (copied > 0 && used_before > SOCK_RX_DEPTH / 2) {
        u32 used_after = (u32)(sk->rx_head - sk->rx_tail);
        u32 free_after = (SOCK_RX_DEPTH - used_after) *
                         (u32)sizeof(sk->rx[0].data);
        if (free_after >= SOCK_RX_DEPTH * (u32)sizeof(sk->rx[0].data) / 2)
            tcp_send_segment(sk, TCPF_ACK, 0, 0);
    }

    if (copied == 0 && sk->tcp_eof) return 0;          /* clean EOF     */
    if (copied == 0) return -1;                        /* EAGAIN        */
    return (int)copied;
}

/* graceful FIN on close(): let acknowledged data drain first so the
 * peer sees a contiguous byte stream, then emit FIN|ACK once and
 * briefly wait for the final ACK. Bounded throughout — abortive
 * close remains acceptable when a peer stalls. Server-side handles
 * (LISTEN / SYN_RCVD children) tear down abortively. */
void net_socket_close_tcp(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS || !sockets[idx].used) return;
    net_socket_t* sk = &sockets[idx];
    if (sk->sock_type != SOCK_STREAM) return;

    if (sk->tcp_state == TS_LISTEN || sk->tcp_state == TS_SYN_RCVD ||
        sk->tcp_state == TS_CLOSED) {
        sk->tcp_state = TS_CLOSED;
        return;
    }
    if (sk->tcp_state != TS_ESTABLISHED) return;

    for (int i = 0; i < 200 && sk->inflight != 0 &&
                      sk->tcp_state == TS_ESTABLISHED; i++) {
        hlt_irq_restore();   /* #tls-if-leak: was sti\nhlt\ncli */
        net_poll();                                  /* ACKs drain RTX */
    }

    tcp_txq_record(sk, sk->snd_seq, TCPF_FIN | TCPF_ACK, 0, 0);
    tcp_send_segment(sk, TCPF_FIN | TCPF_ACK, 0, 0);
    sk->snd_seq   += 1;
    sk->tcp_state  = TS_FIN_SENT;
    tcp_log("TCPBYE", sk, 0);

    for (int i = 0; i < 40 && sk->tcp_state == TS_FIN_SENT; i++) {
        hlt_irq_restore();   /* #tls-if-leak: was sti\nhlt\ncli */
        net_poll();
    }
}

/* ---- Server-side public API (listen/accept plumbing) ---------------*/

int net_socket_listen(int idx, int backlog)
{
    if (idx < 0 || idx >= MAX_SOCKETS || !sockets[idx].used) return -9;
    net_socket_t* sk = &sockets[idx];
    if (sk->sock_type != SOCK_STREAM) return -95;    /* EOPNOTSUPP     */
    if (sk->local_port == 0)          return -22;    /* bind first     */
    if (backlog < 1)       backlog = 1;
    if (backlog > SK_ACC_DEPTH) backlog = SK_ACC_DEPTH;
    sk->backlog   = (u8)backlog;
    sk->acc_head  = 0;
    sk->acc_tail  = 0;
    sk->connected = false;
    sk->tcp_state = TS_LISTEN;
    serial_printf("[TCPLISTEN] lp=%u backlog=%d\n", sk->local_port,
                  backlog);
    return 0;
}

bool net_socket_is_listening(int idx)
{
    return (idx >= 0 && idx < MAX_SOCKETS && sockets[idx].used &&
            sockets[idx].sock_type == SOCK_STREAM &&
            sockets[idx].tcp_state == TS_LISTEN);
}

u16 net_tcp_pending_accepts(int idx)
{
    if (idx < 0 || idx >= MAX_SOCKETS || !sockets[idx].used) return 0;
    return (u16)(sockets[idx].acc_head - sockets[idx].acc_tail);
}

/* Pop one fully-established child socket index off the parent's
 * accept queue (-1 when none pending). The syscall layer turns it
 * into a fresh process fd of type SOCK_STREAM. */
s32 net_socket_accept_pop(int idx)
{
    if (!net_socket_is_listening(idx)) return -1;
    net_socket_t* lst = &sockets[idx];
    while (lst->acc_tail != lst->acc_head) {
        u8 ci = lst->acc_q[lst->acc_tail % SK_ACC_DEPTH];
        lst->acc_tail++;
        if (ci < MAX_SOCKETS && sockets[ci].used &&
            sockets[ci].tcp_state == TS_ESTABLISHED) {
            return (s32)ci;
        }
        /* stale entry (child died before accept): keep scanning */
    }
    return -1;
}
