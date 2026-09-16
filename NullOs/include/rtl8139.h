#ifndef RTL8139_H
#define RTL8139_H

#include "types.h"
#include "pci.h"

#ifdef __cplusplus
extern "C" {
#endif

// RTL8139 register offsets (from BAR0)
#define RTL8139_IDR0          0x00  // MAC ID register 0
#define RTL8139_IDR1          0x04  // MAC ID register 1
#define RTL8139_IDR2          0x08  // MAC ID register 2
#define RTL8139_IDR3          0x0C  // MAC ID register 3
#define RTL8139_IDR4          0x10  // MAC ID register 4
#define RTL8139_IDR5          0x14  // MAC ID register 5
#define RTL8139_MAR0          0x18  // Multicast Address Register 0
#define RTL8139_MAR4          0x28  // Multicast Address Register 4
#define RTL8139_TXDESC0       0x20  // TX Descriptor Start Address (low 32-bit)
#define RTL8139_TXDESC4       0x30  // TX Descriptor Start Address (high 32-bit)
#define RTL8139_TXADDR        0x38  // TX Current Address of Descriptor Read
#define RTL8139_RXDESC0       0x44  // RX Descriptor Start Address
#define RTL8139_RXDESC4       0x48  // RX Descriptor Start Address (high 32-bit)
#define RTL8139_RXADDR        0x4C  // RX Current Address of Descriptor Read
#define RTL8139_RXBUF         0x50  // RX Buffer Start Address
#define RTL8139_RXBUF_LEN     0x54  // RX Buffer Length
#define RTL8139_TXBUF         0x58  // TX Buffer Start Address
#define RTL8139_TXBUF_LEN     0x5C  // TX Buffer Length
#define RTL8139_INTR_MASK     0x3C  // Interrupt Mask Register
#define RTL8139_INTR_STATUS   0x3E  // Interrupt Status Register
#define RTL8139_TX_CONFIG     0x40  // TX Configuration Register
#define RTL8139_RX_CONFIG     0x44  // RX Configuration Register
#define RTL8139_CMD           0x48  // Command Register
#define RTL8139_TX_MISSED     0x4C  // TX Missed Packet Count
#define RTL8139_RX_MISSED     0x50  // RX Missed Packet Count
#define RTL8139_CHIP_VERSION  0x58  // Chip Version
#define RTL8139_CSCR           0x5C  // C+ Command Register
#define RTL8139_CONFIG0       0x60  // Configuration Register 0
#define RTL813_CONFIG1       0x64  // Configuration Register 1
#define RTL8139_CONFIG2       0x68  // Configuration Register 2
#define RTL8139_CONFIG3       0x6C  // Configuration Register 3
#define RTL8139_CONFIG4       0x70  // Configuration Register 4
#define RTL8135_CONFIG5       0x74  // Configuration Register 5
#define RTL8139_TCTR          0x78  // Timer Counter Register
#define RTL8139_MPC          0x7C  // Missed Packet Counter
#define RTL8139_9346CR       0x80  // 9346 CR
#define RTL8139_TCTR_CURD     0x84  // Timer Current Value

// TX Descriptor flags
#define RTL_TX_DESC_FSF        (1 << 31)  // First Segment Descriptor
#define RTL_TX_DESC_LSF        (1 << 30)  // Last Segment Descriptor
#define RTL_TX_DESC_SIZE_MASK  0x3FF

// RX Status
#define RTL_RX_STS_OK         0x01
#define RTL_RX_STS_FAE        0x02  // FIFO Address Error
#define RTL_RX_STS_CRC        0x04  // CRC Error
#define RTL_RX_STS_FOV        0x08  // FIFO Overflow
#define RTL_RX_STS_RUNT       0x10  // Runt Packet
#define RTL_RX_STS_LONG       0x20  // Long Packet

// CMD register bits
#define RTL_CMD_TX_ENABLE    (1 << 2)
#define RTL_CMD_TX_RESET     (1 << 4)
#define RTL_CMD_RX_ENABLE    (1 << 3)
#define RTL_CMD_RX_RESET     (1 << 5)
#define RTL_CMD_RESET        (1 << 6)

// INTR_STATUS bits
#define RTL_INTR_RX_OK       (1 << 0)
#define RTL_INTR_RX_ERR      (1 << 1)
#define RTL_INTR_TX_ERR      (1 << 2)
#define RTL_INTR_TX_OK       (1 << 3)

// RTL8139 driver state
typedef struct {
    const pci_device_t* pci_dev;
    u32 mmio_base;       // MMIO base address (from BAR0)
    u16 irq_line;        // IRQ line from PCI config
    u8  mac[6];         // MAC address
    bool initialized;
    u32 rx_packets;
    u32 tx_packets;
} rtl8139_t;

// Initialize RTL8139 driver for the given PCI device
// Returns 0 on success, negative on error
int rtl8139_init(const pci_device_t* dev);

// Get driver state
const rtl8139_t* rtl8139_get(void);

// Print status
void rtl8139_print_info(void);

// Shell command
void cmd_rtl8139(int argc, char** argv);

// Polling-mode API (new)
const u8* rtl8139_get_mac(void);
bool      rtl8139_is_ready(void);
u64       rtl8139_rx_count(void);
u64       rtl8139_tx_count(void);
int       rtl8139_send(const u8* data, u32 len);
int       rtl8139_poll(u8* buf, u32 buf_size);

#ifdef __cplusplus
}
#endif

#endif // RTL8139_H
