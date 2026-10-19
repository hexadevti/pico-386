// settings.h -- user settings, kept as "key=value" lines in <sd>/pc-386/pico386.cfg.
#pragma once

#include <stdint.h>
#include <string>

struct Settings {
  int volume = 7;            // 0..10
  int frameskip = 0;         // 0 = auto, N = draw 1 frame in N+1
  bool showFps = false;
  int fontSize = 0;          // 80x25 text: 0 = 4x12 cells (fills the panel), 1 = 4x8
  std::string lastDir;       // where the ROM browser was last
  std::string startup;       // the .ini of the startup last chosen ("pc.ini"), for the home menu's cursor
  std::string biosDir = "roms/pc";   // bios.bin + vgabios.bin, relative to the card root
};

extern Settings g_settings;

void settingsLoad();
void settingsSave();

// <sd root>/<rel>, and a mkdir -p for it (the path's own last component included).
std::string sdPath(const char *rel);
bool makeDirs(const std::string &path);
bool isDir(const std::string &path);
