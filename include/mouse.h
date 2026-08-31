#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mouse state
#define MOUSE_BUFFER_SIZE 128

typedef struct {
    s32 x;           // Current X position (screen coords, 0..79)
    s32 y;           // Current Y position (screen coords, 0..24)
    s32 dx;          // Delta X since last read
    s32 dy;          // Delta Y since last read
    s8  dz;          // Delta Z (scroll wheel)
    u8  buttons;     // Button state: bit0=left, bit1=middle, bit2=right
    u8  buttons_prev; // Previous button state (for click detection)
    bool enabled;    // Mouse is active
    u32 packet_count; // Packets processed
} mouse_state_t;

// PS/2 Mouse commands
#define MOUSE_CMD_SET_DEFAULTS   0xF6
#define MOUSE_CMD_ENABLE_STREAM  0xF8
#define MOUSE_CMD_DISABLE_STREAM 0xF5
#define MOUSE_CMD_SET_SAMPLE     0xF3
#define MOUSE_CMD_GET_ID         0xF2
#define MOUSE_CMD_SET_WRAP       0xEC
#define MOUSE_CMD_RESET          0xFF

// PS/2 Controller commands for aux device
#define MOUSE_CMD_AUX_ENABLE     0xA8
#define MOUSE_CMD_AUX_DISABLE    0xA7
#define MOUSE_CMD_AUX_WRITE      0xD4

// Mouse reply codes
#define MOUSE_REPLY_ACK          0xFA
#define MOUSE_REPLY_NAK          0xFE
#define MOUSE_REPLY_BAT_OK       0xAA
#define MOUSE_REPLY_BAT_FAIL     0xFC

// Initialize PS/2 mouse driver
// Returns true on success, false if no mouse detected
bool mouse_init(void);

// Handle mouse IRQ (call from irq_handler for IRQ12)
void mouse_irq_handler(void);

// Get current mouse state
const mouse_state_t* mouse_get_state(void);

// Check if a button was just clicked (rising edge)
bool mouse_button_clicked(u8 button_mask);

// Check if a button was just released (falling edge)
bool mouse_button_released(u8 button_mask);

// Enable/disable mouse cursor drawing
void mouse_set_visible(bool visible);
bool mouse_get_visible(void);

// Draw/hide cursor at current position
void mouse_draw_cursor(void);
void mouse_hide_cursor(void);

#ifdef __cplusplus
}
#endif

#endif // MOUSE_H
