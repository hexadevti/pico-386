// browser.cpp -- picks a ROM: folders first, then files with one of the core's extensions.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <dirent.h>
#include <algorithm>
#include <string>
#include <vector>
#include "browser.h"
#include "settings.h"
#include "ui.h"
#include "../hal/hal.h"

static bool extMatches(const char *name, const char *exts) {
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name) return false;
  const size_t n = strlen(dot);
  for (const char *p = exts; (p = strchr(p, '.')) != nullptr; p++) {
    const char *end = strchr(p + 1, '.');
    const size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && !strncasecmp(p, dot, n)) return true;
  }
  return false;
}

struct Entry {
  std::string name;
  bool dir;
  std::string tag;
};

// Whether a directory entry is a folder: the entry's own type when the system gives one (the
// P4's FAT does), else stat. On a FAT card each stat searches the folder from its start, so a
// stat per file made a folder of 500 games take many seconds to open.
static bool entryIsDir(const std::string &dir, const dirent *e) {
#if defined(DT_DIR) && defined(DT_UNKNOWN)
  if (e->d_type == DT_DIR) return true;
  if (e->d_type != DT_UNKNOWN) return false;
#endif
  return isDir(dir + "/" + e->d_name);
}

// The rows of one folder, with the tags (BrowseOpts::tag) filled in as rows come on screen.
struct Rows {
  const std::string *dir;
  const BrowseOpts *opts;
  std::vector<Entry> *entries;
  std::vector<std::string> *tags;
  std::vector<ui::Item> *items;
  std::vector<bool> tagged;
};

static void prepareRows(void *ctx, int first, int count) {
  Rows &r = *(Rows *)ctx;
  for (int i = first; i < first + count && i < (int)r.entries->size(); i++) {
    const Entry &e = (*r.entries)[i];
    if (r.tagged[i] || e.dir || e.name.empty()) continue;
    r.tagged[i] = true;
    const std::string t = r.opts->tag(*r.dir + "/" + e.name);
    if (t.empty()) continue;
    (*r.tags)[i] = t;
    (*r.items)[i].value = (*r.tags)[i].c_str();
  }
}

static std::string parentOf(const std::string &p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos || s == 0 ? p : p.substr(0, s);
}

bool browseRom(const EmuCore &core, std::string &outPath, const BrowseOpts *opts) {
  const std::string root = hal_sd_root();
  const bool ownDir = opts && opts->startDir;
  const bool pickDir = opts && opts->pickDir;
  std::string dir = ownDir ? opts->startDir : g_settings.lastDir;
  if (dir.empty() || dir.compare(0, root.size(), root) || !isDir(dir)) {
    dir = sdPath("pc-386");                      // the disk images live with the BIOS
    if (!isDir(dir)) dir = root;
  }
  std::string selectName;

  for (;;) {
    std::vector<Entry> entries;
    if (DIR *d = opendir(dir.c_str())) {
      // a big folder on the card takes a moment: after a short while, say how far it got
      const uint32_t t0 = hal_millis();
      uint32_t shown = t0;
      int seen = 0;
      while (dirent *e = readdir(d)) {
        if (hal_millis() - shown > 250) {
          shown = hal_millis();
          const std::string line = std::to_string(seen) + " files and folders so far";
          ui::message("Reading the folder", line.c_str());
        }
        seen++;
        if (e->d_name[0] == '.') continue;
        const bool isD = entryIsDir(dir, e);
        if (isD) { entries.push_back({ e->d_name, true, "" }); continue; }
        if (pickDir) continue;
        if (!extMatches(e->d_name, opts && opts->exts ? opts->exts : core.romExts)) continue;
        std::string tag;
        if (opts && opts->accept && !opts->accept(dir + "/" + e->d_name, tag)) continue;
        entries.push_back({ e->d_name, false, tag });
      }
      closedir(d);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
      if (a.dir != b.dir) return a.dir;
      return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    const bool atRoot = dir == root;
    if (!atRoot) entries.insert(entries.begin(), { "..", true });
    if (!atRoot && pickDir) entries.insert(entries.begin(), { "", true });   // "Use this folder"

    // files show without their extension, which goes in the value column as a tag
    std::vector<std::string> labels(entries.size()), tags(entries.size());
    std::vector<ui::Item> items(entries.size());
    int sel = !atRoot && !pickDir && entries.size() > 1 ? 1 : 0;   // the first entry, not "Parent folder"
    for (size_t i = 0; i < entries.size(); i++) {
      const Entry &e = entries[i];
      if (e.name.empty()) {
        items[i] = { "Use this folder", nullptr, ui::ICON_PLAY };
      } else if (e.name == "..") {
        labels[i] = "Parent folder";
        items[i] = { labels[i].c_str(), nullptr, ui::ICON_UP };
      } else if (e.dir) {
        labels[i] = e.name;
        items[i] = { labels[i].c_str(), nullptr, ui::ICON_FOLDER };
      } else {
        const size_t dot = e.name.find_last_of('.');
        labels[i] = e.name.substr(0, dot);
        if (!e.tag.empty()) tags[i] = e.tag;
        else for (size_t c = dot + 1; c < e.name.size(); c++) tags[i] += (char)toupper((unsigned char)e.name[c]);
        items[i] = { labels[i].c_str(), tags[i].c_str(), ui::ICON_CART };
      }
      if (e.name == selectName) sel = (int)i;
    }
    selectName.clear();

    const std::string shown = dir.size() > root.size() ? dir.substr(root.size()) : std::string("/");
    if (entries.empty()) items.push_back({ "Nothing to choose in this folder", nullptr, ui::ICON_NONE });

    Rows rows{ &dir, opts, &entries, &tags, &items, std::vector<bool>(entries.size()) };
    ui::Screen scr;
    if (opts && opts->tag) {
      scr.prepare = prepareRows;
      scr.ctx = &rows;
    }
    scr.title = opts && opts->title ? opts->title : core.name;
    scr.subtitle = shown.c_str();
    scr.hints = atRoot ? "Enter|Open;Esc|Back" : "Enter|Open;Bs|Up;Esc|Back";
    const uint8_t k = ui::list(scr, items.data(), (int)items.size(), &sel);
    if (k == KEY_ESC || ((k == KEY_BACKSPACE || k == KEY_LEFT) && atRoot)) return false;
    if (k == KEY_BACKSPACE || k == KEY_LEFT) {
      selectName = dir.substr(dir.find_last_of('/') + 1);
      dir = parentOf(dir);
      continue;
    }
    if (k != KEY_ENTER || entries.empty()) continue;
    const Entry &e = entries[sel];
    if (e.name.empty()) {
      outPath = dir;
      if (!ownDir) g_settings.lastDir = parentOf(dir);
      return true;
    }
    if (e.name == "..") {
      selectName = dir.substr(dir.find_last_of('/') + 1);
      dir = parentOf(dir);
    } else if (e.dir) {
      dir += "/" + e.name;
    } else {
      outPath = dir + "/" + e.name;
      if (!ownDir) g_settings.lastDir = dir;
      return true;
    }
  }
}
