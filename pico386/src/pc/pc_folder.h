// pc_folder.h -- a folder on the SD card as a guest hard disk or floppy (see pc_folder.cpp). Plain C, so
// tiny386's ide.c can plug it in as a block device.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PicoFolder PicoFolder;

int pfolder_is_dir(const char *path);
// Scans the folder and builds the disk. NULL only when out of memory.
PicoFolder *pfolder_open(const char *dir);
// The same as a 1.44 MB floppy (emulink, A: and B:). NULL also when the files do not fit on one.
PicoFolder *pfolder_open_floppy(const char *dir);
// Whether a folder disk or floppy has this folder open (two views of one folder would clash).
int pfolder_in_use(const char *dir);
int64_t pfolder_sectors(PicoFolder *f);
// 0, or -1 past the end of the disk.
int pfolder_read(PicoFolder *f, uint64_t sector, uint8_t *buf, int n);
int pfolder_write(PicoFolder *f, uint64_t sector, const uint8_t *buf, int n);
// Whether the guest has written to any open folder disk since it opened.
int pfolder_dirty(void);
int pfolder_is_dirty(PicoFolder *f);
// Applies what the guest changed to this folder, then closes and frees it (a floppy taken out).
void pfolder_close(PicoFolder *f);
// Applies what the guest changed to the folders, then closes and frees every folder disk.
void pfolder_close_all(void);

#ifdef __cplusplus
}
#endif
