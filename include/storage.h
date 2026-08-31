#ifndef STORAGE_H
#define STORAGE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent ramfs backup on ATA drive.
 * Format:
 *   Sector 0: magic(8) | version(4) | node_count(4) | data_offset(4)
 *   Sectors 1..N: file metadata { path[256] size u32 type u32 }
 *   Remaining: file data sequentially
 */

#define STORAGE_MAGIC     "NULFS01"
#define STORAGE_SECTOR    512
#define STORAGE_START_LBA 2048      /* Start at 1MB mark — safe */
#define STORAGE_MAX_NODES 64

int storage_sync(void);       /* Write all ramfs files to disk */
int storage_load(void);       /* Read disk → restore into ramfs */
bool storage_has_data(void);  /* Check if disk has a saved fs   */

void cmd_sync(int argc, char** argv);
void cmd_mount(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // STORAGE_H