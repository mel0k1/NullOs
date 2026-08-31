#ifndef FPU_H
#define FPU_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// FPU/SSE state save area (512 bytes for FXSAVE, 64-byte aligned)
#define FPU_STATE_SIZE 512
#define FPU_ALIGN      64

// Initialize FPU/SSE: set CR0/CR4 bits, execute FNINIT
void fpu_init(void);

// Save FPU state to a 512-byte aligned buffer
void fpu_save(void* state);

// Restore FPU state from a 512-byte aligned buffer
void fpu_restore(void* state);

#ifdef __cplusplus
}
#endif

#endif // FPU_H
