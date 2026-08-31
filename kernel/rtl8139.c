/* ============================================================
 * rtl8139.c — NullOs RTL8139 Ethernet Driver (polling mode)
 * ============================================================
 *
 * QEMU's RTL8139 emulation is well-supported and predictable.
 * We use POLLING instead of interrupts for reliability — many
 * hobby kernels do this because PCI interrupt routing in QEMU
 * can be tricky.
 *
 * RX model: single contiguous 16KB ring buffer. NIC writes
 * received frames sequentially; CAPR register tracks read pos.
 * Each frame has a 4-byte header (status:16 | length:16).
 *
 * TX model: 4 descriptor slots with individual 2KB buffers.
 * Write buffer phys addr → TSAD[n]; write size|OWN → TSD[n].
 */

#include "../include/rtl8139.h"
#include "../include/pci.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/mm.h"
#include "../include/net.h"
#include "../include/serial.h"

/* Register offsets (from MMIO base) */
#define RTL_IDR0            0x00
#define RTL_MAR0            0x08
#define RTL_TSAD0           0x20
#define RTL_TSAD1           0x24
#define RTL_TSAD2           0x28
#define RTL_TSAD3           0x2C
#define RTL_RBSTART         0x30
#define RTL_CR              0x37
#define RTL_CAPR            0x38
#define RTL_IMR             0x3C
#define RTL_ISR             0x3E
#define RTL_TCR             0x40
#define RTL_RCR             0x44
#define RTL_CONFIG1         0x52

/* CR bits */
#define RTL_CR_RST          0x10
#define RTL_CR_RE           0x08    /* RX enable */
#define RTL_CR_TE           0x04    /* TX enable */

/* TSD bits */
#define RTL_TSD_OWN         0x80000000
#define RTL_TSD_SIZE_MASK   0x1FFF

/* RCR bits */
#define RTL_RCR_AAP         0x01    /* accept all physical */
#define RTL_RCR_APM         0x02    /* accept physical match */
#define RTL_RCR_AB          0x08    /* accept broadcast */
/* #rtl-rxwrap-pastbuf: the WRAP bit is INTENTIONALLY NOT SET. Per the
 * RTL8139 datasheet WRAP gives the NIC "1.5KB extra buffer space" so
 * boundary-crossing frames stay contiguous; QEMU implements this by
 * writing linearly PAST RxBufferSize whenever WRAP=1 (the split path
 * is gated on `!(size<65536 && RxWrap)`) — DMA-ing frame tails into
 * whatever memory follows rx_ring[] in .bss and desyncing the CAPR
 * walk (the #tcp-bigfetch-stall death at ~32-65KB, and the ancient
 * 60-130KB jams). With WRAP=0 QEMU relocates the frame tail to
 * ring[0]; our per-byte wrapping header/data reads already handle
 * every seam, so no other change is needed. */
/* #rtl-rblen-64k: RBLEN lives in RCR bits **12:11** (00=8K 01=16K
 * 10=32K 11=64K). The old "0xB << 11" set bits 11,12,14 → RBLEN=11
 * = a 64KB NIC ring while rx_ring[] is 32KB: the NIC DMA'd frames
 * 32KB past our buffer straight into tx_buf and neighbouring .bss,
 * and the CAPR walk (mod 32KB) desynced — the transfer died around
 * the 32-64KB mark with the server retransmitting into the void
 * (the #tcp-bigfetch-stall "vanishing ACKs" were never sent because
 * the frames were never SEEN). 32KB = RBLEN 10 = (2 << 11). */
#define RTL_RCR_RBLEN_32K   (0x2 << 11)  /* 32KB rx buffer */

/* ISR bits — QEMU hw/net/rtl8139.c canonical mapping:
 * RxOK=0x01 RxErr=0x02 TxOK=0x04 TxErr=0x08 RxAovw=0x10 ...
 * (the old driver used TxOK=0x02/TxErr=0x04: every successful
 * transmission was misread as an ERROR — and worse, any interrupt-
 * gating keyed on 0x02 saw phantom receive errors.) */
#define RTL_ISR_ROK         0x01
#define RTL_ISR_RERR        0x02
#define RTL_ISR_TOK         0x04
#define RTL_ISR_TXERR       0x08
#define RTL_ISR_RXOVW       0x10

#define RX_BUF_SIZE     32768
#define TX_BUF_SIZE     2048
#define NUM_TX_DESC     4

/* CRITICAL BUS FACT (QEMU RTL8139): PCI BAR0 is the *I/O PORT* window
 * (bit0=1) and BAR1 is the *MMIO* window. The old driver assumed BAR0
 * was MMIO, so every "register read" dereferenced plain identity-mapped
 * RAM at PA 0xC000: the MAC shown on screen was heap garbage, reset/
 * enable writes went nowhere and NO frame ever touched the wire.
 * We detect the BAR type and route register access through inb/outb
 * port I/O (identical register file), falling back to real MMIO only
 * if BAR0 turns out to be a memory BAR. */
static u32 mmio_base = 0;
static u16 pio_base = 0;
static bool use_pio = false;
static bool nic_ready = false;
static u32 bar0_raw = 0, bar1_raw = 0;
static u32 tx_tok_count = 0, tx_err_count = 0;

/* ---- unified register access (PIO vs MMIO) ---- */
static inline u8 nic_r8(u32 off) {
    return use_pio ? inb(pio_base + off)
                   : *(volatile u8*)(u64)(mmio_base + off);
}
static inline u16 nic_r16(u32 off) {
    return use_pio ? inw(pio_base + off)
                   : *(volatile u16*)(u64)(mmio_base + off);
}
static inline u32 nic_r32(u32 off) {
    return use_pio ? inl(pio_base + off)
                   : *(volatile u32*)(u64)(mmio_base + off);
}
static inline void nic_w8(u32 off, u8 v) {
    if (use_pio) outb(pio_base + off, v);
    else *(volatile u8*)(u64)(mmio_base + off) = v;
}
static inline void nic_w16(u32 off, u16 v) {
    if (use_pio) outw(pio_base + off, v);
    else *(volatile u16*)(u64)(mmio_base + off) = v;
}
static inline void nic_w32(u32 off, u32 v) {
    if (use_pio) outl(pio_base + off, v);
    else *(volatile u32*)(u64)(mmio_base + off) = v;
}

static u8  our_mac[6];

/* RX ring: single contiguous buffer */
static u8  rx_ring[RX_BUF_SIZE] __attribute__((aligned(4096)));
static u32 rx_read_pos = 0;      /* CAPR value */

/* TX buffers */
static u8  tx_buf[NUM_TX_DESC][TX_BUF_SIZE] __attribute__((aligned(4096)));
static u8  tx_current = 0;

/* Stats */
static u64 rx_packets = 0;
static u64 tx_packets = 0;

/* ---- MMIO helpers ---- */

static inline u8  mmio8(u32 off)  { return *(volatile u8*)(u64)(mmio_base + off); }
static inline u16 mmio16(u32 off) { return *(volatile u16*)(u64)(mmio_base + off); }
static inline u32 mmio32(u32 off) { return *(volatile u32*)(u64)(mmio_base + off); }
static inline void mmio8w(u32 off, u8 v)  { *(volatile u8*)(u64)(mmio_base + off) = v; }
static inline void mmio16w(u32 off, u16 v){ *(volatile u16*)(u64)(mmio_base + off) = v; }
static inline void mmio32w(u32 off, u32 v){ *(volatile u32*)(u64)(mmio_base + off) = v; }

/* ---- Public API ---- */

const u8* rtl8139_get_mac(void) { return our_mac; }
bool rtl8139_is_ready(void)     { return nic_ready; }
u64  rtl8139_rx_count(void)     { return rx_packets; }
u64  rtl8139_tx_count(void)     { return tx_packets; }
u32  rtl8139_tx_ok(void)        { return tx_tok_count; }

int rtl8139_init(const pci_device_t* dev)
{
    if (!dev || dev->vendor_id != 0x10EC) {
        vga_print("[RTL] Not a Realtek device\n");
        return -1;
    }

    /* Enable bus master + memory space */
    u32 cmd = pci_read_config(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= 0x7;  /* IO + MEM + BusMaster */
    pci_write_config(dev->bus, dev->dev, dev->func, 0x04, cmd);

    /* ---- BAR detection (THE historical bug was here) --------------
     * QEMU RTL8139: BAR0 = I/O-port window (bit0=1), BAR1 = MMIO.
     * A memory-BAR check first keeps us correct on real hw too.     */
    bar0_raw = pci_read_config(dev->bus, dev->dev, dev->func, 0x10);
    bar1_raw = pci_read_config(dev->bus, dev->dev, dev->func, 0x14);
    if (bar0_raw & 0x1) {
        use_pio   = true;
        pio_base  = (u16)(bar0_raw & 0xFFFC);
        mmio_base = bar1_raw & ~0xF;
        vga_printf("[RTL] BUS: I/O ports @%04x (BAR1 mem %08x unused)\n",
                   pio_base, mmio_base);
    } else {
        use_pio   = false;
        mmio_base = bar0_raw & ~0xF;
        vga_printf("[RTL] BUS: MMIO base: 0x%08x\n", mmio_base);
    }

    /* Software reset */
    nic_w8(RTL_CR, RTL_CR_RST);
    int timeout = 100000;
    while ((nic_r8(RTL_CR) & RTL_CR_RST) && timeout--) ;
    if (!timeout) {
        vga_print("[RTL] Reset timeout!\n");
        return -1;
    }

    /* Read MAC address — with the bus fixed this must be QEMU's
     * well-known default 52:54:00:12:34:56 (was random RAM garbage). */
    for (int i = 0; i < 6; i++) {
        our_mac[i] = nic_r8(RTL_IDR0 + i);
    }
    vga_printf("[RTL] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               our_mac[0], our_mac[1], our_mac[2],
               our_mac[3], our_mac[4], our_mac[5]);

    /* Init RX ring: single contiguous buffer.
     * Kernel VA==PA (HH_OFFSET=0): DMA address is the pointer itself.
     * Buffers are static .bss under 4 GB — safe to truncate to u32. */
    kmemset(rx_ring, 0, RX_BUF_SIZE);
    rx_read_pos = 0;

    /* Init TX buffers */
    kmemset(tx_buf, 0, sizeof(tx_buf));
    tx_current = 0;

    /* [DMA] audit (#nic-dma-audit): log the exact PA windows the NIC
     * may write via bus-mastering. Anything outside these ranges is
     * NOT touched by DMA; when memory corruption appears, diff it
     * against this log first. */
    serial_printf("[DMA] rx_ring PA=%lx..%lx (%u KB), tx_buf PA=%lx..%lx "
                  "(%u KB) — DMA write-exclusivity window\n",
                  (unsigned long)(u64)VIRT_TO_PHYS((u64)&rx_ring[0]),
                  (unsigned long)(u64)VIRT_TO_PHYS((u64)&rx_ring[RX_BUF_SIZE - 1]),
                  (unsigned)(RX_BUF_SIZE / 1024),
                  (unsigned long)(u64)VIRT_TO_PHYS((u64)&tx_buf[0][0]),
                  (unsigned long)(u64)VIRT_TO_PHYS((u64)&tx_buf[NUM_TX_DESC - 1][TX_BUF_SIZE - 1]),
                  (unsigned)(sizeof(tx_buf) / 1024));

    /* Program RX buffer address (physical!) */
    nic_w32(RTL_RBSTART, (u32)(u64)VIRT_TO_PHYS((u64)&rx_ring[0]));

    /* Set up TX descriptors: point to our static buffers */
    for (int i = 0; i < NUM_TX_DESC; i++) {
        u32 phys_addr = (u32)(u64)VIRT_TO_PHYS((u64)&tx_buf[i][0]);
        nic_w32(RTL_TSAD0 + i * 4, phys_addr);
        nic_w32(0x10 + i * 4, 0);              /* TSD: clear ownership */
    }

    /* Interrupt mask: disable ALL (we poll) */
    nic_w16(RTL_IMR, 0);

    /* RX config: accept all + ring length MUST match the software
     * modulus: RBLEN field bits[12:11], 10 => 32 KB. WRAP deliberately
     * clear — see #rtl-rxwrap-pastbuf above. */
    nic_w32(RTL_RCR, RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AB |
            RTL_RCR_RBLEN_32K);

    /* Enable TX + RX */
    nic_w8(RTL_CR, RTL_CR_RE | RTL_CR_TE);

    /* Clear any pending interrupt status */
    nic_w16(RTL_ISR, 0xFFFF);

    /* Consumer pointer preload: classic convention — CAPR starts at
     * SIZE-16 so the FIRST incoming frame lands at (CAPR+16)%SIZE=0,
     * exactly where rtl8139_poll() looks for it. Without this the
     * reader always stared at offset 16 and missed the beginning. */
    nic_w16(RTL_CAPR, (RX_BUF_SIZE - 16) & 0xFFFF);

    nic_ready = true;
    vga_print("[RTL] RTL8139 initialized (polling mode)\n");
    return 0;
}

/* ---- TX: send raw Ethernet frame --------------------------------*/

/* #tcp-bigfetch-stall TX-LOCK: the whole reserve→copy→kick→TOK-wait
 * body runs with IF=0. The holder can never be preempted (no hlt,
 * no sti inside the body), so a plain flag needs no owner tracking
 * and is uncontended in practice — defense in depth for any future
 * interrupt-mode or multi-context sender. */
static volatile int tx_busy = 0;

int rtl8139_send(const u8* data, u32 len)
{
    if (!nic_ready) return -1;
    if (len > TX_BUF_SIZE) return -1;

    /* Serialize against every other TX path: closing IF until the
     * frame is on the wire removes the preemption window that used
     * to let a successor task rebuild+transmit over OUR descriptor
     * (vanishing ACKs under load = stalled >131KB fetches). */
    /* pop has no 32-bit form in long mode: pop the full RFLAGS and
     * truncate — only IF (bit 9) is consumed below. */
    unsigned long rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    u32 eflags = (u32)rflags;
    while (tx_busy) {
        /* Holder runs with IF=0 and never sleeps, so it IS making
         * progress right now on this core: wait with IF open (only
         * when it was open on entry) so the PIT keeps ticking. */
        if (eflags & 0x200) __asm__ volatile ("sti\nhlt\ncli" ::: "memory");
        else                __asm__ volatile ("pause" ::: "memory");
    }
    tx_busy = 1;

    /* #tx-busy-drop: the OLD code bailed out instantly when the
     * current descriptor still had OWN set. Under an ACK storm
     * (every inbound 1440B segment = one ACK) that silently dropped
     * ACKs: the peer stopped receiving window updates and the whole
     * download froze. Wait for a free descriptor instead (the IRQ
     * handler races us for ISR/TOK, so OWN polling is the only
     * reliable completion signal). */
    for (int w = 0; w < 4000; w++) {
        u32 tsd0 = nic_r32(0x10 + tx_current * 4);
        if (!(tsd0 & RTL_TSD_OWN)) break;
        if (w == 2000) {
            /* rotate once: another descriptor may already be free */
            tx_current = (tx_current + 1) % NUM_TX_DESC;
        }
        /* TX lock held with IF=0: a bare hlt would sleep forever
         * (#tx-hlt-noif), and opening IF would drop the lock's
         * preemption shield. Busy pause only — each inl read is a
         * VM exit that gives QEMU room to complete the transmit. */
        __asm__ volatile ("pause" ::: "memory");
    }
    u32 tsd = nic_r32(0x10 + tx_current * 4);
    if (tsd & RTL_TSD_OWN) {
        serial_printf("[TXBUSY] desc=%u tsd=%08x\n",
                      tx_current, tsd);
        tx_busy = 0;
        if (eflags & 0x200) __asm__ volatile ("sti" ::: "memory");
        return -1;
    }

    /* Copy frame to TX buffer */
    kmemcpy(tx_buf[tx_current], data, len);

    /* Pad to minimum 60 bytes (Ethernet minimum without CRC) */
    if (len < 60) {
        kmemset(&tx_buf[tx_current][len], 0, 60 - len);
        len = 60;
    }

    /* Program TX descriptor: start transmission */
    u32 phys_addr = (u32)(u64)VIRT_TO_PHYS((u64)&tx_buf[tx_current][0]);
    nic_w32(RTL_TSAD0 + tx_current * 4, phys_addr);
    nic_w32(0x10 + tx_current * 4, len | RTL_TSD_OWN);  /* OWN=1: card owns */

    /* Wait for Transmit-OK here (µs-scale under QEMU): proves the
     * frame physically left the NIC — separates "wire TX failed"
     * from "host stack dropped it" in every network debugging run. */
    {
        for (int w = 0; w < 100000; w++) {
            u16 isr = nic_r16(RTL_ISR);
            if (isr & (RTL_ISR_TOK | RTL_ISR_TXERR)) {
                if (isr & RTL_ISR_TOK) {
                    tx_tok_count++;
                } else {
                    tx_err_count++;
                    serial_printf("[TXERR] tsd=%08x\n",
                                  nic_r32(0x10 + tx_current * 4));
                }
                nic_w16(RTL_ISR, isr);      /* ack what we consumed */
                break;
            }
        }
    }

    /* breadcrumb: first six frames this boot */
    {
        static int tx_dbg;
        if (tx_dbg < 6 && data && len > 13) {
            tx_dbg++;
            serial_printf("[TXF] n=%d len=%u type=%02x%02x "
                          "dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
                          tx_dbg, len, data[12], data[13],
                          data[0], data[1], data[2],
                          data[3], data[4], data[5]);
            /* IP frame forensics: raw 20-byte header as handed to the
             * NIC, stored checksum, and value recomputed in place
             * with the checksum field excluded. */
            if (data[12] == 0x08 && data[13] == 0x00 && tx_dbg <= 2) {
                u32 rec = 0;
                for (int i = 0; i < 10; i++) {
                    if (i == 5) continue;
                    rec += ((u32)data[14 + i*2] << 8) | data[14 + i*2 + 1];
                }
                while (rec >> 16) rec = (rec & 0xFFFF) + (rec >> 16);
                serial_printf("[IPHDR] h=%02x %02x %02x %02x %02x %02x "
                              "%02x %02x %02x %02x %02x %02x %02x %02x "
                              "%02x %02x %02x %02x %02x %02x "
                              "stored=%02x%02x want=%04x\n",
                              data[14], data[15], data[16], data[17],
                              data[18], data[19], data[20], data[21],
                              data[22], data[23], data[24], data[25],
                              data[26], data[27], data[28], data[29],
                              data[30], data[31], data[32], data[33],
                              data[24], data[25], (u16)((~rec) & 0xFFFF));
            }
        }
    }

    tx_packets++;
    tx_current = (tx_current + 1) % NUM_TX_DESC;
    tx_busy = 0;
    if (eflags & 0x200) __asm__ volatile ("sti" ::: "memory");
    return 0;
}

/* ---- RX: poll for incoming frames -------------------------------*/
/* Returns length of received frame, copies to buf. 0 = no packet. */

int rtl8139_poll(u8* buf, u32 buf_size)
{
    if (!nic_ready) return 0;

    /* Classic RTL8139 ring walk (matches the CAPR preload done in
     * init): frame N lives at (CAPR + 16) % RX_BUF_SIZE with a 4-byte
     * header [status:16 | len:16] followed by the packet INCLUDING
     * its 4-byte CRC. Advancing writes back CAPR = next - 16 so that
     * re-adding 16 on the next poll keeps the pointer in lockstep. */
    u16 capr   = nic_r16(RTL_CAPR);
    u32 offset = ((u32)capr + 16) % RX_BUF_SIZE;

    /* #rtl-rx-header-wrap: the 4-byte frame header STRADDLES the ring
     * boundary every ~8KB of traffic. Reading rx_ring[offset+2] raw
     * walked past the array into the (self-zeroed) ring start, saw
     * len=0, declared the slot empty and NEVER advanced CAPR — the
     * NIC ring jammed and the whole network died after 60-130KB of
     * transfer. Every header byte must wrap individually. */
    u16 rx_status = (u16)(rx_ring[offset]
                          | (rx_ring[(offset + 1) % RX_BUF_SIZE] << 8));
    u16 rx_len    = (u16)(rx_ring[(offset + 2) % RX_BUF_SIZE]
                          | (rx_ring[(offset + 3) % RX_BUF_SIZE] << 8));

    /* Empty slot: header zeroed by us after consumption (or never
     * written yet). Replaying an old frame is impossible because we
     * self-clear headers once consumed — no ISR dependence at all. */
    if (rx_len < 4 || rx_len > RX_BUF_SIZE - 4 || !(rx_status & 0x01)) {
        return 0;
    }

    /* #rx-stale-body-cba: the header bit-test alone is NOT sound.
     * Only the 4 header bytes of a consumed frame are cleared — the
     * BODY stays — and a poll that races ahead of the writer (any
     * transfer lull) reads stale body bytes that can satisfy both
     * ROK=1 and a "sane" length (observed live: pattern text
     * "...BIGFETCH..." read as st=0x4647 len=0x5445 "GF/ET"), after
     * which CAPR jumped ~21KB and the walk was destroyed. The NIC's
     * own write pointer (CBA, reg 0x3A — QEMU RxBufAddr) is the
     * authoritative unread-span source: accept a frame ONLY when its
     * full [header+data+crc] footprint lies inside
     * [CAPR+16, CBA). Frames are written atomically w.r.t. the
     * single vCPU, so partial frames cannot be observed. */
    {
        u16 cba = nic_r16(0x3A);
        u32 rptr   = ((u32)capr + 16) % RX_BUF_SIZE;
        u32 unread = (RX_BUF_SIZE + (u32)cba - rptr) % RX_BUF_SIZE;
        u32 want   = ((u32)rx_len + 4 + 3) & ~3u;
        if (unread == 0 || unread < want) return 0;
    }

    /* one-shot breadcrumb: raw hardware header of first delivered RX */
    {
        static bool rx_dbg;
        if (!rx_dbg) {
            rx_dbg = true;
            serial_printf("[RXRAW] capr=%04x off=%u status=%04x len=%u "
                          "d0=%02x d1=%02x d12=%02x d13=%02x\n",
                          capr, offset, rx_status, rx_len,
                          rx_ring[offset+4], rx_ring[offset+5],
                          rx_ring[offset+16], rx_ring[offset+17]);
        }
    }

    /* Copy frame data (after 4-byte header), excluding CRC */
    u32 data_start = (offset + 4) % RX_BUF_SIZE;
    u32 copy_len = rx_len - 4;                 /* exclude CRC */
    if (copy_len > buf_size) copy_len = buf_size;

    for (u32 i = 0; i < copy_len; i++) {
        buf[i] = rx_ring[(data_start + i) % RX_BUF_SIZE];
    }

    /* Advance CAPR past this frame (4-aligned), self-clear header
     * (each byte wraps the ring individually — same wrap hazard). */
    u32 new_offset = ((offset + rx_len + 4 + 3) & ~3) % RX_BUF_SIZE;
    nic_w16(RTL_CAPR, (u16)((new_offset - 16) & 0xFFFF));
    rx_ring[offset]                         = 0;
    rx_ring[(offset + 1) % RX_BUF_SIZE]     = 0;
    rx_ring[(offset + 2) % RX_BUF_SIZE]     = 0;
    rx_ring[(offset + 3) % RX_BUF_SIZE]     = 0;

    rx_packets++;
    return (int)copy_len;
}

/* ---- IRQ handler (called from idt.c irq_handler) ----------------*/

void rtl8139_irq_handler(void)
{
    if (!nic_ready) return;

    u16 isr = nic_r16(RTL_ISR);
    if (!isr) return;

    /* #poll-deport-irq: NEVER drain the ring or emit ACKs from IRQ
     * context — that re-enters the send path inside a foreign
     * context and mutates tx buffers/descriptors on top of whatever
     * mainline send is in flight (the #tcp-bigfetch-stall family).
     * The mainline poll loops (every blocking syscall) drain the
     * ring continuously; here we only acknowledge the hardware.
     * (IMR=0 today, so this is defensive for future experiments.) */
    nic_w16(RTL_ISR, isr);
}

u32 rtl8139_get_irq_line(void)
{
    return 11;                                   /* QEMU default PCI IRQ */
}

/* #tcp-bigfetch-stall stall-time NIC forensics: called from the
 * blocking read loop when no data arrived for seconds. Prints the
 * exact hardware-vs-software view so a CAPR/RX-ring desync (frames
 * DMA'd but never walked) is distinguishable from a silent NIC. */
void rtl8139_rx_diag(void)
{
    if (!nic_ready) { serial_print("[RXDIAG] nic not ready\n"); return; }
    u16 capr   = nic_r16(RTL_CAPR);
    u32 offset = ((u32)capr + 16) % RX_BUF_SIZE;
    u16 hdr_st = (u16)(rx_ring[offset]
                       | (rx_ring[(offset + 1) % RX_BUF_SIZE] << 8));
    u16 hdr_ln = (u16)(rx_ring[(offset + 2) % RX_BUF_SIZE]
                       | (rx_ring[(offset + 3) % RX_BUF_SIZE] << 8));
    serial_printf("[RXDIAG] capr=%04x off=%u st=%04x len=%u isr=%04x "
                  "cr=%02x rcr=%08x cba=%04x rxpkts=%lu rxpos=%u "
                  "txd=%d %d %d %d\n",
                  capr, offset, hdr_st, hdr_ln,
                  nic_r16(RTL_ISR), nic_r8(RTL_CR),
                  nic_r32(0x44), nic_r16(0x3A),
                  (unsigned long)rx_packets, rx_read_pos,
                  (nic_r32(0x10) >> 31) & 1, (nic_r32(0x14) >> 31) & 1,
                  (nic_r32(0x18) >> 31) & 1, (nic_r32(0x1C) >> 31) & 1);
    /* first 16 bytes the walker is staring at (header + frame head) */
    serial_printf("[RXDIAG] ring@off: %02x %02x %02x %02x %02x %02x %02x %02x "
                  "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                  rx_ring[offset], rx_ring[(offset+1) % RX_BUF_SIZE],
                  rx_ring[(offset+2) % RX_BUF_SIZE],
                  rx_ring[(offset+3) % RX_BUF_SIZE],
                  rx_ring[(offset+4) % RX_BUF_SIZE],
                  rx_ring[(offset+5) % RX_BUF_SIZE],
                  rx_ring[(offset+6) % RX_BUF_SIZE],
                  rx_ring[(offset+7) % RX_BUF_SIZE],
                  rx_ring[(offset+8) % RX_BUF_SIZE],
                  rx_ring[(offset+9) % RX_BUF_SIZE],
                  rx_ring[(offset+10) % RX_BUF_SIZE],
                  rx_ring[(offset+11) % RX_BUF_SIZE],
                  rx_ring[(offset+12) % RX_BUF_SIZE],
                  rx_ring[(offset+13) % RX_BUF_SIZE],
                  rx_ring[(offset+14) % RX_BUF_SIZE],
                  rx_ring[(offset+15) % RX_BUF_SIZE]);
    /* wrap-relocation probe: does a VALID frame header sit at ring[0]
     * while the walker is stuck near the ring end? (#tcp-bigfetch-stall) */
    serial_printf("[RXDIAG] ring@0000: st=%04x len=%u d4..11="
                  "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                  (u16)(rx_ring[0] | (rx_ring[1] << 8)),
                  (u16)(rx_ring[2] | (rx_ring[3] << 8)),
                  rx_ring[4], rx_ring[5], rx_ring[6], rx_ring[7],
                  rx_ring[8], rx_ring[9], rx_ring[10], rx_ring[11]);
}

/* ---- Shell command --------------------------------------------------*/

void cmd_rtl8139(int argc, char** argv)
{
    bool dump = (argc >= 2 && argv[1][0] == 'd');
    if (!nic_ready) {
        vga_print("[RTL8139] Not initialized.\n");
        return;
    }
    if (use_pio) {
        vga_printf("[RTL8139] BUS: I/O ports @%04x\n", pio_base);
    } else {
        vga_printf("[RTL8139] BUS: MMIO @%08x\n", mmio_base);
    }
    vga_printf("[RTL8139] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               our_mac[0], our_mac[1], our_mac[2],
               our_mac[3], our_mac[4], our_mac[5]);
    vga_printf("CR=%02x RCR=%08x ISR=%04x IMR=%04x CAPR=%04x\n",
               nic_r8(RTL_CR), nic_r32(RTL_RCR),
               nic_r16(RTL_ISR), nic_r16(RTL_IMR), nic_r16(RTL_CAPR));
    vga_printf("TSD: %08x %08x %08x %08x\n",
               nic_r32(0x10), nic_r32(0x14),
               nic_r32(0x18), nic_r32(0x1C));
    vga_printf("Mode: Polling | RX: %lu | TX: %lu (tok=%lu err=%lu)\n",
               (unsigned long)rx_packets, (unsigned long)tx_packets,
               (unsigned long)tx_tok_count, (unsigned long)tx_err_count);

    if (dump || (argc >= 2 && argv[1][0] == 't')) {
        extern void net_poll(void);
        vga_print("net_poll() forced...\n");
        net_poll();
        for (u32 r = 0; r < 128; r += 32) {
            vga_printf("RX%03x:", r);
            for (u32 c = 0; c < 32; c++) {
                vga_printf(" %02x", rx_ring[r + c]);
            }
            vga_print("\n");
        }
        /* last used TX descriptor buffer — verifies exactly what
         * bytes were handed to the NIC on the previous transmit */
        u32 last = (tx_current + NUM_TX_DESC - 1) % NUM_TX_DESC;
        vga_printf("TXBUF[%u]:\n", last);
        for (u32 r = 0; r < 64; r += 32) {
            vga_printf("TX%03x:", r);
            for (u32 c = 0; c < 32; c++) {
                vga_printf(" %02x", tx_buf[last][r + c]);
            }
            vga_print("\n");
        }
    }
}

/* Compatibility: shell references rtl8139_get() for driver status */
const rtl8139_t* rtl8139_get(void)
{
    static rtl8139_t info;
    if (!nic_ready) return 0;
    kmemset(&info, 0, sizeof(info));
    kmemcpy(info.mac, our_mac, 6);
    info.initialized = true;
    info.rx_packets = (u32)rx_packets;
    info.tx_packets = (u32)tx_packets;
    return &info;
}
