// pc_pagemem.c -- guest RAM paged through SRAM frames, backed by the PSRAM (see pc_pagemem.h).
#include "../../board.h"

#if defined(PICO386_LOWMEM)

#include "pc_pagemem.h"

#define NONE 0xffffu
#define LENT 0xfffeu                         // s_pageOf of a frame lent out (pm_lend)
#define VRES 0xfffdu                         // s_pageOf of a VRAM frame the VGA has back (pm_vram_*)
#define F_REF   1
#define F_DIRTY 2

uint16_t pm_map[PM_MAX_PAGES];
uint8_t *pm_frame0;
uint8_t *pm_vram;
static uint8_t *s_ifdummy[2];
uint8_t **pm_ifslot = s_ifdummy;

void pm_set_ifslot(uint8_t **slot) {
  pm_ifslot = slot ? slot : s_ifdummy;
}

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
#define PM_MAX_FRAMES 192
static uint16_t  s_pageOf[PM_MAX_FRAMES];
static uint8_t   s_flags[PM_MAX_FRAMES];
// Which pages are in a frame (a bit each), and where each page was last put, by its low bits: a
// fault on a page that is not resident skips the search, and one that is checks the hint first.
// With 192 frames the search was 2% of FastDoom's time on the device (2026-11-14).
#define PM_HINTS 512
static uint8_t   s_res[PM_MAX_PAGES / 8];
static uint8_t   s_hint[PM_HINTS];
#define RES_SET(pg) (s_res[(pg) >> 3] |= (uint8_t)(1u << ((pg) & 7)))
#define RES_CLR(pg) (s_res[(pg) >> 3] &= (uint8_t)~(1u << ((pg) & 7)))
// Runs of frames lent out (pm_lend). The arena's bounds outlive pm_shutdown, so a buffer freed
// after it is still known for an arena one.
#define PM_MAX_LENDS 8
static struct { uint8_t *p; int first, n; } s_lent[PM_MAX_LENDS];
static uint8_t  *s_lendLo, *s_lendHi;
static uint32_t  s_vhiBase;                  // PSRAM address of VRAM byte PM_VRAM_BYTES; 0: none
// The first s_vres frames are VRAM PM_VRAM_LOW to PM_VRAM_BYTES (pm_vram_lend): VRES while the
// VGA has them, paged while lent. Their VRAM waits at s_vstash in the PSRAM meanwhile.
static int       s_vres;
static uint32_t  s_vstash;
static int       s_vhold;
int pm_vram_lent;

void pm_init(uint8_t *frames, int nframes, uint32_t mem_bytes, int nvram) {
  if (nframes > PM_MAX_FRAMES) nframes = PM_MAX_FRAMES;
  if (nvram > nframes - PM_MIN_FRAMES) nvram = 0;
  s_frames = frames;
  pm_frame0 = frames - PM_PAGE_SIZE;
  s_nframes = nframes;
  s_pages = mem_bytes >> PM_PAGE_SHIFT;
  if (s_pages > PM_MAX_PAGES) s_pages = PM_MAX_PAGES;
  memset(pm_map, 0, sizeof(pm_map));
  memset(s_valid, 0, sizeof(s_valid));
  memset(s_res, 0, sizeof(s_res));
  for (int f = 0; f < nframes; f++) { s_pageOf[f] = f < nvram ? VRES : NONE; s_flags[f] = 0; }
  s_vres = nvram;
  s_vstash = 0;
  s_vhold = 0;
  pm_vram_lent = 0;
  s_hand = 0;
  s_faults = s_writebacks = 0;
  pm_ifslot = s_ifdummy;
  memset(s_lent, 0, sizeof(s_lent));
  s_lendLo = frames;
  s_lendHi = frames + ((size_t)nframes << PM_PAGE_SHIFT);
}

void pm_shutdown(void) {
  pm_ifslot = s_ifdummy;
  memset(pm_map, 0, sizeof(pm_map));
  memset(s_lent, 0, sizeof(s_lent));
  s_frames = NULL;
  s_nframes = 0;
  s_pages = 0;
  s_vhiBase = 0;
  s_vres = 0;
  pm_vram_lent = 0;
}

// VRAM above PM_VRAM_BYTES (pc_pagemem.h): one 256-byte line in SRAM, written back when another
// is needed. Windows 3.0 blits from there a scan line at a time, so most accesses hit it.
#define VHI_LINE 256u
static uint32_t s_vhiTag = 0xffffffffu;      // the line's offset above PM_VRAM_BYTES
static int      s_vhiDirty;
static uint32_t s_vhiLine[VHI_LINE / 4];

void pm_vram_hi_init(uint32_t psram) {
  s_vhiBase = psram;
  s_vhiTag = 0xffffffffu;
  s_vhiDirty = 0;
  if (!psram) return;
  memset(s_vhiLine, 0, sizeof(s_vhiLine));
  for (uint32_t o = 0; o < PM_VRAM_FULL - PM_VRAM_BYTES; o += VHI_LINE)
    hal_psram_write(psram + o, s_vhiLine, VHI_LINE);
}

uint8_t *pm_vram_hi(uint32_t byte, int write) {
  if (!s_vhiBase || byte < PM_VRAM_BYTES || byte >= PM_VRAM_FULL) return NULL;
  const uint32_t off = byte - PM_VRAM_BYTES, tag = off & ~(VHI_LINE - 1);
  if (tag != s_vhiTag) {
    if (s_vhiDirty) hal_psram_write(s_vhiBase + s_vhiTag, s_vhiLine, VHI_LINE);
    hal_psram_read(s_vhiBase + tag, s_vhiLine, VHI_LINE);
    s_vhiTag = tag;
    s_vhiDirty = 0;
  }
  if (write) s_vhiDirty = 1;
  return (uint8_t *)s_vhiLine + (off & (VHI_LINE - 1));
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
    if (pg == NONE) return f;
    if (pg >= VRES) continue;                 // lent out, or VRAM
    if (s_flags[f] & F_REF) {
      s_flags[f] &= ~F_REF;
      pm_map[pg] = 0;
      continue;
    }
    return f;
  }
}

static int frameOf(uint32_t page) {
  if (!(s_res[page >> 3] & (1u << (page & 7)))) return -1;
  const int h = s_hint[page & (PM_HINTS - 1)];
  if (h < s_nframes && s_pageOf[h] == page) return h;
  for (int f = 0; f < s_nframes; f++)
    if (s_pageOf[f] == page) {
      s_hint[page & (PM_HINTS - 1)] = (uint8_t)f;
      return f;
    }
  return -1;
}

uint8_t *pm_fault(uint32_t page, int write) {
  if (page >= s_pages) return NULL;
  int f = frameOf(page);
  uint8_t *fr;
  if (f < 0) {
    s_faults++;
    PM_IF_DROP();                             // the victim may hold the running code
    f = pickVictim();
    fr = s_frames + ((size_t)f << PM_PAGE_SHIFT);
    const uint16_t old = s_pageOf[f];
    const int valid = (s_valid[page >> 3] >> (page & 7)) & 1;
    int loaded = 0;
    if (old != NONE) {
      pm_map[old] = 0;
      RES_CLR(old);
      if (s_flags[f] & F_DIRTY) {
        // The new page comes in first and the old one goes out behind it, while the guest runs
        // on (hal_psram_swap): a write-back no longer stalls the fault (2026-11-14).
        hal_psram_swap((uint32_t)old << PM_PAGE_SHIFT,
                       valid ? page << PM_PAGE_SHIFT : HAL_PSRAM_NONE, fr, PM_PAGE_SIZE);
        s_valid[old >> 3] |= (uint8_t)(1u << (old & 7));
        s_writebacks++;
        loaded = valid;
      }
    }
    if (!loaded) {
      if (valid) hal_psram_read(page << PM_PAGE_SHIFT, fr, PM_PAGE_SIZE);
      else memset(fr, 0, PM_PAGE_SIZE);
    }
    s_pageOf[f] = (uint16_t)page;
    s_flags[f] = 0;
    RES_SET(page);
    s_hint[page & (PM_HINTS - 1)] = (uint8_t)f;
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
  if (old >= VRES) return;
  PM_IF_DROP();
  pm_map[old] = 0;
  RES_CLR(old);
  if (s_flags[f] & F_DIRTY) {
    hal_psram_write((uint32_t)old << PM_PAGE_SHIFT, s_frames + ((size_t)f << PM_PAGE_SHIFT), PM_PAGE_SIZE);
    s_valid[old >> 3] |= (uint8_t)(1u << (old & 7));
    s_writebacks++;
  }
  s_pageOf[f] = NONE;
  s_flags[f] = 0;
}

int pm_get_page(uint32_t page, void *buf) {
  const int f = frameOf(page);
  if (f >= 0) {
    memcpy(buf, s_frames + ((size_t)f << PM_PAGE_SHIFT), PM_PAGE_SIZE);
    return 1;
  }
  if (page >= s_pages || !(s_valid[page >> 3] & (1u << (page & 7)))) return 0;
  hal_psram_read(page << PM_PAGE_SHIFT, buf, PM_PAGE_SIZE);
  return 1;
}

void pm_put_page(uint32_t page, const void *buf) {
  if (page >= s_pages) return;
  const int f = frameOf(page);
  if (f >= 0) {
    uint8_t *fr = s_frames + ((size_t)f << PM_PAGE_SHIFT);
    if (buf) memcpy(fr, buf, PM_PAGE_SIZE);
    else memset(fr, 0, PM_PAGE_SIZE);
    s_flags[f] |= F_DIRTY;
    pm_map[page] = 0;                         // the next access maps it again, writable
    PM_IF_DROP();
  } else if (buf) {
    hal_psram_write(page << PM_PAGE_SHIFT, buf, PM_PAGE_SIZE);
    s_valid[page >> 3] |= (uint8_t)(1u << (page & 7));
  } else {
    s_valid[page >> 3] &= (uint8_t)~(1u << (page & 7));
  }
}

void *pm_lend(size_t bytes, const uint8_t *above) {
  const int n = (int)((bytes + PM_PAGE_SIZE - 1) >> PM_PAGE_SHIFT);
  if (!s_frames || n <= 0) return NULL;
  int slot = 0;
  while (slot < PM_MAX_LENDS && s_lent[slot].p) slot++;
  if (slot == PM_MAX_LENDS) return NULL;
  int paging = 0;
  for (int f = 0; f < s_nframes; f++) paging += s_pageOf[f] < VRES || s_pageOf[f] == NONE;
  if (paging - n < PM_MIN_FRAMES) return NULL;
  const int lo = above > s_frames ? (int)((above - s_frames + PM_PAGE_SIZE - 1) >> PM_PAGE_SHIFT) : 0;
  for (int f0 = s_nframes - n; f0 >= lo; f0--) {       // the highest free run
    int k = 0;
    while (k < n && (s_pageOf[f0 + k] < VRES || s_pageOf[f0 + k] == NONE)) k++;
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

void pm_vram_stash(uint32_t psram) { s_vstash = psram; }

void pm_vram_lend(void) {
  if (pm_vram_lent || !s_vres || !s_vstash || s_vhold) return;
  hal_psram_write(s_vstash, s_frames, (size_t)s_vres << PM_PAGE_SHIFT);
  for (int f = 0; f < s_vres; f++) { s_pageOf[f] = NONE; s_flags[f] = 0; }
  pm_vram_lent = 1;
}

void pm_vram_reclaim(int hold) {
  if (hold) s_vhold = 1;
  if (!pm_vram_lent) return;
  for (int f = 0; f < s_vres; f++) {
    evict(f);
    s_pageOf[f] = VRES;
  }
  hal_psram_read(s_vstash, s_frames, (size_t)s_vres << PM_PAGE_SHIFT);
  pm_vram_lent = 0;
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
