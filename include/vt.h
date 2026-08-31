#ifndef VT_H
#define VT_H

#include "types.h"
#include "vga.h"
#include "shell.h"

#define VT_MAX_CONSOLES 4

#ifdef __cplusplus
extern "C" {
#endif

// Initialize virtual terminal system (call after heap_init)
void vt_init(void);

// Switch to virtual console (0 = Alt+F1, 1 = Alt+F2, ...)
// No-op if target is invalid or already active.
void vt_switch(int target);

// Get active console number (0-based)
int vt_get_active(void);

#ifdef __cplusplus
}
#endif

#endif // VT_H
