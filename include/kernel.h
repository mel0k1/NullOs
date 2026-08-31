#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Kernel version
#define NULLOS_VERSION_MAJOR 0
#define NULLOS_VERSION_MINOR 1
#define NULLOS_VERSION_PATCH 0
#define NULLOS_NAME       "NullOs"
#define NULLOS_VERSION "0.1.0"

// Initialize kernel subsystems
void kernel_init(void* mbi_ptr);

// Main kernel loop
void kernel_mainloop();

// Shutdown the system
void kernel_shutdown();

// Reboot the system
void kernel_reboot();

// Panic - critical error handler
void kernel_panic(const char* message);

// Print kernel info
void kernel_print_info();

// Kernel entry point (called from assembly bootloader)
void kernel_main(void* mbi_ptr);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_H
