#include "../include/ac97.h"
#include "../include/pci.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/mm.h"

// ============================================================
// Intel AC'97 Audio Controller Driver for NullOs
// ============================================================
//
// Supports Intel ICH AC'97 audio (PCI class 0x0401, prog_if 0x00).
// Provides PCM output via Bus Master DMA.
//
// The ICH controller has two BARs:
//   BAR0: NAMBAR (Native Audio Mixer) - codec register access
//   BAR1: NABMBAR (Native Audio Bus Master) - DMA engine
// ============================================================

static ac97_t drv;
static bool drv_active = false;

// ---- I/O helpers (supports both I/O port and MMIO) ----

static inline u32 nabm_read(const ac97_t* d, u32 offset) {
    if (d->nabm_is_io) {
        return inl(d->nabmbar + offset);
    } else {
        return *(volatile u32*)(u64)(d->nabmbar + offset);
    }
}

static inline void nabm_write(const ac97_t* d, u32 offset, u32 val) {
    if (d->nabm_is_io) {
        outl(d->nabmbar + offset, val);
    } else {
        *(volatile u32*)(u64)(d->nabmbar + offset) = val;
    }
}

static inline u16 nam_read(const ac97_t* d, u32 offset) {
    if (d->nam_is_io) {
        return inw(d->nambar + offset);
    } else {
        return *(volatile u16*)(u64)(d->nambar + offset);
    }
}

static inline void nam_write(const ac97_t* d, u32 offset, u16 val) {
    if (d->nam_is_io) {
        outw(d->nambar + offset, val);
    } else {
        *(volatile u16*)(u64)(d->nambar + offset) = val;
    }
}

// ---- Codec access ----

static bool codec_wait_ready(const ac97_t* d) {
    u32 start = 0;
    while ((nabm_read(d, AC97_BM_POCTRL) & (1 << 1)) && start < AC97_CODEC_READY_TIMEOUT) {
        start++;
    }
    return !(nabm_read(d, AC97_BM_POCTRL) & (1 << 1));
}

static u16 codec_read_reg(const ac97_t* d, u8 reg) {
    // Write register address to NAMBAR offset (slot 0 = PCM out)
    nam_write(d, reg, 0);
    // Wait for codec ready
    for (volatile int i = 0; i < 1000; i++);
    // Read value from NAMBAR offset + 2
    return nam_read(d, reg + 2);
}

static void codec_write_reg(const ac97_t* d, u8 reg, u16 value) {
    nam_write(d, reg, value);
    for (volatile int i = 0; i < 1000; i++);
}

// ---- Buffer Descriptor List (BDL) ----
// Each BDL entry is 8 bytes: {u32 buf_addr, u16 buf_len, u16 flags}

static void setup_bdl(ac97_t* d) {
    // Allocate BDL (8 bytes per entry)
    d->bdl = (u8*)kzalloc(sizeof(u32) * AC97_NUM_BUFS * 2);
    if (!d->bdl) return;

    d->bdl_phys = (u32)vmm_get_phys((u64)d->bdl);
    if (d->bdl_phys == 0) d->bdl_phys = (u32)(u64)d->bdl;  // Identity mapped

    // Fill BDL entries
    for (int i = 0; i < AC97_NUM_BUFS; i++) {
        u32 offset = i * 8;

        // Buffer address (32-bit)
        u32 phys = d->dma_phys[i];
        d->bdl[offset + 0] = phys & 0xFF;
        d->bdl[offset + 1] = (phys >> 8) & 0xFF;
        d->bdl[offset + 2] = (phys >> 16) & 0xFF;
        d->bdl[offset + 3] = (phys >> 24) & 0xFF;

        // Buffer length in samples (16-bit stereo = 4 bytes per sample frame)
        u16 len = AC97_BUF_SIZE / 4;  // Number of sample frames
        d->bdl[offset + 4] = len & 0xFF;
        d->bdl[offset + 5] = (len >> 8) & 0xFF;

        // Flags: IOC on last buffer only
        u16 flags = (i == AC97_NUM_BUFS - 1) ? AC97_BD_IOC : 0;
        d->bdl[offset + 6] = flags & 0xFF;
        d->bdl[offset + 7] = (flags >> 8) & 0xFF;
    }

    // Program BDL base address into NABMBAR
    nabm_write(d, AC97_BM_POBDB, d->bdl_phys);

    // Set last valid index
    nabm_write(d, AC97_BM_POLV, AC97_NUM_BUFS - 1);

    // Set current index to 0
    nabm_write(d, AC97_BM_POCIV, 0);
}

// ---- DMA buffer helpers ----

static bool alloc_dma_buffers(ac97_t* d) {
    for (int i = 0; i < AC97_NUM_BUFS; i++) {
        d->dma_buffers[i] = (u8*)kzalloc(AC97_BUF_SIZE);
        if (!d->dma_buffers[i]) {
            vga_print("[AC97] Failed to allocate DMA buffer\n");
            return false;
        }
        d->dma_phys[i] = (u32)vmm_get_phys((u64)d->dma_buffers[i]);
        if (d->dma_phys[i] == 0) {
            d->dma_phys[i] = (u32)(u64)d->dma_buffers[i];
        }
    }
    return true;
}

// ============================================================
// Public API
// ============================================================

int ac97_init(const pci_device_t* dev) {
    if (!dev) return -1;

    // Check class: Multimedia, Audio, Intel AC'97
    if (dev->class_code != AC97_PCI_CLASS ||
        dev->subclass != AC97_PCI_SUBCLASS) {
        vga_printf("[AC97] Not an audio device (class=%02X subclass=%02X)\n",
                   dev->class_code, dev->subclass);
        return -2;
    }

    kmemset(&drv, 0, sizeof(ac97_t));
    drv.pci_dev = dev;

    // Determine BAR types and addresses
    // ICH AC'97 typically has:
    //   BAR0 = NAMBAR (mixer, I/O space)
    //   BAR1 = NABMBAR (bus master, I/O space)
    u32 bar0 = dev->bar[0];
    u32 bar1 = dev->bar[1];

    drv.nambar = (bar0 & ~0x3);
    drv.nam_is_io = (bar0 & 1) != 0;
    drv.nabmbar = (bar1 & ~0x3);
    drv.nabm_is_io = (bar1 & 1) != 0;
    drv.irq_line = dev->irq_line;

    vga_printf("[AC97] Found at %02X:%02X.%X (NAM=%s 0x%04X, NABM=%s 0x%04X)\n",
               dev->bus, dev->dev, dev->func,
               drv.nam_is_io ? "IO" : "MM", drv.nambar,
               drv.nabm_is_io ? "IO" : "MM", drv.nabmbar);

    // Enable bus mastering and I/O space
    u32 cmd = pci_read_config(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE;
    pci_write_config(dev->bus, dev->dev, dev->func, 0x04, cmd);

    // Cold reset codec via NABMBAR
    nabm_write(&drv, AC97_BM_POCTRL, AC97_BM_CTRL_RESET);
    for (volatile int i = 0; i < 100000; i++);
    nabm_write(&drv, AC97_BM_POCTRL, 0);
    for (volatile int i = 0; i < 100000; i++);

    // Wait for codec ready
    if (!codec_wait_ready(&drv)) {
        vga_print("[AC97] Codec not ready after reset\n");
        return -3;
    }

    // Read codec vendor/product ID
    drv.vendor_id = codec_read_reg(&drv, AC97_MIX_VID);
    drv.product_id = codec_read_reg(&drv, AC97_MIX_PID);
    vga_printf("[AC97] Codec: vendor=0x%04X product=0x%04X\n",
               drv.vendor_id, drv.product_id);

    // Set master volume to reasonable level
    // Master volume register: bits 0-7 = left, bits 8-15 = right
    // 0x0000 = 0 dB (max), 0x3F3F = -46.5 dB (min, muted)
    codec_write_reg(&drv, AC97_MIX_MASTER_VOL, 0x0808);  // ~-3 dB
    // PCM out volume
    codec_write_reg(&drv, AC97_MIX_PCM_VOL, 0x0808);

    // Allocate DMA buffers
    if (!alloc_dma_buffers(&drv)) {
        return -4;
    }

    // Set up Buffer Descriptor List
    setup_bdl(&drv);

    // Enable interrupts on completion
    u8 ctrl = nabm_read(&drv, AC97_BM_POCTRL);
    ctrl |= AC97_BM_CTRL_IOCE;
    nabm_write(&drv, AC97_BM_POCTRL, ctrl);

    drv.initialized = true;
    drv_active = true;

    vga_print("[AC97] Driver initialized (PCM out, ");
    vga_printf("%u x %uKB DMA buffers)\n",
               AC97_NUM_BUFS, AC97_BUF_SIZE / 1024);
    vga_printf("[AC97] IRQ line: %u\n", (u32)drv.irq_line);

    return 0;
}

const ac97_t* ac97_get(void) {
    return drv.initialized ? &drv : NULL;
}

void ac97_print_info(void) {
    if (!drv.initialized) {
        vga_print("[AC97] Not initialized.\n");
        return;
    }

    vga_print("\nAC97 Audio Status:\n");
    vga_print("-------------------\n");
    vga_printf("  Codec:  0x%04X:0x%04X\n", drv.vendor_id, drv.product_id);
    vga_printf("  NAMBAR: %s 0x%04X\n",
               drv.nam_is_io ? "IO" : "MM", drv.nambar);
    vga_printf("  NABMBAR: %s 0x%04X\n",
               drv.nabm_is_io ? "IO" : "MM", drv.nabmbar);
    vga_printf("  DMA buffers: %u x %u KB\n",
               AC97_NUM_BUFS, AC97_BUF_SIZE / 1024);
    vga_printf("  BDL phys: 0x%08X\n", drv.bdl_phys);

    u8 status = nabm_read(&drv, AC97_BM_POSTATUS);
    vga_print("  Status: 0x");
    vga_print_hex(status);
    vga_print(" (");
    if (status & AC97_BM_STATUS_BCIS) vga_print("BCIS ");
    if (status & AC97_BM_STATUS_DCH) vga_print("HALTED ");
    if (status & AC97_BM_STATUS_FIFOS) vga_print("FIFO_ERR ");
    vga_print(")\n");
}

// Integer-only sine lookup for the test tone.
// The kernel is built with -mno-sse/-mno-mmx: any FP operation (even a
// `double` return value) makes GCC fail with "SSE register return with
// SSE disabled". We precompute one period of sine in fixed point.
#define TONE_PHASE_BITS 10
#define TONE_TABLE_SIZE (1 << TONE_PHASE_BITS)          // 1024 entries
static s16 tone_table[TONE_TABLE_SIZE];
static bool tone_table_ready = false;

static void tone_table_init(void) {
    if (tone_table_ready) return;
    // Parabolic sine approximation, amplitude 9830 (~30% of 32767):
    //   sin(t) ~= (4*t*(pi-t)/pi^2) for t in [0..pi]
    for (int i = 0; i < TONE_TABLE_SIZE; i++) {
        // u = fraction of half period [0..8192)
        s64 t = ((s64)i * 8192) >> TONE_PHASE_BITS;      // 0..8191
        s64 val;
        if (t < 4096) {
            val = (4 * t * (4096 - t)) / (4096 * 4096);  // 0..1 in Q12
        } else {
            s64 u = t - 4096;
            val = -(4 * u * (4096 - u)) / (4096 * 4096);
        }
        tone_table[i] = (s16)((val * 9830) >> 12);
    }
    tone_table_ready = true;
}

void ac97_test_tone(void) {
    if (!drv.initialized) {
        vga_print("[AC97] Not initialized.\n");
        return;
    }

    tone_table_init();

    // Generate a 440 Hz sine wave into all DMA buffers using the
    // integer phase accumulator: phase step per sample at 48 kHz =
    // 440 * 1024 / 48000 ~= 9.39 -> fixed point 9*256? Use Q16 steps.
    u32 phase_step = (u32)(((u64)440 * TONE_TABLE_SIZE * 65536) / AC97_SAMPLE_RATE);
    u32 phase = 0;

    u32 total_samples = AC97_NUM_BUFS * (AC97_BUF_SIZE / 4);

    for (u32 buf = 0; buf < AC97_NUM_BUFS; buf++) {
        u8* p = drv.dma_buffers[buf];
        u32 frames = AC97_BUF_SIZE / 4;

        for (u32 i = 0; i < frames; i++) {
            // Generate sine wave sample (16-bit, -32768 to 32767)
            s16 val = tone_table[(phase >> 16) & (TONE_TABLE_SIZE - 1)];

            // Left channel
            p[i * 4 + 0] = val & 0xFF;
            p[i * 4 + 1] = (val >> 8) & 0xFF;
            // Right channel (same)
            p[i * 4 + 2] = val & 0xFF;
            p[i * 4 + 3] = (val >> 8) & 0xFF;

            phase += phase_step;
        }
    }

    // Reset current index to 0
    nabm_write(&drv, AC97_BM_POCIV, 0);

    // Start playback: set Run/Pause bit
    u8 ctrl = nabm_read(&drv, AC97_BM_POCTRL);
    ctrl |= AC97_BM_CTRL_RPBM;
    nabm_write(&drv, AC97_BM_POCTRL, ctrl);

    vga_printf("[AC97] Playing %u ms of 440 Hz tone (%u samples)\n",
               (u32)(total_samples * 1000 / AC97_SAMPLE_RATE),
               total_samples);

    // Wait for playback to complete (~500ms)
    // In a real driver, this would be interrupt-driven
    for (volatile u64 i = 0; i < 30000000; i++);

    // Stop playback
    ctrl = nabm_read(&drv, AC97_BM_POCTRL);
    ctrl &= ~AC97_BM_CTRL_RPBM;
    nabm_write(&drv, AC97_BM_POCTRL, ctrl);

    vga_print("[AC97] Test tone complete.\n");
}

void cmd_ac97(int argc, char** argv) {
    (void)argc;
    if (!drv.initialized) {
        vga_print("[AC97] Driver not initialized.\n");
        vga_print("  Use 'drivers' to init, then 'ac97' again.\n");
        return;
    }

    if (argc >= 2 && kstrcmp(argv[1], "tone") == 0) {
        ac97_test_tone();
    } else {
        ac97_print_info();
    }
}
