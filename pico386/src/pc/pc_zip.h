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
// A zip of floppy images (a game's disks, as .ima/.img) as a floppy: the first of its images
// ("Disk 1") written to out, unpacking it first. 1 = an image, 0 = no images (out is the folder,
// a folder floppy), -1 as pczip_folder.
int pczip_floppy(const char *zip, char *out, size_t outSize);

#ifdef __cplusplus
}

#include <string>
#include <vector>

// The floppy images (.ima/.img/.vfd up to 2.88 MB) under a folder a zip unpacked to, as full paths
// sorted with the numbers counted ("Disk 2" before "Disk 10").
std::vector<std::string> pczip_images(const std::string &dir);
// The folder under /pico-386/.unzipped that path is in, "" when it is not in one.
std::string pczip_set_dir(const std::string &path);
// The zip that folder was unpacked from (its marker), "" when unknown.
std::string pczip_source(const std::string &dir);
// "Disk 2" from a file name holding "(Disk 2)", else the name without its extension.
std::string pczip_disk_label(const std::string &path);

extern "C" {
#endif

#ifdef __cplusplus
}
#endif
