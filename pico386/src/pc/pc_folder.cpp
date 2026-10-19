// pc_folder.cpp -- a folder on the SD card as a guest hard disk (the D: drive).
//
// The guest sees a FAT16 disk that exists only here: at power-on the folder is scanned, and the
// boot sector, FAT and directories are built in RAM. A file's clusters are read straight from the
// file on the card, so even a big folder costs little memory and opens quickly.
//
// While the PC runs, nothing it writes reaches the folder:
// - FAT and directory changes stay in RAM.
// - Written data clusters go to a scratch file, /pc-386/.pico386-d.tmp.
// When the machine powers off, the final FAT16 tree is read back through the same view and
// compared with the scan. New and changed files are written, deleted ones are removed, and
// renamed or moved ones are renamed. If the emulator crashes or the battery is pulled first, the
// folder is left as it was and the session's changes are lost.
//
// Long names survive:
// - The scan writes VFAT long-name entries. DOS skips them and Windows 95 shows them.
// - At power-off, each file is matched to what it was by its first cluster, or else by its
//   short name in the same folder. So README.TXT edited in DOS is saved back to readme.txt.
//
// Safety: if the guest rewrote the partition table or the boot sector (FDISK, FORMAT), nothing is
// applied, so a FORMAT D: cannot wipe the folder.
//
// The disk is the smallest of 64 MB to 2000 MB that holds the files with at least 32 MB free.
// Up to 504 MB it is an ordinary FAT16 partition for any DOS. Larger, it is type 0Eh (FAT16 LBA),
// for FreeDOS and Windows 95. Files that do not fit are left out, and so are dot-files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "pc_folder.h"
#include "../frontend/settings.h"
#include "../hal/hal.h"

namespace {

constexpr uint32_t kSpt = 63, kPartStart = 63, kCyl = 16 * 63;
constexpr uint32_t kRsvd = 1, kNFats = 2, kRootEnt = 512, kRootSecs = kRootEnt * 32 / 512;
constexpr uint32_t kMaxClusters = 65524;            // FAT16's limit
constexpr int kMaxDepth = 24;
constexpr uint32_t kSizesMB[] = { 64, 128, 256, 504, 1000, 2000 };
constexpr uint32_t kMinFree = 32u << 20;

void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
uint32_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

// What the scan found: one per file or folder ([0] is the folder itself).
struct Node {
  std::string host;              // its path on the card
  std::string name;              // its name there
  uint8_t sfn[11];               // the 8.3 name the guest sees
  int lfn = 0;                   // long-name entries it has (0: the short name is its name)
  bool dir = false;
  bool skipped = false;          // left out: no room
  int parent = -1;
  uint32_t size = 0, first = 0, nclus = 0;
  uint16_t date = 0, time = 0;
  std::vector<int> kids;
  std::vector<uint8_t> dirBuf;   // a folder's clusters; the guest's writes to them land here
};

// The tree as the guest left it, read back at power-off.
struct Final {
  std::string name;
  uint8_t sfn[11];
  bool dir = false;
  uint32_t first = 0, size = 0;
  std::vector<uint32_t> chain;
  std::vector<Final> kids;
  int orig = -1;                 // the scanned node it is, or -1 when new
  bool changed = false;          // a file whose content must be written
  std::string stage;             // where that content waits until it is moved into place
};

} // namespace

struct PicoFolder {
  std::string root, label;
  std::vector<Node> nodes;
  std::vector<uint8_t> rootDir;
  std::vector<uint16_t> fat;                         // the first FAT; both copies read from it
  std::map<uint32_t, std::vector<uint8_t>> extra;    // written sectors outside FAT/root/data
  std::unordered_map<uint32_t, uint32_t> spill;      // cluster -> its slot in the scratch file
  std::vector<std::pair<uint32_t, int>> byFirst;     // (first cluster, node), ascending
  std::string spillPath;
  FILE *spillF = nullptr;
  FILE *cur = nullptr;                               // the last file read, kept open
  int curNode = -1;
  uint32_t total = 0, part = 0, spc = 0, cb = 0, fatSz = 0;
  uint32_t fat0 = 0, root0 = 0, data0 = 0, maxClus = 0;   // maxClus: one past the last cluster
  uint32_t serial = 0;
  uint8_t ptype = 0;
  bool dirty = false, spillFailed = false;
};

namespace {

std::vector<PicoFolder *> s_open;

// ---- names ----------------------------------------------------------------------------------------

bool sfnChar(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

std::string shortText(const uint8_t *sfn) {
  std::string s;
  for (int i = 0; i < 8 && sfn[i] != ' '; i++) s += (char)sfn[i];
  if (sfn[8] != ' ') {
    s += '.';
    for (int i = 8; i < 11 && sfn[i] != ' '; i++) s += (char)sfn[i];
  }
  return s;
}

// The 8.3 name for name, unique among used (11-char keys). Returns true when it is not the name
// itself, so the long name has to be stored too.
bool makeShort(const std::string &name, std::vector<std::string> &used, uint8_t out[11]) {
  const size_t dot = name.find_last_of('.');
  const std::string base = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot + 1);
  bool lossy = false;
  auto clean = [&lossy](const std::string &s, size_t max) {
    std::string r;
    for (unsigned char c : s) {
      if (c == ' ' || c == '.') { lossy = true; continue; }
      char u = (char)toupper(c);
      if (c >= 0x80 || !sfnChar((unsigned char)u)) { u = '_'; lossy = true; }
      if (r.size() < max) r += u; else lossy = true;
    }
    return r;
  };
  std::string b = clean(base, 8);
  const std::string e = clean(ext, 3);
  if (b.empty()) { b = "_"; lossy = true; }
  auto key = [](const std::string &bb, const std::string &ee) {
    return (bb + "        ").substr(0, 8) + (ee + "   ").substr(0, 3);
  };
  std::string k = key(b, e);
  const bool taken = std::find(used.begin(), used.end(), k) != used.end();
  if (lossy || taken) {
    for (int i = 1; i < 1000000; i++) {
      const std::string tail = "~" + std::to_string(i);
      k = key(b.substr(0, 8 - tail.size()) + tail, e);
      if (std::find(used.begin(), used.end(), k) == used.end()) break;
    }
  }
  used.push_back(k);
  memcpy(out, k.data(), 11);
  return name != shortText(out);
}

std::vector<uint16_t> toUtf16(const std::string &s) {
  std::vector<uint16_t> u;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    i += n;
    u.push_back(cp > 0xFFFF ? '_' : (uint16_t)cp);
  }
  return u;
}

std::string toUtf8(const std::vector<uint16_t> &u) {
  std::string s;
  for (uint16_t c : u) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
  }
  return s;
}

uint8_t sfnSum(const uint8_t *sfn) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
  return sum;
}

const int kLfnOff[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

// ---- the scan --------------------------------------------------------------------------------------

void dosTime(time_t t, uint16_t &date, uint16_t &tm) {
  const struct tm *l = localtime(&t);
  if (!l || l->tm_year < 80) { date = (1 << 5) | 1; tm = 0; return; }
  date = (uint16_t)(((l->tm_year - 80) << 9) | ((l->tm_mon + 1) << 5) | l->tm_mday);
  tm = (uint16_t)((l->tm_hour << 11) | (l->tm_min << 5) | (l->tm_sec / 2));
}

void scanDir(PicoFolder &f, int di, int depth) {
  DIR *d = opendir(f.nodes[di].host.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent *e = readdir(d))
    if (e->d_name[0] != '.') names.push_back(e->d_name);
  closedir(d);
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
  std::vector<std::string> used;
  for (const std::string &name : names) {
    Node n;
    n.host = f.nodes[di].host + "/" + name;
    struct stat st;
    if (stat(n.host.c_str(), &st) != 0) continue;
    n.dir = S_ISDIR(st.st_mode);
    if (!n.dir && !S_ISREG(st.st_mode)) continue;
    if (n.dir && depth >= kMaxDepth) continue;
    n.name = name;
    n.parent = di;
    n.size = n.dir ? 0 : (uint32_t)st.st_size;
    dosTime(st.st_mtime, n.date, n.time);
    if (makeShort(name, used, n.sfn)) {
      const size_t len = toUtf16(name).size();
      n.lfn = len <= 255 ? (int)((len + 12) / 13) : 0;
    }
    const int idx = (int)f.nodes.size();
    f.nodes.push_back(std::move(n));
    f.nodes[di].kids.push_back(idx);
    if (f.nodes[idx].dir) scanDir(f, idx, depth + 1);
  }
}

uint32_t dirEntries(const PicoFolder &f, const Node &d) {
  uint32_t n = d.parent < 0 ? 1 : 2;                 // the volume label, or "." and ".."
  for (int k : d.kids)
    if (!f.nodes[k].skipped) n += 1 + f.nodes[k].lfn;
  return n;
}

// The disk geometry for a size, and the clusters the scan needs at that cluster size.
bool geometry(PicoFolder &f, uint32_t mb) {
  f.total = mb * 2048 / kCyl * kCyl;
  f.part = f.total - kPartStart;
  for (f.spc = 1; f.spc <= 64; f.spc *= 2) {
    const uint32_t tmp2 = 256 * f.spc + kNFats;
    f.fatSz = (f.part - kRsvd - kRootSecs + tmp2 - 1) / tmp2;
    const uint32_t clusters = (f.part - kRsvd - kNFats * f.fatSz - kRootSecs) / f.spc;
    if (clusters <= kMaxClusters) {
      f.maxClus = clusters + 2;
      break;
    }
  }
  if (f.spc > 64) return false;
  f.cb = f.spc * 512;
  f.fat0 = kPartStart + kRsvd;
  f.root0 = f.fat0 + kNFats * f.fatSz;
  f.data0 = f.root0 + kRootSecs;
  f.ptype = f.part < 65536 ? 0x04 : f.total <= 1024 * kCyl ? 0x06 : 0x0E;
  return true;
}

uint32_t clustersNeeded(const PicoFolder &f, const Node &n) {
  if (n.dir) return (dirEntries(f, n) * 32 + f.cb - 1) / f.cb;
  return (uint32_t)(((uint64_t)n.size + f.cb - 1) / f.cb);
}

void putEntry(uint8_t *e, const uint8_t *sfn, uint8_t attr, uint32_t first, uint32_t size,
              uint16_t date, uint16_t tm) {
  memset(e, 0, 32);
  memcpy(e, sfn, 11);
  e[11] = attr;
  put16(e + 14, tm);
  put16(e + 16, date);
  put16(e + 18, date);
  put16(e + 22, tm);
  put16(e + 24, date);
  put16(e + 26, first);
  put32(e + 28, size);
}

// A folder's entries: long-name entries before each short one, as VFAT has them.
void fillDir(PicoFolder &f, const Node &d, uint8_t *p, uint32_t cap) {
  uint32_t i = 0;
  auto next = [&]() -> uint8_t * { return i < cap ? p + 32 * i++ : nullptr; };
  for (int k : d.kids) {
    const Node &n = f.nodes[k];
    if (n.skipped) continue;
    if (n.lfn) {
      const std::vector<uint16_t> u = toUtf16(n.name);
      const uint8_t sum = sfnSum(n.sfn);
      for (int s = n.lfn; s >= 1; s--) {
        uint8_t *e = next();
        if (!e) return;
        memset(e, 0, 32);
        e[0] = (uint8_t)(s | (s == n.lfn ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (int c = 0; c < 13; c++) {
          const size_t j = (size_t)(s - 1) * 13 + c;
          put16(e + kLfnOff[c], j < u.size() ? u[j] : j == u.size() ? 0 : 0xFFFF);
        }
      }
    }
    uint8_t *e = next();
    if (!e) return;
    putEntry(e, n.sfn, n.dir ? 0x10 : 0x20, n.first, n.size, n.date, n.time);
  }
}

int ownerOf(const PicoFolder &f, uint32_t c) {
  auto it = std::upper_bound(f.byFirst.begin(), f.byFirst.end(), std::make_pair(c, 0x7FFFFFFF));
  if (it == f.byFirst.begin()) return -1;
  --it;
  const Node &n = f.nodes[it->second];
  return c < n.first + n.nclus ? it->second : -1;
}

void closeCur(PicoFolder &f) {
  if (f.cur) fclose(f.cur);
  f.cur = nullptr;
  f.curNode = -1;
}

// ---- the disk ------------------------------------------------------------------------------------

void putChs(uint8_t *p, uint32_t lba) {
  if (lba >= 1024 * kCyl) { p[0] = 0xFE; p[1] = 0xFF; p[2] = 0xFF; return; }
  const uint32_t c = lba / kCyl, h = lba / kSpt % 16, s = lba % kSpt + 1;
  p[0] = (uint8_t)h;
  p[1] = (uint8_t)(s | ((c >> 2) & 0xC0));
  p[2] = (uint8_t)c;
}

void mbrSector(const PicoFolder &f, uint8_t *s) {
  s[0] = 0xCD; s[1] = 0x18;                          // not bootable: INT 18h
  put32(s + 0x1B8, f.serial);
  uint8_t *pe = s + 0x1BE;
  putChs(pe + 1, kPartStart);
  pe[4] = f.ptype;
  putChs(pe + 5, f.total - 1);
  put32(pe + 8, kPartStart);
  put32(pe + 12, f.part);
  s[510] = 0x55; s[511] = 0xAA;
}

void bootSector(const PicoFolder &f, uint8_t *s) {
  s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
  memcpy(s + 3, "PICO386 ", 8);
  put16(s + 11, 512);
  s[13] = (uint8_t)f.spc;
  put16(s + 14, kRsvd);
  s[16] = kNFats;
  put16(s + 17, kRootEnt);
  put16(s + 19, f.part < 65536 ? f.part : 0);
  s[21] = 0xF8;
  put16(s + 22, f.fatSz);
  put16(s + 24, kSpt);
  put16(s + 26, f.total <= 1024 * kCyl ? 16 : 255);
  put32(s + 28, kPartStart);
  put32(s + 32, f.part < 65536 ? 0 : f.part);
  s[36] = 0x81;
  s[38] = 0x29;
  put32(s + 39, f.serial);
  memcpy(s + 43, f.label.data(), 11);
  memcpy(s + 54, "FAT16   ", 8);
  s[62] = 0xCD; s[63] = 0x18;
  s[510] = 0x55; s[511] = 0xAA;
}

// len bytes of cluster c from off: the scratch copy, a folder's RAM, the file, or zeros.
void readCluster(PicoFolder &f, uint32_t c, uint32_t off, uint8_t *buf, uint32_t len) {
  auto sp = f.spill.find(c);
  if (sp != f.spill.end()) {
    fseek(f.spillF, (long)((uint64_t)sp->second * f.cb + off), SEEK_SET);
    if (fread(buf, 1, len, f.spillF) != len) memset(buf, 0, len);
    return;
  }
  memset(buf, 0, len);
  const int o = ownerOf(f, c);
  if (o < 0) return;
  const Node &n = f.nodes[o];
  const uint64_t at = (uint64_t)(c - n.first) * f.cb + off;
  if (n.dir) {
    memcpy(buf, n.dirBuf.data() + at, len);
    return;
  }
  if (at >= n.size) return;
  if (f.curNode != o) {
    closeCur(f);
    f.cur = fopen(n.host.c_str(), "rb");
    if (!f.cur) return;
    f.curNode = o;
  }
  const uint32_t want = (uint32_t)std::min<uint64_t>(len, n.size - at);
  fseek(f.cur, (long)at, SEEK_SET);
  fread(buf, 1, want, f.cur);
}

void readSector(PicoFolder &f, uint32_t s, uint8_t *buf) {
  auto it = f.extra.find(s);
  if (it != f.extra.end()) { memcpy(buf, it->second.data(), 512); return; }
  memset(buf, 0, 512);
  if (s == 0) { mbrSector(f, buf); return; }
  if (s == kPartStart) { bootSector(f, buf); return; }
  if (s >= f.fat0 && s < f.root0) {
    memcpy(buf, (const uint8_t *)f.fat.data() + (size_t)((s - f.fat0) % f.fatSz) * 512, 512);
    return;
  }
  if (s >= f.root0 && s < f.data0) { memcpy(buf, f.rootDir.data() + (size_t)(s - f.root0) * 512, 512); return; }
  if (s >= f.data0) {
    const uint32_t rel = s - f.data0, c = 2 + rel / f.spc;
    if (c < f.maxClus) readCluster(f, c, rel % f.spc * 512, buf, 512);
  }
}

void writeSector(PicoFolder &f, uint32_t s, const uint8_t *buf) {
  f.dirty = true;
  if (s >= f.fat0 && s < f.root0) {
    // the second FAT is a copy DOS keeps in step; the first one is the one that counts
    if (s < f.fat0 + f.fatSz) memcpy((uint8_t *)f.fat.data() + (size_t)(s - f.fat0) * 512, buf, 512);
    return;
  }
  if (s >= f.root0 && s < f.data0) { memcpy(f.rootDir.data() + (size_t)(s - f.root0) * 512, buf, 512); return; }
  if (s >= f.data0) {
    const uint32_t rel = s - f.data0, c = 2 + rel / f.spc, off = rel % f.spc * 512;
    if (c >= f.maxClus) return;
    auto sp = f.spill.find(c);
    if (sp == f.spill.end()) {
      const int o = ownerOf(f, c);
      if (o >= 0 && f.nodes[o].dir) {
        Node &n = f.nodes[o];
        memcpy(n.dirBuf.data() + (size_t)(c - n.first) * f.cb + off, buf, 512);
        return;
      }
      // the first write to a cluster: its scratch copy starts as what the cluster holds now
      if (!f.spillF) f.spillF = fopen(f.spillPath.c_str(), "w+b");
      if (!f.spillF) {
        if (!f.spillFailed) hal_log("folder: cannot create %s, writes are lost", f.spillPath.c_str());
        f.spillFailed = true;
        return;
      }
      std::vector<uint8_t> tmp(f.cb);
      readCluster(f, c, 0, tmp.data(), f.cb);
      const uint32_t slot = (uint32_t)f.spill.size();
      fseek(f.spillF, (long)((uint64_t)slot * f.cb), SEEK_SET);
      fwrite(tmp.data(), 1, f.cb, f.spillF);
      sp = f.spill.emplace(c, slot).first;
    }
    fseek(f.spillF, (long)((uint64_t)sp->second * f.cb + off), SEEK_SET);
    fwrite(buf, 1, 512, f.spillF);
    return;
  }
  f.extra[s].assign(buf, buf + 512);
}

// ---- power-off: the guest's tree back onto the card ----------------------------------------------

std::vector<uint32_t> chainOf(const PicoFolder &f, uint32_t c) {
  std::vector<uint32_t> chain;
  while (c >= 2 && c < f.maxClus && chain.size() < f.maxClus) {
    chain.push_back(c);
    const uint32_t next = f.fat[c];
    if (next >= 0xFFF8) break;
    c = next;
  }
  return chain;
}

bool goodName(const std::string &n) {
  if (n.empty() || n == "." || n == "..") return false;
  for (unsigned char c : n)
    if (c < 0x20 || strchr("/\\:*?\"<>|", c)) return false;
  return true;
}

void parseEntries(const uint8_t *p, size_t count, std::vector<Final> &out) {
  std::vector<uint16_t> lname;
  int lnext = 0;
  uint8_t lsum = 0;
  bool lvalid = false;
  for (size_t i = 0; i < count; i++) {
    const uint8_t *e = p + 32 * i;
    if (e[0] == 0) break;
    if (e[0] == 0xE5) { lvalid = false; continue; }
    if (e[11] == 0x0F) {
      const int seq = e[0] & 0x1F;
      if (e[0] & 0x40) {
        lvalid = seq >= 1 && seq <= 20;
        lnext = seq;
        lsum = e[13];
        lname.assign((size_t)seq * 13, 0);
      }
      if (!lvalid || seq != lnext || e[13] != lsum) { lvalid = false; continue; }
      for (int c = 0; c < 13; c++) lname[(size_t)(seq - 1) * 13 + c] = (uint16_t)get16(e + kLfnOff[c]);
      lnext--;
      continue;
    }
    const bool hadLong = lvalid && lnext == 0 && sfnSum(e) == lsum;
    lvalid = false;
    if (e[11] & 0x08 || e[0] == '.') continue;      // the volume label, "." and ".."
    Final fn;
    memcpy(fn.sfn, e, 11);
    if (fn.sfn[0] == 0x05) fn.sfn[0] = 0xE5;
    if (hadLong) {
      std::vector<uint16_t> u;
      for (uint16_t c : lname) { if (!c) break; u.push_back(c); }
      fn.name = toUtf8(u);
    }
    if (!goodName(fn.name)) fn.name = shortText(fn.sfn);
    if (!goodName(fn.name)) continue;
    fn.dir = (e[11] & 0x10) != 0;
    fn.first = get16(e + 26);
    fn.size = fn.dir ? 0 : get32(e + 28);
    out.push_back(std::move(fn));
  }
}

void parseTree(PicoFolder &f, Final &d, const std::vector<uint8_t> &bytes, int depth,
               std::vector<bool> &seenDir) {
  parseEntries(bytes.data(), bytes.size() / 32, d.kids);
  for (Final &k : d.kids) {
    k.chain = chainOf(f, k.first);
    if (!k.dir || depth >= kMaxDepth || k.chain.empty() || seenDir[k.chain[0]]) continue;
    seenDir[k.chain[0]] = true;
    std::vector<uint8_t> sub(k.chain.size() * f.cb);
    for (size_t i = 0; i < k.chain.size(); i++) readCluster(f, k.chain[i], 0, sub.data() + i * f.cb, f.cb);
    parseTree(f, k, sub, depth + 1, seenDir);
  }
}

// First pass: a file or folder whose first cluster is still the one the scan gave it is that
// node, wherever it is now (renamed, moved).
void matchByCluster(PicoFolder &f, Final &d, std::vector<bool> &claimed) {
  for (Final &k : d.kids) {
    if (k.first) {
      const int o = ownerOf(f, k.first);
      if (o > 0 && f.nodes[o].first == k.first && f.nodes[o].dir == k.dir && !claimed[o]) {
        k.orig = o;
        claimed[o] = true;
      }
    }
    if (k.dir) matchByCluster(f, k, claimed);
  }
}

// Second pass: a file saved anew (new clusters) is still the one with its short name there.
void matchByName(PicoFolder &f, Final &d, int dOrig, std::vector<bool> &claimed) {
  for (Final &k : d.kids) {
    if (k.orig < 0 && dOrig >= 0) {
      for (int n : f.nodes[dOrig].kids) {
        const Node &o = f.nodes[n];
        if (!claimed[n] && !o.skipped && o.dir == k.dir && !memcmp(o.sfn, k.sfn, 11)) {
          k.orig = n;
          claimed[n] = true;
          break;
        }
      }
    }
    // the host name is kept unless the guest gave it another one
    if (k.orig >= 0 && !memcmp(f.nodes[k.orig].sfn, k.sfn, 11) && k.name == shortText(k.sfn))
      k.name = f.nodes[k.orig].name;
    if (!k.dir) {
      bool same = k.orig >= 0 && k.first == f.nodes[k.orig].first && k.size == f.nodes[k.orig].size &&
                  k.chain.size() == f.nodes[k.orig].nclus;
      for (size_t i = 0; same && i < k.chain.size(); i++)
        same = k.chain[i] == k.first + i && !f.spill.count(k.chain[i]);
      k.changed = !same;
    }
    if (k.dir) matchByName(f, k, k.orig, claimed);
  }
}

int s_stageSeq;

void stageFiles(PicoFolder &f, Final &d, const std::string &staging) {
  std::vector<uint8_t> buf(f.cb);
  for (Final &k : d.kids) {
    if (k.dir) { stageFiles(f, k, staging); continue; }
    if (!k.changed) continue;
    k.stage = staging + "/" + std::to_string(s_stageSeq++);
    FILE *o = fopen(k.stage.c_str(), "wb");
    if (!o) { hal_log("folder: cannot write %s", k.stage.c_str()); k.stage.clear(); continue; }
    uint32_t left = k.size;
    for (size_t i = 0; i < k.chain.size() && left; i++) {
      const uint32_t n = left < f.cb ? left : f.cb;
      readCluster(f, k.chain[i], 0, buf.data(), n);
      fwrite(buf.data(), 1, n, o);
      left -= n;
    }
    fclose(o);
  }
}

bool samePath(const std::string &a, const std::string &b) { return !strcasecmp(a.c_str(), b.c_str()); }

// Renames a folder and keeps every path below it up to date.
void moveTree(std::vector<std::string> &cur, const std::string &from, const std::string &to) {
  if (rename(from.c_str(), to.c_str()) != 0) {
    hal_log("folder: cannot move %s to %s", from.c_str(), to.c_str());
    return;
  }
  for (std::string &p : cur)
    if (p == from || (p.size() > from.size() && !p.compare(0, from.size(), from) && p[from.size()] == '/'))
      p = to + p.substr(from.size());
}

void applyTree(PicoFolder &f, const Final &d, const std::string &dpath, std::vector<std::string> &cur) {
  for (const Final &k : d.kids) {
    const std::string target = dpath + "/" + k.name;
    if (k.dir) {
      if (k.orig >= 0 && !samePath(cur[k.orig], target)) moveTree(cur, cur[k.orig], target);
      else if (k.orig < 0) makeDirs(target);
      applyTree(f, k, target, cur);
    } else if (!k.changed) {
      if (!samePath(cur[k.orig], target)) {
        if (rename(cur[k.orig].c_str(), target.c_str()) != 0)
          hal_log("folder: cannot move %s to %s", cur[k.orig].c_str(), target.c_str());
        else cur[k.orig] = target;
      }
    } else if (!k.stage.empty()) {
      if (k.orig >= 0) remove(cur[k.orig].c_str());
      if (rename(k.stage.c_str(), target.c_str()) != 0)
        hal_log("folder: cannot write %s (kept as %s)", target.c_str(), k.stage.c_str());
    }
  }
}

void commit(PicoFolder &f) {
  if (f.extra.count(0) || f.extra.count(kPartStart)) {
    hal_log("folder: %s was repartitioned or formatted; its changes are not applied", f.root.c_str());
    return;
  }
  Final top;
  std::vector<bool> seenDir(f.maxClus, false), claimed(f.nodes.size(), false);
  parseTree(f, top, f.rootDir, 0, seenDir);
  matchByCluster(f, top, claimed);
  matchByName(f, top, 0, claimed);

  // 1. New and changed files are written aside first, while every old file is still in place.
  const std::string staging = f.root + "/.pico386-commit";
  makeDirs(staging);
  stageFiles(f, top, staging);
  closeCur(f);

  // 2. Files the guest deleted go.
  int removed = 0;
  for (size_t i = 1; i < f.nodes.size(); i++)
    if (!claimed[i] && !f.nodes[i].skipped && !f.nodes[i].dir && remove(f.nodes[i].host.c_str()) == 0) removed++;

  // 3. Folders made or moved, files moved or put in place.
  std::vector<std::string> cur(f.nodes.size());
  for (size_t i = 0; i < f.nodes.size(); i++) cur[i] = f.nodes[i].host;
  applyTree(f, top, f.root, cur);

  // 4. Folders the guest deleted, deepest first (one that still holds files left out stays).
  std::vector<int> gone;
  for (size_t i = 1; i < f.nodes.size(); i++)
    if (!claimed[i] && !f.nodes[i].skipped && f.nodes[i].dir) gone.push_back((int)i);
  std::sort(gone.begin(), gone.end(), [&cur](int a, int b) { return cur[a].size() > cur[b].size(); });
  for (int i : gone) rmdir(cur[i].c_str());
  rmdir(staging.c_str());
  hal_log("folder: %s updated (%d files removed)", f.root.c_str(), removed);
}

void freeFolder(PicoFolder *f) {
  closeCur(*f);
  if (f->spillF) fclose(f->spillF);
  if (!f->spillPath.empty()) remove(f->spillPath.c_str());
  delete f;
}

} // namespace

// ---- the C API ------------------------------------------------------------------------------------

extern "C" int pfolder_is_dir(const char *path) { return path && path[0] && isDir(path); }

extern "C" PicoFolder *pfolder_open(const char *dir) {
  PicoFolder *f = new (std::nothrow) PicoFolder;
  if (!f) return nullptr;
  f->root = dir;
  while (f->root.size() > 1 && f->root.back() == '/') f->root.pop_back();
  f->spillPath = sdPath(("pc-386/.pico386-d" + std::to_string(s_open.size()) + ".tmp").c_str());
  f->serial = hal_micros() ^ 0x50433836;

  // the volume label: the folder's name
  const std::string base = f->root.substr(f->root.find_last_of('/') + 1);
  f->label = "           ";
  for (size_t i = 0, j = 0; i < base.size() && j < 11; i++) {
    const char u = (char)toupper((unsigned char)base[i]);
    if (sfnChar((unsigned char)u) || u == ' ') f->label[j++] = u;
  }
  if (f->label == "           ") f->label = "SD FOLDER  ";

  Node top;
  top.host = f->root;
  top.dir = true;
  f->nodes.push_back(top);
  scanDir(*f, 0, 0);

  // the root directory has room for 511 entries next to the label
  uint32_t rootUsed = 1;
  for (int k : f->nodes[0].kids) {
    Node &n = f->nodes[k];
    if (rootUsed + 1 + n.lfn > kRootEnt) n.skipped = true;
    else rootUsed += 1 + n.lfn;
  }
  for (size_t i = 1; i < f->nodes.size(); i++)
    if (f->nodes[f->nodes[i].parent].skipped) f->nodes[i].skipped = true;

  // the smallest size with room to spare
  for (uint32_t mb : kSizesMB) {
    geometry(*f, mb);
    uint64_t need = 0;
    for (size_t i = 1; i < f->nodes.size(); i++)
      if (!f->nodes[i].skipped) need += clustersNeeded(*f, f->nodes[i]);
    if ((f->maxClus - 2 - need) * (uint64_t)f->cb >= kMinFree && need < f->maxClus - 2) break;
  }

  f->fat.assign((size_t)f->fatSz * 256, 0);
  f->fat[0] = 0xFFF8;
  f->fat[1] = 0xFFFF;
  uint32_t next = 2;
  for (size_t i = 1; i < f->nodes.size(); i++) {
    Node &n = f->nodes[i];
    if (f->nodes[n.parent].skipped) n.skipped = true;
    if (n.skipped) continue;
    const uint32_t c = clustersNeeded(*f, n);
    if (next + c > f->maxClus) {
      n.skipped = true;
      hal_log("folder: no room for %s", n.host.c_str());
      continue;
    }
    n.first = c ? next : 0;
    n.nclus = c;
    for (uint32_t j = 0; j < c; j++) f->fat[next + j] = (uint16_t)(j + 1 < c ? next + j + 1 : 0xFFFF);
    if (c) f->byFirst.push_back({ next, (int)i });
    next += c;
  }

  // the directories, now that every first cluster is known
  f->rootDir.assign(kRootEnt * 32, 0);
  uint8_t sfnLabel[11];
  memcpy(sfnLabel, f->label.data(), 11);
  putEntry(f->rootDir.data(), sfnLabel, 0x08, 0, 0, f->nodes.size() > 1 ? f->nodes[1].date : 0x21, 0);
  fillDir(*f, f->nodes[0], f->rootDir.data() + 32, kRootEnt - 1);
  for (size_t i = 1; i < f->nodes.size(); i++) {
    Node &n = f->nodes[i];
    if (!n.dir || n.skipped) continue;
    n.dirBuf.assign((size_t)n.nclus * f->cb, 0);
    static const uint8_t kDot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    static const uint8_t kDotDot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    putEntry(n.dirBuf.data(), kDot, 0x10, n.first, 0, n.date, n.time);
    putEntry(n.dirBuf.data() + 32, kDotDot, 0x10, f->nodes[n.parent].first, 0, n.date, n.time);
    fillDir(*f, n, n.dirBuf.data() + 64, (uint32_t)(n.dirBuf.size() / 32 - 2));
  }

  size_t files = 0;
  uint64_t bytes = 0;
  for (const Node &n : f->nodes)
    if (!n.dir && !n.skipped) { files++; bytes += n.size; }
  // the desktop build quits through exit() (window closed, test scripts): save on the way out
  static bool hooked = false;
  if (!hooked) { atexit(pfolder_close_all); hooked = true; }
  hal_log("folder: %s as a %u MB disk, %u files (%u KB), %u-byte clusters",
          f->root.c_str(), (unsigned)(f->total / 2048), (unsigned)files, (unsigned)(bytes >> 10),
          (unsigned)f->cb);
  s_open.push_back(f);
  return f;
}

extern "C" int64_t pfolder_sectors(PicoFolder *f) { return f ? f->total : 0; }

extern "C" int pfolder_read(PicoFolder *f, uint64_t sector, uint8_t *buf, int n) {
  if (!f || sector + n > f->total) return -1;
  for (int i = 0; i < n; i++) readSector(*f, (uint32_t)(sector + i), buf + 512 * i);
  return 0;
}

extern "C" int pfolder_write(PicoFolder *f, uint64_t sector, const uint8_t *buf, int n) {
  if (!f || sector + n > f->total) return -1;
  for (int i = 0; i < n; i++) writeSector(*f, (uint32_t)(sector + i), buf + 512 * i);
  return 0;
}

extern "C" int pfolder_dirty(void) {
  for (PicoFolder *f : s_open)
    if (f->dirty) return 1;
  return 0;
}

extern "C" void pfolder_close_all(void) {
  for (PicoFolder *f : s_open) {
    if (f->dirty) commit(*f);
    freeFolder(f);
  }
  s_open.clear();
}
