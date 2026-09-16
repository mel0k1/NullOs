#ifndef AC97_H
#define AC97_H

#include "types.h"
#include "pci.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Intel AC'97 Audio Controller Driver
// ============================================================
//
// Supports PCI audio devices with class 0x04, subclass 0x01
// (Audio, prog_if 0x00 = Intel ICH AC'97).
//
// The ICH AC'97 controller uses a NAMBAR (Native Audio Mixer
// Base Address Register) for mixer access and an NABMBAR for
// Bus Master DMA.
//
// Limitations:
//   - PCM output only (no input)
//   - Single buffer playback
//   - 16-bit stereo, 48 kHz
//   - No volume control via mixer (uses default)
// ============================================================

// AC97 PCI class codes
#define AC97_PCI_CLASS    0x04
#define AC97_PCI_SUBCLASS 0x01

// Intel ICH AC'97 Bus Master Register offsets (from NABMBAR)
// These are for the PCM OUT channel
#define AC97_BM_POBDB   0x10  // PCM Out Buffer Descriptor Base
#define AC97_BM_POCIV   0x14  // PCM Out Current Index Value
#define AC97_BM_POLV    0x18  // PCM Out Last Valid Index
#define AC97_BM_POSTATUS 0x1C // PCM Out Status
#define AC97_BM_POCTRL  0x1D // PCM Out Control

// Bus Master Status bits
#define AC97_BM_STATUS_BCIS  (1 << 2)  // Buffer Completion Interrupt Status
#define AC97_BM_STATUS_LVBCI (1 << 3)  // Last Valid Buffer Completion Interrupt
#define AC97_BM_STATUS_FIFOS (1 << 4)  // FIFO Error
#define AC97_BM_STATUS_PI    (1 << 5)  // Preferred Interrupt
#define AC97_BM_STATUS_DCH   (1 << 0)  // DMA Controller Halted
#define AC97_BM_STATUS_CELV  (1 << 1)  // Current Equals Last Valid

// Bus Master Control bits
#define AC97_BM_CTRL_IOCE    (1 << 0)  // Interrupt On Completion Enable
#define AC97_BM_CTRL_IOIE    (1 << 1)  // Interrupt On Last Enable
#define AC97_BM_CTRL_RPBM    (1 << 3)  // Run/Pause Bus Master
#define AC97_BM_CTRL_RESET   (1 << 2)  // Reset

// NAMBAR (Native Audio Mixer BAR) offsets
#define AC97_MIX_RESET     0x00  // Codec Reset Register
#define AC97_MIX_MASTER_VOL 0x02  // Master Volume
#define AC97_MIX_PCM_VOL   0x18  // PCM Out Volume
#define AC97_MIX_EXT_VOL   0x3A  // Extended Audio Status/Control
#define AC97_MIX_VID       0x24  // Vendor ID
#define AC97_MIX_PID       0x26  // Product ID

// AC97 register access constants
#define AC97_CODEC_READY_TIMEOUT  1000
#define AC97_CODEC_SLOT_SHIFT    16

// Buffer Descriptor List Entry
// Each entry is 8 bytes: 2 bytes flags + 4 bytes buffer ptr + 2 bytes length
#define AC97_BD_IOC  (1 << 15) // Interrupt on Completion
#define AC97_BD_BUP  (1 << 14) // Buffer Underrun Policy

#define AC97_NUM_BUFS  4
#define AC97_BUF_SIZE 4096  // 4 KB per buffer
#define AC97_SAMPLE_RATE 48000
#define AC97_CHANNELS    2
#define AC97_BITS_PER_SAMPLE 16

// Driver state
typedef struct {
    const pci_device_t* pci_dev;
    u32 nabmbar;       // Bus Master BAR (I/O or MMIO)
    u32 nambar;       // Mixer BAR (I/O)
    u16 irq_line;
    bool nabm_is_io;    // true = I/O port, false = MMIO
    bool nam_is_io;
    bool initialized;
    u32 samples_played;
    u16 vendor_id;
    u16 product_id;
    // DMA buffers
    u8*  dma_buffers[AC97_NUM_BUFS];
    u32  dma_phys[AC97_NUM_BUFS];
    u8*  bdl;           // Buffer Descriptor List
    u32  bdl_phys;
} ac97_t;

// Initialize AC97 driver for the given PCI device
// Returns 0 on success, negative on error
int ac97_init(const pci_device_t* dev);

// Get driver state
const ac97_t* ac97_get(void);

// Print status info
void ac97_print_info(void);

// Play a simple test tone (440 Hz sine wave, 0.5 seconds)
void ac97_test_tone(void);

// Shell command
void cmd_ac97(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // AC97_H
