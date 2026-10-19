// pc_platform.cpp -- the platform hooks tiny386 expects (pc.h "Platform HAL"), on top of our HAL,
// plus the tracked heap/stdio that tiny386_port.h routes the core through.
//
// The core allocates with malloc and never frees, and it keeps its disk images open in FILEs for
// as long as the machine exists. Every block and FILE it takes is kept on a list here, so
// t386_release_all() can end a session: it closes the FILEs (flushing guest disk writes to the
// card) and frees every block, after which a new machine can be built from scratch.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hal/hal.h"
#include "pc_glue.h"
#include "pc_folder.h"

// Blocks up to this size go to internal RAM when it has room (CPU state, TLB, device structs);
// anything larger (guest RAM, VRAM, the framebuffer) to PSRAM.
static constexpr size_t kFastMax = 64 * 1024;

struct Block {
  Block *prev, *next;
  size_t size;
  size_t pad;                 // keeps the payload 16-byte aligned on ILP32
};
static_assert(sizeof(Block) == 16, "header must keep 16-byte alignment");

static Block s_blocks = { &s_blocks, &s_blocks, 0, 0 };
static size_t s_bytes = 0, s_count = 0;

static void *track(size_t n, bool big) {
  const size_t total = sizeof(Block) + (n ? n : 1);
  Block *b = (Block *)(big || n > kFastMax ? hal_alloc_big(total) : hal_alloc_fast(total));
  if (!b) {
    hal_log("tiny386: out of memory (%u bytes, %u in use)", (unsigned)n, (unsigned)s_bytes);
    return nullptr;
  }
  b->size = n;
  b->next = s_blocks.next;
  b->prev = &s_blocks;
  s_blocks.next->prev = b;
  s_blocks.next = b;
  s_bytes += n;
  s_count++;
  return b + 1;               // hal_alloc_* zero the memory
}

static void untrack(void *p) {
  if (!p) return;
  Block *b = (Block *)p - 1;
  b->prev->next = b->next;
  b->next->prev = b->prev;
  s_bytes -= b->size;
  s_count--;
  hal_free(b);
}

// ---- open files ------------------------------------------------------------------------------
static constexpr int kMaxFiles = 16;
static FILE *s_files[kMaxFiles];

extern "C" {

void *t386_malloc(size_t n) { return track(n, false); }

void *t386_calloc(size_t n, size_t m) { return track(n * m, false); }

void *t386_realloc(void *p, size_t n) {
  if (!p) return track(n, false);
  Block *b = (Block *)p - 1;
  if (n <= b->size) return p;
  void *q = track(n, false);
  if (!q) return nullptr;
  memcpy(q, p, b->size);
  untrack(p);
  return q;
}

void t386_free(void *p) { untrack(p); }

char *t386_strdup(const char *s) {
  const size_t n = strlen(s) + 1;
  char *d = (char *)track(n, false);
  if (d) memcpy(d, s, n);
  return d;
}

FILE *t386_fopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  if (!f) {
    hal_log("tiny386: cannot open %s (%s)", path, mode);
    return nullptr;
  }
  for (int i = 0; i < kMaxFiles; i++)
    if (!s_files[i]) { s_files[i] = f; return f; }
  hal_log("tiny386: more than %d open files", kMaxFiles);
  return f;                   // still works, just isn't closed by t386_release_all()
}

int t386_fclose(FILE *f) {
  for (int i = 0; i < kMaxFiles; i++)
    if (s_files[i] == f) s_files[i] = nullptr;
  return fclose(f);
}

void t386_log(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  hal_log("%s", buf);
}

void t386_release_all() {
  pfolder_close_all();          // writes folder disks back while their tree is still readable
  for (int i = 0; i < kMaxFiles; i++)
    if (s_files[i]) { fclose(s_files[i]); s_files[i] = nullptr; }
  if (s_count) hal_log("tiny386: releasing %u blocks, %u KB", (unsigned)s_count, (unsigned)(s_bytes >> 10));
  while (s_blocks.next != &s_blocks) untrack(s_blocks.next + 1);
}

void t386_flush_files() {
  for (int i = 0; i < kMaxFiles; i++)
    if (s_files[i]) fflush(s_files[i]);
}

size_t t386_heap_bytes() { return s_bytes; }

// ---- pc.h platform HAL -----------------------------------------------------------------------
// The guest's clock: wall time minus the time the machine spent stopped (see t386_clock_skip).
static uint32_t s_clockSkip = 0;
uint32_t get_uticks() { return hal_micros() - s_clockSkip; }

void t386_clock_skip(uint32_t us) { s_clockSkip += us; }

void *pcmalloc(long size) { return track((size_t)size, false); }

void *bigmalloc(size_t size) { return track(size, true); }

int load_rom(void *phys_mem, const char *file, uint32_t addr, int backward) {
  FILE *fp = fopen(file, "rb");
  if (!fp) {
    hal_log("load_rom: cannot open %s", file);
    return 0;
  }
  fseek(fp, 0, SEEK_END);
  const long len = ftell(fp);
  rewind(fp);
  uint8_t *dst = (uint8_t *)phys_mem + (backward ? addr - len : addr);
  const size_t got = fread(dst, 1, (size_t)len, fp);
  fclose(fp);
  hal_log("load_rom: %s, %ld bytes at 0x%05lx", file, len, (unsigned long)(backward ? addr - len : addr));
  return (int)got;
}

} // extern "C"
