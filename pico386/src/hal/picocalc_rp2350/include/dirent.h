// dirent.h -- POSIX directory reading for the RP2350 build, over arduino-pico's SDFS
// (posix_rp2350.cpp). newlib's own <dirent.h> is a "not supported" stub on arm-none-eabi; the
// build puts this folder first on the include path. Same shape as emu8's pico_shim/dirent.h, but
// the DIR is opaque so C files could include it too, and paths carry the "/sd" mount prefix.
#pragma once

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
  unsigned char d_type;
  char          d_name[256];   // SDFS hands back long names: 255 + NUL
};

typedef struct pico386_dir DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);

#ifdef __cplusplus
}
#endif
