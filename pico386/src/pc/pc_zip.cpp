// pc_zip.cpp -- .zip files as disks. A zip (a game, as the DOS game archives come) is unpacked
// once to /pico-386/.unzipped/<zip name>/, and that folder is what the guest gets, through the
// folder disk (pc_folder.cpp). So a zip mounts as D: like any folder, and what the guest writes
// there (saved games, settings) is kept in the unpacked folder, not in the zip.
//
// A marker file in the folder records the zip it came from (path, size, date). The next start
// with the same zip reuses the folder as it is. If the zip changed, or the last unpacking did not
// finish, the folder is emptied and unpacked again.
//
// A zip that holds floppy images (the disks of a game, as .ima files) goes to A:/B: as those
// images, not as a folder floppy holding them: the first one is mounted, and the drive's menu
// offers the others (pczip_images).
//
// The inflater is our own, written from RFC 1951: zips only use "stored" and "deflated", and
// neither the desktop nor the P4 build has zlib.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>
#include "pc_zip.h"
#include "pc_glue.h"
#include "../frontend/settings.h"
#include "../frontend/ui.h"
#include "../hal/hal.h"

namespace {

constexpr const char *kMarker = ".pico386-zip";

uint32_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

uint32_t s_crcTable[256];

uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
  if (!s_crcTable[1]) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      s_crcTable[i] = c;
    }
  }
  crc = ~crc;
  while (n--) crc = s_crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ---- inflate -----------------------------------------------------------------------------------

// The compressed bytes of one entry, read from the zip in blocks.
struct Input {
  FILE *f;
  uint32_t left;                 // compressed bytes not read from the file yet
  uint8_t buf[1024];
  size_t pos = 0, len = 0;
  uint32_t bitBuf = 0;
  int bitCnt = 0;
  bool err = false;              // read past the entry's data

  uint32_t byte() {
    if (pos == len) {
      const size_t want = left < sizeof buf ? left : sizeof buf;
      len = want ? fread(buf, 1, want, f) : 0;
      left -= (uint32_t)len;
      pos = 0;
      if (!len) { err = true; return 0; }
    }
    return buf[pos++];
  }
  uint32_t bits(int n) {
    while (bitCnt < n) { bitBuf |= byte() << bitCnt; bitCnt += 8; }
    const uint32_t v = bitBuf & ((1u << n) - 1);
    bitBuf >>= n;
    bitCnt -= n;
    return v;
  }
};

// The PSRAM ring (RP2350 only; pcmem_scratch is -1 elsewhere).
#if defined(PICO386_LOWMEM)
void ringWrite(long at, const uint8_t *p, size_t n) { hal_psram_write((uint32_t)at, p, n); }
void ringRead(long at, uint8_t *p, size_t n) { hal_psram_read((uint32_t)at, p, n); }
#else
void ringWrite(long, const uint8_t *, size_t) {}
void ringRead(long, uint8_t *, size_t) {}
#endif

// The inflated bytes, written to the file a block at a time. Deflate copies from up to 32 KB back.
// On the RP2350, with a PC running, the heap has about 10 KB: only the last 2 KB are kept in SRAM,
// and every block written also goes to a 32 KB ring in the PSRAM (pcmem_scratch), which the
// copies from further back read. Elsewhere the SRAM block is the whole 32 KB window.
struct Output {
  FILE *f;
  long ring = pcmem_scratch();
  uint32_t size = ring >= 0 ? 2048 : 32768;        // a power of two that divides 32 KB
  std::vector<uint8_t> win = std::vector<uint8_t>(size);
  uint32_t total = 0, crc = 0;
  bool err = false;              // the card is full

  void flush(size_t n) {
    crc = crc32(crc, win.data(), n);
    if (fwrite(win.data(), 1, n, f) != n) err = true;
    if (ring >= 0) ringWrite(ring + ((total - (uint32_t)n) & 32767), win.data(), n);
  }
  void put(uint8_t b) {
    win[total & (size - 1)] = b;
    if ((++total & (size - 1)) == 0) flush(size);
  }
  void finish() { if (total & (size - 1)) flush(total & (size - 1)); }
  // len (3..258) bytes from d back; d <= total.
  void copy(uint32_t d, uint32_t len) {
    if (d <= size) {                               // still in SRAM, and may overlap what it writes
      for (uint32_t i = 0; i < len; i++) put(win[(total - d) & (size - 1)]);
      return;
    }
    // From the ring up to the block not written out yet, then from SRAM. d > size >= len, so
    // all of it is before total and nothing it reads is overwritten until it is gathered.
    uint8_t tmp[258];
    const uint32_t from = total - d, written = total & ~(size - 1);
    const uint32_t nr = from + len <= written ? len : written - from;
    const uint32_t at = from & 32767, first = nr < 32768 - at ? nr : 32768 - at;
    ringRead(ring + at, tmp, first);
    if (first < nr) ringRead(ring, tmp + first, nr - first);
    for (uint32_t i = nr; i < len; i++) tmp[i] = win[(from + i) & (size - 1)];
    for (uint32_t i = 0; i < len; i++) put(tmp[i]);
  }
};

// A canonical Huffman code as counts per length and the symbols in code order (as zlib's puff).
struct Huffman {
  int16_t count[16];
  int16_t symbol[288];
};

// false: over-subscribed lengths, not a valid code.
bool build(Huffman &h, const uint8_t *lengths, int n) {
  memset(h.count, 0, sizeof h.count);
  for (int i = 0; i < n; i++) h.count[lengths[i]]++;
  int left = 1;
  for (int len = 1; len < 16; len++) {
    left = (left << 1) - h.count[len];
    if (left < 0) return false;
  }
  int16_t offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
  for (int i = 0; i < n; i++)
    if (lengths[i]) h.symbol[offs[lengths[i]]++] = (int16_t)i;
  return true;
}

// The next symbol, or -1 for a code the table does not have.
int decode(Input &in, const Huffman &h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    code |= (int)in.bits(1);
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
                                51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
                                4, 4, 5, 5, 5, 5, 0 };
const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
                                 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
                                 16385, 24577 };
const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
                                 10, 10, 11, 11, 12, 12, 13, 13 };

// One Huffman-coded block, up to its end-of-block symbol.
bool codes(Input &in, Output &out, const Huffman &lit, const Huffman &dist) {
  for (;;) {
    int sym = decode(in, lit);
    if (sym < 0 || in.err || out.err) return false;
    if (sym < 256) { out.put((uint8_t)sym); continue; }
    if (sym == 256) return true;
    sym -= 257;
    if (sym >= 29) return false;
    const uint32_t len = kLenBase[sym] + in.bits(kLenExtra[sym]);
    const int ds = decode(in, dist);
    if (ds < 0 || ds >= 30) return false;
    const uint32_t d = kDistBase[ds] + in.bits(kDistExtra[ds]);
    if (d > out.total) return false;                // before the start of the file
    out.copy(d, len);
  }
}

bool inflate(Input &in, Output &out) {
  // the code tables are large for the emulator's 32 KB stack
  std::vector<Huffman> tables(2);
  Huffman &lit = tables[0], &dist = tables[1];
  for (;;) {
    const bool last = in.bits(1);
    const uint32_t type = in.bits(2);
    if (type == 0) {                                // stored: byte-aligned LEN, NLEN, the bytes
      in.bitBuf = 0;
      in.bitCnt = 0;
      const uint32_t len = in.byte() | (in.byte() << 8);
      const uint32_t nlen = in.byte() | (in.byte() << 8);
      if (len != (~nlen & 0xFFFF)) return false;
      for (uint32_t i = 0; i < len && !in.err; i++) out.put((uint8_t)in.byte());
      if (in.err || out.err) return false;
    } else if (type == 1) {                         // the fixed codes
      uint8_t l[288];
      int i = 0;
      for (; i < 144; i++) l[i] = 8;
      for (; i < 256; i++) l[i] = 9;
      for (; i < 280; i++) l[i] = 7;
      for (; i < 288; i++) l[i] = 8;
      build(lit, l, 288);
      for (i = 0; i < 30; i++) l[i] = 5;
      build(dist, l, 30);
      if (!codes(in, out, lit, dist)) return false;
    } else if (type == 2) {                         // codes sent in the block
      const int nlen = (int)in.bits(5) + 257, ndist = (int)in.bits(5) + 1, ncode = (int)in.bits(4) + 4;
      static const uint8_t kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
      if (nlen > 286 || ndist > 30) return false;
      uint8_t l[320];
      memset(l, 0, sizeof l);
      for (int i = 0; i < ncode; i++) l[kOrder[i]] = (uint8_t)in.bits(3);
      if (!build(lit, l, 19)) return false;
      for (int i = 0; i < nlen + ndist;) {
        const int sym = decode(in, lit);
        if (sym < 0 || in.err) return false;
        if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
        uint8_t v = 0;
        int rep;
        if (sym == 16) {
          if (!i) return false;
          v = l[i - 1];
          rep = 3 + (int)in.bits(2);
        } else if (sym == 17) {
          rep = 3 + (int)in.bits(3);
        } else {
          rep = 11 + (int)in.bits(7);
        }
        if (i + rep > nlen + ndist) return false;
        while (rep--) l[i++] = v;
      }
      if (!l[256]) return false;                    // no end-of-block code
      if (!build(lit, l, nlen) || !build(dist, l + nlen, ndist)) return false;
      if (!codes(in, out, lit, dist)) return false;
    } else {
      return false;
    }
    if (last) return !in.err && !out.err;
  }
}

// ---- the zip -----------------------------------------------------------------------------------

struct Entry {
  std::string name;              // made safe for the card: '/' separators, no "..", no ':'
  bool dir;
  uint16_t method;
  uint32_t crc, csize, usize, local;
};

// A name of more than 64 bytes as its start, a hash of the whole and its end, which keeps the
// "(Disk 2) [!].ima" the archives put last. Game sets name their images and zips with every detail
// ("Police Quest - ... (Sierra On-Line, Inc.) (1.44M) (Disk 1) [!].ima", 140 bytes), and the full
// path to one went past the desktop's 260-character paths and the 256-byte lines the .ini is read in.
std::string shortName(const std::string &n) {
  constexpr size_t kMax = 64, kHead = 20, kTail = 36;
  if (n.size() <= kMax) return n;
  size_t head = kHead, tail = n.size() - kTail;
  while (head && ((unsigned char)n[head] & 0xC0) == 0x80) head--;      // not inside a UTF-8 character
  while (tail < n.size() && ((unsigned char)n[tail] & 0xC0) == 0x80) tail++;
  char hash[8];
  snprintf(hash, sizeof hash, "%06x", (unsigned)(crc32(0, (const uint8_t *)n.data(), n.size()) & 0xFFFFFF));
  return n.substr(0, head) + "~" + hash + "~" + n.substr(tail);
}

// A zip name as a path under the folder, "" when it would leave it ("../x", "/x", "C:x").
std::string safeName(std::string n) {
  for (char &c : n) {
    if (c == '\\') c = '/';
    else if (strchr(":*?\"<>|", c) || (unsigned char)c < 32) c = '_';
  }
  if (n.empty() || n[0] == '/') return "";
  std::string out;
  size_t s = 0;
  while (s <= n.size()) {
    size_t e = n.find('/', s);
    if (e == std::string::npos) e = n.size();
    const std::string part = n.substr(s, e - s);
    if (part == "..") return "";
    if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + shortName(part);
    s = e + 1;
  }
  return out;
}

// Calls fn(entry) for each file and folder in the zip's central directory, in order, and stops
// when fn returns false; false when the directory is unreadable or fn stopped. Nothing is kept:
// on the RP2350 the heap has about 10 KB while a PC runs, and a list of every entry (a name each)
// ran out of it. fn may move around the file; the walk seeks back to the next entry itself.
template <class Fn> bool eachEntry(FILE *f, Fn fn) {
  if (fseek(f, 0, SEEK_END) != 0) return false;
  const long size = ftell(f);
  if (size < 22) return false;
  // the end-of-central-directory record: the last 22 bytes, or before a comment of up to 64 KB,
  // looked for from the end in 1 KB steps that overlap by its 22 bytes
  uint8_t t[1024], eocd[22];
  bool found = false;
  const long lowest = size > 65557 ? size - 65557 : 0;
  for (long end = size; !found && end - 22 >= lowest; end -= (long)sizeof t - 21) {
    const long start = end - (long)sizeof t > lowest ? end - (long)sizeof t : lowest;
    const size_t n = (size_t)(end - start);
    if (fseek(f, start, SEEK_SET) != 0 || fread(t, 1, n, f) != n) return false;
    for (long i = (long)n - 22; i >= 0 && !found; i--)
      if (get32(t + i) == 0x06054b50) { memcpy(eocd, t + i, 22); found = true; }
    if (start == lowest) break;
  }
  if (!found) return false;
  const uint32_t count = get16(eocd + 10), cdSize = get32(eocd + 12), cdOff = get32(eocd + 16);
  if ((long)cdOff + (long)cdSize > size) return false;    // also a Zip64 archive (0xFFFFFFFF)
  for (uint32_t p = 0, i = 0; i < count; i++) {
    uint8_t h[46];
    if (p + 46 > cdSize || fseek(f, (long)(cdOff + p), SEEK_SET) != 0 || fread(h, 1, 46, f) != 46 ||
        get32(h) != 0x02014b50)
      return false;
    const uint32_t nameLen = get16(h + 28), skip = get16(h + 30) + get16(h + 32);
    const uint32_t next = p + 46 + nameLen + skip;
    if (next > cdSize) return false;
    std::string raw(nameLen, ' ');
    if (nameLen && fread(&raw[0], 1, nameLen, f) != nameLen) return false;
    Entry e;
    e.name = safeName(raw);
    e.dir = !raw.empty() && (raw.back() == '/' || raw.back() == '\\');
    e.method = (uint16_t)get16(h + 10);
    e.crc = get32(h + 16);
    e.csize = get32(h + 20);
    e.usize = get32(h + 24);
    e.local = get32(h + 42);
    if (get16(h + 8) & 1) {
      hal_log("zip: %s is encrypted, skipped", raw.c_str());
    } else if (e.name.empty()) {
      hal_log("zip: unsafe name %s, skipped", raw.c_str());
    } else if (!fn(e)) {
      return false;
    }
    p = next;
  }
  return true;
}

// Writes one file entry to path.
bool extract(FILE *zf, const Entry &e, const std::string &path) {
  uint8_t lh[30];
  if (fseek(zf, e.local, SEEK_SET) != 0 || fread(lh, 1, 30, zf) != 30 || get32(lh) != 0x04034b50) return false;
  if (fseek(zf, (long)e.local + 30 + get16(lh + 26) + get16(lh + 28), SEEK_SET) != 0) return false;
  FILE *of = fopen(path.c_str(), "wb");
  if (!of) {
    hal_log("zip: cannot write %s", path.c_str());
    return false;
  }
  std::vector<Input> inBox(1);                       // its 1 KB buffer: not on the stack either
  Input &in = inBox[0];
  in.f = zf;
  in.left = e.csize;
  Output out;
  out.f = of;
  bool ok;
  if (e.method == 0) {
    for (uint32_t i = 0; i < e.csize && !in.err && !out.err; i++) out.put((uint8_t)in.byte());
    ok = !in.err;
  } else if (e.method == 8) {
    ok = inflate(in, out);
  } else {
    hal_log("zip: %s uses compression method %u", e.name.c_str(), (unsigned)e.method);
    ok = false;
  }
  out.finish();
  ok = ok && !out.err && out.total == e.usize && out.crc == e.crc;
  if (fclose(of) != 0) ok = false;
  if (!ok) {
    hal_log("zip: %s failed (%u of %u bytes)", e.name.c_str(), (unsigned)out.total, (unsigned)e.usize);
    remove(path.c_str());
  }
  return ok;
}

// Empties a folder under /pico-386/.unzipped (a stale or half-unpacked one). One entry at a time,
// reopening the folder after each, so no list of names takes heap (and nothing is removed from
// a folder while it is being read). It stops at the first entry that will not go.
void removeTree(const std::string &dir) {
  for (;;) {
    std::string p;
    if (DIR *d = opendir(dir.c_str())) {
      while (dirent *e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { p = dir + "/" + e->d_name; break; }
      closedir(d);
    }
    if (p.empty()) return;
    if (isDir(p)) {
      removeTree(p);
      if (rmdir(p.c_str()) != 0) return;
    } else if (remove(p.c_str()) != 0) {
      return;
    }
  }
}

std::string readMarker(const std::string &dir) {
  FILE *f = fopen((dir + "/" + kMarker).c_str(), "r");
  if (!f) return "";
  char buf[512];
  const size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}

bool unpack(const char *zip, const std::string &dir, const std::string &title) {
  FILE *zf = fopen(zip, "rb");
  if (!zf) return false;
  // the whole directory is read once before anything is removed, so a broken zip leaves the
  // folder as it was
  size_t count = 0;
  if (!eachEntry(zf, [&count](const Entry &) { count++; return true; })) {
    hal_log("zip: %s is not a zip file", zip);
    fclose(zf);
    return false;
  }
  removeTree(dir);
  makeDirs(dir);
  size_t i = 0;
  uint32_t shown = 0;
  const bool ok = eachEntry(zf, [&](const Entry &e) {
    const std::string path = dir + "/" + e.name;
    if (i == 0 || hal_millis() - shown > 250) {      // the progress, a few times a second
      shown = hal_millis();
      const std::string line = std::to_string(i + 1) + " of " + std::to_string(count) + " files";
      ui::message(("Unpacking " + title).c_str(), line.c_str());
    }
    i++;
    if (e.dir) { makeDirs(path); return true; }
    const size_t slash = path.find_last_of('/');
    makeDirs(path.substr(0, slash));
    return extract(zf, e, path);
  });
  fclose(zf);
  hal_log("zip: %s, %u entries to %s: %s", zip, (unsigned)count, dir.c_str(), ok ? "ok" : "FAILED");
  return ok;
}

bool isFloppyImage(const std::string &path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  const char *ext = path.c_str() + dot;
  if (strcasecmp(ext, ".ima") && strcasecmp(ext, ".img") && strcasecmp(ext, ".vfd")) return false;
  struct stat st;
  return stat(path.c_str(), &st) == 0 && st.st_size > 0 && st.st_size <= 2949120;
}

// Each name is looked at as it is read, not gathered first: a game of hundreds of files would not
// fit the RP2350's heap as a list.
void findImages(const std::string &dir, std::vector<std::string> &out, int depth) {
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;                       // also the marker
    const std::string p = dir + "/" + e->d_name;
    if (isFloppyImage(p)) out.push_back(p);
    else if (depth < 3 && isDir(p)) findImages(p, out, depth + 1);
  }
  closedir(d);
}

// Case-blind, with runs of digits compared as numbers.
bool naturalLess(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
      unsigned long x = 0, y = 0;
      while (i < a.size() && isdigit((unsigned char)a[i])) x = x * 10 + (a[i++] - '0');
      while (j < b.size() && isdigit((unsigned char)b[j])) y = y * 10 + (b[j++] - '0');
      if (x != y) return x < y;
      continue;
    }
    const int ca = tolower((unsigned char)a[i++]), cb = tolower((unsigned char)b[j++]);
    if (ca != cb) return ca < cb;
  }
  return a.size() - i < b.size() - j;
}

} // namespace

std::vector<std::string> pczip_images(const std::string &dir) {
  std::vector<std::string> out;
  findImages(dir, out, 0);
  // by disk first: a shortened name has its hash ahead of the "(Disk 2)"
  std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) {
    const std::string la = pczip_disk_label(a), lb = pczip_disk_label(b);
    return naturalLess(la, lb) || (!naturalLess(lb, la) && naturalLess(a, b));
  });
  return out;
}

std::string pczip_set_dir(const std::string &path) {
  const std::string top = sdPath("pico-386/.unzipped") + "/";
  if (path.size() <= top.size() || strncasecmp(path.c_str(), top.c_str(), top.size())) return "";
  const size_t end = path.find('/', top.size());
  return end == std::string::npos ? "" : path.substr(0, end);
}

std::string pczip_source(const std::string &dir) {
  const std::string m = readMarker(dir);
  return m.substr(0, m.find('\n'));
}

std::string pczip_disk_label(const std::string &path) {
  std::string name = path.substr(path.find_last_of('/') + 1);
  for (size_t i = 0; i + 6 < name.size(); i++) {
    if (strncasecmp(name.c_str() + i, "disk", 4)) continue;
    size_t k = i + 4;
    while (k < name.size() && (name[k] == ' ' || name[k] == '_' || name[k] == '-')) k++;
    size_t e = k;
    while (e < name.size() && isalnum((unsigned char)name[e]) && e - k < 3) e++;
    // "Disk 2", "Disk 2 of 5", "Disk A"; not "Diskette"
    if (e > k && (e == name.size() || !isalpha((unsigned char)name[e])) &&
        (isdigit((unsigned char)name[k]) || e - k == 1))
      return "Disk " + name.substr(k, e - k);
  }
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

extern "C" int pczip_is_zip(const char *path) {
  const size_t n = path ? strlen(path) : 0;
  return n > 4 && !strcasecmp(path + n - 4, ".zip");
}

extern "C" int pczip_folder(const char *zip, char *out, size_t outSize) {
  struct stat st;
  if (stat(zip, &st) != 0) return -1;
  std::string base = zip;
  const size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  base = base.substr(0, base.size() - 4);
  base = shortName(base);
  while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();   // not on FAT
  if (base.empty()) base = "zip";
  const std::string dir = sdPath(("pico-386/.unzipped/" + base).c_str());
  if ((size_t)snprintf(out, outSize, "%s", dir.c_str()) >= outSize) return -1;

  char id[512];
  snprintf(id, sizeof id, "%s\n%ld %ld\n", zip, (long)st.st_size, (long)st.st_mtime);
  if (readMarker(dir) == id) return 0;             // unpacked before, from this very zip

  pcsnap_touch(dir.c_str());                       // a D: snapshot of the old contents is stale
  if (!unpack(zip, dir, base)) {
    remove((dir + "/" + kMarker).c_str());
    return -1;
  }
  FILE *f = fopen((dir + "/" + kMarker).c_str(), "w");
  if (!f) return -1;
  fputs(id, f);
  fclose(f);
  return 0;
}

extern "C" int pczip_floppy(const char *zip, char *out, size_t outSize) {
  if (pczip_folder(zip, out, outSize) != 0) return -1;
  const std::vector<std::string> images = pczip_images(out);
  if (images.empty()) return 0;
  if ((size_t)snprintf(out, outSize, "%s", images[0].c_str()) >= outSize) return -1;
  return 1;
}
