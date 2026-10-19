// pc_zip.h -- .zip files as disks (see pc_zip.cpp). Plain C, so pc_glue.c can map a zip in the
// .ini to the folder it unpacks to.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int pczip_is_zip(const char *path);
// The folder the zip is unpacked to (written to out), unpacking it first when it is not there yet
// or the zip changed since. 0, or -1 when the zip cannot be read or the card is full.
int pczip_folder(const char *zip, char *out, size_t outSize);

#ifdef __cplusplus
}
#endif
