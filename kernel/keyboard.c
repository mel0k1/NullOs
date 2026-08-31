#include "../include/keyboard.h"
#include "../include/vga.h"
#include "../include/kbd_scancodes.h"

// US QWERTY scancode set 1
static const char scancode_to_char[] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};

// Shift layout
static const char scancode_to_char_shift[] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

// Global keyboard state.
// head/tail are volatile: head is written by the IRQ handler (producer),
// tail by task context (consumer). Without volatile the compiler may
// cache them across the hlt() wait loops in the shell/getchar paths.
static keyboard_state_t kbd_state;
static bool kbd_use_set2 = false;  // Set to true after kbd_switch_to_set2()
static bool kbd_in_f0_prefix = false;  // Set 2 break code prefix
static bool kbd_in_e0_prefix = false;  // Extended key prefix
static bool kbd_raw_mode = false;  // Raw mode for games/fullscreen
/* Consumer-stream Ctrl state: updated by keyboard_getchar as it pops
 * events IN ORDER (authoritative for any scancode set, unlike the
 * producer's ring-lazy modifiers). */
static bool kb_getchar_ctrl = false;

void keyboard_init() {
    kbd_state.head = 0;
    kbd_state.tail = 0;
    kbd_state.modifiers = 0;
    kbd_state.caps_lock = false;
    kbd_state.scroll_lock = false;
    kbd_state.num_lock = false;
    kbd_use_set2 = false;
    kbd_in_f0_prefix = false;
    kbd_in_e0_prefix = false;

    for (int i = 0; i < KBD_BUFFER_SIZE; i++) {
        kbd_state.buffer[i] = 0;
    }
}

// Enable scancode set 2 (call after keyboard_init)
void keyboard_enable_set2(void) {
    extern void serial_printf(const char*, ...);
    if (kbd_switch_to_set2()) {
        kbd_use_set2 = true;
        serial_printf("[KBD] Set2 ENABLED\n");
    } else {
        vga_print("[KBD] Set 2 switch failed, staying on Set 1.\n");
        serial_printf("[KBD] Set2 SWITCH FAILED - staying on Set1\n");
    }
}

u8 keyboard_read_scancode() {
    while ((inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) == 0);
    return inb(KBD_DATA_PORT);
}

static void keyboard_buffer_push(u8 scancode) {
    u32 next_head = (kbd_state.head + 1) % KBD_BUFFER_SIZE;
    if (next_head == kbd_state.tail) return;  // buffer full
    kbd_state.buffer[kbd_state.head] = scancode;
    kbd_state.head = next_head;
}

u8 keyboard_buffer_pop() {
    if (kbd_state.head == kbd_state.tail) return 0;
    u8 scancode = kbd_state.buffer[kbd_state.tail];
    kbd_state.tail = (kbd_state.tail + 1) % KBD_BUFFER_SIZE;
    return scancode;
}

bool keyboard_haschar() {
    return kbd_state.head != kbd_state.tail;
}

u8 keyboard_get_modifiers() {
    return kbd_state.modifiers;
}

char keyboard_scancode_to_char(u8 scancode, u8 modifiers) {
    char c = 0;
    if (scancode < sizeof(scancode_to_char)) {
        if (modifiers & KBD_SHIFT_PRESSED) {
            c = scancode_to_char_shift[scancode];
        } else {
            c = scancode_to_char[scancode];
        }
        // Caps Lock for letters
        if (kbd_state.caps_lock && c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        } else if (kbd_state.caps_lock && c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }
    return c;
}

char keyboard_getchar() {
    while (!keyboard_haschar()) {
        hlt();
    }

    u8 scancode = keyboard_buffer_pop();

    bool released = (scancode & 0x80) != 0;
    scancode &= 0x7F;

    /* Stream-tracked Ctrl state (consumer side): authoritative for ANY
     * scancode-set because this ring preserves event ORDER exactly as
     * produced. Producer-side kbd_state.modifiers may be stale/absent
     * when running raw Set-1 (no translation hook there), so ISIG must
     * not depend on it. Tracking here keeps Ctrl-fold correct even for
     * raw streams where make/break interleave arbitrarily deep.      */
    {
        if (scancode == 0x1D) {
            kb_getchar_ctrl = !released;
            return 0;                     /* modifier: never data      */
        }
    }

    /* Modifier/caps state is maintained at IRQ time by the producer
     * (keyboard_handler) so ordering survives buffer overflow. Here we
     * only swallow these codes so they never look like data. */
    switch (scancode) {
        case 0x2A: case 0x36:                 /* Shift            */
        case 0x38:                            /* Alt              */
        case 0x3A:                            /* Caps Lock        */
            return 0;
    }

    if (released) return 0;

    char c = keyboard_scancode_to_char(scancode, kbd_state.modifiers);

    /* Ctrl-letter folding + VSUSP delivery in the CONSUMER as a safety
     * net covering raw Set-1 operation (the producer hook only runs in
     * Set-2 mode). POSIX: Ctrl+letter = c&0x1F.                       */
    {
        if (kb_getchar_ctrl) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
            else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        }
        if ((u8)c == 26 && !kbd_raw_mode) {
            extern void tty_on_ctrl_z(void);
            tty_on_ctrl_z();
            return 0;
        }
    }
    return c;
}

void keyboard_handler() {
    {
        static unsigned int kdbg = 0;
        if ((++kdbg & 7) == 1) {
            extern void serial_printf(const char*, ...);
            serial_printf("[KBD] hits=%u\n", kdbg);
        }
    }
    u8 raw = keyboard_read_scancode();

    // Raw mode: buffer raw bytes without any processing
    if (kbd_raw_mode) {
        keyboard_buffer_push(raw);
        return;
    }

    /* ============================================================
     * RAW SET-1 MODE ISIG (fork-v2): without Set-2 translation this
     * handler historically pushed EVERYTHING straight to the ring,
     * which meant (a) no modifier tracking at all and (b) ^Z waiting
     * inside the ring until some reader showed up — meanwhile ash is
     * blocked in wait4() with NO reader, so jobs could never be
     * stopped by keyboard. Track Ctrl here minimally and deliver
     * VSUSP at IRQ time exactly like the Set-2 path does.
     * ============================================================ */
    if (!kbd_use_set2) {
        static bool s1_prod_ctrl = false;
        extern void serial_printf(const char*, ...);
        u8 code = raw & 0x7F;
        bool brk = (raw & 0x80) != 0;
        if (code == 0x1D) {                    /* Ctrl L               */
            s1_prod_ctrl = !brk;
            return;                            /* never data           */
        }
        if (code == 0x2A || code == 0x36 || code == 0x38 ||
            code == 0x3A)
            return;                            /* other modifiers      */
        if (!brk) {
            char c = keyboard_scancode_to_char(code, kbd_state.modifiers);
            if (s1_prod_ctrl && c >= 'a' && c <= 'z')
                c = (char)(c - 'a' + 1);
            else if (s1_prod_ctrl && c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 1);
            if ((u8)c == 26) {                 /* VSUSP (^Z)           */
                extern void tty_on_ctrl_z(void);
                serial_printf("[ISIG] VSUSP at prod (set1)\n");
                tty_on_ctrl_z();
                return;
            }
        }
        keyboard_buffer_push(raw);
        return;
    }

    /* (Set-1 handling completed above; Set-2 translation follows.) */

    // Handle F0 prefix (break code)
    if (raw == 0xF0) {
        kbd_in_f0_prefix = true;
        return;
    }

    // Handle E0 prefix (extended key)
    if (raw == 0xE0) {
        kbd_in_e0_prefix = true;
        return;
    }

    // Translate scancode
    bool is_break = kbd_in_f0_prefix;
    bool is_extended = kbd_in_e0_prefix;

    u8 set1_scancode = kbd_set2_to_set1(raw, is_extended);

    // Reset prefix state
    kbd_in_f0_prefix = false;
    kbd_in_e0_prefix = false;

    if (set1_scancode == 0) {
        // Unmapped scancode (e.g. PAUSE, PRINT SCREEN) — skip
        return;
    }

    // Convert to Set 1 format: make = scancode, break = scancode | 0x80
    // For extended keys, add 0xE0 prefix first
    if (is_extended) {
        keyboard_buffer_push(0xE0);
    }

    /* Modifier state must track EVENT ORDER at IRQ time: the consumer
     * side (keyboard_getchar) drains this ring lazily and the ring is
     * tiny (32). During an output flood (^Z test: `yes` spamming) old
     * entries are overwritten before any read() runs, so a lazily
     * tracked "Ctrl is down" could vanish or arrive late -> plain 'z'
     * leaked into the shell instead of SIGTSTP. Producer-side updates
     * keep the flag authoritative even when queued bytes drop. */
    {
        u8 code = set1_scancode;
        bool brk = is_break;
        u8* m = &kbd_state.modifiers;
        switch (code) {
        case 0x1D:                       /* Ctrl */
            if (!brk) *m |= KBD_CTRL_PRESSED;
            else      *m &= ~KBD_CTRL_PRESSED;
            break;
        case 0x2A: case 0x36:            /* L/R Shift */
            if (!brk) *m |= KBD_SHIFT_PRESSED;
            else      *m &= ~KBD_SHIFT_PRESSED;
            break;
        case 0x38:                       /* Alt */
            if (!brk) *m |= KBD_ALT_PRESSED;
            else      *m &= ~KBD_ALT_PRESSED;
            break;
        case 0x3A:                       /* Caps Lock (toggle on make) */
            if (!brk) kbd_state.caps_lock = !kbd_state.caps_lock;
            break;
        }
    }

    /* POSIX terminal ISIG at PRODUCER time (fork-v2): while a fg job
     * runs, NOTHING may read the console (ash blocks in wait4), so a
     * lazily-consumed VSUSP would sit in the ring forever and ^Z would
     * never stop anything. The producer knows modifiers authoritatively
     * (updated above), so deliver SIGTSTP RIGHT HERE on key make and
     * swallow the byte — no reader required.
     * NOTE: scancode tables map plain letters ('z'), no Ctrl folding
     * existed anywhere before this block (the historical consumer-side
     * check compared against a value that could never occur!). We fold
     * Ctrl+letter -> control-code EXPLICITLY here, POSIX-style
     * (c & 0x1F), currently honoring VSUSP(^Z); Idempotent with any
     * future consumer hooks via the stop_sig latch.                  */
    if (!kbd_raw_mode && !is_break && !is_extended) {
        u8 m = kbd_state.modifiers;
        char c = keyboard_scancode_to_char(set1_scancode, m);
        {
            extern void serial_printf(const char*, ...);
            static unsigned int km_dbg = 0;
            if ((++km_dbg & 3) == 1)
                serial_printf("[KM] sc=%x m=%x c=%d\n", set1_scancode, m,
                              (int)c);
        }
        if ((m & KBD_CTRL_PRESSED) && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);         /* Ctrl-letter fold      */
        else if ((m & KBD_CTRL_PRESSED) && c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        if ((u8)c == 26) {                   /* VSUSP (^Z)            */
            extern void serial_printf(const char*, ...);
            extern void tty_on_ctrl_z(void);
            serial_printf("[ISIG] VSUSP detected in IRQ (m=%x)\n", m);
            tty_on_ctrl_z();
            return;
        }
    }

    keyboard_buffer_push(is_break ? (set1_scancode | 0x80) : set1_scancode);
}

// Modifier management for shell raw-input mode
void keyboard_set_modifier(u8 bit) {
    kbd_state.modifiers |= bit;
}

void keyboard_clear_modifier(u8 bit) {
    kbd_state.modifiers &= ~bit;
}

void keyboard_toggle_caps_lock(void) {
    kbd_state.caps_lock = !kbd_state.caps_lock;
}

void keyboard_set_raw_mode(bool enabled) {
    kbd_raw_mode = enabled;
    // Reset translation state when switching modes
    kbd_in_f0_prefix = false;
    kbd_in_e0_prefix = false;
}

bool keyboard_get_raw_mode(void) {
    return kbd_raw_mode;
}
