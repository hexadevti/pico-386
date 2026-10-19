// pc_profiles.cpp -- the PC and the home menu that starts it.
//
// The machine is a tiny386 .ini on the card (/pc-386/pc.ini), written with the defaults below the
// first time it is needed and the user's to edit afterwards. Choosing it opens its drives screen
// (pc_disks.cpp), which edits the drive keys of that .ini. Everything chosen anywhere (drives, the
// keyboard mouse, "Boot a disk image") is written to it, so the PC starts the same next time.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <string>
#include "pc_internal.h"
#include "pc_glue.h"
#include "../frontend/ui.h"
#include "../frontend/browser.h"
#include "../frontend/settings.h"
#include "../hal/hal.h"
#include "../../core.h"

extern const EmuCore kPC386;

namespace {

bool exists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

long fileSize(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

bool writeText(const std::string &path, const char *text) {
  makeDirs(sdPath("pc-386"));
  FILE *f = fopen(path.c_str(), "w");
  if (!f) {
    hal_log("cannot write %s", path.c_str());
    return false;
  }
  fputs(text, f);
  fclose(f);
  return true;
}

// The C: a new pc.ini starts with: the first of these on the card (the last two are the old DOS
// and Windows machines' disks), else none.
std::string defaultHardDisk() {
  for (const char *n : { "pc-386/c.img", "pc-386/dos.img", "pc-386/win.img" })
    if (exists(sdPath(n))) return n;
  return "";
}

// The one machine: a 486 with an FPU and as much RAM as tiny386 takes (t386_start lowers it to
// what PSRAM has room for). 1 MB of VGA memory covers every mode the 800x600 framebuffer shows.
std::string machineIni(const std::string &drives, const char *header) {
  return std::string(header) +
      "[pc]\n"
      "; t386_start lowers it when PSRAM has no room\n"
      "mem_size = " + std::to_string(T386_MEM_MAX_MB) + "M\n"
      "vga_mem_size = 1M\n" + drives +
      "fill_cmos = 1\n"
      "\n"
      "[cpu]\n"
      "gen = 4\n"
      "fpu = 1\n"
      "\n"
      "[pico]\n"
      "; 1: the keyboard mouse (Ctrl+Tab) is on; saved when it changes\n"
      "mouse = 0\n"
      "; cd: boot from the CD before C: (set by \"Boot a disk image\")\n"
      ";boot = cd\n";
}

std::string pcIni() {
  const std::string p = sdPath("pc-386/pc.ini");
  if (!exists(p)) {
    hal_log("writing the default %s", p.c_str());
    const std::string c = defaultHardDisk();
    const std::string drives = (c.empty() ? std::string(";hda = pc-386/c.img\n") : "hda = " + c + "\n") +
        ";fda = pc-386/boot.img\n"
        ";cdc = pc-386/cd.iso\n";
    writeText(p, machineIni(drives, "; pico-386 PC. tiny386's .ini keys; paths are relative to the SD card root.\n").c_str());
  }
  return p;
}

bool hasExt(const std::string &p, const char *ext) {
  const size_t n = strlen(ext);
  return p.size() > n && !strcasecmp(p.c_str() + p.size() - n, ext);
}

// "Boot a disk image": the image goes into the PC's own drive (a floppy to A:, a CD image to the
// CD, anything else to C:) and the PC boots from it. It stays there, like a disk left in a real
// PC, so the next start finds it too. A CD boots before C: through [pico] boot = cd (CMOS boot
// order, see pc_glue.c); any other image sets the normal order back.
bool bootImage(const std::string &image, std::string &iniOut) {
  iniOut = pcIni();
  const long size = fileSize(image);
  const bool cd = hasExt(image, ".iso");
  const bool floppy = !cd && size > 0 && size <= 2949120;
  const int drive = cd ? DRIVE_CD : floppy ? DRIVE_A : DRIVE_C;
  return pcdisk_set_drive(iniOut, drive, image) &&
         pcini_set_in(iniOut, "pico", "boot", cd ? "cd" : "");
}

std::string s_title;

} // namespace

const char *pcprof_title() { return s_title.c_str(); }

bool pcprof_mouse_default(const char *iniPath) {
  FILE *f = fopen(iniPath, "r");
  if (!f) return false;
  char line[200];
  bool inPico = false, on = false;
  while (fgets(line, sizeof line, f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '[') { inPico = !strncmp(s, "[pico]", 6); continue; }
    if (inPico && !strncmp(s, "mouse", 5)) {
      const char *eq = strchr(s, '=');
      if (eq) on = atoi(eq + 1) != 0;
    }
  }
  fclose(f);
  return on;
}

// The second line of a machine row: what its drives hold.
static std::string drivesLine(const std::string &ini) {
  std::string line;
  for (int d : { DRIVE_C, DRIVE_D, DRIVE_A, DRIVE_B, DRIVE_CD }) {
    const std::string rel = pcdisk_drive(ini, d);
    if (rel.empty() && d != DRIVE_C) continue;
    if (!line.empty()) line += "  ";
    line += std::string(pcdisk_drive_letter(d)) + " " + pcdisk_drive_label(rel);
  }
  return line;
}

bool pcprof_home(std::string &iniOut) {
  enum { ROW_PC, ROW_IMAGE, ROW_CREATE, ROW_SETTINGS, ROW_EXIT, ROWS };
  int sel = g_settings.profile;
  for (;;) {
    const std::string ini = pcIni(), drives = drivesLine(ini);
    ui::Item items[ROWS];
    items[ROW_PC] = { "PC", nullptr, ui::ICON_PLAY, drives.c_str() };
    items[ROW_IMAGE] = { "Boot a disk image", nullptr, ui::ICON_FOLDER, "Floppy, hard disk or CD from /pc-386" };
    items[ROW_CREATE] = { "Create a hard disk", nullptr, ui::ICON_SAVE, "A new, formatted C: drive (32-504 MB)" };
    items[ROW_SETTINGS] = { "Settings", nullptr, ui::ICON_GEAR, "Volume, FPS meter" };
#if defined(BOARD_DESKTOP)
    items[ROW_EXIT] = { "Quit", nullptr, ui::ICON_EXIT, "Close the window" };
#else
    items[ROW_EXIT] = { "Exit to loader", nullptr, ui::ICON_EXIT, "Back to the esp32loader menu" };
#endif
    ui::Screen scr;
    scr.title = "pico-386";
    scr.subtitle = "386 PC + VGA";
    scr.hints = "Up/Dn|Move;Enter|Select";
    scr.logo = true;
    const uint8_t k = ui::list(scr, items, ROWS, &sel);
    if (k != KEY_ENTER) continue;
    if (sel == ROW_PC) {
      g_settings.profile = sel;
      settingsSave();
      if (!pcdisk_machine_menu(ini, "PC")) continue;
      s_title = "PC";
      iniOut = ini;
      return true;
    }
    if (sel == ROW_IMAGE) {
      std::string image;
      if (!pcdisk_browse(DISK_NONE, "Boot a disk image", image)) continue;
      g_settings.profile = ROW_PC;            // next time the cursor is on the PC, which has it now
      settingsSave();
      if (bootImage(image, iniOut)) {
        s_title = "PC";
        return true;
      }
      ui::message("Error", "Cannot write /pc-386/pc.ini", 1500);
      continue;
    }
    if (sel == ROW_CREATE) { pcdisk_create_menu(); continue; }
    if (sel == ROW_SETTINGS) { pcprof_settings_menu(); continue; }
    if (sel == ROW_EXIT) {
      hal_reboot_to_loader();
      return false;
    }
  }
}
