// posix_rp2350.cpp -- the POSIX file calls arduino-pico's VFS leaves out. VFS.map("/sd", SDFS)
// (system_rp2350.cpp) gives open/read/write/lseek/close/unlink/stat to fopen() and friends, but
// newlib's mkdir, rmdir and rename end in nosys stubs, and there is no readdir at all. These
// strip the mount prefix and call SDFS. Linking our own definitions keeps newlib's out.
// tiny386 also wants clock_gettime (RDTSC, the RTC) and usleep, which newlib lacks here.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#include <SDFS.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <FreeRTOS.h>
#include <task.h>
#include "include/dirent.h"

// "/sd/pc-386" -> "/pc-386", "/sd" -> "/". Anything else is not on the card.
static const char *sdPath(const char *p) {
  static const size_t n = sizeof(SD_MOUNT) - 1;
  if (!p || strncmp(p, SD_MOUNT, n) != 0) return nullptr;
  if (p[n] == '\0') return "/";
  return p[n] == '/' ? p + n : nullptr;
}

extern "C" int mkdir(const char *path, mode_t) {
  const char *p = sdPath(path);
  if (!p) { errno = ENOENT; return -1; }
  if (SDFS.exists(p)) { errno = EEXIST; return -1; }
  if (SDFS.mkdir(p)) return 0;
  errno = EIO;
  return -1;
}

extern "C" int rmdir(const char *path) {
  const char *p = sdPath(path);
  if (p && SDFS.rmdir(p)) return 0;
  errno = p ? ENOTEMPTY : ENOENT;
  return -1;
}

extern "C" int rename(const char *from, const char *to) {
  const char *a = sdPath(from), *b = sdPath(to);
  if (a && b && SDFS.rename(a, b)) return 0;
  errno = (a && b) ? EIO : ENOENT;
  return -1;
}

// Every clock is the 64-bit microsecond timer from boot.
extern "C" int clock_gettime(clockid_t, struct timespec *ts) {
  const uint64_t us = time_us_64();
  ts->tv_sec = (time_t)(us / 1000000);
  ts->tv_nsec = (long)(us % 1000000) * 1000;
  return 0;
}

extern "C" int usleep(useconds_t us) {
  if (us >= 1000) vTaskDelay(pdMS_TO_TICKS(us / 1000));
  else busy_wait_us_32(us);
  return 0;
}

// ---- directories ------------------------------------------------------------------------------
// fs::Dir already has readdir's shape: next() steps, fileName()/isDirectory() answer without
// opening each entry, so a big folder lists without a stat per file.
struct pico386_dir {
  fs::Dir       dir;
  struct dirent ent;
};

extern "C" DIR *opendir(const char *path) {
  const char *p = sdPath(path);
  if (!p) { errno = ENOENT; return nullptr; }
  if (strcmp(p, "/") != 0) {
    File f = SDFS.open(p, "r");
    const bool dir = f && f.isDirectory();
    f.close();
    if (!dir) { errno = ENOENT; return nullptr; }
  }
  DIR *d = new DIR();
  d->dir = SDFS.openDir(p);
  return d;
}

extern "C" struct dirent *readdir(DIR *d) {
  if (!d || !d->dir.next()) return nullptr;
  const String name = d->dir.fileName();
  strncpy(d->ent.d_name, name.c_str(), sizeof(d->ent.d_name) - 1);
  d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
  d->ent.d_type = d->dir.isDirectory() ? DT_DIR : DT_REG;
  return &d->ent;
}

extern "C" int closedir(DIR *d) {
  if (!d) return -1;
  delete d;
  return 0;
}

#endif // BOARD_PICOCALC_RP2350
