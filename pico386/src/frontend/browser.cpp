// browser.cpp -- picks a ROM: folders first, then files with one of the core's extensions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <dirent.h>
#include <algorithm>
#include <string>
#include "browser.h"
#include "settings.h"
#include "ui.h"
#include "../hal/hal.h"
#include "../pc/pc_glue.h"   // pcmem_lend

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

// One folder's listing. On the RP2350 only about 10 KB of heap is free while a PC runs, and the
// listing as vectors of strings and Items (about 150 bytes a file, more while a vector grew)
// aborted in a folder of 60 zips. So the entries are packed one after another in a pool (a flag
// byte, 1 for a folder, the length of the rest, then the label, and for a file its extension and
// the value column, each ending in a NUL), which is the PSRAM no machine uses on the RP2350
// (pcmem_spare) and the heap elsewhere. Only their offsets stay in SRAM, 4 bytes a file, in a
// table sized by a first pass over the folder and lent by the PC's pager when it can.
struct Entry {
  bool dir;
  const char *label, *ext, *value;
  char buf[2 + 256 + 32 + 8];
};

struct Listing {
  uint32_t *off = nullptr;   // the entries, sorted
  bool lent = false;
  int n = 0, cap = 0;
  long base = -1, room = 0;  // the pool in PSRAM, else in heap
  std::string heap;
  uint32_t used = 0;
  ~Listing() {
    if (off && !(lent && pcmem_free(off))) free(off);
  }
  bool alloc(int count) {
    const size_t bytes = (size_t)(count > 0 ? count : 1) * sizeof(uint32_t);
    lent = (off = (uint32_t *)pcmem_lend(bytes)) != nullptr;
    if (!off) off = (uint32_t *)malloc(bytes);
    cap = count;
    base = pcmem_spare(&room);
    return off != nullptr;
  }
  void add(bool dir, const char *label, size_t labelLen, const char *ext, const char *value) {
    char e[sizeof Entry::buf];
    const size_t x = ext ? strlen(ext) + 1 : 0, v = value ? strlen(value) + 1 : 0;
    const size_t len = labelLen + 1 + x + v;
    if (n == cap || len + 3 > sizeof e) return;               // the folder grew since; too long
    if (base >= 0 && used + 3 + len > (uint32_t)room) return;
    e[0] = dir;
    e[1] = (char)(len & 0xff);
    e[2] = (char)(len >> 8);
    char *p = e + 3;
    memcpy(p, label, labelLen);
    p[labelLen] = 0;
    p += labelLen + 1;
    if (ext) { memcpy(p, ext, x); p += x; }
    if (value) { memcpy(p, value, v); p += v; }
    off[n++] = used;
#if defined(PICO386_LOWMEM)
    if (base >= 0) hal_psram_write((uint32_t)(base + used), e, 3 + len);
    else
#endif
      heap.append(e, 3 + len);
    used += 3 + len;
  }
  // Entry i (by its offset): its strings point into e.buf.
  void get(uint32_t at, Entry &e) const {
#if defined(PICO386_LOWMEM)
    if (base >= 0) {
      hal_psram_read((uint32_t)(base + at), e.buf, 3);
      const size_t len = (uint8_t)e.buf[1] | (uint8_t)e.buf[2] << 8;
      hal_psram_read((uint32_t)(base + at + 3), e.buf + 3, len);
    } else
#endif
    {
      const size_t len = (uint8_t)heap[at + 1] | (uint8_t)heap[at + 2] << 8;
      memcpy(e.buf, heap.data() + at, 3 + len);
    }
    e.dir = e.buf[0] != 0;
    e.label = e.buf + 3;
    e.ext = e.label + strlen(e.label) + 1;
    e.value = e.ext + strlen(e.ext) + 1;
  }
  std::string name(int i) const {
    Entry e;
    get(off[i], e);
    return e.dir ? std::string(e.label) : std::string(e.label) + "." + e.ext;
  }
};

// The rows on screen: "Use this folder" and "Parent folder" first when they are there, then the
// listing. The tags (BrowseOpts::tag) of the rows shown last are kept in a small ring.
struct Rows {
  const std::string *dir;
  const BrowseOpts *opts;
  const Listing *list;
  int head;                  // rows before the listing
  bool useRow;               // the first row is "Use this folder"
  struct Tag { int row = -1; std::string text; } tags[32];
  int next = 0;
  Entry row[2];              // ui::list uses each row before it asks for the next
  int flip = 0;
  const Tag *tagOf(int row) const {
    for (const Tag &t : tags) if (t.row == row) return &t;
    return nullptr;
  }
};

static void prepareRows(void *ctx, int first, int count) {
  Rows &r = *(Rows *)ctx;
  for (int i = first; i < first + count; i++) {
    const int e = i - r.head;
    if (e < 0 || e >= r.list->n || r.tagOf(i)) continue;
    Rows::Tag &t = r.tags[r.next];
    r.next = (r.next + 1) % (int)(sizeof r.tags / sizeof r.tags[0]);
    t.row = i;
    t.text.clear();
    Entry &x = r.row[0];
    r.list->get(r.list->off[e], x);
    if (!x.dir) t.text = r.opts->tag(*r.dir + "/" + x.label + "." + x.ext);
  }
}

static ui::Item rowAt(void *ctx, int i) {
  Rows &r = *(Rows *)ctx;
  if (r.list->n == 0 && i == r.head) return { "Nothing to choose in this folder", nullptr, ui::ICON_NONE };
  if (i < r.head) {
    if (i == 0 && r.useRow) return { "Use this folder", nullptr, ui::ICON_PLAY };
    return { "Parent folder", nullptr, ui::ICON_UP };
  }
  Entry &e = r.row[r.flip ^= 1];
  r.list->get(r.list->off[i - r.head], e);
  if (e.dir) return { e.label, nullptr, ui::ICON_FOLDER };
  const Rows::Tag *t = r.tagOf(i);
  return { e.label, t && !t->text.empty() ? t->text.c_str() : e.value, ui::ICON_CART };
}

static std::string parentOf(const std::string &p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos || s == 0 ? p : p.substr(0, s);
}

// Reads one folder into l: its folders, and the files the options take. false when it doesn't fit.
static bool readFolder(const std::string &dir, const EmuCore &core, const BrowseOpts *opts, Listing &l) {
  const bool pickDir = opts && opts->pickDir;
  const char *exts = opts && opts->exts ? opts->exts : core.romExts;
  // a big folder on the card takes a moment: after a short while, say how far it got
  uint32_t shown = hal_millis();
  int seen = 0;
  auto progress = [&]() {
    seen++;
    if (hal_millis() - shown <= 250) return;
    shown = hal_millis();
    const std::string line = std::to_string(seen) + " files and folders so far";
    ui::message("Reading the folder", line.c_str());
  };
  // the first pass only counts what may be listed
  int count = 0;
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d)) {
      progress();
      if (e->d_name[0] == '.') continue;
      if ((!pickDir && extMatches(e->d_name, exts)) || entryIsDir(dir, e)) count++;
    }
    closedir(d);
  }
  if (!l.alloc(count)) return false;
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d)) {
      progress();
      if (e->d_name[0] == '.') continue;
      const size_t len = strlen(e->d_name);
      if (entryIsDir(dir, e)) { l.add(true, e->d_name, len, nullptr, nullptr); continue; }
      if (pickDir || !extMatches(e->d_name, exts)) continue;
      // files show without their extension, which goes in the value column as a tag
      const char *dot = strrchr(e->d_name, '.');
      std::string tag;
      if (opts && opts->accept && !opts->accept(dir + "/" + e->d_name, tag)) continue;
      if (tag.size() > 31) tag.resize(31);
      if (tag.empty()) for (const char *c = dot + 1; *c; c++) tag += (char)toupper((unsigned char)*c);
      l.add(false, e->d_name, dot - e->d_name, dot + 1, tag.c_str());
    }
    closedir(d);
  }
  // In PSRAM each comparison reads both entries: a folder of 500 takes a fraction of a second.
  Entry a, b;
  std::sort(l.off, l.off + l.n, [&](uint32_t x, uint32_t y) {
    l.get(x, a);
    l.get(y, b);
    if (a.dir != b.dir) return a.dir;                      // folders first
    const int c = strcasecmp(a.label, b.label);
    return c ? c < 0 : strcasecmp(a.ext, b.ext) < 0;
  });
  return true;
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
    Listing list;
    if (!readFolder(dir, core, opts, list)) {
      ui::message("Too many files", "This folder doesn't fit in memory", 2000);
      if (dir == root) return false;
      selectName = dir.substr(dir.find_last_of('/') + 1);
      dir = parentOf(dir);
      continue;
    }
    const bool atRoot = dir == root;
    Rows rows;
    rows.dir = &dir;
    rows.opts = opts;
    rows.list = &list;
    rows.useRow = !atRoot && pickDir;
    rows.head = atRoot ? 0 : pickDir ? 2 : 1;
    int sel = !atRoot && !pickDir && list.n > 0 ? 1 : 0;   // the first entry, not "Parent folder"
    for (int i = 0; i < list.n && !selectName.empty(); i++)
      if (selectName == list.name(i)) { sel = rows.head + i; break; }
    selectName.clear();
    const int n = rows.head + (list.n > 0 ? list.n : 1);

    const std::string shown = dir.size() > root.size() ? dir.substr(root.size()) : std::string("/");
    ui::Screen scr;
    scr.itemAt = rowAt;
    scr.ctx = &rows;
    if (opts && opts->tag) scr.prepare = prepareRows;
    scr.title = opts && opts->title ? opts->title : core.name;
    scr.subtitle = shown.c_str();
    scr.hints = atRoot ? "Enter|Open;Esc|Back" : "Enter|Open;Bs|Up;Esc|Back";
    const uint8_t k = ui::list(scr, nullptr, n, &sel);
    if (k == KEY_ESC || ((k == KEY_BACKSPACE || k == KEY_LEFT) && atRoot)) return false;
    if (k == KEY_BACKSPACE || k == KEY_LEFT) {
      selectName = dir.substr(dir.find_last_of('/') + 1);
      dir = parentOf(dir);
      continue;
    }
    if (k != KEY_ENTER) continue;
    if (sel < rows.head) {
      if (sel == 0 && rows.useRow) {
        outPath = dir;
        if (!ownDir) g_settings.lastDir = parentOf(dir);
        return true;
      }
      selectName = dir.substr(dir.find_last_of('/') + 1);
      dir = parentOf(dir);
      continue;
    }
    const int i = sel - rows.head;
    if (i >= list.n) continue;                             // "Nothing to choose"
    Entry e;
    list.get(list.off[i], e);
    if (e.dir) {
      dir += "/" + std::string(e.label);
    } else {
      outPath = dir + "/" + list.name(i);
      if (!ownDir) g_settings.lastDir = dir;
      return true;
    }
  }
}
