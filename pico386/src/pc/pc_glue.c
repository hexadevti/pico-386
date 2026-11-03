// pc_glue.c -- pico-386's side of the tiny386 core: builds a PC from a profile .ini and exposes
// it through the plain C API in pc_glue.h. Compiled as C with the core's own build settings.
#include "tiny386/tiny386_port.h"
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "tiny386/pc.h"
#include "pc_glue.h"
#include "pc_folder.h"
#include "pc_zip.h"
#include "tiny386/snap.h"

static PC *s_pc;
#if defined(PICO386_LOWMEM)
static int s_parked;                    // the arena's start is in PSRAM while the menu uses it
// The menu canvas covers VRAM and, as VRAM is smaller, the first page frames too (pcmem_park).
#define PARK_BYTES (PANEL_W * PANEL_H * 2 > PM_VRAM_BYTES ? PANEL_W * PANEL_H * 2 : PM_VRAM_BYTES)
#endif
static char s_err_file[160];
// What A: and B: hold, as emulink got it (a zip as the folder it unpacked to), so a swap can tell
// a folder in the other drive or in D: from the one it replaces.
static char s_fd_path[2][256];

const char *t386_error_file() { return s_err_file; }

// ---- configuration ---------------------------------------------------------------------------

static int is_absolute(const char *p) {
  if (p[0] == '/' || p[0] == '\\') return 1;
  return p[0] && p[1] == ':';           // "C:..." on the desktop build
}

// Makes a relative path absolute under root. The old string came from the tracked heap (or is a
// literal default), and so does the new one: both go away with t386_release_all().
static const char *resolve(const char *path, const char *root) {
  if (!path || !path[0] || is_absolute(path)) return path;
  const size_t n = strlen(root) + 1 + strlen(path) + 1;
  char *full = (char *)malloc(n);
  if (!full) return path;
  snprintf(full, n, "%s/%s", root, path);
  return full;
}

static int file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

// tiny386 asserts (or exits) on any image it cannot use, so every file is checked up front.
static int check_file(const char *path) {
  if (!path || !path[0] || file_exists(path) || pfolder_is_dir(path)) return 1;
  snprintf(s_err_file, sizeof s_err_file, "%s", path);
  t386_log("tiny386: missing %s", path);
  return 0;
}

// dir/name, on the tracked heap like resolve()'s strings.
static const char *join(const char *dir, const char *name) {
  const size_t n = strlen(dir) + 1 + strlen(name) + 1;
  char *full = (char *)malloc(n);
  if (!full) return name;
  snprintf(full, n, "%s%s%s", dir, dir[0] ? "/" : "", name);
  return full;
}

// A floppy as emulink takes it: an image or a folder as it is, a zip as the folder it unpacks to,
// or as its first disk when it holds floppy images. T386_FD_* on failure.
static int floppy_source(const char *path, char *out, size_t n) {
  if (pczip_is_zip(path)) {
    if (!file_exists(path)) return T386_FD_MISSING;
    if (pczip_floppy(path, out, n) < 0) return T386_FD_UNPACK;
    return 0;
  }
  if (!file_exists(path) && !pfolder_is_dir(path)) return T386_FD_MISSING;
  snprintf(out, n, "%s", path);
  return 0;
}

// tiny386's .ini keys, plus [pico] boot = cd. The rest of [pico] is read by pc_profiles.cpp.
static int s_boot_cd;

static int parse_ini(void *user, const char *section, const char *name, const char *value) {
  if (!strcmp(section, "pico") && !strcmp(name, "boot")) s_boot_cd = (value[0] | 0x20) == 'c';
  return parse_conf_ini(user, section, name, value);
}

int t386_start(const char *ini_path, const char *sd_root, const char *bios_dir, uint16_t *fb) {
  s_err_file[0] = 0;
  if (s_pc) t386_stop();

  PCConfig conf;
  memset(&conf, 0, sizeof conf);
  conf.mem_size = 8 * 1024 * 1024;
  conf.vga_mem_size = 256 * 1024;
  conf.fill_cmos = 1;
  conf.cpu_gen = 4;
  conf.fpu = 0;
  s_boot_cd = 0;
  if (ini_parse(ini_path, parse_ini, &conf) != 0) {
    snprintf(s_err_file, sizeof s_err_file, "%s", ini_path);
    t386_log("tiny386: cannot parse %s", ini_path);
    t386_release_all();
    return T386_ERR_INI;
  }
  // The framebuffer is ours, whatever [display] says.
  conf.width = T386_FB_W;
  conf.height = T386_FB_H;
  if (conf.cpu_gen < 3) conf.cpu_gen = 3;
  if (conf.cpu_gen > 5) conf.cpu_gen = 5;
  if (conf.vga_mem_size < 256 * 1024) conf.vga_mem_size = 256 * 1024;
  conf.bios = join(bios_dir, "bios.bin");
  conf.vga_bios = join(bios_dir, "vgabios.bin");
  conf.enable_serial = 0;
  conf.linuxstart = conf.kernel = conf.initrd = conf.cmdline = NULL;

  // Every machine has a CD drive (secondary master, empty unless the .ini fills it), so a disc
  // can be put in while it runs.
  if (!conf.disks[2] || !conf.disks[2][0]) {
    conf.disks[2] = "";
    conf.iscd[2] = 1;
  }

  conf.bios = resolve(conf.bios, sd_root);
  conf.vga_bios = resolve(conf.vga_bios, sd_root);
  int ok = check_file(conf.bios) && check_file(conf.vga_bios);
  for (int i = 0; ok && i < 4; i++) {
    conf.disks[i] = resolve(conf.disks[i], sd_root);
    // A zip is a folder disk: the folder it unpacks to (pc_zip.cpp).
    if (!conf.iscd[i] && pczip_is_zip(conf.disks[i]) && file_exists(conf.disks[i])) {
      char dir[256];
      if (pczip_folder(conf.disks[i], dir, sizeof dir) != 0) {
        snprintf(s_err_file, sizeof s_err_file, "%s", conf.disks[i]);
        t386_log("tiny386: cannot unpack %s", conf.disks[i]);
        t386_release_all();
        return T386_ERR_ZIP;
      }
      conf.disks[i] = join("", dir);
    }
    // A missing CD image just means an empty drive; a missing hard disk is an error.
    if (conf.iscd[i] && conf.disks[i] && conf.disks[i][0] && !file_exists(conf.disks[i])) {
      t386_log("tiny386: no CD image %s, drive left empty", conf.disks[i]);
      conf.disks[i] = "";
    } else {
      ok = check_file(conf.disks[i]);
    }
  }
  for (int i = 0; ok && i < 2; i++) {
    s_fd_path[i][0] = 0;
    conf.fdd[i] = resolve(conf.fdd[i], sd_root);
    if (!conf.fdd[i] || !conf.fdd[i][0]) continue;
    char src[256];
    const int r = floppy_source(conf.fdd[i], src, sizeof src);
    if (r != 0) {
      t386_log("tiny386: %s %s, drive left empty", r == T386_FD_UNPACK ? "cannot unpack" : "no floppy",
               conf.fdd[i]);
      conf.fdd[i] = NULL;
      continue;
    }
    // one folder in two drives would be two disks writing back over each other
    int clash = i == 1 && conf.fdd[0] && !strcasecmp(s_fd_path[0], src);
    for (int j = 0; j < 4; j++)
      clash |= !conf.iscd[j] && conf.disks[j] && !strcasecmp(conf.disks[j], src);
    if (clash) {
      t386_log("tiny386: %s is in another drive, %c: left empty", src, 'A' + i);
      conf.fdd[i] = NULL;
      continue;
    }
    conf.fdd[i] = join("", src);
    snprintf(s_fd_path[i], sizeof s_fd_path[i], "%s", src);
  }
  if (!ok) {
    t386_release_all();
    return T386_ERR_FILE;
  }

  // Floppies of a size the BIOS does not know trip an assert in pc_new(): try them on a
  // scratch emulink first.
  if (conf.fdd[0] || conf.fdd[1]) {
    EMULINK *probe = emulink_init();
    for (int i = 0; probe && i < 2; i++) {
      if (!conf.fdd[i]) continue;
      if (emulink_attach_floppy(probe, i, conf.fdd[i]) != 0) {
        t386_log("tiny386: %s %s, drive left empty", conf.fdd[i],
                 pfolder_is_dir(conf.fdd[i]) ? "does not fit on a floppy" : "is not a floppy image size");
        conf.fdd[i] = NULL;
        s_fd_path[i][0] = 0;
      }
      emulink_attach_floppy(probe, i, NULL);
    }
    free(probe);                        // else it sits in the middle of the RP2350's heap
  }

#if defined(PICO386_LOWMEM)
  // Guest RAM lives in PSRAM and is paged through the SRAM arena's frames; VRAM is the arena's
  // first PM_VRAM_BYTES. The PSRAM's end is where the menu parks the arena (pcmem_park), and
  // the zip inflater's window is below that (pcmem_scratch).
  {
    size_t arena_n = 0;
    uint8_t *arena = hal_arena(&arena_n);
    const long room = pcmem_scratch() & ~0xfffffL;
    const int nframes = arena && arena_n > PM_VRAM_BYTES ? (int)((arena_n - PM_VRAM_BYTES) >> PM_PAGE_SHIFT) : 0;
    conf.vga_mem_size = PM_VRAM_BYTES;
    if (conf.mem_size > room) conf.mem_size = room;
    if (conf.mem_size > (long)PM_MAX_RAM) conf.mem_size = PM_MAX_RAM;
    if (conf.mem_size < 1L << 20 || nframes < 8) {
      t386_log("tiny386: no PSRAM (%u KB) or SRAM arena (%u frames)", (unsigned)(hal_psram_size() >> 10),
               (unsigned)nframes);
      t386_release_all();
      return T386_ERR_MEMORY;
    }
    s_parked = 0;
    pm_vram = arena;
    pm_init(arena + PM_VRAM_BYTES, nframes, (uint32_t)conf.mem_size);
    t386_log("tiny386: paging guest RAM through %d frames of SRAM", nframes);
  }
#else
  // Guest RAM and VRAM are the big allocations; tiny386 does not check them.
  // pc.ini asks for T386_MEM_MAX_MB, which may be more than PSRAM has: step down 4 MB at a
  // time to what fits, keeping 2 MB for the framebuffer, D:'s tables and the rest.
  if (conf.mem_size > (long)T386_MEM_MAX_MB << 20) conf.mem_size = (long)T386_MEM_MAX_MB << 20;
  void *probe;
  while (!(probe = bigmalloc((size_t)conf.mem_size + (size_t)conf.vga_mem_size + 2 * 1024 * 1024)) &&
         conf.mem_size > 4L << 20)
    conf.mem_size -= 4L << 20;
  if (!probe) {
    t386_log("tiny386: not enough PSRAM for %ld KB RAM + %ld KB VRAM",
             conf.mem_size >> 10, conf.vga_mem_size >> 10);
    t386_release_all();
    return T386_ERR_MEMORY;
  }
  free(probe);
#endif

  t386_log("tiny386: %ld KB RAM, %ld KB VRAM, gen %d, fpu %d", conf.mem_size >> 10,
           conf.vga_mem_size >> 10, conf.cpu_gen, conf.fpu);
  s_pc = pc_new(NULL, NULL, (u8 *)fb, &conf);
  if (!s_pc) {
    t386_release_all();
    return T386_ERR_MEMORY;
  }
  // SeaBIOS's boot order (its QEMU CMOS bytes: 3Dh = first | second << 4, 38h high nibble =
  // third; 1 floppy, 2 hard disk, 3 CD). Left at 0 it tried C: before the CD.
  cmos_set(s_pc->cmos, 0x3d, s_boot_cd ? 0x13 : 0x21);   // CD, A: / A:, C:
  cmos_set(s_pc->cmos, 0x38, s_boot_cd ? 0x20 : 0x30);   // then C: / then CD
  load_bios_and_reset(s_pc);
  return T386_OK;
}

void t386_set_line_sink(T386LineFunc *f, void *opaque) {
#if defined(PICO386_LOWMEM)
  if (s_pc) vga_set_line_sink(s_pc->vga, f, opaque);
#else
  (void)f; (void)opaque;
#endif
}

void t386_stop() {
  t386_release_all();                   // closes the images (flushing writes), frees everything
  s_pc = NULL;
#if defined(PICO386_LOWMEM)
  pm_shutdown();
  s_parked = 0;
#endif
}

#if defined(PICO386_LOWMEM)
// The menu draws its canvas over the arena's start, VRAM and the first page frames (the frames
// are restored byte for byte, so the page map stays right), so that goes to the end of the PSRAM
// while the menu is up. A machine restarted from the menu (C:/D: change) starts with s_parked
// clear: its BIOS sets the mode and clears VRAM anyway.
void pcmem_park() {
  if (!s_pc || s_parked) return;
  hal_psram_write((uint32_t)(hal_psram_size() - PARK_BYTES), pm_vram, PARK_BYTES);
  s_parked = 1;
}

void pcmem_unpark() {
  if (!s_pc || !s_parked) return;
  hal_psram_read((uint32_t)(hal_psram_size() - PARK_BYTES), pm_vram, PARK_BYTES);
  s_parked = 0;
}

// Frames past the canvas only: the folder floppy builds its FAT while the menu is drawn.
void *pcmem_lend(size_t n) { return pm_vram ? pm_lend(n, pm_vram + PARK_BYTES) : NULL; }
int pcmem_free(void *p) { return pm_unlend(p); }

long pcmem_scratch() { return (long)hal_psram_size() - (long)PARK_BYTES - PCMEM_SCRATCH_BYTES; }

long pcmem_spare(long *bytes) {
  *bytes = PCMEM_SPARE_BYTES;
  return pcmem_scratch() - (long)PM_MAX_RAM >= PCMEM_SPARE_BYTES ? (long)PM_MAX_RAM : -1;
}

long pcmem_folders(long *bytes) {
  const long at = (long)PM_MAX_RAM + PCMEM_SPARE_BYTES;
  *bytes = pcmem_scratch() - at;
  return *bytes > 0 ? at : -1;
}
#else
void pcmem_park() {}
void pcmem_unpark() {}
void *pcmem_lend(size_t n) { (void)n; return NULL; }
int pcmem_free(void *p) { (void)p; return 0; }
long pcmem_scratch() { return -1; }
long pcmem_spare(long *bytes) { *bytes = 0; return -1; }
long pcmem_folders(long *bytes) { *bytes = 0; return -1; }
#endif

// Where the guest is (CS, EIP), for the RP2350's bring-up heartbeat. Read from the other core
// while it runs, so only roughly right.
void t386_where(uint32_t *cs, uint32_t *eip) {
  *cs = *eip = 0;
  if (s_pc) cpui386_get_pc(s_pc->cpu, cs, eip);
}

uint32_t t386_instructions() {
  return s_pc ? (uint32_t)cpui386_get_cycle(s_pc->cpu) : 0;
}

// The timer's health, for the heartbeat: channel 0's reload value, how many periods its IRQs are
// behind the clock, IRQ0s raised, and IRQ0s that found the last one still pending (lost).
extern uint32_t pit_irq0_fired, pic_irq0_merged;
void i8254_info(PITState *pit, uint32_t *count, uint32_t *behind);
void t386_timer_info(uint32_t *count, uint32_t *behind, uint32_t *fired, uint32_t *lost) {
  *count = *behind = 0;
  if (s_pc) i8254_info(s_pc->pit, count, behind);
  *fired = pit_irq0_fired;
  *lost = pic_irq0_merged;
}

void t386_reset() {
  if (s_pc) s_pc->reset_request = 1;
}

// ---- running ---------------------------------------------------------------------------------

static void (*s_audio_hook)(int);

void t386_set_audio_hook(void (*hook)(int sound_written)) { s_audio_hook = hook; }

extern uint32_t pico_spk_writes, pico_adlib_writes;

void t386_run_until(uint32_t deadline_us) {
  if (!s_pc) return;
  // vga_step() keeps the retrace bit (3DA) moving, which DOS programs poll to pace themselves,
  // so it runs after every pc_step(). Drawing is left to the frontend (t386_refresh).
  // The audio hook may make sound now: music written in the middle of a long frame then plays
  // near when it was written, not all at the frame's end.
  static uint32_t sound;
  do {
    pc_step(s_pc);
    vga_step(s_pc->vga);
    if (s_audio_hook) {
      const uint32_t now = pico_adlib_writes + pico_spk_writes;
      s_audio_hook(now != sound);
      sound = now;
    }
  } while ((int32_t)(deadline_us - get_uticks()) > 0);
}

extern uint32_t sb16_starved;
uint32_t t386_sb_starved() { return sb16_starved; }

void t386_mix(int16_t *stereo, int n) {
  if (n > 1024) n = 1024;
  memset(stereo, 0, (size_t)n * 4);              // the SB16 leaves it untouched while idle
  if (!s_pc) return;
  mixer_callback(s_pc, (uint8_t *)stereo, n * 4);
}

void t386_key(int down, int keycode) {
  if (s_pc) ps2_put_keycode(s_pc->kbd, down, keycode);
}

void t386_mouse(int dx, int dy, int buttons) {
  if (s_pc) ps2_mouse_event(s_pc->mouse, dx, dy, 0, buttons);
}

static void on_redraw(void *opaque, int x, int y, int w, int h) {
  (void)x; (void)y; (void)w; (void)h;
  *(int *)opaque = 1;
}

int t386_refresh(int full) {
  int drawn = 0;
  if (s_pc) vga_refresh(s_pc->vga, on_redraw, &drawn, full);
  return drawn;
}

void t386_info(VGAPicoInfo *info) {
  if (s_pc) vga_pico_info(s_pc->vga, info);
  else memset(info, 0, sizeof *info);
}

int t386_swap_floppy(int drive, const char *path) {
  if (!s_pc || drive < 0 || drive > 1) return T386_FD_SIZE;
  char src[256] = "";
  if (path && path[0]) {
    const int r = floppy_source(path, src, sizeof src);
    if (r != 0) return r;
    // a folder already open elsewhere (the other drive, or D:); the one this drive holds is fine
    if (pfolder_is_dir(src) && pfolder_in_use(src) && strcasecmp(src, s_fd_path[drive]))
      return T386_FD_IN_USE;
  }
  const int folder = src[0] && pfolder_is_dir(src);
  const int r = emulink_attach_floppy(s_pc->emulink, drive, src[0] ? src : NULL);
  snprintf(s_fd_path[drive], sizeof s_fd_path[drive], "%s", r == 0 ? src : "");
  if (!src[0]) return 0;
  return r == 0 ? 0 : folder ? T386_FD_TOO_BIG : T386_FD_SIZE;
}

int t386_floppy_dirty(int drive) {
  return s_pc && drive >= 0 && drive < 2 && pfolder_is_dirty(emulink_floppy_folder(s_pc->emulink, drive));
}

void t386_change_cd(int ide_drive, const char *path) {
  if (!s_pc || ide_drive < 0 || ide_drive > 3) return;
  ide_change_cd(ide_drive < 2 ? s_pc->ide : s_pc->ide2, ide_drive & 1, path);
}

// ---- snapshots ----
// Memory goes as 1 KB pages, each a byte (0: all zeros, 1: data follows) and then its data, so
// the RAM DOS never touched costs a byte a page. mem is NULL for paged guest RAM (the RP2350).
#define SNAP_PAGE 1024

static int snap_zero(const uint32_t *w) {
  for (int i = 0; i < SNAP_PAGE / 4; i++)
    if (w[i]) return 0;
  return 1;
}

static void snap_mem(SnapIO *io, uint8_t *mem, uint32_t bytes) {
  uint32_t buf[SNAP_PAGE / 4];
  for (uint32_t pg = 0; pg < bytes / SNAP_PAGE && !io->err; pg++) {
    uint8_t *p = mem ? mem + pg * SNAP_PAGE : (uint8_t *)buf;
    uint8_t has = 0;
    if (!io->load) {
#if defined(PICO386_LOWMEM)
      has = mem ? 1 : pm_get_page(pg, buf);
#else
      has = 1;
#endif
      has = has && !snap_zero((const uint32_t *)p);
      snap_io(io, &has, 1);
      if (has) snap_io(io, p, SNAP_PAGE);
      continue;
    }
    snap_io(io, &has, 1);
    if (has) snap_io(io, p, SNAP_PAGE);
    if (io->err) break;
    if (mem) {
      if (!has) memset(p, 0, SNAP_PAGE);
    }
#if defined(PICO386_LOWMEM)
    else {
      pm_put_page(pg, has ? buf : NULL);
    }
#endif
  }
}

static int snap_run(FILE *f, int load) {
  if (!s_pc) return T386_SNAP_FORMAT;
  SnapIO io = { f, load, 0 };
  uint32_t now = get_uticks();
  snap_check(&io, 0x54333836);                    // "T386"
  snap_check(&io, (unsigned)s_pc->phys_mem_size);
  snap_check(&io, (unsigned)s_pc->vga_mem_size);
  snap_io(&io, &now, sizeof now);
  pc_snap(s_pc, &io);
#if defined(PICO386_LOWMEM)
  snap_mem(&io, NULL, (uint32_t)s_pc->phys_mem_size);
#else
  snap_mem(&io, (uint8_t *)s_pc->phys_mem, (uint32_t)s_pc->phys_mem_size);
#endif
  snap_mem(&io, (uint8_t *)s_pc->vga_mem, (uint32_t)s_pc->vga_mem_size);
  snap_check(&io, 0x454e4421);                    // "END!"
  if (io.err) return io.err == 2 ? T386_SNAP_FORMAT : T386_SNAP_IO;
  // The guest's clock carries on from the snapshot's: the PIT, the cursor blink and the
  // keyboard's typematic delay are all timed against it.
  if (load) t386_clock_set(now);
  return 0;
}

int t386_save_state(FILE *f) { return snap_run(f, 0); }
int t386_load_state(FILE *f) { return snap_run(f, 1); }
