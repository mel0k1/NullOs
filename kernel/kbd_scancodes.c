#include "../include/kbd_scancodes.h"
#include "../include/keyboard.h"
#include "../include/vga.h"
#include "../include/timer.h"

// ============================================================
// PS/2 keyboard command interface
// ============================================================

static void kbd_wait_input_ready(void) {
    u32 start = timer_get_ticks();
    while ((inb(KBD_STATUS_PORT) & KBD_STATUS_INPUT_BUFFER) != 0) {
        if ((timer_get_ticks() - start) > 100) return;  // 1 second timeout
    }
}

static bool kbd_wait_output_ready(void) {
    u32 start = timer_get_ticks();
    while ((inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) == 0) {
        if ((timer_get_ticks() - start) > 100) return false;
    }
    return true;
}

void kbd_controller_cmd(u8 cmd) {
    kbd_wait_input_ready();
    outb(KBD_COMMAND_PORT, cmd);
}

void kbd_send_cmd(u8 cmd) {
    kbd_wait_input_ready();
    outb(KBD_DATA_PORT, cmd);
}

bool kbd_wait_ack(u32 timeout_ms) {
    u32 start = timer_get_ticks();
    u32 tick_limit = (timeout_ms * TIMER_FREQ + 999) / 1000;

    while ((timer_get_ticks() - start) < tick_limit) {
        if (kbd_wait_output_ready()) {
            u8 reply = inb(KBD_DATA_PORT);
            if (reply == KBD_REPLY_ACK) return true;
            if (reply == KBD_REPLY_RESEND) return false;
        }
    }
    return false;
}

bool kbd_switch_to_set2(void) {
    // Disable scanning first
    kbd_send_cmd(KBD_CMD_DISABLE_SCANNING);
    if (!kbd_wait_ack(100)) {
        vga_print("[KBD] WARNING: No ACK on disable scanning\n");
    }

    // Small delay
    for (volatile int i = 0; i < 10000; i++);

    // Set scancode set 2
    kbd_send_cmd(KBD_CMD_SET_SCANCODE_SET);
    if (!kbd_wait_ack(100)) {
        vga_print("[KBD] WARNING: No ACK on set scancode command\n");
    }

    kbd_send_cmd(KBD_CMD_SCANCODE_SET_2);
    if (!kbd_wait_ack(100)) {
        vga_print("[KBD] WARNING: No ACK on set scancode set 2\n");
        // Try to re-enable scanning and return false
        kbd_send_cmd(KBD_CMD_ENABLE_SCANNING);
        return false;
    }

    // Small delay
    for (volatile int i = 0; i < 10000; i++);

    // Re-enable scanning
    kbd_send_cmd(KBD_CMD_ENABLE_SCANNING);
    if (!kbd_wait_ack(100)) {
        vga_print("[KBD] WARNING: No ACK on re-enable scanning\n");
    }

    vga_print("[KBD] Switched to scancode set 2.\n");
    return true;
}

// ============================================================
// Set 2 → Set 1 translation
// ============================================================

u8 kbd_set2_to_set1(u8 scancode, bool extended) {
    if (extended) {
        if (scancode < 128) {
            return set2_ext_to_set1_table[scancode];
        }
        return 0;
    }
    if (scancode < 128) {
        return set2_to_set1_table[scancode];
    }
    return 0;
}
