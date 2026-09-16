#ifndef PCI_H
#define PCI_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// PCI Configuration Space
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

// PCI Command register bits
#define PCI_CMD_IO_SPACE    (1 << 0)
#define PCI_CMD_MEM_SPACE   (1 << 1)
#define PCI_CMD_BUS_MASTER  (1 << 2)
#define PCI_CMD_INTX_DISABLE (1 << 10)

// PCI Class codes (base)
#define PCI_CLASS_OLD        0x00
#define PCI_CLASS_STORAGE     0x01
#define PCI_CLASS_NETWORK     0x02
#define PCI_CLASS_DISPLAY     0x03
#define PCI_CLASS_MULTIMEDIA  0x04
#define PCI_CLASS_MEMORY      0x05
#define PCI_CLASS_BRIDGE      0x06
#define PCI_CLASS_COMM        0x07
#define PCI_CLASS_SYSTEM      0x08
#define PCI_CLASS_INPUT       0x09
#define PCI_CLASS_DOCKING     0x0A
#define PCI_CLASS_PROCESSOR   0x0B
#define PCI_CLASS_SERIAL      0x0C
#define PCI_CLASS_WIRELESS    0x0D
#define PCI_CLASS_I2O         0x0E
#define PCI_CLASS_SATCOM      0x0F
#define PCI_CLASS_CRYPTO      0x10
#define PCI_CLASS_DASP        0x11
#define PCI_CLASS_ACCEL       0x12
#define PCI_CLASS_OTHER       0xFF

// PCI device info
#define PCI_MAX_DEVICES 64

typedef struct {
    u16 vendor_id;    // 0xFFFF = no device
    u16 device_id;
    u16 command;
    u16 status;
    u8  revision;
    u8  prog_if;
    u8  subclass;
    u8  class_code;
    u8  header_type;
    u8  irq_line;
    u8  irq_pin;

    // Base Address Registers
    u32 bar[6];

    // Bus/Device/Function
    u8  bus;
    u8  dev;
    u8  func;
} pci_device_t;

// Initialize PCI enumeration
void pci_init(void);

// Get list of found devices
u32 pci_get_device_count(void);
const pci_device_t* pci_get_device(u32 index);

// Read/write config space
u32 pci_read_config(u8 bus, u8 dev, u8 func, u8 offset);
void pci_write_config(u8 bus, u8 dev, u8 func, u8 offset, u32 val);

// Print all PCI devices
void pci_print_all(void);

// Shell command
void cmd_pci(int argc, char** argv);

// Get class name string
const char* pci_class_name(u8 class_code, u8 subclass);

// Lookup helpers (shared with shell 'drivers' command)
const char* pci_vendor_name(u16 vid);
const char* pci_device_name(u16 vid, u16 did, u8 class_code, u8 subclass);
const char* pci_suggest_driver(u8 class_code, u8 subclass, u8 prog_if);

#ifdef __cplusplus
}
#endif

#endif // PCI_H
