#ifndef NET_H
#define NET_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// NullOs Network Stack — Ethernet, ARP, IPv4, ICMP
//
// QEMU user-net defaults:
//   Guest IP:    10.0.2.15
//   Gateway/DNS: 10.0.2.2
//   Subnet:      255.255.255.0
// ============================================================

/* NullOs network identity */
#define NULLOS_IP        "\x0A\x00\x02\x0F"          /* 10.0.2.15  */
#define NULLOS_IP_U32    0x0F02000A                   /* same, u32 LE */
#define GATEWAY_IP       "\x0A\x00\x02\x02"           /* 10.0.2.2   */
#define SUBNET_MASK      "\xFF\xFF\xFF\x00"

/* EtherType */
#define ETHERTYPE_IPV4   0x0800
#define ETHERTYPE_ARP    0x0806

/* ARP opcodes */
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2

/* IP protocol numbers */
#define IPPROTO_ICMP     1
#define IPPROTO_TCP      6

/* ICMP types */
#define ICMP_ECHO_REPLY  0
#define ICMP_ECHO_REQ    8

/* Buffer sizes */
#define NET_MTU          1500
#define NET_MAX_FRAME    (NET_MTU + 14)              /* MTU + Ethernet header */
#define ARP_TABLE_SIZE   8

/* ---- Structures (all packed, network byte order on wire) ------- */

typedef struct __attribute__((packed)) {
    u8  dst_mac[6];
    u8  src_mac[6];
    u16 ethertype;                                /* BE: 0x0806=ARP, 0x0800=IPv4 */
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    u16 hw_type;                                  /* 1 = Ethernet                */
    u16 proto_type;                               /* 0x0800 = IPv4               */
    u8  hw_len;                                   /* 6                           */
    u8  proto_len;                                /* 4                           */
    u16 opcode;                                   /* 1=request, 2=reply          */
    u8  sender_mac[6];
    u8  sender_ip[4];
    u8  target_mac[6];
    u8  target_ip[4];
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    u8  ver_ihl;                                  /* version<<4 | ihl            */
    u8  tos;
    u16 total_len;
    u16 id;
    u16 flags_frag;                               /* DF|MF|frag_offset           */
    u8  ttl;
    u8  protocol;                                 /* 1=ICMP, 6=TCP, 17=UDP       */
    u16 checksum;
    u8  src_ip[4];
    u8  dst_ip[4];
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} icmp_hdr_t;

/* winsize for ioctl TIOCGWINSZ */
typedef struct __attribute__((packed)) {
    u16 ws_row;
    u16 ws_col;
    u16 ws_xpixel;
    u16 ws_ypixel;
} winsize_t;

/* ARP cache entry */
typedef struct {
    bool valid;
    u8   ip[4];
    u8   mac[6];
} arp_entry_t;

/* ---- UDP ---- */

#define IPPROTO_UDP       17

typedef struct __attribute__((packed)) {
    u16 src_port;                                 /* BE */
    u16 dst_port;                                 /* BE */
    u16 length;                                   /* header + data, BE */
    u16 checksum;
} udp_hdr_t;

#define NET_UDP_HEADER_LEN 8

/* ---- TCP ---- */

#define NET_TCP_HEADER_LEN 20

/* TCP flags (header->flags byte) */
#define TCPF_FIN   0x01
#define TCPF_SYN   0x02
#define TCPF_RST   0x04
#define TCPF_PSH   0x08
#define TCPF_ACK   0x10

/* Minimal TCP state machine. Client side is complete; LISTEN/SYN_RCVD
 * implement the server half (accept queue feeds sys_accept()).       */
#define TS_CLOSED       0
#define TS_SYN_SENT     1
#define TS_ESTABLISHED  2
#define TS_FIN_SENT     3
#define TS_CLOSED_WAIT  4                          /* FIN sent+acked / reset */
#define TS_LISTEN       5                          /* passive open          */
#define TS_SYN_RCVD     6                          /* server mid-handshake  */

typedef struct __attribute__((packed)) {
    u16 src_port;                                 /* BE */
    u16 dst_port;                                 /* BE */
    u32 seq;                                      /* BE */
    u32 ack;                                      /* BE */
    u8  doff_res;                                 /* data_offset<<4 | resv */
    u8  flags;
    u16 window;                                   /* BE */
    u16 checksum;
    u16 urgent;
} tcp_hdr_t;

/* ---- Sockets ---------------------------------------------------------*/

#define AF_INET           2
#define SOCK_STREAM       1
#define SOCK_DGRAM        2
#define SOCK_RAW          3                       /* user ICMP pings */
#define IPPROTO_UDP_NUM   17
#define IPPROTO_TCP_NUM   6
#define IPPROTO_ICMP_NUM  1
#define MAX_SOCKETS       16

typedef struct {
    bool     used;
    u16      local_port;
    u8       remote_ip[4];
    u16      remote_port;
    bool     connected;

    /* protocol flavor + minimal TCP client/server state */
    u8       sock_type;                           /* SOCK_DGRAM/STREAM/RAW */
    u8       ip_proto;                            /* 6 TCP, 17 UDP, 1 ICMP */
    u8       tcp_state;                           /* TS_*               */
    bool     tcp_eof;                             /* peer FIN received  */
    u32      snd_seq;                             /* next seq to send   */
    u32      snd_una;                             /* oldest unacked seq */
    u32      rcv_seq;                             /* next expected peer */
    u16      peer_window;                         /* last advertised rx window */
    u8       backlog;                             /* listen() backlog   */

    /* Retransmission queue: unacked segments we can re-send verbatim.
     * A pure-FIN entry carries len==0 and occupies one sequence byte. */
    #define SK_TXQ_DEPTH 4
    struct {
        bool used;
        u32  seq;                                 /* wire seq of entry  */
        u16  len;
        u8   flags;                               /* PSH|ACK / FIN|ACK  */
        u8   retries;
        u32  t_sent_ms;
        u32  rto_ms;
        u8   data[1400];
    } txq[SK_TXQ_DEPTH];
    u16 inflight;                                 /* bytes not yet acked+FINs */

    /* Listen/accept queue: indices of ESTABLISHED child sockets. */
    #define SK_ACC_DEPTH 4
    volatile u32 acc_head;                        /* enqueue index */
    volatile u32 acc_tail;                        /* dequeue index */
    u8 acc_q[SK_ACC_DEPTH];

    /* Receive queue: simple ring buffer of datagrams/segments */
    #define SOCK_RX_DEPTH 16
    struct {
        u16 len;
        u8  src_ip[4];
        u16 src_port;
        u8  data[1024];
    } rx[SOCK_RX_DEPTH];
    volatile u32 rx_head;                         /* write index */
    volatile u32 rx_tail;                         /* read index  */
    /* POSIX dup()/dup2()/fcntl(F_DUPFD) may alias ONE socket from
     * several process fds (busybox ping does dup2+close(orig)!).
     * The resource dies only with its LAST alias, refcounted here. */
    u8       sk_refs;
} net_socket_t;

/* ---- Functions --------------------------------------------------*/

void net_init(void);
void net_receive_frame(const u8* data, u32 len);
void net_poll(void);                            /* drain RX ring         */
u64  net_get_rx_packets(void);
u64  net_get_tx_packets(void);

void cmd_ping(int argc, char** argv);

/* Socket API for syscall layer */
int  net_socket_alloc(void);
void net_socket_free(int fd);
int  net_socket_connect(int fd, const u8* ip, u16 port);
int  net_socket_bind(int fd, const u8* ip, u16 port);       /* returns 0/-EADDRINUSE */
int  net_socket_sendto(int fd, const u8* ip, u16 port, const u8* data, u16 len);
int  net_socket_recvfrom(int fd, void* buf, u32 count, u8* src_ip, u16* src_port);
bool net_socket_is_used(int fd);
bool net_socket_has_data(int fd);                           /* queued datagrams?     */
bool net_socket_is_bound(int fd);
u16  net_socket_local_port(int fd);
void net_socket_set_local_port(int fd, u16 port);
bool net_socket_is_connected(int fd);
int  net_socket_get_remote(int fd, u8* ip, u16* port);
bool net_arp_ready(const u8* ip);                           /* MAC cached for ip?    */
bool net_arp_ready_route(const u8* dst_ip);                 /* via default gateway  */
void net_arp_kick_route(const u8* dst_ip);                  /* re-request for route */
void net_arp_kick(const u8* ip);                            /* re-send ARP request   */

/* TCP stream extensions */
u32  net_htonl(u32 v);
int  net_socket_set_type(int fd, int sock_type);            /* DGRAM or STREAM       */
int  net_socket_type(int fd);
void net_socket_set_proto(int fd, int proto);               /* raw/icmp flavor    */
bool net_socket_is_icmp(int fd);                            /* user-ping socket?  */
void net_icmp_send(const u8* dst_ip, const u8* data, u16 len); /* verbatim ICMP   */
void net_socket_icmp_arm(int fd, const u8* dst_ip,
                         const u8* data, u16 len);          /* 1s pacer arm       */
int  net_socket_connect_tcp(int fd);                        /* blocking 3WHS         */
void net_tcp_receive(const u8* src_ip, const u8* seg, u32 seg_len);
int  net_socket_stream_send(int fd, const void* buf, u32 len);
int  net_socket_stream_recv(int fd, void* buf, u32 count);  /* -1 EAGAIN,0 EOF,n>0 */
bool net_socket_has_eof(int fd);
void net_socket_close_tcp(int fd);                          /* graceful FIN          */

/* Server-side TCP (LISTEN/accept) + retransmission support */
int  net_socket_listen(int idx, int backlog);               /* 0 ok / -err   */
s32  net_socket_accept_pop(int idx);                        /* child idx|-1  */
bool net_socket_is_listening(int idx);
u16  net_tcp_pending_accepts(int idx);
void net_tcp_rtx_walk(void);                                /* timers+resend */

#ifdef __cplusplus
}
#endif

#endif // NET_H
