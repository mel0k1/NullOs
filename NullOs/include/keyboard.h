#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ports
#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_COMMAND_PORT 0x64

// Status bits
#define KBD_STATUS_OUTPUT_BUFFER  0x01
#define KBD_STATUS_INPUT_BUFFER   0x02
#define KBD_STATUS_SYSTEM_FLAG    0x04
#define KBD_STATUS_COMMAND_DATA   0x08
#define KBD_STATUS_TIMEOUT_ERROR  0x40
#define KBD_STATUS_PARITY_ERROR   0x80

// Modifier bits
#define KBD_CTRL_PRESSED  0x01
#define KBD_SHIFT_PRESSED 0x02
#define KBD_ALT_PRESSED   0x04

// Buffer size
#define KBD_BUFFER_SIZE   256

// Keyboard state
struct keyboard_state {
    u8  buffer[KBD_BUFFER_SIZE];
    volatile u32 head;   // written by IRQ producer
    volatile u32 tail;   // written by task-context consumer
    u8  modifiers;
    bool caps_lock;
    bool scroll_lock;
    bool num_lock;
};
typedef struct keyboard_state keyboard_state_t;

// Functions
void    keyboard_init(void);
void    keyboard_handler(void);
char    keyboard_getchar(void);   // Blocking input (translates scancode to char)
bool    keyboard_haschar(void);   // Check if char available
u8      keyboard_read_scancode(void);
u8      keyboard_buffer_pop(void);  // Pop raw scancode from buffer
u8      keyboard_get_modifiers(void);  // Get current modifier state

// Scancode to char translation (used by shell for raw input)
char    keyboard_scancode_to_char(u8 scancode, u8 modifiers);

// Modifier management (for shell raw-input mode)
void    keyboard_set_modifier(u8 bit);
void    keyboard_clear_modifier(u8 bit);
void    keyboard_toggle_caps_lock(void);

// Raw mode: disables scancode translation and modifier tracking.
// In raw mode, keyboard_handler() buffers raw bytes as-is.
// keyboard_getchar() returns 0 (use keyboard_buffer_pop() for raw scancodes).
void    keyboard_set_raw_mode(bool enabled);
bool    keyboard_get_raw_mode(void);

// Enable scancode set 2 (optional, call after keyboard_init)
void    keyboard_enable_set2(void);

#ifdef __cplusplus
}
#endif

#endif // KEYBOARD_H
