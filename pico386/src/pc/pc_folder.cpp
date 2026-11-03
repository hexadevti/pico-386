// pc_folder.cpp -- a folder on the SD card as a guest disk: a hard disk (the D: drive), or a
// 1.44 MB floppy in A: or B:.
//
// The guest sees a FAT16 disk that exists only here: at power-on the folder is scanned, and the
// boot sector, FAT and directories are built in RAM. A file's clusters are read straight from the
// file on the card, so even a big folder costs little memory and opens quickly.
//
// While the PC runs, nothing it writes reaches the folder:
// - FAT and directory changes stay in RAM.
// - Written data clusters go to a scratch file, /pc-386/.pico386-dN.tmp.
// On the RP2350 the FAT, the directories and the scan's nodes live in the PSRAM (a hard disk's
// FAT alone is up to 128 KB, and the heap has about 10 KB with a PC running).
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
// The hard disk is the smallest of 64 MB to 2000 MB that holds the files with at least 32 MB free.
// Up to 504 MB it is an ordinary FAT16 partition for any DOS. Larger, it is type 0Eh (FAT16 LBA),
// for FreeDOS and Windows 95. Files that do not fit are left out, and so are dot-files.
//
// The floppy is a standard 1.44 MB FAT12 disk (no partition table), so the BIOS and DOS take it
// like any other: it can be swapped while DOS runs, as a floppy is meant to be. Each one gets a
// new volume serial, which is how DOS tells that the disk changed. A folder that does not fit is
// refused rather than left incomplete. It is written back when it is ejected or swapped.
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
#include <array>
#include <map>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>
#include "pc_folder.h"
#include "pc_glue.h"
#include "../frontend/settings.h"
#include "../hal/hal.h"

namespace {

constexpr uint32_t kSpt = 63, kPartStart = 63, kCyl = 16 * 63;
constexpr uint32_t kRsvd = 1, kNFats = 2, kRootEnt = 512;
constexpr uint32_t kFdSectors = 2880, kFdSpt = 18, kFdRootEnt = 224, kFdFatSz = 9;   // 1.44 MB
constexpr uint32_t kMaxClusters = 65524;            // FAT16's limit
constexpr int kMaxDepth = 24;
constexpr uint32_t kSizesMB[] = { 64, 128, 256, 504, 1000, 2000 };
constexpr uint32_t kMinFree = 32u << 20;

// RP2350: while a disk is built, the scan's nodes and names take SRAM the pager lends
// (pcmem_lend) when it can, as with a PC running the heap has only about 10 KB; so does the tree
// read back when it is written back (s_lendOk). Once built, the disk's tables move to the PSRAM.
bool s_lendOk = false;
template <class T> struct LentAlloc {
  using value_type = T;
  LentAlloc() = default;
  template <class U> LentAlloc(const LentAlloc<U> &) {}
  T *allocate(size_t n) {
    if (s_lendOk && n * sizeof(T) >= 1024)
      if (void *p = pcmem_lend(n * sizeof(T))) return (T *)p;
    return (T *)::operator new(n * sizeof(T));
  }
  void deallocate(T *p, size_t) { if (!pcmem_free(p)) ::operator delete(p); }
};
template <class T, class U> bool operator==(const LentAlloc<T> &, const LentAlloc<U> &) { return true; }
template <class T, class U> bool operator!=(const LentAlloc<T> &, const LentAlloc<U> &) { return false; }
using Bytes = std::vector<uint8_t, LentAlloc<uint8_t>>;

void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
uint32_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

// ---- where the tables live -------------------------------------------------------------------------
// A disk's FAT, root directory and folders, and on the RP2350 its nodes too, each in a Store. On
// the RP2350 a Store is a block of the PSRAM (pcmem_folders), reached through hal_psram_read and
// write: a hard disk's FAT alone is up to 128 KB, and the heap has about 10 KB with a PC running,
// which is what froze the device as soon as D: held a folder or a zip. Elsewhere it is RAM.
#if defined(PICO386_LOWMEM)
struct PsBlock { uint32_t at, size; };
PsBlock s_ps[48];                // the blocks in use, by address
int s_nps = 0;

// First fit; 0 when there is no room (the region never starts at 0, which is guest RAM).
uint32_t psAlloc(uint32_t n) {
  long room = 0;
  const long base = pcmem_folders(&room);
  if (base <= 0 || !n || s_nps == (int)(sizeof s_ps / sizeof *s_ps)) return 0;
  n = (n + 63) & ~63u;
  const uint32_t top = (uint32_t)(base + room);
  uint32_t at = (uint32_t)base;
  int i = 0;
  for (;; i++) {
    const uint32_t end = i < s_nps ? s_ps[i].at : top;
    if (end - at >= n) break;
    if (i == s_nps) {
      hal_log("folder: no PSRAM left for %u KB", (unsigned)(n >> 10));
      return 0;
    }
    at = s_ps[i].at + s_ps[i].size;
  }
  memmove(s_ps + i + 1, s_ps + i, (size_t)(s_nps - i) * sizeof *s_ps);
  s_ps[i] = { at, n };
  s_nps++;
  const uint8_t zero[256] = {};
  for (uint32_t k = 0; k < n; k += sizeof zero) hal_psram_write(at + k, zero, std::min<uint32_t>(sizeof zero, n - k));
  return at;
}

void psFree(uint32_t at) {
  for (int i = 0; i < s_nps; i++)
    if (s_ps[i].at == at) {
      memmove(s_ps + i, s_ps + i + 1, (size_t)(s_nps - i - 1) * sizeof *s_ps);
      s_nps--;
      return;
    }
}

struct Store {
  uint32_t at = 0, n = 0;
  Store() = default;
  Store(const Store &) = delete;
  Store &operator=(const Store &) = delete;
  ~Store() { release(); }
  bool alloc(size_t size) {
    release();
    at = psAlloc((uint32_t)size);
    n = at ? (uint32_t)size : 0;
    return at || !size;
  }
  void release() { if (at) psFree(at); at = n = 0; }
  size_t size() const { return n; }
  void read(size_t off, void *dst, size_t len) const { hal_psram_read(at + (uint32_t)off, dst, len); }
  void write(size_t off, const void *src, size_t len) { hal_psram_write(at + (uint32_t)off, src, len); }
};
#else
struct Store {
  Bytes v;
  bool alloc(size_t size) { v.assign(size, 0); return true; }
  void release() { Bytes().swap(v); }
  size_t size() const { return v.size(); }
  void read(size_t off, void *dst, size_t len) const { memcpy(dst, v.data() + off, len); }
  void write(size_t off, const void *src, size_t len) {
    if (off + len > v.size()) v.resize(off + len);   // the scan's nodes and names, not counted first
    memcpy(v.data() + off, src, len);
  }
};
#endif

// What the scan found: one per file or folder ([0] is the folder itself), in a Store with their
// names, read one at a time (nodeAt, nameOf). On the RP2350 that is the PSRAM from the start: in
// SRAM, a game of 430 files took 30 KB, more than the heap and the pager's frames had.
struct Node {
  uint32_t name = 0;             // its name on the card, in the names pool
  int parent = -1;
  int kid0 = 0;                  // its children are nodes kid0 .. kid0 + nkids - 1
  uint32_t size = 0;
  uint32_t first = 0, nclus = 0; // first rises with the index: a node with no clusters has the
                                 // next one's first, and its entry says 0 (entryFirst)
  uint32_t dirOff = 0;           // a folder's clusters, in PicoFolder::dirs
  uint16_t date = 0, time = 0;
  uint16_t nkids = 0;
  uint8_t sfn[11];               // the 8.3 name the guest sees
  uint8_t lfn = 0;               // long-name entries it has (0: the short name is its name)
  bool dir = false;
  bool skipped = false;          // left out: no room
};

// The tree as the guest left it, read back at power-off: one flat list in which each folder's
// entries are one run, with the names in one pool, as the scan keeps its nodes. A tree of
// strings and vectors took about 200 bytes a file, more than the RP2350's heap has for a floppy.
struct Final {
  uint32_t name = 0;             // in the names pool
  int parent = -1, kid0 = 0, nkids = 0;
  int orig = -1;                 // the scanned node it is, or -1 when new
  int stage = -1;                // a changed file's copy in the staging folder
  uint32_t first = 0, size = 0;
  uint8_t sfn[11];
  uint8_t depth = 0;
  bool dir = false;
  bool open = false;             // a folder whose entries are read (not a loop, not too deep)
  bool changed = false;          // a file whose content must be written
  bool hostName = false;         // it keeps the name it had on the card
};
using FinalList = std::vector<Final, LentAlloc<Final>>;
using NamePool = std::vector<char, LentAlloc<char>>;

} // namespace

struct PicoFolder {
  std::string root, label;
  Store nodeStore;                                   // the scan's nodes
  Store nameStore;                                   // their names, each ending in a NUL
  uint32_t namesLen = 0;
  size_t nodeCap = (size_t)-1;                       // what the stores were sized for (RP2350)
#if defined(PICO386_LOWMEM)
  Store spillMap;                                    // cluster -> its slot in the scratch file + 1, 2 bytes each
#else
  std::unordered_map<uint32_t, uint32_t> spill;      // cluster -> its slot in the scratch file
#endif
  uint32_t nnodes = 0, nspill = 0;
  Store rootDir;
  Store fat;                                         // the first FAT as on disk; both copies read from it
  Store dirs;                                        // every folder's clusters; the guest's writes to them land here
  std::map<uint32_t, std::vector<uint8_t>> extra;    // written sectors outside FAT/root/data
  int own = -1;                                      // ownerOf's last answer, and a copy of that node
  Node ownNode;
  std::string spillPath;
  FILE *spillF = nullptr;
  FILE *cur = nullptr;                               // the last file read, kept open
  int curNode = -1;
  uint32_t total = 0, part = 0, spc = 0, cb = 0, fatSz = 0;
  uint32_t fat0 = 0, root0 = 0, data0 = 0, maxClus = 0;   // maxClus: one past the last cluster
  uint32_t serial = 0;
  uint32_t boot = kPartStart;                        // the boot sector's LBA (0 on a floppy)
  uint32_t rootEnt = kRootEnt, rootSecs = kRootEnt * 32 / 512;
  bool floppy = false;                               // FAT12, no partition table
  uint8_t ptype = 0, media = 0xF8;
  bool dirty = false, spillFailed = false, extraFull = false;
};

namespace {

std::vector<PicoFolder *> s_open;

Node nodeAt(const PicoFolder &f, int i) {
  Node n;
  f.nodeStore.read((size_t)i * sizeof(Node), &n, sizeof n);
  return n;
}

void setNode(PicoFolder &f, int i, const Node &n) { f.nodeStore.write((size_t)i * sizeof(Node), &n, sizeof n); }

uint32_t entryFirst(const Node &n) { return n.nclus ? n.first : 0; }

std::string nameOf(const PicoFolder &f, const Node &n) {
  std::string s;
  char buf[64];
  for (size_t at = n.name; at < f.namesLen; at += sizeof buf) {
    const size_t k = std::min<size_t>(sizeof buf, f.namesLen - at);
    f.nameStore.read(at, buf, k);
    const char *z = (const char *)memchr(buf, 0, k);
    s.append(buf, z ? (size_t)(z - buf) : k);
    if (z) break;
  }
  return s;
}

// Adds a name to the pool; false when the pool is full (the folder grew since it was counted).
bool addName(PicoFolder &f, const char *name, uint32_t &at) {
  const size_t len = strlen(name) + 1;
  if (f.nodeCap != (size_t)-1 && f.namesLen + len > f.nameStore.size()) return false;
  at = f.namesLen;
  f.nameStore.write(at, name, len);
  f.namesLen += (uint32_t)len;
  return true;
}

// A node's path on the card, from its parents' names.
std::string hostOf(const PicoFolder &f, int i) {
  const Node n = nodeAt(f, i);
  if (n.parent < 0) return f.root;
  return hostOf(f, n.parent) + "/" + nameOf(f, n);
}

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

// The 8.3 name for name, unique among the short names of sibs[0..nsibs). Returns true when it is
// not the name itself, so the long name has to be stored too.
using Sfn = std::array<uint8_t, 11>;

bool makeShort(const std::string &name, const Sfn *sibs, int nsibs, uint8_t out[11]) {
  auto taken = [&](const std::string &k) {
    for (int i = 0; i < nsibs; i++)
      if (!memcmp(sibs[i].data(), k.data(), 11)) return true;
    return false;
  };
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
  if (lossy || taken(k)) {
    for (int i = 1; i < 1000000; i++) {
      const std::string tail = "~" + std::to_string(i);
      k = key(b.substr(0, 8 - tail.size()) + tail, e);
      if (!taken(k)) break;
    }
  }
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

bool entryIsDir(const std::string &dir, const dirent *e) {
#if defined(DT_DIR) && defined(DT_UNKNOWN)
  if (e->d_type == DT_DIR) return true;
  if (e->d_type != DT_UNKNOWN) return false;
#endif
  return isDir(dir + "/" + e->d_name);
}

// The files and folders under dir (at most what scanDir keeps), and the bytes their names take,
// so a floppy's nodes and names can be sized once instead of growing: a vector that grows holds
// its old and new blocks at once. A folder is closed before the ones in it are counted, as each
// one open costs heap; finding the k-th subfolder again reads the folder again.
void countTree(const std::string &dir, int depth, size_t &nodes, size_t &bytes) {
  size_t dirs = 0;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    nodes++;
    bytes += strlen(e->d_name) + 1;
    if (entryIsDir(dir, e)) dirs++;
  }
  closedir(d);
  for (size_t k = 0; k < dirs && depth < kMaxDepth; k++) {
    std::string sub;
    if ((d = opendir(dir.c_str()))) {
      size_t seen = 0;
      while (dirent *e = readdir(d))
        if (e->d_name[0] != '.' && entryIsDir(dir, e) && seen++ == k) { sub = dir + "/" + e->d_name; break; }
      closedir(d);
    }
    if (!sub.empty()) countTree(sub, depth + 1, nodes, bytes);
  }
}

// Folder di's children go in one run of nodes, sorted by name; then each subfolder's, after them.
// They go to the node store as they are read, and SRAM holds 13 bytes a child while they are
// sorted and given short names: a game's folder can have a thousand files.
void scanDir(PicoFolder &f, int di, int depth) {
  const std::string dir = hostOf(f, di);
  size_t cap;
  {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    size_t count = 0;
    while (dirent *e = readdir(d)) count += e->d_name[0] != '.';
    closedir(d);
    cap = std::min<size_t>({ count, 65535, f.nodeCap - f.nnodes });
  }
  const int kid0 = (int)f.nnodes;
  int n = 0;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if ((size_t)n == cap) break;                     // FAT's most entries a folder has, or what was counted
    struct stat st;
    if (stat((dir + "/" + e->d_name).c_str(), &st) != 0) continue;
    Node k;
    k.dir = S_ISDIR(st.st_mode);
    if (!k.dir && !S_ISREG(st.st_mode)) continue;
    if (k.dir && depth >= kMaxDepth) continue;
    if (!addName(f, e->d_name, k.name)) break;
    k.parent = di;
    k.size = k.dir ? 0 : (uint32_t)st.st_size;
    dosTime(st.st_mtime, k.date, k.time);
    setNode(f, kid0 + n++, k);
  }
  closedir(d);
  f.nnodes = (uint32_t)(kid0 + n);

  // sorted by name: the order, then the nodes moved along its cycles, two at a time in SRAM
  {
    std::vector<uint16_t, LentAlloc<uint16_t>> order(n);
    for (int i = 0; i < n; i++) order[i] = (uint16_t)i;
    std::sort(order.begin(), order.end(), [&f, kid0](uint16_t a, uint16_t b) {
      return strcasecmp(nameOf(f, nodeAt(f, kid0 + a)).c_str(), nameOf(f, nodeAt(f, kid0 + b)).c_str()) < 0;
    });
    for (int s0 = 0; s0 < n; s0++) {
      if (order[s0] == s0) continue;
      const Node held = nodeAt(f, kid0 + s0);
      int j = s0;
      while (order[j] != s0) {
        const int from = order[j];
        setNode(f, kid0 + j, nodeAt(f, kid0 + from));
        order[j] = (uint16_t)j;
        j = from;
      }
      setNode(f, kid0 + j, held);
      order[j] = (uint16_t)j;
    }
  }
  {
    std::vector<Sfn, LentAlloc<Sfn>> sfns(n);
    for (int i = 0; i < n; i++) {
      Node k = nodeAt(f, kid0 + i);
      const std::string name = nameOf(f, k);
      if (makeShort(name, sfns.data(), i, k.sfn)) {
        const size_t len = toUtf16(name).size();
        k.lfn = len <= 255 ? (uint8_t)((len + 12) / 13) : 0;
      }
      memcpy(sfns[i].data(), k.sfn, 11);
      setNode(f, kid0 + i, k);
    }
  }
  Node p = nodeAt(f, di);
  p.kid0 = kid0;
  p.nkids = (uint16_t)n;
  setNode(f, di, p);
  for (int i = kid0; i < kid0 + n; i++)
    if (nodeAt(f, i).dir) scanDir(f, i, depth + 1);
}

uint32_t dirEntries(const PicoFolder &f, const Node &d) {
  uint32_t n = d.parent < 0 ? 1 : 2;                 // the volume label, or "." and ".."
  for (int k = d.kid0; k < d.kid0 + d.nkids; k++) {
    const Node c = nodeAt(f, k);
    if (!c.skipped) n += 1 + c.lfn;
  }
  return n;
}

// The disk geometry for a size, and the clusters the scan needs at that cluster size.
bool geometry(PicoFolder &f, uint32_t mb) {
  f.total = mb * 2048 / kCyl * kCyl;
  f.part = f.total - kPartStart;
  for (f.spc = 1; f.spc <= 64; f.spc *= 2) {
    const uint32_t tmp2 = 256 * f.spc + kNFats;
    f.fatSz = (f.part - kRsvd - f.rootSecs + tmp2 - 1) / tmp2;
    const uint32_t clusters = (f.part - kRsvd - kNFats * f.fatSz - f.rootSecs) / f.spc;
    if (clusters <= kMaxClusters) {
      f.maxClus = clusters + 2;
      break;
    }
  }
  if (f.spc > 64) return false;
  f.cb = f.spc * 512;
  f.fat0 = kPartStart + kRsvd;
  f.root0 = f.fat0 + kNFats * f.fatSz;
  f.data0 = f.root0 + f.rootSecs;
  f.ptype = f.part < 65536 ? 0x04 : f.total <= 1024 * kCyl ? 0x06 : 0x0E;
  return true;
}

// The 1.44 MB floppy, as FORMAT A: makes it.
void floppyGeometry(PicoFolder &f) {
  f.floppy = true;
  f.boot = 0;
  f.media = 0xF0;
  f.rootEnt = kFdRootEnt;
  f.rootSecs = kFdRootEnt * 32 / 512;
  f.total = f.part = kFdSectors;
  f.spc = 1;
  f.cb = 512;
  f.fatSz = kFdFatSz;
  f.fat0 = kRsvd;
  f.root0 = f.fat0 + kNFats * f.fatSz;
  f.data0 = f.root0 + f.rootSecs;
  f.maxClus = (f.total - f.data0) / f.spc + 2;
}

// The FAT's byte at off. On the RP2350 through a one-sector cache that every folder shares, as
// building the FAT sets every entry one by one; fatSync writes it back before the guest reads
// the FAT, and fatDrop forgets it.
#if defined(PICO386_LOWMEM)
struct FatCache {
  PicoFolder *f = nullptr;
  uint32_t sec = 0;
  bool dirty = false;
  uint8_t b[512];
};
FatCache s_fc;

void fatSync(PicoFolder &f) {
  if (s_fc.f != &f || !s_fc.dirty) return;
  f.fat.write((size_t)s_fc.sec * 512, s_fc.b, 512);
  s_fc.dirty = false;
}

void fatDrop(PicoFolder &f) { if (s_fc.f == &f) s_fc.f = nullptr; }

uint8_t *fatByte(PicoFolder &f, uint32_t off, bool write) {
  const uint32_t sec = off / 512;
  if (s_fc.f != &f || s_fc.sec != sec) {
    if (s_fc.f) fatSync(*s_fc.f);
    s_fc.f = &f;
    s_fc.sec = sec;
    s_fc.dirty = false;
    f.fat.read((size_t)sec * 512, s_fc.b, 512);
  }
  s_fc.dirty |= write;
  return s_fc.b + off % 512;
}
#else
void fatSync(PicoFolder &) {}
void fatDrop(PicoFolder &) {}
uint8_t *fatByte(PicoFolder &f, uint32_t off, bool) { return f.fat.v.data() + off; }
#endif

// FAT entries: 16 bits, or 12 on a floppy. An end-of-chain mark reads as >= 0xFFF8 either way.
uint32_t fatGet(PicoFolder &f, uint32_t c) {
  if (!f.floppy) return get16(fatByte(f, 2 * c, false));
  const uint32_t lo = *fatByte(f, c * 3 / 2, false), hi = *fatByte(f, c * 3 / 2 + 1, false);
  const uint32_t v = lo | hi << 8;
  const uint32_t e = c & 1 ? v >> 4 : v & 0xFFF;
  return e >= 0xFF8 ? 0xF000 | e : e;
}

void fatSet(PicoFolder &f, uint32_t c, uint32_t v) {
  if (!f.floppy) { put16(fatByte(f, 2 * c, true), v); return; }
  const uint32_t off = c * 3 / 2;
  v &= 0xFFF;
  uint8_t *p = fatByte(f, off, true);
  *p = c & 1 ? (uint8_t)((*p & 0x0F) | (v << 4)) : (uint8_t)v;
  p = fatByte(f, off + 1, true);
  *p = c & 1 ? (uint8_t)(v >> 4) : (uint8_t)((*p & 0xF0) | (v >> 8));
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

// A folder's entries, into st from base: long-name entries before each short one, as VFAT has them.
void fillDir(PicoFolder &f, const Node &d, Store &st, size_t base, uint32_t cap) {
  uint32_t i = 0;
  uint8_t e[32];
  auto put = [&]() { if (i < cap) st.write(base + 32 * (size_t)i++, e, 32); return i < cap; };
  for (int k = d.kid0; k < d.kid0 + d.nkids; k++) {
    const Node n = nodeAt(f, k);
    if (n.skipped) continue;
    if (n.lfn) {
      const std::vector<uint16_t> u = toUtf16(nameOf(f, n));
      const uint8_t sum = sfnSum(n.sfn);
      for (int s = n.lfn; s >= 1; s--) {
        memset(e, 0, 32);
        e[0] = (uint8_t)(s | (s == n.lfn ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (int c = 0; c < 13; c++) {
          const size_t j = (size_t)(s - 1) * 13 + c;
          put16(e + kLfnOff[c], j < u.size() ? u[j] : j == u.size() ? 0 : 0xFFFF);
        }
        if (!put()) return;
      }
    }
    putEntry(e, n.sfn, n.dir ? 0x10 : 0x20, entryFirst(n), n.size, n.date, n.time);
    if (!put()) return;
  }
}

// The node whose clusters hold c, or -1; *out gets a copy of it. Clusters were handed out in node
// order, so it is the last node with clusters that starts at or before c. The last answer is
// kept, as the guest reads a file's clusters in a row.
int ownerOf(PicoFolder &f, uint32_t c, Node *out) {
  if (f.own < 0 || c < f.ownNode.first || c >= f.ownNode.first + f.ownNode.nclus) {
    uint32_t lo = 1, hi = f.nnodes;                  // the first node starting after c
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      if (nodeAt(f, (int)mid).first <= c) lo = mid + 1;
      else hi = mid;
    }
    int o = (int)lo - 1;
    Node n;
    while (o >= 1 && (n = nodeAt(f, o)).nclus == 0) o--;
    if (o < 1 || c >= n.first + n.nclus) return -1;
    f.own = o;
    f.ownNode = n;
  }
  if (out) *out = f.ownNode;
  return f.own;
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
  put16(s + 17, f.rootEnt);
  put16(s + 19, f.part < 65536 ? f.part : 0);
  s[21] = f.media;
  put16(s + 22, f.fatSz);
  put16(s + 24, f.floppy ? kFdSpt : kSpt);
  put16(s + 26, f.floppy ? 2 : f.total <= 1024 * kCyl ? 16 : 255);
  put32(s + 28, f.boot);
  put32(s + 32, f.part < 65536 ? 0 : f.part);
  s[36] = f.floppy ? 0x00 : 0x81;
  s[38] = 0x29;
  put32(s + 39, f.serial);
  memcpy(s + 43, f.label.data(), 11);
  memcpy(s + 54, f.floppy ? "FAT12   " : "FAT16   ", 8);
  s[62] = 0xCD; s[63] = 0x18;
  s[510] = 0x55; s[511] = 0xAA;
}

// The scratch file's slot that holds cluster c, or -1. On the RP2350 a table in the PSRAM, made
// at the first write: a map entry per written cluster would soon run the heap out.
long spillSlot(PicoFolder &f, uint32_t c) {
  if (!f.nspill) return -1;
#if defined(PICO386_LOWMEM)
  uint16_t v = 0;
  f.spillMap.read((size_t)c * 2, &v, 2);
  return v ? (long)v - 1 : -1;
#else
  auto it = f.spill.find(c);
  return it == f.spill.end() ? -1 : (long)it->second;
#endif
}

bool spillAdd(PicoFolder &f, uint32_t c, uint32_t slot) {
#if defined(PICO386_LOWMEM)
  if (!f.spillMap.size() && !f.spillMap.alloc((size_t)f.maxClus * 2)) return false;
  const uint16_t v = (uint16_t)(slot + 1);
  f.spillMap.write((size_t)c * 2, &v, 2);
#else
  f.spill.emplace(c, slot);
#endif
  f.nspill++;
  return true;
}

// len bytes of cluster c from off: the scratch copy, a folder's clusters, the file, or zeros.
void readCluster(PicoFolder &f, uint32_t c, uint32_t off, uint8_t *buf, uint32_t len) {
  const long sp = spillSlot(f, c);
  if (sp >= 0) {
    fseek(f.spillF, (long)((uint64_t)sp * f.cb + off), SEEK_SET);
    if (fread(buf, 1, len, f.spillF) != len) memset(buf, 0, len);
    return;
  }
  memset(buf, 0, len);
  Node n;
  const int o = ownerOf(f, c, &n);
  if (o < 0) return;
  const uint64_t at = (uint64_t)(c - n.first) * f.cb + off;
  if (n.dir) {
    f.dirs.read(n.dirOff + (size_t)at, buf, len);
    return;
  }
  if (at >= n.size) return;
  if (f.curNode != o) {
    closeCur(f);
    f.cur = fopen(hostOf(f, o).c_str(), "rb");
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
  if (s == f.boot) { bootSector(f, buf); return; }
  if (s == 0) { mbrSector(f, buf); return; }
  if (s >= f.fat0 && s < f.root0) {
    fatSync(f);
    f.fat.read((size_t)((s - f.fat0) % f.fatSz) * 512, buf, 512);
    return;
  }
  if (s >= f.root0 && s < f.data0) { f.rootDir.read((size_t)(s - f.root0) * 512, buf, 512); return; }
  if (s >= f.data0) {
    const uint32_t rel = s - f.data0, c = 2 + rel / f.spc;
    if (c < f.maxClus) readCluster(f, c, rel % f.spc * 512, buf, 512);
  }
}

void writeSector(PicoFolder &f, uint32_t s, const uint8_t *buf) {
  f.dirty = true;
  if (s >= f.fat0 && s < f.root0) {
    // the second FAT is a copy DOS keeps in step; the first one is the one that counts
    if (s < f.fat0 + f.fatSz) {
      fatSync(f);
      fatDrop(f);
      f.fat.write((size_t)(s - f.fat0) * 512, buf, 512);
    }
    return;
  }
  if (s >= f.root0 && s < f.data0) { f.rootDir.write((size_t)(s - f.root0) * 512, buf, 512); return; }
  if (s >= f.data0) {
    const uint32_t rel = s - f.data0, c = 2 + rel / f.spc, off = rel % f.spc * 512;
    if (c >= f.maxClus) return;
    long sp = spillSlot(f, c);
    if (sp < 0) {
      Node n;
      if (ownerOf(f, c, &n) >= 0 && n.dir) {
        f.dirs.write(n.dirOff + (size_t)(c - n.first) * f.cb + off, buf, 512);
        return;
      }
      // the first write to a cluster: its scratch copy starts as what the cluster holds now,
      // copied a sector at a time (a cluster is up to 32 KB)
      if (!f.spillF) f.spillF = fopen(f.spillPath.c_str(), "w+b");
      if (f.spillF) {
        sp = (long)f.nspill;
        uint8_t tmp[512];
        for (uint32_t k = 0; k < f.cb; k += 512) {
          readCluster(f, c, k, tmp, 512);
          fseek(f.spillF, (long)((uint64_t)sp * f.cb + k), SEEK_SET);
          fwrite(tmp, 1, 512, f.spillF);
        }
      }
      if (!f.spillF || !spillAdd(f, c, (uint32_t)sp)) {
        if (!f.spillFailed) hal_log("folder: cannot keep the writes to %s, they are lost", f.root.c_str());
        f.spillFailed = true;
        return;
      }
    }
    fseek(f.spillF, (long)((uint64_t)sp * f.cb + off), SEEK_SET);
    fwrite(buf, 1, 512, f.spillF);
    return;
  }
#if defined(PICO386_LOWMEM)
  // a few at most, as each is 512 bytes of heap; past that the disk counts as reformatted (the
  // commit refuses it), as that is what writes all over these sectors
  if (!f.extra.count(s) && f.extra.size() >= 4) {
    if (!f.extraFull) hal_log("folder: sector %u written outside the FAT, ignored", (unsigned)s);
    f.extraFull = true;
    return;
  }
#endif
  f.extra[s].assign(buf, buf + 512);
}

// ---- power-off: the guest's tree back onto the card ----------------------------------------------

bool goodName(const std::string &n) {
  if (n.empty() || n == "." || n == "..") return false;
  for (unsigned char c : n)
    if (c < 0x20 || strchr("/\\:*?\"<>|", c)) return false;
  return true;
}

// Calls fn with each 32-byte entry of a folder, the root directory when first is 0, a sector at a
// time, until fn returns false.
template <class Fn> void eachEntry(PicoFolder &f, uint32_t first, Fn fn) {
  uint8_t sec[512];
  if (!first) {
    for (size_t s = 0; s < f.rootDir.size(); s += 512) {
      f.rootDir.read(s, sec, 512);
      for (int i = 0; i < 16; i++)
        if (!fn(sec + 32 * i)) return;
    }
    return;
  }
  for (uint32_t c = first, steps = 0; c >= 2 && c < f.maxClus && steps < f.maxClus; steps++) {
    for (uint32_t s = 0; s < f.spc; s++) {
      readCluster(f, c, s * 512, sec, 512);
      for (int i = 0; i < 16; i++)
        if (!fn(sec + 32 * i)) return;
    }
    c = fatGet(f, c);
  }
}

// Turns a folder's entries back into names, long ones included.
struct EntryParser {
  uint16_t lname[20 * 13];
  int lnext = 0;
  uint8_t lsum = 0;
  bool lvalid = false;

  // 0 at the end of the folder; 2 when e is a file or folder (fn and name set); else 1.
  int feed(const uint8_t *e, Final &fn, std::string &name) {
    if (e[0] == 0) return 0;
    if (e[0] == 0xE5) { lvalid = false; return 1; }
    if (e[11] == 0x0F) {
      const int seq = e[0] & 0x1F;
      if (e[0] & 0x40) {
        lvalid = seq >= 1 && seq <= 20;
        lnext = seq;
        lsum = e[13];
        if (lvalid) memset(lname, 0, (size_t)seq * 13 * 2);
      }
      if (!lvalid || seq != lnext || e[13] != lsum) { lvalid = false; return 1; }
      for (int c = 0; c < 13; c++) lname[(seq - 1) * 13 + c] = (uint16_t)get16(e + kLfnOff[c]);
      lnext--;
      return 1;
    }
    const bool hadLong = lvalid && lnext == 0 && sfnSum(e) == lsum;
    lvalid = false;
    if (e[11] & 0x08 || e[0] == '.') return 1;       // the volume label, "." and ".."
    fn = Final();
    memcpy(fn.sfn, e, 11);
    if (fn.sfn[0] == 0x05) fn.sfn[0] = 0xE5;
    name.clear();
    if (hadLong) {
      std::vector<uint16_t> u;
      for (int i = 0; i < 20 * 13 && lname[i]; i++) u.push_back(lname[i]);
      name = toUtf8(u);
    }
    if (!goodName(name)) name = shortText(fn.sfn);
    if (!goodName(name)) return 1;
    fn.dir = (e[11] & 0x10) != 0;
    fn.first = get16(e + 26);
    fn.size = fn.dir ? 0 : get32(e + 28);
    return 2;
  }
};

// Whether a folder entry is one to read too: not too deep, and not one already read (a loop).
// The folders read so far are a list of first clusters, as a bit per cluster is 8 KB.
using SeenDirs = std::vector<uint32_t, LentAlloc<uint32_t>>;
bool opens(const PicoFolder &f, const Final &k, int depth, SeenDirs &seenDir) {
  if (!k.dir || depth >= kMaxDepth || k.first < 2 || k.first >= f.maxClus) return false;
  if (std::find(seenDir.begin(), seenDir.end(), k.first) != seenDir.end()) return false;
  seenDir.push_back(k.first);
  return true;
}

// The guest's tree, breadth first: list[0] is the root, and each folder's entries follow as one
// run. A first pass counts the entries and their names' bytes, so both are sized once (a vector
// that grows holds its old and new blocks at once).
void readTree(PicoFolder &f, FinalList &list, NamePool &names) {
  size_t count = 1, bytes = 1;
  {
    SeenDirs seenDir;
    std::vector<uint32_t> queue(1, 0);               // first cluster | depth << 24
    for (size_t q = 0; q < queue.size(); q++) {
      const int depth = (int)(queue[q] >> 24);
      EntryParser p;
      Final k;
      std::string name;
      eachEntry(f, queue[q] & 0xFFFFFF, [&](const uint8_t *e) {
        const int r = p.feed(e, k, name);
        if (r == 2) {
          count++;
          bytes += name.size() + 1;
          if (opens(f, k, depth + 1, seenDir)) queue.push_back(k.first | (uint32_t)(depth + 1) << 24);
        }
        return r != 0;
      });
    }
  }
  list.reserve(count);
  names.reserve(bytes);
  SeenDirs seenDir;
  Final top;
  top.dir = top.open = true;
  top.orig = 0;
  list.push_back(top);
  names.push_back(0);
  for (size_t i = 0; i < list.size(); i++) {
    if (!list[i].open) continue;
    list[i].kid0 = (int)list.size();
    EntryParser p;
    Final k;
    std::string name;
    const int depth = list[i].depth;
    eachEntry(f, list[i].first, [&](const uint8_t *e) {
      const int r = p.feed(e, k, name);
      if (r == 2 && list.size() < list.capacity()) {
        k.parent = (int)i;
        k.depth = (uint8_t)(depth + 1);
        k.open = opens(f, k, depth + 1, seenDir);
        k.name = (uint32_t)names.size();
        names.insert(names.end(), name.c_str(), name.c_str() + name.size() + 1);
        list.push_back(k);
      }
      return r != 0;
    });
    list[i].nkids = (int)list.size() - list[i].kid0;
  }
}

// Whether a file the guest left is still exactly the one the scan found: the same clusters, in a
// row as the scan laid them out, none of them written.
bool unchanged(PicoFolder &f, const Final &k) {
  if (k.orig < 0) return false;
  const Node o = nodeAt(f, k.orig);
  if (k.first != entryFirst(o) || k.size != o.size) return false;
  uint32_t n = 0;
  for (uint32_t c = k.first; c >= 2 && c < f.maxClus && n < f.maxClus;) {
    if (c != k.first + n || spillSlot(f, c) >= 0) return false;
    n++;
    const uint32_t next = fatGet(f, c);
    if (next >= 0xFFF8) break;
    c = next;
  }
  return n == o.nclus;
}

// Matches what the guest left to what the scan found. First: a file or folder whose first cluster
// is still the one the scan gave it is that node, wherever it is now (renamed, moved). Then a file
// saved anew (new clusters) is still the one with its short name in the same folder. Breadth
// first, so a folder is matched before its entries.
void matchTree(PicoFolder &f, FinalList &list, const NamePool &names, std::vector<bool> &claimed) {
  for (size_t i = 1; i < list.size(); i++) {
    Final &k = list[i];
    Node n;
    if (k.first && ownerOf(f, k.first, &n) > 0 && n.first == k.first && n.dir == k.dir && !claimed[f.own]) {
      k.orig = f.own;
      claimed[f.own] = true;
    }
  }
  for (size_t i = 1; i < list.size(); i++) {
    Final &k = list[i];
    const int dOrig = list[k.parent].orig;
    if (k.orig < 0 && dOrig >= 0) {
      const Node dn = nodeAt(f, dOrig);
      for (int n = dn.kid0; n < dn.kid0 + dn.nkids; n++) {
        if (claimed[n]) continue;
        const Node o = nodeAt(f, n);
        if (!o.skipped && o.dir == k.dir && !memcmp(o.sfn, k.sfn, 11)) {
          k.orig = n;
          claimed[n] = true;
          break;
        }
      }
    }
    // the host name is kept unless the guest gave it another one
    if (k.orig >= 0 && !memcmp(nodeAt(f, k.orig).sfn, k.sfn, 11) && names.data() + k.name == shortText(k.sfn))
      k.hostName = true;
    if (!k.dir) k.changed = !unchanged(f, k);
  }
}

int s_stageSeq;

std::string stagePath(const std::string &staging, int n) { return staging + "/" + std::to_string(n); }

void stageFiles(PicoFolder &f, FinalList &list, const std::string &staging) {
  uint8_t buf[512];
  for (Final &k : list) {
    if (k.dir || !k.changed) continue;
    k.stage = s_stageSeq++;
    const std::string path = stagePath(staging, k.stage);
    FILE *o = fopen(path.c_str(), "wb");
    if (!o) { hal_log("folder: cannot write %s", path.c_str()); k.stage = -1; continue; }
    uint32_t left = k.size;
    for (uint32_t c = k.first, steps = 0; left && c >= 2 && c < f.maxClus && steps < f.maxClus; steps++) {
      for (uint32_t s = 0; s < f.spc && left; s++) {
        const uint32_t n = left < 512 ? left : 512;
        readCluster(f, c, s * 512, buf, n);
        fwrite(buf, 1, n, o);
        left -= n;
      }
      c = fatGet(f, c);
    }
    fclose(o);
  }
}

bool samePath(const std::string &a, const std::string &b) { return !strcasecmp(a.c_str(), b.c_str()); }

// Renames done so far (old path, new path), so a node's path now can be worked out from the one
// the scan saw (curOf), instead of keeping every path.
using Moves = std::vector<std::pair<std::string, std::string>>;

std::string curOf(const PicoFolder &f, const Moves &moves, int i) {
  std::string p = hostOf(f, i);
  for (const auto &m : moves) {
    const std::string &from = m.first;
    if (p == from || (p.size() > from.size() && !p.compare(0, from.size(), from) && p[from.size()] == '/'))
      p = m.second + p.substr(from.size());
  }
  return p;
}

void moveTo(Moves &moves, const std::string &from, const std::string &to) {
  if (rename(from.c_str(), to.c_str()) != 0) hal_log("folder: cannot move %s to %s", from.c_str(), to.c_str());
  else moves.emplace_back(from, to);
}

std::string targetOf(const PicoFolder &f, const FinalList &list, const NamePool &names, int i) {
  const Final &k = list[i];
  if (k.parent < 0) return f.root;
  return targetOf(f, list, names, k.parent) + "/" +
         (k.hostName ? nameOf(f, nodeAt(f, k.orig)) : std::string(names.data() + k.name));
}

// Folders made or moved, files moved or put in place; parents before their entries.
void applyTree(PicoFolder &f, const FinalList &list, const NamePool &names, const std::string &staging,
               Moves &moves) {
  for (size_t i = 1; i < list.size(); i++) {
    const Final &k = list[i];
    const std::string target = targetOf(f, list, names, (int)i);
    if (k.dir) {
      if (k.orig < 0) makeDirs(target);
      else {
        const std::string cur = curOf(f, moves, k.orig);
        if (!samePath(cur, target)) moveTo(moves, cur, target);
      }
    } else if (!k.changed) {
      const std::string cur = curOf(f, moves, k.orig);
      if (!samePath(cur, target)) moveTo(moves, cur, target);
    } else if (k.stage >= 0) {
      if (k.orig >= 0) remove(curOf(f, moves, k.orig).c_str());
      const std::string stage = stagePath(staging, k.stage);
      if (rename(stage.c_str(), target.c_str()) != 0)
        hal_log("folder: cannot write %s (kept as %s)", target.c_str(), stage.c_str());
    }
  }
}

void commit(PicoFolder &f) {
  if (f.extra.count(0) || f.extra.count(f.boot) || f.extraFull) {
    hal_log("folder: %s was repartitioned or formatted; its changes are not applied", f.root.c_str());
    return;
  }
  FinalList list;
  NamePool names;
  readTree(f, list, names);
  std::vector<bool> claimed(f.nnodes, false);
  matchTree(f, list, names, claimed);

  // 1. New and changed files are written aside first, while every old file is still in place.
  const std::string staging = f.root + "/.pico386-commit";
  makeDirs(staging);
  stageFiles(f, list, staging);
  closeCur(f);

  // 2. Files the guest deleted go.
  int removed = 0;
  for (uint32_t i = 1; i < f.nnodes; i++) {
    if (claimed[i]) continue;
    const Node n = nodeAt(f, (int)i);
    if (!n.skipped && !n.dir && remove(hostOf(f, (int)i).c_str()) == 0) removed++;
  }

  // 3. Folders made or moved, files moved or put in place.
  Moves moves;
  applyTree(f, list, names, staging, moves);

  // 4. Folders the guest deleted, deepest first (one that still holds files left out stays).
  std::vector<std::string> gone;
  for (uint32_t i = 1; i < f.nnodes; i++) {
    if (claimed[i]) continue;
    const Node n = nodeAt(f, (int)i);
    if (!n.skipped && n.dir) gone.push_back(curOf(f, moves, (int)i));
  }
  std::sort(gone.begin(), gone.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
  for (const std::string &d : gone) rmdir(d.c_str());
  rmdir(staging.c_str());
  hal_log("folder: %s updated (%d files removed)", f.root.c_str(), removed);
}

void freeFolder(PicoFolder *f) {
  closeCur(*f);
  fatDrop(*f);
  if (f->spillF) fclose(f->spillF);
  if (!f->spillPath.empty()) remove(f->spillPath.c_str());
  delete f;
}

} // namespace

// ---- the C API ------------------------------------------------------------------------------------

extern "C" int pfolder_is_dir(const char *path) { return path && path[0] && isDir(path); }

namespace {

// Builds the disk; false when there is no memory for it, or a floppy's files do not fit.
bool build(PicoFolder &f, bool floppy) {
  if (floppy) floppyGeometry(f);
  f.serial = hal_micros() ^ 0x50433836;

  // the volume label: the folder's name
  const std::string base = f.root.substr(f.root.find_last_of('/') + 1);
  f.label = "           ";
  for (size_t i = 0, j = 0; i < base.size() && j < 11; i++) {
    const char u = (char)toupper((unsigned char)base[i]);
    if (sfnChar((unsigned char)u) || u == ' ') f.label[j++] = u;
  }
  if (f.label == "           ") f.label = "SD FOLDER  ";

  // The RP2350 keeps the nodes and names in the PSRAM, sized by a first walk; elsewhere they grow.
#if defined(PICO386_LOWMEM)
  {
    size_t count = 1, bytes = 1;
    countTree(f.root, 0, count, bytes);
    if (!f.nodeStore.alloc(count * sizeof(Node)) || !f.nameStore.alloc(bytes)) return false;
    f.nodeCap = count;
  }
#endif
  Node top;
  top.dir = true;
  if (!addName(f, "", top.name)) return false;
  setNode(f, 0, top);
  f.nnodes = 1;
  scanDir(f, 0, 0);
  const int nn = (int)f.nnodes;

  // the root directory has room for 511 entries next to the label
  uint32_t rootUsed = 1;
  top = nodeAt(f, 0);
  for (int k = top.kid0; k < top.kid0 + top.nkids; k++) {
    Node n = nodeAt(f, k);
    if (rootUsed + 1 + n.lfn > f.rootEnt) { n.skipped = true; setNode(f, k, n); }
    else rootUsed += 1 + n.lfn;
  }
  for (int i = 1; i < nn; i++) {
    Node n = nodeAt(f, i);
    if (!n.skipped && nodeAt(f, n.parent).skipped) { n.skipped = true; setNode(f, i, n); }
  }

  // the smallest size with room to spare (a floppy has one size)
  for (uint32_t mb : kSizesMB) {
    if (floppy) break;
    geometry(f, mb);
    uint64_t need = 0;
    for (int i = 1; i < nn; i++) {
      const Node n = nodeAt(f, i);
      if (!n.skipped) need += clustersNeeded(f, n);
    }
    if ((f.maxClus - 2 - need) * (uint64_t)f.cb >= kMinFree && need < f.maxClus - 2) break;
  }

  // Clusters go out in node order, so ownerOf can search the nodes themselves. A node with none
  // (skipped, or empty) still gets first = next, which keeps first rising with the index.
  if (!f.fat.alloc((size_t)f.fatSz * 512)) return false;
  fatSet(f, 0, 0xFF00 | f.media);
  fatSet(f, 1, 0xFFFF);
  uint32_t next = 2;
  bool left = false;
  for (int i = 1; i < nn; i++) {
    Node n = nodeAt(f, i);
    if (!n.skipped && nodeAt(f, n.parent).skipped) n.skipped = true;
    const uint32_t c = n.skipped ? 0 : clustersNeeded(f, n);
    if (next + c > f.maxClus) {
      n.skipped = true;
      hal_log("folder: no room for %s", hostOf(f, i).c_str());
    }
    n.first = next;
    n.nclus = n.skipped ? 0 : c;
    for (uint32_t j = 0; j < n.nclus; j++) fatSet(f, next + j, j + 1 < n.nclus ? next + j + 1 : 0xFFFF);
    next += n.nclus;
    left |= n.skipped;
    setNode(f, i, n);
  }
  fatSync(f);

  // a floppy with something left out would be a game with files missing: refuse it
  if (floppy && left) {
    hal_log("folder: %s does not fit on a floppy", f.root.c_str());
    return false;
  }

  // the directories, now that every first cluster is known
  if (!f.rootDir.alloc((size_t)f.rootEnt * 32)) return false;
  uint8_t e[64];
  uint8_t sfnLabel[11];
  memcpy(sfnLabel, f.label.data(), 11);
  putEntry(e, sfnLabel, 0x08, 0, 0, nn > 1 ? nodeAt(f, 1).date : 0x21, 0);
  f.rootDir.write(0, e, 32);
  fillDir(f, nodeAt(f, 0), f.rootDir, 32, f.rootEnt - 1);
  size_t dirBytes = 0;
  for (int i = 1; i < nn; i++) {
    Node n = nodeAt(f, i);
    if (!n.dir || n.skipped) continue;
    n.dirOff = (uint32_t)dirBytes;
    dirBytes += (size_t)n.nclus * f.cb;
    setNode(f, i, n);
  }
  if (!f.dirs.alloc(dirBytes)) return false;
  size_t files = 0;
  uint64_t bytes = 0;
  for (int i = 1; i < nn; i++) {
    const Node n = nodeAt(f, i);
    if (n.skipped) continue;
    if (!n.dir) { files++; bytes += n.size; continue; }
    static const uint8_t kDot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    static const uint8_t kDotDot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    putEntry(e, kDot, 0x10, n.first, 0, n.date, n.time);
    putEntry(e + 32, kDotDot, 0x10, n.parent ? nodeAt(f, n.parent).first : 0, 0, n.date, n.time);
    f.dirs.write(n.dirOff, e, 64);
    fillDir(f, n, f.dirs, n.dirOff + 64, n.nclus * f.cb / 32 - 2);
  }

  hal_log("folder: %s as a %u KB %s, %u files (%u KB), %u-byte clusters, tables %u KB",
          f.root.c_str(), (unsigned)(f.total / 2), floppy ? "floppy" : "disk", (unsigned)files,
          (unsigned)(bytes >> 10), (unsigned)f.cb,
          (unsigned)((f.fat.size() + f.rootDir.size() + f.dirs.size() + f.nnodes * sizeof(Node) +
                      f.namesLen) >> 10));
  return true;
}

PicoFolder *openFolder(const char *dir, bool floppy) {
  PicoFolder *f = new (std::nothrow) PicoFolder;
  if (!f) return nullptr;
  f->root = dir;
  while (f->root.size() > 1 && f->root.back() == '/') f->root.pop_back();
  // a scratch file no open folder uses (floppies come and go while the PC runs)
  for (int i = 0; f->spillPath.empty(); i++) {
    const std::string p = sdPath(("pc-386/.pico386-d" + std::to_string(i) + ".tmp").c_str());
    bool used = false;
    for (PicoFolder *o : s_open) used |= o->spillPath == p;
    if (!used) f->spillPath = p;
  }
  // On the RP2350 a big folder can still run the heap out: refuse the disk rather than abort().
  bool ok = false;
  s_lendOk = true;
  try {
    ok = build(*f, floppy);
  } catch (const std::bad_alloc &) {
    hal_log("folder: no memory for %s", f->root.c_str());
  }
  s_lendOk = false;
  if (!ok) {
    f->spillPath.clear();                            // not made yet, and maybe another's name later
    freeFolder(f);
    return nullptr;
  }
  // the desktop build quits through exit() (window closed, test scripts): save on the way out
  static bool hooked = false;
  if (!hooked) { atexit(pfolder_close_all); hooked = true; }
  s_open.push_back(f);
  return f;
}

// Writes a folder back, with the SRAM the pager lends for the tree it reads.
void closeOne(PicoFolder *f) {
  if (f->dirty) {
    pcsnap_touch(f->root.c_str());     // snapshots taken before no longer match the folder
    s_lendOk = true;
    try {
      commit(*f);
    } catch (const std::bad_alloc &) {
      hal_log("folder: no memory to write %s back; its changes are lost", f->root.c_str());
    }
    s_lendOk = false;
  }
  freeFolder(f);
}

} // namespace

extern "C" PicoFolder *pfolder_open(const char *dir) { return openFolder(dir, false); }

extern "C" PicoFolder *pfolder_open_floppy(const char *dir) { return openFolder(dir, true); }

extern "C" int pfolder_in_use(const char *dir) {
  std::string d = dir ? dir : "";
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  for (PicoFolder *f : s_open)
    if (!strcasecmp(f->root.c_str(), d.c_str())) return 1;
  return 0;
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

extern "C" int pfolder_is_dirty(PicoFolder *f) { return f && f->dirty; }

extern "C" const char *pfolder_root(int i) {
  return i >= 0 && i < (int)s_open.size() ? s_open[i]->root.c_str() : nullptr;
}

extern "C" void pfolder_close(PicoFolder *f) {
  for (size_t i = 0; i < s_open.size(); i++) {
    if (s_open[i] != f) continue;
    s_open.erase(s_open.begin() + i);
    closeOne(f);
    return;
  }
}

extern "C" void pfolder_close_all(void) {
  for (PicoFolder *f : s_open) closeOne(f);
  s_open.clear();
}
