// pc_profiles.cpp -- the startups and the home menu that starts them.
//
// A startup is the one PC (see machineIni) with a set of drives: a tiny386 .ini in /pc-386 whose
// [pico] name the home menu shows. pc.ini is the first, written with the defaults below the first
// time it is needed ("PC" until renamed); "Create new startup" adds startN.ini files. Any other
// .ini in the folder (test machines) has no name and is not listed. Choosing a startup opens its
// drives screen (pc_disks.cpp), which edits that .ini, as does everything else chosen for it (the
// keyboard mouse, the boot order), so it starts the same next time.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>
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
      "; the keyboard mouse (both Shifts or Ctrl+Tab): 0 arrows, 1 mouse, 2 both; saved when it changes\n"
      "mouse = 0\n"
      "; its button keys: alt rshift lshift ctrl space enter tab bs z x none (the in-game menu)\n"
      "mouse_left = alt\n"
      "mouse_right = rshift\n"
      "; the name the home menu shows (Rename on the drives screen)\n"
      ";name = PC\n"
      "; cd: boot from the CD before C: (Boot from, on the drives screen)\n"
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

constexpr size_t kNameMax = 28;          // characters in a startup's name

// What the home menu calls a startup: [pico] name, "PC" for a pc.ini that has none.
std::string startupName(const std::string &ini) {
  const std::string n = pcini_get_in(ini, "pico", "name");
  return n.empty() ? "PC" : n;
}

struct Startup {
  std::string ini, file, name;           // full path, the file name ("pc.ini"), what is shown
};

// pc.ini first, then every named .ini in /pc-386 by name.
std::vector<Startup> listStartups() {
  std::vector<Startup> v;
  const std::string pc = pcIni();
  v.push_back({ pc, "pc.ini", startupName(pc) });
  const std::string dir = sdPath("pc-386");
  std::vector<Startup> more;
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d)) {
      const std::string f = e->d_name;
      if (f.size() < 5 || strcasecmp(f.c_str() + f.size() - 4, ".ini") || !strcasecmp(f.c_str(), "pc.ini")) continue;
      const std::string ini = dir + "/" + f;
      const std::string name = pcini_get_in(ini, "pico", "name");
      if (!name.empty()) more.push_back({ ini, f, name });
    }
    closedir(d);
  }
  std::sort(more.begin(), more.end(),
            [](const Startup &a, const Startup &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
  v.insert(v.end(), more.begin(), more.end());
  return v;
}

// Asks for a name: true with it in name, false on Esc.
bool askName(const char *title, std::string &name) {
  char buf[kNameMax + 1];
  snprintf(buf, sizeof buf, "%s", name.c_str());
  if (!ui::editText(title, "Its name on the home menu", buf, sizeof buf)) return false;
  name = buf;
  return true;
}

// "Create new startup": asks the name and writes the first free /pc-386/startN.ini, with the
// default C: and the rest empty. Returns its path, or "" (Esc, or the card cannot be written).
std::string createStartup() {
  std::string name = "New startup";
  if (!askName("Create new startup", name)) return "";
  std::string ini;
  for (int i = 1; i < 1000; i++) {
    ini = sdPath(("pc-386/start" + std::to_string(i) + ".ini").c_str());
    if (!exists(ini)) break;
  }
  const std::string c = defaultHardDisk();
  const std::string drives = (c.empty() ? std::string(";hda = pc-386/c.img\n") : "hda = " + c + "\n") +
      ";fda = pc-386/boot.img\n"
      ";cdc = pc-386/cd.iso\n";
  if (!writeText(ini, machineIni(drives, "; a pico-386 startup. tiny386's .ini keys; paths are relative to the SD card root.\n").c_str()) ||
      !pcini_set_in(ini, "pico", "name", name)) {
    ui::message("Error", "Cannot write the startup (card full?)", 2000);
    return "";
  }
  return ini;
}

std::string s_title;

} // namespace

const char *pcprof_title() { return s_title.c_str(); }

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

// A startup's drives screen, then Start, Rename or Delete. true = start it.
static bool openStartup(const Startup &st, std::string &iniOut) {
  const bool isPc = st.file == "pc.ini";
  std::string name = st.name;
  for (;;) {
    switch (pcdisk_machine_menu(st.ini, name.c_str(), !isPc)) {
      case MACHINE_START:
        s_title = name;
        iniOut = st.ini;
        return true;
      case MACHINE_RENAME:
        if (askName("Rename", name) && !pcini_set_in(st.ini, "pico", "name", name))
          ui::message("Error", "Cannot write the .ini", 1500);
        name = startupName(st.ini);
        continue;
      case MACHINE_DELETE:
        if (remove(st.ini.c_str()) != 0) ui::message("Error", "Cannot delete the .ini", 1500);
        return false;
      default:
        return false;
    }
  }
}

bool pcprof_home(std::string &iniOut) {
  int sel = -1;
  for (;;) {
    const std::vector<Startup> startups = listStartups();
    const int ns = (int)startups.size();
    const int rowNew = ns, rowCreate = ns + 1, rowSettings = ns + 2, rowExit = ns + 3, rows = ns + 4;
    if (sel < 0) {                        // the startup last chosen
      sel = 0;
      for (int i = 0; i < ns; i++)
        if (startups[i].file == g_settings.startup) sel = i;
    }
    if (sel >= rows) sel = rows - 1;
    std::vector<std::string> drives(ns);
    std::vector<ui::Item> items(rows);
    for (int i = 0; i < ns; i++) {
      drives[i] = drivesLine(startups[i].ini);
      items[i] = { startups[i].name.c_str(), nullptr, ui::ICON_PLAY, drives[i].c_str() };
    }
    items[rowNew] = { "Create new startup", nullptr, ui::ICON_FOLDER, "A named set of drives (C:, a CD, a zip...)" };
    items[rowCreate] = { "Create a hard disk", nullptr, ui::ICON_SAVE, "A new, formatted C: drive (32-504 MB)" };
    items[rowSettings] = { "Settings", nullptr, ui::ICON_GEAR, "Volume, FPS meter" };
#if defined(BOARD_DESKTOP)
    items[rowExit] = { "Quit", nullptr, ui::ICON_EXIT, "Close the window" };
#else
    items[rowExit] = { "Exit to loader", nullptr, ui::ICON_EXIT, "Back to the esp32loader menu" };
#endif
    ui::Screen scr;
    scr.title = "pico-386";
    scr.subtitle = "386 PC + VGA";
    scr.hints = "Up/Dn|Move;Enter|Select";
    scr.logo = true;
    const uint8_t k = ui::list(scr, items.data(), rows, &sel);
    if (k != KEY_ENTER) continue;
    if (sel < ns) {
      g_settings.startup = startups[sel].file;
      settingsSave();
      if (openStartup(startups[sel], iniOut)) return true;
      sel = -1;                           // renamed (re-sorted) or deleted: find it again
      continue;
    }
    if (sel == rowNew) {
      const std::string ini = createStartup();
      if (ini.empty()) continue;
      g_settings.startup = ini.substr(ini.rfind('/') + 1);
      settingsSave();
      sel = -1;                           // its row, wherever the sort put it
      if (openStartup({ ini, g_settings.startup, startupName(ini) }, iniOut)) return true;
      continue;
    }
    if (sel == rowCreate) { pcdisk_create_menu(); continue; }
    if (sel == rowSettings) { pcprof_settings_menu(); continue; }
    if (sel == rowExit) {
      hal_reboot_to_loader();
      return false;
    }
  }
}
