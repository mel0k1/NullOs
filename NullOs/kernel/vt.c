#include "../include/vt.h"
#include "../include/vga.h"
#include "../include/mm.h"
#include "../include/string.h"
#include "../include/serial.h"

// ============================================================
// Per-console state: VGA snapshot + shell state
// ============================================================

typedef struct {
    vga_snapshot_t vga;
    shell_t        sh;
    bool           initialized;
} vt_console_t;

static vt_console_t* consoles[VT_MAX_CONSOLES]
    __attribute__((section(".vtlow")));
static int active_console __attribute__((section(".vtlow"))) = 0;

// ============================================================
// Init — allocate all consoles, mark console 0 as active/ready
// ============================================================

void vt_init(void) {
    for (int i = 0; i < VT_MAX_CONSOLES; i++) {
        consoles[i] = (vt_console_t*)kzalloc(sizeof(vt_console_t));
        {
            extern u64 pmm_get_available_memory(void);
            serial_printf("[VT] console %d at %lx, avail=%lu KB\n",
                          i, (u64)consoles[i],
                          (unsigned long)(pmm_get_available_memory() >> 10));
        }
        if (!consoles[i]) {
            // Out of memory — leave this console as NULL
            continue;
        }
        consoles[i]->vga.cursor_x = 0;
        consoles[i]->vga.cursor_y = 0;
        consoles[i]->vga.fg = VGA_LIGHT_GREY;
        consoles[i]->vga.bg = VGA_BLACK;
        consoles[i]->initialized = (i == 0);
    }
    active_console = 0;
}

// ============================================================
// Switch — save current console, restore target console
// ============================================================

void vt_switch(int target) {
    if (target < 0 || target >= VT_MAX_CONSOLES) return;
    if (target == active_console) return;
    if (!consoles[active_console]) return;
    if (!consoles[target]) return;

    // --- Save current console ---
    vga_save_snapshot(&consoles[active_console]->vga);
    kmemcpy(&consoles[active_console]->sh, &shell, sizeof(shell_t));

    // --- Switch ---
    active_console = target;

    // --- Restore new console ---
    vga_restore_snapshot(&consoles[active_console]->vga);
    kmemcpy(&shell, &consoles[active_console]->sh, sizeof(shell_t));

    // Reset scrollback — it is global and belongs to the previous console
    vga_scrollback_init();
}

int vt_get_active(void) {
    return active_console;
}
