// pc_snapshot.cpp -- snapshots of a running PC, per startup, to start from later where it stopped.
//
// A snapshot is /pc-386/snapshots/<startup>/sN.snp: a header with its name, the startup's .ini
// as it was (so the same drives come back) and the generation of each disk it had open, then the
// machine itself (t386_save_state: the devices, guest RAM, VRAM). Restoring writes that .ini back
// over the startup's (keeping the startup's name), starts the machine and loads the state into it.
//
// DOS keeps parts of its disks in RAM (the FAT, buffers, SMARTDRV), so a snapshot only fits the
// disks as they were when it was taken. Each disk's generation, in /pc-386/snapshots/disks.gen,
// goes up the first time it is written after a start or a snapshot (t386_fwrite), and when a
// folder disk is written back or a zip unpacked again; restoring a snapshot whose disks moved on
// asks first. A folder disk the guest has written to cannot be snapshotted: its changes live in
// pc_folder's tables until the machine stops, and those are not part of the state.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <algorithm>
#include <string>
#include <vector>
#include "pc_internal.h"
#include "pc_glue.h"
#include "pc_folder.h"
#include "../frontend/ui.h"
#include "../frontend/settings.h"
#include "../hal/hal.h"

namespace {

constexpr char kMagic[8] = { 'P', '3', '8', '6', 'S', 'N', 'P', '1' };
constexpr size_t kMaxIni = 16384;
std::string s_pending;                 // the snapshot pc_loadRom restores next

std::string genFile() { return sdPath("pc-386/snapshots/disks.gen"); }

std::string snapDir(const std::string &ini) {
  std::string base = ini.substr(ini.find_last_of('/') + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base.resize(dot);
  return sdPath(("pc-386/snapshots/" + base).c_str());
}

// The card's paths are written relative to its root, as in the .ini files.
std::string relOf(const char *path) { return pcdisk_rel(path); }

uint32_t genOf(const std::string &rel) {
  FILE *f = fopen(genFile().c_str(), "r");
  if (!f) return 0;
  char line[300];
  uint32_t gen = 0;
  while (fgets(line, sizeof line, f)) {
    char *sp = strchr(line, ' ');
    if (!sp) continue;
    char *end = sp + strcspn(sp, "\r\n");
    *end = 0;
    if (!strcasecmp(sp + 1, rel.c_str())) { gen = (uint32_t)strtoul(line, nullptr, 10); break; }
  }
  fclose(f);
  return gen;
}

// ---- the file's pieces -------------------------------------------------------------------------

bool put(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
bool get(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

bool putStr(FILE *f, const std::string &s) {
  const uint32_t n = (uint32_t)s.size();
  return put(f, &n, 4) && put(f, s.data(), n);
}

bool getStr(FILE *f, std::string &s, size_t max) {
  uint32_t n;
  if (!get(f, &n, 4) || n > max) return false;
  s.resize(n);
  return get(f, &s[0], n);
}

struct DiskRec { std::string rel; uint32_t gen; };

struct Header {
  std::string name, ini;
  std::vector<DiskRec> disks;
};

// Reads the header; f is left at the machine state.
bool readHeader(FILE *f, Header &h, bool nameOnly) {
  char magic[8];
  if (!get(f, magic, 8) || memcmp(magic, kMagic, 8) || !getStr(f, h.name, 200)) return false;
  if (nameOnly) return true;
  uint32_t n;
  if (!getStr(f, h.ini, kMaxIni) || !get(f, &n, 4) || n > 64) return false;
  h.disks.resize(n);
  for (DiskRec &d : h.disks)
    if (!getStr(f, d.rel, 300) || !get(f, &d.gen, 4)) return false;
  return true;
}

std::string readAll(const std::string &path) {
  std::string s;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return s;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < kMaxIni) s.append(buf, n);
  fclose(f);
  return s;
}

int numberOf(const char *file) {
  if ((file[0] != 's' && file[0] != 'S')) return -1;
  char *end;
  const long n = strtol(file + 1, &end, 10);
  return end != file + 1 && !strcasecmp(end, ".snp") ? (int)n : -1;
}

} // namespace

// ---- generations ---------------------------------------------------------------------------------

extern "C" void pcsnap_touch(const char *path) {
  // Small enough to rewrite whole: one line per disk ever written ("7 pc-386/dos.img").
  const std::string rel = relOf(path);
  const std::string all = readAll(genFile());
  std::string out;
  uint32_t gen = 0;
  size_t at = 0;
  while (at < all.size()) {
    size_t nl = all.find('\n', at);
    if (nl == std::string::npos) nl = all.size();
    std::string line = all.substr(at, nl - at);
    at = nl + 1;
    while (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    if (!strcasecmp(line.c_str() + sp + 1, rel.c_str())) gen = (uint32_t)strtoul(line.c_str(), nullptr, 10);
    else out += line + "\n";
  }
  out += std::to_string(gen + 1) + " " + rel + "\n";
  makeDirs(sdPath("pc-386/snapshots"));
  FILE *f = fopen(genFile().c_str(), "wb");
  if (!f) return;
  fputs(out.c_str(), f);
  fclose(f);
}

// ---- the list ------------------------------------------------------------------------------------

std::vector<PcSnapshot> pcsnap_list(const std::string &ini) {
  std::vector<PcSnapshot> out;
  const std::string dir = snapDir(ini);
  DIR *d = opendir(dir.c_str());
  if (!d) return out;
  std::vector<std::pair<int, std::string>> files;
  while (dirent *e = readdir(d))
    if (numberOf(e->d_name) >= 0) files.push_back({ numberOf(e->d_name), e->d_name });
  closedir(d);
  std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first > b.first; });   // newest first
  for (const auto &fn : files) {
    const std::string path = dir + "/" + fn.second;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) continue;
    Header h;
    if (readHeader(f, h, true)) out.push_back({ path, h.name });
    fclose(f);
  }
  return out;
}

// ---- saving ----------------------------------------------------------------------------------------

const char *pcsnap_save(const std::string &ini, const std::string &name) {
  if (pfolder_dirty()) return "A folder disk has unsaved changes";
  const std::string dir = snapDir(ini);
  if (!makeDirs(dir)) return "Cannot make the snapshot folder";
  int next = 1;
  for (const PcSnapshot &s : pcsnap_list(ini)) {
    const int n = numberOf(s.path.c_str() + s.path.find_last_of('/') + 1);
    if (n >= next) next = n + 1;
  }
  const std::string path = dir + "/s" + std::to_string(next) + ".snp";
  t386_flush_files();                  // the disks as the snapshot knows them
  t386_files_snapped();                // so the next write to one bumps its generation
  std::vector<DiskRec> disks;
  for (int i = 0; i < 16; i++)
    if (const char *p = t386_write_file(i)) disks.push_back({ relOf(p), 0 });
  for (int i = 0; const char *p = pfolder_root(i); i++) disks.push_back({ relOf(p), 0 });
  for (DiskRec &d : disks) d.gen = genOf(d.rel);

  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return "Cannot write the snapshot";
  const uint32_t n = (uint32_t)disks.size();
  bool ok = put(f, kMagic, 8) && putStr(f, name) && putStr(f, readAll(ini)) && put(f, &n, 4);
  for (const DiskRec &d : disks) ok = ok && putStr(f, d.rel) && put(f, &d.gen, 4);
  ok = ok && t386_save_state(f) == 0;
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    remove(path.c_str());
    return "Cannot write the snapshot (card full?)";
  }
  hal_log("snapshot: %s saved as %s", name.c_str(), path.c_str());
  return nullptr;
}

// ---- restoring -------------------------------------------------------------------------------------

bool pcsnap_disks_changed(const std::string &snap) {
  FILE *f = fopen(snap.c_str(), "rb");
  if (!f) return false;
  Header h;
  const bool ok = readHeader(f, h, false);
  fclose(f);
  if (!ok) return false;
  for (const DiskRec &d : h.disks)
    if (genOf(d.rel) != d.gen) return true;
  return false;
}

void pcsnap_set_pending(const std::string &snap) { s_pending = snap; }

std::string pcsnap_take_pending() {
  std::string s;
  s.swap(s_pending);
  return s;
}

bool pcsnap_write_ini(const std::string &snap, const std::string &ini) {
  FILE *f = fopen(snap.c_str(), "rb");
  if (!f) return false;
  Header h;
  const bool ok = readHeader(f, h, false);
  fclose(f);
  if (!ok || h.ini.empty()) return false;
  const std::string name = pcini_get_in(ini, "pico", "name");   // a startup renamed since keeps its name
  FILE *o = fopen(ini.c_str(), "wb");
  if (!o) return false;
  const bool wrote = put(o, h.ini.data(), h.ini.size());
  if (fclose(o) != 0 || !wrote) return false;
  if (pcini_get_in(ini, "pico", "name") != name) pcini_set_in(ini, "pico", "name", name);
  return true;
}

const char *pcsnap_restore(const std::string &snap) {
  FILE *f = fopen(snap.c_str(), "rb");
  if (!f) return "Cannot open the snapshot";
  Header h;
  int r = readHeader(f, h, false) ? t386_load_state(f) : T386_SNAP_FORMAT;
  fclose(f);
  if (r == T386_SNAP_FORMAT) return "Snapshot from another version";
  if (r != 0) return "Cannot read the snapshot";
  hal_log("snapshot: %s restored", snap.c_str());
  return nullptr;
}

// ---- the list screen ---------------------------------------------------------------------------

bool pcsnap_pick(const std::string &ini, const char *title, std::string &out) {
  int sel = 0;
  for (;;) {
    const std::vector<PcSnapshot> snaps = pcsnap_list(ini);
    if (snaps.empty()) {
      ui::message(title, "No snapshots yet: save one from the menu (Ctrl+Esc)", 2200);
      return false;
    }
    std::vector<ui::Item> items(snaps.size());
    for (size_t i = 0; i < snaps.size(); i++) items[i] = { snaps[i].name.c_str(), nullptr, ui::ICON_LOAD };
    ui::Screen scr;
    scr.title = title;
    scr.subtitle = "Start from where a snapshot stopped";
    scr.hints = "Enter|Select;Esc|Back";
    if (sel >= (int)snaps.size()) sel = (int)snaps.size() - 1;
    if (ui::list(scr, items.data(), (int)items.size(), &sel) != KEY_ENTER) return false;

    const PcSnapshot &s = snaps[sel];
    const bool changed = pcsnap_disks_changed(s.path);
    ui::Item acts[3] = {
      { "Start from it", nullptr, ui::ICON_PLAY,
        changed ? "A disk was written since: DOS may damage it" : "The drives it had come back too" },
      { "Delete", nullptr, ui::ICON_EXIT },
      { "Back", nullptr, ui::ICON_SKIP },
    };
    ui::Screen as;
    as.title = s.name.c_str();
    as.subtitle = title;
    as.hints = "Enter|Select;Esc|Back";
    int a = 0;
    if (ui::list(as, acts, 3, &a) != KEY_ENTER || a == 2) continue;
    if (a == 1) {
      if (remove(s.path.c_str()) != 0) ui::message("Error", "Cannot delete the snapshot", 1500);
      continue;
    }
    out = s.path;
    return true;
  }
}
