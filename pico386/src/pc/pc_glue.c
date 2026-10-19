// pc_glue.c -- pico-386's side of the tiny386 core: builds a PC from a profile .ini and exposes
// it through the plain C API in pc_glue.h. Compiled as C with the core's own build settings.
#include "tiny386/tiny386_port.h"
#include <stdint.h>
#include <string.h>
#include "tiny386/pc.h"
#include "pc_glue.h"
#include "pc_folder.h"

static PC *s_pc;
static char s_err_file[160];

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
    // A missing CD image just means an empty drive; a missing hard disk is an error.
    if (conf.iscd[i] && conf.disks[i] && conf.disks[i][0] && !file_exists(conf.disks[i])) {
      t386_log("tiny386: no CD image %s, drive left empty", conf.disks[i]);
      conf.disks[i] = "";
    } else {
      ok = check_file(conf.disks[i]);
    }
  }
  for (int i = 0; ok && i < 2; i++) {
    conf.fdd[i] = resolve(conf.fdd[i], sd_root);
    if (conf.fdd[i] && conf.fdd[i][0] && !file_exists(conf.fdd[i])) {
      t386_log("tiny386: no floppy image %s, drive left empty", conf.fdd[i]);
      conf.fdd[i] = NULL;
    }
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
        t386_log("tiny386: %s is not a floppy image size, drive left empty", conf.fdd[i]);
        conf.fdd[i] = NULL;
      }
      emulink_attach_floppy(probe, i, NULL);
    }
  }

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

void t386_stop() {
  t386_release_all();                   // closes the images (flushing writes), frees everything
  s_pc = NULL;
}

uint32_t t386_instructions() {
  return s_pc ? (uint32_t)cpui386_get_cycle(s_pc->cpu) : 0;
}

void t386_reset() {
  if (s_pc) s_pc->reset_request = 1;
}

// ---- running ---------------------------------------------------------------------------------

void t386_run_until(uint32_t deadline_us) {
  if (!s_pc) return;
  // vga_step() keeps the retrace bit (3DA) moving, which DOS programs poll to pace themselves,
  // so it runs after every pc_step(). Drawing is left to the frontend (t386_refresh).
  do {
    pc_step(s_pc);
    vga_step(s_pc->vga);
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
  if (!s_pc || drive < 0 || drive > 1) return -1;
  return emulink_attach_floppy(s_pc->emulink, drive, path);
}

void t386_change_cd(int ide_drive, const char *path) {
  if (!s_pc || ide_drive < 0 || ide_drive > 3) return;
  ide_change_cd(ide_drive < 2 ? s_pc->ide : s_pc->ide2, ide_drive & 1, path);
}
