#include "../include/pci.h"
#include "../include/vga.h"
#include "../include/string.h"

static pci_device_t devices[PCI_MAX_DEVICES];
static u32 device_count = 0;
static bool pci_initialized = false;

static u32 pci_read_raw(u8 bus, u8 dev, u8 func, u8 offset) {
    u32 addr = (u32)0x80000000 | ((u32)bus << 16) |
               ((u32)dev << 11) | ((u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write_raw(u8 bus, u8 dev, u8 func, u8 offset, u32 val) {
    u32 addr = (u32)0x80000000 | ((u32)bus << 16) |
               ((u32)dev << 11) | ((u32)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

u32 pci_read_config(u8 bus, u8 dev, u8 func, u8 offset) {
    return pci_read_raw(bus, dev, func, offset);
}

void pci_write_config(u8 bus, u8 dev, u8 func, u8 offset, u32 val) {
    pci_write_raw(bus, dev, func, offset, val);
}

static void pci_probe_device(u8 bus, u8 dev, u8 func) {
    u32 vendor_device = pci_read_raw(bus, dev, func, 0);
    u16 vendor = vendor_device & 0xFFFF;
    if (vendor == 0xFFFF) return;

    if (device_count >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &devices[device_count];
    kmemset(d, 0, sizeof(pci_device_t));

    d->vendor_id = vendor;
    d->device_id = (vendor_device >> 16) & 0xFFFF;

    u32 class_rev = pci_read_raw(bus, dev, func, 8);
    d->revision   = class_rev & 0xFF;
    d->prog_if    = (class_rev >> 8) & 0xFF;
    d->subclass   = (class_rev >> 16) & 0xFF;
    d->class_code = (class_rev >> 24) & 0xFF;

    u32 cmd_status = pci_read_raw(bus, dev, func, 4);
    d->command = cmd_status & 0xFFFF;
    d->status  = (cmd_status >> 16) & 0xFFFF;

    d->header_type = (pci_read_raw(bus, dev, func, 12) >> 16) & 0xFF;

    d->irq_line = pci_read_raw(bus, dev, func, 0x3C) & 0xFF;
    d->irq_pin  = (pci_read_raw(bus, dev, func, 0x3C) >> 8) & 0xFF;

    u8 hdr_type = d->header_type & 0x7F;
    u8 bar_count = (hdr_type == 0) ? 6 : (hdr_type == 1) ? 2 : 0;
    for (u8 i = 0; i < bar_count; i++) {
        d->bar[i] = pci_read_raw(bus, dev, func, 0x10 + i * 4);
    }

    d->bus  = bus;
    d->dev  = dev;
    d->func = func;

    device_count++;
}

void pci_init(void) {
    device_count = 0;
    kmemset(devices, 0, sizeof(devices));

    // NOTE: bus must be wider than u8 — `u8 bus < 256` wraps around
    // and enumerates forever.
    for (u32 bus = 0; bus < 256; bus++) {
        bool any_on_bus = false;
        for (u8 dev = 0; dev < 32; dev++) {
            for (u8 func = 0; func < 8; func++) {
                u32 vd = pci_read_raw(bus, dev, func, 0);
                if ((vd & 0xFFFF) == 0xFFFF) continue;

                any_on_bus = true;
                pci_probe_device(bus, dev, func);

                if (func == 0) {
                    u8 ht = (pci_read_raw(bus, dev, 0, 12) >> 16) & 0xFF;
                    if (!(ht & 0x80)) break;
                }
            }
        }
        (void)any_on_bus;
    }

    pci_initialized = true;
}

u32 pci_get_device_count(void) {
    return device_count;
}

const pci_device_t* pci_get_device(u32 index) {
    if (index >= device_count) return NULL;
    return &devices[index];
}

const char* pci_class_name(u8 class_code, u8 subclass) {
    switch (class_code) {
        case 0x01:
            switch (subclass) {
                case 0x00: return "IDE Controller";
                case 0x01: return "IDE Controller (ATA)";
                case 0x06: return "SATA Controller";
                case 0x08: return "NVMe Controller";
                default: return "Storage";
            }
        case 0x02: return "Network Controller";
        case 0x03:
            switch (subclass) {
                case 0x00: return "VGA Controller";
                case 0x01: return "XGA Controller";
                case 0x02: return "3D Controller";
                default: return "Display Controller";
            }
        case 0x06: return "Bridge";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB xHCI Controller";
                case 0x0C: return "USB Device Controller";
                default: return "USB Controller";
            }
        case 0x0D: return "Wireless Controller";
        case 0x0E: return "Intelligent I/O";
        case 0x10: return "Encryption Controller";
        case 0x11: return "Signal Processing";
        case 0x12: return "Processing Accelerator";
        case 0x13: return "Non-Essential Instrumentation";
        default: return "Unknown";
    }
}

// Known vendor IDs
const char* pci_vendor_name(u16 vid) {
    switch (vid) {
        case 0x8086: return "Intel";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD/ATI";
        case 0x1AF4: return "Virtio";
        case 0x1234: return "Bochs/QEMU";
        case 0x15AD: return "VMware";
        case 0x1B36: return "QEMU (PCI)";
        default: return NULL;
    }
}

// Known device IDs per vendor
const char* pci_device_name(u16 vid, u16 did, u8 class_code, u8 subclass) {
    // QEMU/Bochs virtual devices
    if (vid == 0x1234) {
        if (did == 0x1111) return "Bochs VGA";
    }
    if (vid == 0x8086) {
        // Intel devices
        if (did == 0x7010 || did == 0x7111) return "PIIX3/PIIX4 IDE";
        if (did == 0x2922) return "ICH9 SATA/AHCI";
        if (did == 0x100E || did == 0x1502) return "GbE NIC (e1000)";
        if (did == 0x10D3) return "82574L GbE";
        if (class_code == 0x06) return "Host/PCI Bridge";
        if (class_code == 0x0C && subclass == 0x03) return "xHCI USB";
    }
    if (vid == 0x1AF4) {
        if (did >= 0x1000 && did <= 0x103F) return "Virtio Network";
        if (did >= 0x1040 && did <= 0x107F) return "Virtio Block";
        if (did >= 0x1080 && did <= 0x10BF) return "Virtio Console";
        if (did >= 0x10C0 && did <= 0x10FF) return "Virtio GPU";
        return "Virtio Device";
    }
    return NULL;
}

// Decode BAR size and type
static void pci_print_bar(u32 bar_val, u8 bar_num) {
    if (bar_val == 0) return;

    bool io = (bar_val & 1) != 0;
    bool pref64 = !io && (bar_val & 4) != 0;
    u64 addr;

    if (io) {
        addr = bar_val & 0xFFFFFFFC;
        vga_printf("     BAR%u: I/O  0x%04lx (size unknown)\n", bar_num, (unsigned long)addr);
    } else {
        addr = bar_val & 0xFFFFFFF0;
        const char* type = pref64 ? "MEM64" : "MEM32";
        vga_printf("     BAR%u: %s 0x%08lx (size unknown)\n", bar_num, type, (unsigned long)addr);
    }
}

// Determine suggested driver for a device
const char* pci_suggest_driver(u8 class_code, u8 subclass, u8 prog_if) {
    switch (class_code) {
        case 0x01:
            switch (subclass) {
                case 0x00:
                case 0x01: return "ata";    // IDE
                case 0x06:
                    if (prog_if == 0x01) return "ahci";
                    return "sata";
                case 0x08: return "nvme";
                default: return "storage";
            }
        case 0x02: return "nic";
        case 0x03: return "vga/fb";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "xhci";
                case 0x00: return "uhci";
                case 0x10: return "ehci";
                default: return "usb";
            }
        case 0x06: return "bridge";
        default: return NULL;
    }
}

void pci_print_all(void) {
    if (!pci_initialized) {
        vga_print("[PCI] Not initialized.\n");
        return;
    }

    vga_print("\nPCI Device List (with driver binding):\n");
    vga_print("--------------------------------------\n");

    if (device_count == 0) {
        vga_print("  (no PCI devices found)\n");
        return;
    }

    for (u32 i = 0; i < device_count; i++) {
        const pci_device_t* d = &devices[i];
        const char* vendor = pci_vendor_name(d->vendor_id);
        const char* devname = pci_device_name(d->vendor_id, d->device_id, d->class_code, d->subclass);
        const char* classname = pci_class_name(d->class_code, d->subclass);
        const char* driver = pci_suggest_driver(d->class_code, d->subclass, d->prog_if);

        // Device header
        vga_printf("%02u:%02u.%u ", (u32)d->bus, (u32)d->dev, (u32)d->func);

        if (vendor) {
            vga_printf("%-8s ", vendor);
        } else {
            vga_printf("0x%04X   ", (u32)d->vendor_id);
        }

        if (devname) {
            vga_printf("%-24s ", devname);
        } else {
            vga_printf("0x%04X%-18s ", (u32)d->device_id, "");
        }

        // Class info
        if (classname) {
            vga_print(classname);
        }
        vga_putchar('\n');

        // BARs
        for (u8 b = 0; b < 6; b++) {
            pci_print_bar(d->bar[b], b);
        }

        // Driver suggestion
        if (driver) {
            vga_printf("     Driver: %s", driver);
            if (d->irq_line != 0 && d->irq_line != 0xFF) {
                vga_printf(", IRQ %u", (u32)d->irq_line);
            }
            vga_putchar('\n');
        }

        // Status flags
        u16 cmd = d->command;
        bool has_io = (cmd & PCI_CMD_IO_SPACE) != 0;
        bool has_mem = (cmd & PCI_CMD_MEM_SPACE) != 0;
        bool has_bmaster = (cmd & PCI_CMD_BUS_MASTER) != 0;
        if (has_io || has_mem || has_bmaster) {
            vga_print("     Enabled: ");
            if (has_io) vga_print("IO ");
            if (has_mem) vga_print("MEM ");
            if (has_bmaster) vga_print("BusMaster ");
            vga_putchar('\n');
        }
    }
    vga_printf("\nTotal: %u device(s)\n", device_count);
}

void cmd_pci(int argc, char** argv) {
    (void)argc; (void)argv;
    pci_print_all();
}
