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
#if defined(PICO386_LOWMEM)
#include "pc_pagemem.h"
#endif

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

// Guest RAM and VRAM (bigmalloc) come from one PSRAM block that is kept for the whole session and
// reused by every machine, never freed with the rest. Freed with each machine, the room for them
// was split by whatever the menus and the folder disks allocated in between: on the P4 the first
// PC got 24 MB of RAM and every restart after it 20 MB, so a snapshot taken in one could not be
// loaded into the other (its RAM size differed).
static uint8_t *s_pool = nullptr;
static size_t s_poolSize = 0, s_poolUsed = 0;

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
// The images opened for writing, for the snapshots: the first write to one after a start or a
// snapshot bumps its generation (pcsnap_touch), so a snapshot knows when its disks changed later.
static char *s_writePath[kMaxFiles];
static bool s_wrote[kMaxFiles];

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

// The guest's debug port (0x402, SeaBIOS's log) on the P4: whole lines through hal_log, which
// holds the log lock, so they no longer cut into the serial remote's screenshot rows.
int t386_putchar(int c) {
  static char line[128];
  static size_t len = 0;
  if (c != '\n' && len < sizeof(line) - 1) { line[len++] = (char)c; return c; }
  if (c != '\n') line[len++] = (char)c;
  line[len] = 0;
  len = 0;
  hal_log("%s", line);
  return c;
}

FILE *t386_fopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  if (!f) {
    hal_log("tiny386: cannot open %s (%s)", path, mode);
    return nullptr;
  }
  for (int i = 0; i < kMaxFiles; i++)
    if (!s_files[i]) {
      s_files[i] = f;
      s_wrote[i] = false;
      if (strchr(mode, '+') || strchr(mode, 'w')) {
        const size_t n = strlen(path) + 1;
        if ((s_writePath[i] = (char *)malloc(n))) memcpy(s_writePath[i], path, n);
      }
      return f;
    }
  hal_log("tiny386: more than %d open files", kMaxFiles);
  return f;                   // still works, just isn't closed by t386_release_all()
}

static void forgetFile(int i) {
  s_files[i] = nullptr;
  free(s_writePath[i]);
  s_writePath[i] = nullptr;
}

int t386_fclose(FILE *f) {
  for (int i = 0; i < kMaxFiles; i++)
    if (s_files[i] == f) forgetFile(i);
  return fclose(f);
}

size_t t386_fwrite(const void *p, size_t size, size_t n, FILE *f) {
  for (int i = 0; i < kMaxFiles; i++)
    if (s_files[i] == f && s_writePath[i] && !s_wrote[i]) {
      s_wrote[i] = true;
      pcsnap_touch(s_writePath[i]);
    }
  return fwrite(p, size, n, f);
}

const char *t386_write_file(int i) { return i >= 0 && i < kMaxFiles && s_files[i] ? s_writePath[i] : nullptr; }

void t386_files_snapped() {
  for (int i = 0; i < kMaxFiles; i++) s_wrote[i] = false;
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
#if defined(PICO386_LOWMEM)
  pm_set_ifslot(NULL);                  // the CPU's fetch pointer goes with its block
#endif
  for (int i = 0; i < kMaxFiles; i++)
    if (s_files[i]) { fclose(s_files[i]); forgetFile(i); }
  if (s_count) hal_log("tiny386: releasing %u blocks, %u KB", (unsigned)s_count, (unsigned)(s_bytes >> 10));
  while (s_blocks.next != &s_blocks) untrack(s_blocks.next + 1);
  s_poolUsed = 0;                       // the block itself stays for the next machine
  // Folder disks are written back last: their tables are pc_folder's own, and on the RP2350 the
  // write-back needs the heap the machine's blocks held (about 10 KB is free while it runs).
  pfolder_close_all();
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
void t386_clock_set(uint32_t now) { s_clockSkip = hal_micros() - now; }

void *pcmalloc(long size) { return track((size_t)size, false); }

int t386_reserve_big(size_t bytes, size_t margin) {
  s_poolUsed = 0;
  if (s_pool && s_poolSize >= bytes) return 1;
  // A bigger machine than the block holds: let it go and look for room, with margin bytes to
  // spare beside it (the probe is freed, so the block lands where it was).
  if (s_pool) hal_free(s_pool);
  s_pool = nullptr;
  s_poolSize = 0;
  void *probe = hal_alloc_big(bytes + margin);
  if (!probe) return 0;
  hal_free(probe);
  if (!(s_pool = (uint8_t *)hal_alloc_big(bytes))) return 0;
  s_poolSize = bytes;
  return 1;
}

// pc_new zeroes guest RAM and VRAM itself, so a block reused from the last machine is fine.
void *bigmalloc(size_t size) {
  const size_t n = (size + 15) & ~(size_t)15;
  if (s_pool && s_poolSize - s_poolUsed >= n) {
    void *p = s_pool + s_poolUsed;
    s_poolUsed += n;
    return p;
  }
  return track(size, true);
}

int load_rom(void *phys_mem, const char *file, uint32_t addr, int backward) {
  FILE *fp = fopen(file, "rb");
  if (!fp) {
    hal_log("load_rom: cannot open %s", file);
    return 0;
  }
  fseek(fp, 0, SEEK_END);
  const long len = ftell(fp);
  rewind(fp);
#if defined(PICO386_LOWMEM)
  // Guest RAM is paged: read in chunks and write them through the pager.
  (void)phys_mem;
  uint32_t at = backward ? addr - (uint32_t)len : addr;
  size_t got = 0;
  uint8_t buf[512];
  for (size_t k; (k = fread(buf, 1, sizeof(buf), fp)) > 0; at += (uint32_t)k, got += k)
    pm_write(at, buf, k);
#else
  uint8_t *dst = (uint8_t *)phys_mem + (backward ? addr - len : addr);
  const size_t got = fread(dst, 1, (size_t)len, fp);
#endif
  fclose(fp);
  hal_log("load_rom: %s, %ld bytes at 0x%05lx", file, len, (unsigned long)(backward ? addr - len : addr));
  return (int)got;
}

} // extern "C"
