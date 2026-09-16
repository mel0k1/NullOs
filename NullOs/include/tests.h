#ifndef TESTS_H
#define TESTS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Run the kernel self-test suite (memory, buddy, pmm, fs, scheduler,
// slab, spinlocks). Prints PASS/FAIL per test and a summary.
// Registered as the `test` shell command.
void cmd_test(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // TESTS_H
