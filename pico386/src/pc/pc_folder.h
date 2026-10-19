// pc_folder.h -- a folder on the SD card as a guest hard disk (see pc_folder.cpp). Plain C, so
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
int64_t pfolder_sectors(PicoFolder *f);
// 0, or -1 past the end of the disk.
int pfolder_read(PicoFolder *f, uint64_t sector, uint8_t *buf, int n);
int pfolder_write(PicoFolder *f, uint64_t sector, const uint8_t *buf, int n);
// Whether the guest has written to any open folder disk since it opened.
int pfolder_dirty(void);
// Applies what the guest changed to the folders, then closes and frees every folder disk.
void pfolder_close_all(void);

#ifdef __cplusplus
}
#endif
