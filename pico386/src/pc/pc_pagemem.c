// pc_pagemem.c -- guest RAM paged through SRAM frames, backed by the PSRAM (see pc_pagemem.h).
#include "../../board.h"

#if defined(PICO386_LOWMEM)

#include "pc_pagemem.h"

#define NONE 0xffffu
#define LENT 0xfffeu                         // s_pageOf of a frame lent out (pm_lend)
#define F_REF   1
#define F_DIRTY 2

uint16_t pm_map[PM_MAX_PAGES];
uint8_t *pm_frame0;
uint8_t *pm_vram;

static uint8_t  *s_frames;
static int       s_nframes;
static uint32_t  s_pages;                    // guest pages
// Pages that were ever written back. One never written is all zeros: it is cleared in SRAM
// instead of read, so the PSRAM needs no clearing at boot.
static uint8_t   s_valid[PM_MAX_PAGES / 8];
static int       s_hand;
static uint32_t  s_faults, s_writebacks;
// Per frame: its page (NONE when free) and F_* flags. There is no page -> frame table: a
// soft-unmapped page is found again by searching these, which costs about a microsecond against
// the few hundred of a real fault, and saves 2 bytes per guest page.
#define PM_MAX_FRAMES 128
static uint16_t  s_pageOf[PM_MAX_FRAMES];
static uint8_t   s_flags[PM_MAX_FRAMES];
// Runs of frames lent out (pm_lend). The arena's bounds outlive pm_shutdown, so a buffer freed
// after it is still known for an arena one.
#define PM_MAX_LENDS 8
static struct { uint8_t *p; int first, n; } s_lent[PM_MAX_LENDS];
static uint8_t  *s_lendLo, *s_lendHi;

void pm_init(uint8_t *frames, int nframes, uint32_t mem_bytes) {
  if (nframes > PM_MAX_FRAMES) nframes = PM_MAX_FRAMES;
  s_frames = frames;
  pm_frame0 = frames - PM_PAGE_SIZE;
  s_nframes = nframes;
  s_pages = mem_bytes >> PM_PAGE_SHIFT;
  if (s_pages > PM_MAX_PAGES) s_pages = PM_MAX_PAGES;
  memset(pm_map, 0, sizeof(pm_map));
  memset(s_valid, 0, sizeof(s_valid));
  for (int f = 0; f < nframes; f++) { s_pageOf[f] = NONE; s_flags[f] = 0; }
  s_hand = 0;
  s_faults = s_writebacks = 0;
  memset(s_lent, 0, sizeof(s_lent));
  s_lendLo = frames;
  s_lendHi = frames + ((size_t)nframes << PM_PAGE_SHIFT);
}

void pm_shutdown(void) {
  memset(pm_map, 0, sizeof(pm_map));
  memset(s_lent, 0, sizeof(s_lent));
  s_frames = NULL;
  s_nframes = 0;
  s_pages = 0;
}

uint32_t pm_faults(void)     { return s_faults; }
uint32_t pm_writebacks(void) { return s_writebacks; }

// The clock: a frame used since the hand last passed is unmapped (so its next use faults and
// marks it again) and skipped; the first one that was not is the victim.
static int pickVictim(void) {
  for (;;) {
    const int f = s_hand;
    if (++s_hand == s_nframes) s_hand = 0;
    const uint16_t pg = s_pageOf[f];
    if (pg == LENT) continue;
    if (pg == NONE) return f;
    if (s_flags[f] & F_REF) {
      s_flags[f] &= ~F_REF;
      pm_map[pg] = 0;
      continue;
    }
    return f;
  }
}

static int frameOf(uint32_t page) {
  for (int f = 0; f < s_nframes; f++)
    if (s_pageOf[f] == page) return f;
  return -1;
}

uint8_t *pm_fault(uint32_t page, int write) {
  if (page >= s_pages) return NULL;
  int f = frameOf(page);
  uint8_t *fr;
  if (f < 0) {
    s_faults++;
    f = pickVictim();
    fr = s_frames + ((size_t)f << PM_PAGE_SHIFT);
    const uint16_t old = s_pageOf[f];
    if (old != NONE) {
      pm_map[old] = 0;
      if (s_flags[f] & F_DIRTY) {
        hal_psram_write((uint32_t)old << PM_PAGE_SHIFT, fr, PM_PAGE_SIZE);
        s_valid[old >> 3] |= (uint8_t)(1u << (old & 7));
        s_writebacks++;
      }
    }
    if (s_valid[page >> 3] & (1u << (page & 7)))
      hal_psram_read(page << PM_PAGE_SHIFT, fr, PM_PAGE_SIZE);
    else
      memset(fr, 0, PM_PAGE_SIZE);
    s_pageOf[f] = (uint16_t)page;
    s_flags[f] = 0;
  } else {
    fr = s_frames + ((size_t)f << PM_PAGE_SHIFT);
  }
  s_flags[f] |= F_REF;
  if (write) s_flags[f] |= F_DIRTY;
  pm_map[page] = (uint16_t)(((f + 1) << 1) | (s_flags[f] & F_DIRTY ? PM_WRITABLE : 0));
  return fr;
}

// Empties frame f, writing its page back if it was written.
static void evict(int f) {
  const uint16_t old = s_pageOf[f];
  if (old == NONE || old == LENT) return;
  pm_map[old] = 0;
  if (s_flags[f] & F_DIRTY) {
    hal_psram_write((uint32_t)old << PM_PAGE_SHIFT, s_frames + ((size_t)f << PM_PAGE_SHIFT), PM_PAGE_SIZE);
    s_valid[old >> 3] |= (uint8_t)(1u << (old & 7));
    s_writebacks++;
  }
  s_pageOf[f] = NONE;
  s_flags[f] = 0;
}

void *pm_lend(size_t bytes, const uint8_t *above) {
  const int n = (int)((bytes + PM_PAGE_SIZE - 1) >> PM_PAGE_SHIFT);
  if (!s_frames || n <= 0) return NULL;
  int slot = 0;
  while (slot < PM_MAX_LENDS && s_lent[slot].p) slot++;
  if (slot == PM_MAX_LENDS) return NULL;
  int paging = 0;
  for (int f = 0; f < s_nframes; f++) paging += s_pageOf[f] != LENT;
  if (paging - n < PM_MIN_FRAMES) return NULL;
  const int lo = above > s_frames ? (int)((above - s_frames + PM_PAGE_SIZE - 1) >> PM_PAGE_SHIFT) : 0;
  for (int f0 = s_nframes - n; f0 >= lo; f0--) {       // the highest free run
    int k = 0;
    while (k < n && s_pageOf[f0 + k] != LENT) k++;
    if (k < n) continue;
    for (k = 0; k < n; k++) {
      evict(f0 + k);
      s_pageOf[f0 + k] = LENT;
    }
    s_lent[slot].p = s_frames + ((size_t)f0 << PM_PAGE_SHIFT);
    s_lent[slot].first = f0;
    s_lent[slot].n = n;
    return s_lent[slot].p;
  }
  return NULL;
}

int pm_unlend(void *p) {
  const uint8_t *b = (const uint8_t *)p;
  if (!b || b < s_lendLo || b >= s_lendHi) return 0;
  for (int i = 0; i < PM_MAX_LENDS; i++) {
    if (s_lent[i].p != b) continue;
    for (int k = 0; k < s_lent[i].n; k++) s_pageOf[s_lent[i].first + k] = NONE;
    s_lent[i].p = NULL;
    break;
  }
  return 1;                                  // an arena buffer either way, never the heap's
}

uint8_t pm_slow_read8(uint32_t a) {
  uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 0);
  return p ? p[a & PM_PAGE_MASK] : 0;
}
uint16_t pm_slow_read16(uint32_t a) {
  if ((a & PM_PAGE_MASK) != PM_PAGE_MASK) {
    uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 0);
    return p ? ((const pm_u16_t *)(p + (a & PM_PAGE_MASK)))->v : 0;
  }
  return pm_read8(a) | (pm_read8(a + 1) << 8);
}
uint32_t pm_slow_read32(uint32_t a) {
  if ((a & PM_PAGE_MASK) <= (PM_PAGE_MASK - 3)) {
    uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 0);
    return p ? ((const pm_u32_t *)(p + (a & PM_PAGE_MASK)))->v : 0;
  }
  return pm_read8(a) | (pm_read8(a + 1) << 8) | (pm_read8(a + 2) << 16) |
         ((uint32_t)pm_read8(a + 3) << 24);
}
void pm_slow_write8(uint32_t a, uint8_t v) {
  uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 1);
  if (p) p[a & PM_PAGE_MASK] = v;
}
void pm_slow_write16(uint32_t a, uint16_t v) {
  if ((a & PM_PAGE_MASK) != PM_PAGE_MASK) {
    uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 1);
    if (p) ((pm_u16_t *)(p + (a & PM_PAGE_MASK)))->v = v;
    return;
  }
  pm_write8(a, (uint8_t)v);
  pm_write8(a + 1, (uint8_t)(v >> 8));
}
void pm_slow_write32(uint32_t a, uint32_t v) {
  if ((a & PM_PAGE_MASK) <= (PM_PAGE_MASK - 3)) {
    uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 1);
    if (p) ((pm_u32_t *)(p + (a & PM_PAGE_MASK)))->v = v;
    return;
  }
  for (int i = 0; i < 4; i++) pm_write8(a + i, (uint8_t)(v >> (i * 8)));
}

void pm_read(uint32_t a, void *dst, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  while (n) {
    size_t k = PM_PAGE_SIZE - (a & PM_PAGE_MASK);
    if (k > n) k = n;
    uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 0);
    if (p) memcpy(d, p + (a & PM_PAGE_MASK), k);
    else memset(d, 0, k);
    a += (uint32_t)k; d += k; n -= k;
  }
}

void pm_write(uint32_t a, const void *src, size_t n) {
  const uint8_t *s = (const uint8_t *)src;
  while (n) {
    size_t k = PM_PAGE_SIZE - (a & PM_PAGE_MASK);
    if (k > n) k = n;
    uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, 1);
    if (p) memcpy(p + (a & PM_PAGE_MASK), s, k);
    a += (uint32_t)k; s += k; n -= k;
  }
}

uint8_t *pm_ptr(uint32_t a, int write) {
  uint8_t *p = pm_fault(a >> PM_PAGE_SHIFT, write);
  return p ? p + (a & PM_PAGE_MASK) : NULL;
}

#endif // PICO386_LOWMEM
