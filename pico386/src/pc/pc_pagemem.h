// pc_pagemem.h -- guest RAM paged through SRAM (PICO386_LOWMEM, the RP2350 build).
//
// The RP2350 has 520 KB of SRAM and the PicoCalc's 8 MB PSRAM sits behind a PIO, not on the
// bus, so the guest's RAM lives in the PSRAM and only a few dozen 1 KB pages of it are in SRAM
// ("frames") at a time. tiny386 reaches RAM only through its pload/pstore helpers, the ISA DMA
// byte loops and three string fast paths; under T386_PAGED those call the accessors below.
//
// Pages are 1 KB, not the x86's 4 KB: DOS's hot data (vectors, BIOS data, stacks, bits of the
// kernel and BIOS) is spread thinly, and 64 KB of frames held far more of it. Booting FreeDOS on
// the desktop (2026-10-28), 64 KB of frames took 313k faults as 4 KB pages, 276k as 2 KB, 49k as
// 1 KB and 41k as 512 B, and a 1 KB fault moves a quarter of the data.
//
// pm_map[page] is (frame number + 1) << 1 of a resident page, or 0 to take the slow path; bit 0
// set means it may be written too. 2 bytes, not a pointer: the 8 KB saved keeps i386.c in SRAM. The slow path faults the page in (clock replacement, dirty frames written
// back) and maps it; a clean page is mapped read-only, so its first store is what marks it dirty.
// A page is soft-unmapped (0) when the clock hand passes it, which is how a page still in use
// gets its second chance.
#ifndef PC_PAGEMEM_H
#define PC_PAGEMEM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PM_PAGE_SHIFT 10
#define PM_PAGE_SIZE  (1u << PM_PAGE_SHIFT)
#define PM_PAGE_MASK  (PM_PAGE_SIZE - 1)
// 4 MB of guest RAM: the map costs 4 bytes a page in SRAM, and at this speed nothing that wants
// more would run anyway.
#define PM_MAX_RAM    (4u << 20)
#define PM_MAX_PAGES  (PM_MAX_RAM >> PM_PAGE_SHIFT)
// The VGA's memory, at the start of the arena. 160 KB, not a real VGA's 256: mode 12h needs
// 150 KB (4 planes of 0x9600), and the 96 KB saved holds the interpreter in SRAM. The rest, up to
// PM_VRAM_FULL, lives in PSRAM (pm_vram_hi): the guest can store and read it (Windows 3.0's VGA
// driver keeps bitmaps there), but the screen never shows it, so smooth scrolling and mode X
// page flips above 40 KB per plane still show the wrong page.
#define PM_VRAM_BYTES (160u * 1024)
#define PM_VRAM_FULL  (256u * 1024)
#define PM_WRITABLE   1u

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t pm_map[PM_MAX_PAGES];
// Frame n of the arena is at pm_frame0 + ((n + 1) << PM_PAGE_SHIFT), so a map entry finds it.
extern uint8_t *pm_frame0;
// The VGA's memory: the start of the SRAM arena, set by t386_start before pc_new takes it.
extern uint8_t *pm_vram;
// i386.c keeps two pointers into the frame holding the running code (cpu->ifetch.ptr, the next
// instruction byte, and .fbase, the frame itself), so fetching skips pm_entry and pm_read8 for
// every byte (40% of the time in CheckIt's CPU test on the device, 2026-11-13). The pager clears
// both through this slot whenever a frame changes hands, so an instruction whose data access
// faults goes back to the slow fetch. pm_set_ifslot(NULL) points it at a dummy again (pm_init
// does, and t386_release_all before freeing the CPU).
extern uint8_t **pm_ifslot;
void pm_set_ifslot(uint8_t **slot);
#define PM_IF_DROP() (pm_ifslot[0] = pm_ifslot[1] = NULL)

// The HAL's PSRAM (hal.h; a malloc'd buffer on the desktop).
size_t hal_psram_size(void);
void   hal_psram_read(uint32_t addr, void *dst, size_t n);
void   hal_psram_write(uint32_t addr, const void *src, size_t n);
// The SRAM arena: VRAM (PM_VRAM_BYTES), then the page frames. The menu canvas reuses its start.
uint8_t *hal_arena(size_t *size);

// frames: nframes * PM_PAGE_SIZE of SRAM, at least 2-byte aligned. mem_bytes of guest RAM, all
// of it reading as zero.
void     pm_init(uint8_t *frames, int nframes, uint32_t mem_bytes);
void     pm_shutdown(void);
// VRAM bytes PM_VRAM_BYTES to PM_VRAM_FULL, at PSRAM address psram (0: none), cleared.
void     pm_vram_hi_init(uint32_t psram);
// VRAM byte `byte` (PM_VRAM_BYTES or above) in a cached line, which it marks dirty when write is
// set; valid until the next call. NULL past PM_VRAM_FULL. Callers stay inside one aligned dword.
uint8_t *pm_vram_hi(uint32_t byte, int write);
uint8_t *pm_fault(uint32_t page, int write);          // the frame, mapped; NULL past the RAM
uint8_t  pm_slow_read8(uint32_t a);
uint16_t pm_slow_read16(uint32_t a);
uint32_t pm_slow_read32(uint32_t a);
void     pm_slow_write8(uint32_t a, uint8_t v);
void     pm_slow_write16(uint32_t a, uint16_t v);
void     pm_slow_write32(uint32_t a, uint32_t v);
void     pm_read(uint32_t a, void *dst, size_t n);
void     pm_write(uint32_t a, const void *src, size_t n);
// The frame behind a, faulted in, for an access that stays inside a's page. Valid until the
// next pm_* call that can fault.
uint8_t *pm_ptr(uint32_t a, int write);
// Lends a run of frames as plain SRAM, for buffers the heap has no room for (a folder floppy's
// FAT and root directory, while the PC runs). They come from the highest frames at or above
// `above`, so the menu canvas, which covers the arena's start, never draws over them. NULL when
// that would leave the pager fewer than PM_MIN_FRAMES. pm_unlend takes them back; it returns 0
// for a pointer that is not the arena's (a heap block), 1 otherwise, even after pm_shutdown.
#define PM_MIN_FRAMES 16
void    *pm_lend(size_t bytes, const uint8_t *above);
int      pm_unlend(void *p);
// Snapshots: page `page` copied out or in without faulting it in, so the frames keep what the
// guest was using. pm_get_page returns 0 (buf untouched) for a page never written, which reads
// as zeros. pm_put_page with buf NULL makes the page zeros again.
int      pm_get_page(uint32_t page, void *buf);
void     pm_put_page(uint32_t page, const void *buf);
// Fault and write-back counts since pm_init, for the FPS line.
uint32_t pm_faults(void);
uint32_t pm_writebacks(void);

#ifdef __cplusplus
}
#endif

// Unaligned little-endian access; packed structs keep the Cortex-M33 off LDRD/LDM, which fault
// on unaligned addresses.
typedef struct { uint16_t v; } __attribute__((packed)) pm_u16_t;
typedef struct { uint32_t v; } __attribute__((packed)) pm_u32_t;

static inline uint32_t pm_entry(uint32_t a) {
  const uint32_t pg = a >> PM_PAGE_SHIFT;
  return pg < PM_MAX_PAGES ? pm_map[pg] : 0;
}
#define PM_FRAME(e) (pm_frame0 + (((e) >> 1) << PM_PAGE_SHIFT))

static inline uint8_t pm_read8(uint32_t a) {
  const uint32_t e = pm_entry(a);
  return e ? PM_FRAME(e)[a & PM_PAGE_MASK] : pm_slow_read8(a);
}
static inline uint16_t pm_read16(uint32_t a) {
  const uint32_t e = pm_entry(a);
  if (e && (a & PM_PAGE_MASK) != PM_PAGE_MASK)
    return ((const pm_u16_t *)(PM_FRAME(e) + (a & PM_PAGE_MASK)))->v;
  return pm_slow_read16(a);
}
static inline uint32_t pm_read32(uint32_t a) {
  const uint32_t e = pm_entry(a);
  if (e && (a & PM_PAGE_MASK) <= PM_PAGE_MASK - 3)
    return ((const pm_u32_t *)(PM_FRAME(e) + (a & PM_PAGE_MASK)))->v;
  return pm_slow_read32(a);
}
static inline void pm_write8(uint32_t a, uint8_t v) {
  const uint32_t e = pm_entry(a);
  if (e & PM_WRITABLE) PM_FRAME(e)[a & PM_PAGE_MASK] = v;
  else pm_slow_write8(a, v);
}
static inline void pm_write16(uint32_t a, uint16_t v) {
  const uint32_t e = pm_entry(a);
  if ((e & PM_WRITABLE) && (a & PM_PAGE_MASK) != PM_PAGE_MASK)
    ((pm_u16_t *)(PM_FRAME(e) + (a & PM_PAGE_MASK)))->v = v;
  else pm_slow_write16(a, v);
}
static inline void pm_write32(uint32_t a, uint32_t v) {
  const uint32_t e = pm_entry(a);
  if ((e & PM_WRITABLE) && (a & PM_PAGE_MASK) <= PM_PAGE_MASK - 3)
    ((pm_u32_t *)(PM_FRAME(e) + (a & PM_PAGE_MASK)))->v = v;
  else pm_slow_write32(a, v);
}

#endif
