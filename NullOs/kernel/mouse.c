#include "../include/mouse.h"
#include "../include/keyboard.h"
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/timer.h"

// ============================================================
// PS/2 Mouse Driver for NullOs
// ============================================================
//
// PS/2 mouse uses IRQ12 (mapped to vector 44 in IDT).
// The mouse is the "auxiliary device" on the PS/2 controller.
//
// Protocol: Stream mode, 3-byte packets:
//   Byte 0: YO X7 Y6 Y5 X6 Y4 Y3 X5  1  M  R  L
//   Byte 1: X7..X0 (movement in X)
//   Byte 2: Y7..Y0 (movement in Y)
//
// For scroll wheel (IntelliMouse Explorer, ID=4):
//   Byte 0: YO X7 Y6 Y5 X6 Y4 Y3 X5  1  1  R  L
//   Byte 3: Z7..Z0 (scroll) + buttons 4/5
// ============================================================

// Mouse state
static mouse_state_t mouse;
static bool mouse_visible = true;

// Cursor backup for XOR drawing
static u16 cursor_backup[2 * 12];  // 2 rows x 12 cols cursor

// Packet assembly state
static u8  packet_bytes[4];
static u8  packet_index = 0;
static u8  mouse_device_id = 0;  // 0=standard, 3=IntelliMouse, 4=Explorer

// ---- PS/2 helper functions ----

static void mouse_wait_input(void) {
    u32 start = timer_get_ticks();
    while ((inb(KBD_STATUS_PORT) & KBD_STATUS_INPUT_BUFFER) != 0) {
        if ((timer_get_ticks() - start) > 100) return;
    }
}

static bool mouse_wait_output(void) {
    u32 start = timer_get_ticks();
    while ((inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) == 0) {
        if ((timer_get_ticks() - start) > 100) return false;
    }
    return true;
}

static bool mouse_write_cmd(u8 cmd) {
    mouse_wait_input();
    outb(KBD_COMMAND_PORT, MOUSE_CMD_AUX_WRITE);
    mouse_wait_input();
    outb(KBD_DATA_PORT, cmd);

    if (!mouse_wait_output()) return false;
    return inb(KBD_DATA_PORT) == MOUSE_REPLY_ACK;
}

static bool mouse_read_byte(u8* val) {
    if (!mouse_wait_output()) return false;
    *val = inb(KBD_DATA_PORT);
    return true;
}

/* Flush stale bytes from the controller output buffer. Before the
 * aux handshake the KEYBOARD may have left ACKs/self-test results
 * queued; without this, mouse_write_cmd consumes a keyboard byte as
 * the mouse ACK and the whole stream desyncs (#mouse-stale-obuf). */
static void mouse_drain_output(void) {
    for (int i = 0; i < 32; i++) {
        if ((inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) == 0)
            break;
        (void)inb(KBD_DATA_PORT);
        for (volatile int d = 0; d < 200; d++);
    }
}

/* Controller configuration byte (command 0x20 read / 0x60 write):
 * bit1 = enable aux (mouse) IRQ12, bit5 = aux clock disable.
 * The historical driver NEVER enabled the aux IRQ — packets arrived
 * from the device but IRQ12 stayed silent (#mouse-irq-never-armed). */
static bool mouse_controller_aux_irq(bool enable) {
    mouse_wait_input();
    outb(KBD_COMMAND_PORT, 0x20);            /* read config            */
    if (!mouse_wait_output()) return false;
    u8 cfg = inb(KBD_DATA_PORT);
    if (enable) cfg |=  (1u << 1);           /* enable aux IRQ         */
    else        cfg &= ~(1u << 1);
    if (enable) cfg &= ~(1u << 5);           /* aux clock must run     */
    mouse_wait_input();
    outb(KBD_COMMAND_PORT, 0x60);            /* write config           */
    mouse_wait_input();
    outb(KBD_DATA_PORT, cfg);
    return true;
}

// ---- Cursor drawing ----

// Simple arrow cursor: 2 bytes wide, 12 rows
// Bit 1 = foreground pixel, Bit 0 = background pixel
static const u16 cursor_shape[12] = {
    0b0000001100000000,
    0b0000011110000000,
    0b0000111111000000,
    0b0001111111100000,
    0b0011111111110000,
    0b0011111111110000,
    0b0111111111111000,
    0b0110011110011000,
    0b0110000110001100,
    0b0110000110001100,
    0b0000000110000000,
    0b0000000110000000,
};

#define CURSOR_W  12
#define CURSOR_H  12
#define CURSOR_FG VGA_WHITE
#define CURSOR_BG VGA_BLACK

static void save_cursor_area(s32 mx, s32 my) {
    for (s32 row = 0; row < CURSOR_H; row++) {
        s32 screen_y = my + row;
        if (screen_y < 0 || screen_y >= VGA_HEIGHT) continue;
        for (s32 col = 0; col < (CURSOR_W + 7) / 8; col++) {
            s32 screen_x = mx + col * 8;
            if (screen_x < 0 || screen_x >= VGA_WIDTH) continue;
            // Read from VGA memory
            cursor_backup[row * ((CURSOR_W + 7) / 8) + col] =
                *((volatile u16*)(u64)(VGA_MEMORY + (screen_y * VGA_WIDTH + screen_x) * 2));
        }
    }
}

static void restore_cursor_area(s32 mx, s32 my) {
    for (s32 row = 0; row < CURSOR_H; row++) {
        s32 screen_y = my + row;
        if (screen_y < 0 || screen_y >= VGA_HEIGHT) continue;
        for (s32 col = 0; col < (CURSOR_W + 7) / 8; col++) {
            s32 screen_x = mx + col * 8;
            if (screen_x < 0 || screen_x >= VGA_WIDTH) continue;
            *((volatile u16*)(u64)(VGA_MEMORY + (screen_y * VGA_WIDTH + screen_x) * 2)) =
                cursor_backup[row * ((CURSOR_W + 7) / 8) + col];
        }
    }
}

static void draw_cursor_pixels(s32 mx, s32 my) {
    for (s32 row = 0; row < CURSOR_H; row++) {
        s32 screen_y = my + row;
        if (screen_y < 0 || screen_y >= VGA_HEIGHT) continue;
        u16 bits = cursor_shape[row];
        for (s32 bit = 0; bit < CURSOR_W; bit++) {
            if (!(bits & (1 << (15 - bit)))) continue;
            s32 screen_x = mx + bit;
            if (screen_x < 0 || screen_x >= VGA_WIDTH) continue;
            // Draw foreground pixel
            *((volatile u16*)(u64)(VGA_MEMORY + (screen_y * VGA_WIDTH + screen_x) * 2)) =
                (u16)(CURSOR_BG << 12 | CURSOR_FG);
        }
    }
}

// ---- Packet processing ----

static void mouse_process_standard_packet(void) {
    u8 b0 = packet_bytes[0];
    u8 b1 = packet_bytes[1];
    u8 b2 = packet_bytes[2];

    // Save previous buttons
    mouse.buttons_prev = mouse.buttons;

    // Update position
    mouse.dx += (s32)(s8)b1;
    mouse.dy += (s32)(s8)b2;

    // Update buttons: bit 0=left, bit 1=right, bit 2=middle
    mouse.buttons = 0;
    if (b0 & 0x01) mouse.buttons |= 0x01;  // Left
    if (b0 & 0x02) mouse.buttons |= 0x04;  // Middle (PS/2 bit 2 = our bit 2)
    if (b0 & 0x04) mouse.buttons |= 0x02;  // Right  (PS/2 bit 1 = our bit 1)

    mouse.dz = 0;
    mouse.packet_count++;
}

static void mouse_process_explorer_packet(void) {
    mouse_process_standard_packet();

    if (mouse_device_id >= 3 && packet_index > 3) {
        u8 b3 = packet_bytes[3];
        mouse.dz = (s8)(b3 & 0x0F);
        // Sign extend 4-bit to 8-bit
        if (mouse.dz & 0x08) mouse.dz |= (s8)0xF0;

        // Extra buttons
        if (b3 & 0x10) mouse.buttons |= 0x08;  // Button 4 (back)
        if (b3 & 0x20) mouse.buttons |= 0x10;  // Button 5 (forward)
    } else if (mouse_device_id >= 3) {
        u8 b3 = packet_bytes[3];
        mouse.dz = (s8)b3;
    }
}

// ============================================================
// Public API
// ============================================================

bool mouse_init(void) {
    kmemset(&mouse, 0, sizeof(mouse_state_t));
    packet_index = 0;
    mouse_device_id = 0;
    mouse.enabled = false;
    mouse_visible = true;

    // Step 0: flush stale keyboard bytes from the output buffer, then
    // arm the aux channel: enable IRQ12 in the controller config and
    // enable the auxiliary port. (#mouse-stale-obuf,
    //  #mouse-irq-never-armed — the mouse had NEVER initialized here.)
    mouse_drain_output();
    if (!mouse_controller_aux_irq(true)) {
        vga_print("[MOUSE] Controller config write failed\n");
        return false;
    }
    mouse_wait_input();
    outb(KBD_COMMAND_PORT, MOUSE_CMD_AUX_ENABLE);
    for (volatile int i = 0; i < 1000; i++);
    mouse_drain_output();

    // Step 2: Send mouse reset (with byte-level forensics)
    {
        extern void serial_printf(const char*, ...);
        mouse_wait_input();
        outb(KBD_COMMAND_PORT, MOUSE_CMD_AUX_WRITE);
        mouse_wait_input();
        outb(KBD_DATA_PORT, MOUSE_CMD_RESET);
        u8 b0 = 0, b1 = 0, b2 = 0;
        bool g0 = mouse_wait_output(), g1 = false, g2 = false;
        if (g0) b0 = inb(KBD_DATA_PORT);
        g1 = mouse_wait_output();
        if (g1) b1 = inb(KBD_DATA_PORT);
        g2 = mouse_wait_output();
        if (g2) b2 = inb(KBD_DATA_PORT);
        serial_printf("[MOUSEDBG] reset resp: g=%d%d%d b=%02x %02x %02x\n",
                      g0, g1, g2, b0, b1, b2);
        if (!g0 || b0 != MOUSE_REPLY_ACK) {
            vga_print("[MOUSE] No response to reset (no mouse?)\n");
            return false;
        }
        if (g1 && b1 == MOUSE_REPLY_BAT_OK) {
            u8 dev_id = g2 ? b2 : 0;
            if (!g2) mouse_read_byte(&dev_id);
            mouse_device_id = dev_id;
        } else if (!mouse_read_byte(&b1) || b1 != MOUSE_REPLY_BAT_OK) {
            vga_print("[MOUSE] Self-test failed\n");
            return false;
        } else {
            u8 dev_id = 0;
            if (!mouse_read_byte(&dev_id)) {
                vga_print("[MOUSE] No device ID after BAT\n");
                return false;
            }
            mouse_device_id = dev_id;
        }
    }

    // Step 3: Set default settings
    mouse_write_cmd(MOUSE_CMD_SET_DEFAULTS);

    // Step 4: Enable scroll wheel (IntelliMouse) if possible
    // Try to set device ID to 3 (IntelliMouse)
    mouse_write_cmd(MOUSE_CMD_SET_SAMPLE);
    mouse_write_cmd(200);  // 200 samples/sec
    mouse_write_cmd(MOUSE_CMD_SET_SAMPLE);
    mouse_write_cmd(100);
    mouse_write_cmd(MOUSE_CMD_SET_SAMPLE);
    mouse_write_cmd(80);

    // Request IntelliMouse ID
    mouse_write_cmd(MOUSE_CMD_GET_ID);
    u8 new_id;
    if (mouse_read_byte(&new_id) && new_id == 0x03) {
        mouse_device_id = 3;
        vga_print("[MOUSE] IntelliMouse detected (scroll wheel)\n");

        // Try to get Explorer ID (5-button mouse with scroll)
        mouse_write_cmd(MOUSE_CMD_SET_SAMPLE);
        mouse_write_cmd(200);
        mouse_write_cmd(MOUSE_CMD_SET_SAMPLE);
        mouse_write_cmd(200);
        mouse_write_cmd(MOUSE_CMD_SET_SAMPLE);
        mouse_write_cmd(80);
        mouse_write_cmd(MOUSE_CMD_GET_ID);
        u8 exp_id;
        if (mouse_read_byte(&exp_id) && exp_id == 0x04) {
            mouse_device_id = 4;
            vga_print("[MOUSE] Explorer mouse detected (5 buttons)\n");
        }
    }

    // Step 5: Set sample rate to 100 Hz
    mouse_write_cmd(MOUSE_CMD_SET_SAMPLE);
    mouse_write_cmd(100);

    // Step 6: Enable stream mode (continuous reporting)
    if (!mouse_write_cmd(MOUSE_CMD_ENABLE_STREAM)) {
        vga_print("[MOUSE] Failed to enable stream mode\n");
        return false;
    }

    mouse.x = VGA_WIDTH / 2;
    mouse.y = VGA_HEIGHT / 2;
    mouse.enabled = true;
    mouse.packet_count = 0;

    const char* type_str = "Standard";
    if (mouse_device_id == 3) type_str = "IntelliMouse";
    else if (mouse_device_id == 4) type_str = "Explorer";

    vga_printf("[MOUSE] PS/2 mouse initialized (ID=%u, %s)\n",
               (u32)mouse_device_id, type_str);
    vga_print("[MOUSE] IRQ12 enabled, stream mode active\n");

    return true;
}

void mouse_irq_handler(void) {
    extern void vmm_pml4_guard(int); vmm_pml4_guard(4);
    if (!mouse.enabled) {
        // Read and discard
        inb(KBD_DATA_PORT);
        return;
    }

    u8 data = inb(KBD_DATA_PORT);

    // Check if this is a mouse packet (bit 3 of byte 0 must be set)
    // For the first byte of a packet
    if (packet_index == 0 && !(data & 0x08)) {
        // Not a valid packet start — discard
        return;
    }

    /* FIX(#mouse-midpacket-resync): the old mid-packet resync fired on
     * ANY data byte with bit 3 set — but X/Y delta bytes legitimately
     * have bit 3 set for half of all values (0x08-0x0F, 0x18-0x1F...),
     * so normal fast movement desynchronized the stream: a delta byte
     * got stored as a status byte and every following packet was
     * parsed against the wrong layout (garbage deltas, phantom clicks,
     * cursor jumps). Plausibility testing belongs ONLY to byte 0
     * (checked above); mid-packet bytes are accepted as-is. */
    packet_bytes[packet_index++] = data;

    u8 expected = (mouse_device_id >= 3) ? 4 : 3;
    if (packet_index >= expected) {
        // Process complete packet
        if (mouse_device_id >= 3) {
            mouse_process_explorer_packet();
        } else {
            mouse_process_standard_packet();
        }

        // Update absolute position
        s32 old_x = mouse.x;
        s32 old_y = mouse.y;

        mouse.x += mouse.dx;
        mouse.y += mouse.dy;

        // Clamp to screen bounds
        if (mouse.x < 0) mouse.x = 0;
        if (mouse.x >= VGA_WIDTH) mouse.x = VGA_WIDTH - 1;
        if (mouse.y < 0) mouse.y = 0;
        if (mouse.y >= VGA_HEIGHT) mouse.y = VGA_HEIGHT - 1;

        // Redraw cursor if position changed and visible
        if (mouse_visible && (mouse.x != old_x || mouse.y != old_y || mouse.dx != 0 || mouse.dy != 0)) {
            // Hide at old position, show at new position
            restore_cursor_area(old_x, old_y);
            save_cursor_area(mouse.x, mouse.y);
            draw_cursor_pixels(mouse.x, mouse.y);
        }

        // evdev mirror: RAW packet deltas (pre-clamp values that were
        // just consumed for the position update above), button
        // transitions, wheel — the libinput substrate stream.
        {
            extern void evdev_mouse_packet(s32, s32, s32, u32, u32);
            evdev_mouse_packet(mouse.dx, mouse.dy, mouse.dz,
                               mouse.buttons, mouse.buttons_prev);
        }

        // Reset deltas
        mouse.dx = 0;
        mouse.dy = 0;
        mouse.dz = 0;

        // Reset packet assembly
        packet_index = 0;
    }
}

const mouse_state_t* mouse_get_state(void) {
    return &mouse;
}

bool mouse_button_clicked(u8 button_mask) {
    return (mouse.buttons & button_mask) && !(mouse.buttons_prev & button_mask);
}

bool mouse_button_released(u8 button_mask) {
    return !(mouse.buttons & button_mask) && (mouse.buttons_prev & button_mask);
}

void mouse_set_visible(bool visible) {
    if (visible && !mouse_visible && mouse.enabled) {
        // Show cursor
        mouse_visible = true;
        save_cursor_area(mouse.x, mouse.y);
        draw_cursor_pixels(mouse.x, mouse.y);
    } else if (!visible && mouse_visible) {
        // Hide cursor
        restore_cursor_area(mouse.x, mouse.y);
        mouse_visible = false;
    }
}

bool mouse_get_visible(void) {
    return mouse_visible;
}

void mouse_draw_cursor(void) {
    if (!mouse.enabled || !mouse_visible) return;
    save_cursor_area(mouse.x, mouse.y);
    draw_cursor_pixels(mouse.x, mouse.y);
}

void mouse_hide_cursor(void) {
    if (!mouse.enabled || !mouse_visible) return;
    restore_cursor_area(mouse.x, mouse.y);
}

// ============================================================
// Shell command: mouse
// ============================================================

void cmd_mouse(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!mouse.enabled) {
        vga_print("[MOUSE] Not initialized or no mouse detected.\n");
        return;
    }

    const mouse_state_t* s = mouse_get_state();
    vga_print("\nPS/2 Mouse State:\n");
    vga_print("------------------\n");
    vga_printf("  Position:  (%d, %d)\n", s->x, s->y);
    vga_printf("  Buttons:   %s%s%s\n",
               (s->buttons & 0x01) ? "L" : "_",
               (s->buttons & 0x02) ? "R" : "_",
               (s->buttons & 0x04) ? "M" : "_");
    vga_printf("  Device ID: %u (%s)\n",
               (u32)mouse_device_id,
               mouse_device_id >= 4 ? "Explorer" :
               mouse_device_id >= 3 ? "IntelliMouse" : "Standard");
    vga_printf("  Packets:   %u\n", (u32)s->packet_count);
    vga_printf("  Visible:   %s\n", mouse_visible ? "yes" : "no");
    vga_print("\n");
}
