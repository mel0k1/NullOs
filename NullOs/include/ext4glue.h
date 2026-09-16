#ifndef EXT4GLUE_H
#define EXT4GLUE_H

/* lwext4 persistence glue (kernel/ext4glue.c).
 * See that file for the full design commentary. */

// Attempt to mount the primary ATA disk as ext4 and attach every
// volume file into the ramfs as a live backend node. Returns EOK (0)
// on success; a negative kernel value or lwext4 errno otherwise.
int  ext4_storage_init(void);

// Copy ramfs-native files (backend==NULL) into the ext4 volume and
// flush the block cache. -1 when ext4 is not mounted.
int  ext4_storage_sync(void);

bool ext4_is_mounted(void);

// Shell commands
void cmd_ext4mount(int argc, char** argv);
void cmd_ext4sync(int argc, char** argv);

#endif /* EXT4GLUE_H */
