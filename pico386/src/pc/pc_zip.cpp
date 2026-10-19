// pc_zip.cpp -- .zip files as disks. A zip (a game, as the DOS game archives come) is unpacked
// once to /pc-386/.unzipped/<zip name>/, and that folder is what the guest gets, through the
// folder disk (pc_folder.cpp). So a zip mounts as D: like any folder, and what the guest writes
// there (saved games, settings) is kept in the unpacked folder, not in the zip.
//
// A marker file in the folder records the zip it came from (path, size, date). The next start
// with the same zip reuses the folder as it is. If the zip changed, or the last unpacking did not
// finish, the folder is emptied and unpacked again.
//
// The inflater is our own, written from RFC 1951: zips only use "stored" and "deflated", and
// neither the desktop nor the P4 build has zlib.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "pc_zip.h"
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
  uint8_t buf[4096];
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

// The inflated bytes: the 32 KB window deflate copies from, written to the file each time it fills.
struct Output {
  FILE *f;
  std::vector<uint8_t> win = std::vector<uint8_t>(32768);
  uint32_t total = 0, crc = 0;
  bool err = false;              // the card is full

  void flush(size_t n) {
    crc = crc32(crc, win.data(), n);
    if (fwrite(win.data(), 1, n, f) != n) err = true;
  }
  void put(uint8_t b) {
    win[total & 32767] = b;
    if ((++total & 32767) == 0) flush(32768);
  }
  void finish() { if (total & 32767) flush(total & 32767); }
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
    for (uint32_t i = 0; i < len; i++) out.put(out.win[(out.total - d) & 32767]);
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
    if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
    s = e + 1;
  }
  return out;
}

bool readDirectory(FILE *f, std::vector<Entry> &entries) {
  if (fseek(f, 0, SEEK_END) != 0) return false;
  const long size = ftell(f);
  if (size < 22) return false;
  // the end-of-central-directory record: the last 22 bytes, or before a comment of up to 64 KB
  const long tail = size < 65557 ? size : 65557;
  std::vector<uint8_t> t((size_t)tail);
  if (fseek(f, size - tail, SEEK_SET) != 0 || fread(t.data(), 1, t.size(), f) != t.size()) return false;
  long eocd = -1;
  for (long i = tail - 22; i >= 0 && eocd < 0; i--)
    if (get32(&t[i]) == 0x06054b50) eocd = i;
  if (eocd < 0) return false;
  const uint32_t count = get16(&t[eocd + 10]), cdSize = get32(&t[eocd + 12]), cdOff = get32(&t[eocd + 16]);
  if ((long)cdOff + (long)cdSize > size) return false;    // also a Zip64 archive (0xFFFFFFFF)
  std::vector<uint8_t> cd(cdSize);
  if (fseek(f, cdOff, SEEK_SET) != 0 || fread(cd.data(), 1, cd.size(), f) != cd.size()) return false;
  for (uint32_t p = 0, i = 0; i < count; i++) {
    if (p + 46 > cdSize || get32(&cd[p]) != 0x02014b50) return false;
    const uint8_t *h = &cd[p];
    const uint32_t nameLen = get16(h + 28), next = p + 46 + nameLen + get16(h + 30) + get16(h + 32);
    if (next > cdSize) return false;
    Entry e;
    const std::string raw((const char *)h + 46, nameLen);
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
    } else {
      entries.push_back(e);
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
  std::vector<Input> inBox(1);                       // its 4 KB buffer: not on the stack either
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

// Empties a folder under /pc-386/.unzipped (a stale or half-unpacked one).
void removeTree(const std::string &dir) {
  if (DIR *d = opendir(dir.c_str())) {
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    for (const std::string &n : names) {
      const std::string p = dir + "/" + n;
      if (isDir(p)) {
        removeTree(p);
        rmdir(p.c_str());
      } else {
        remove(p.c_str());
      }
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
  std::vector<Entry> entries;
  if (!readDirectory(zf, entries)) {
    hal_log("zip: %s is not a zip file", zip);
    fclose(zf);
    return false;
  }
  removeTree(dir);
  makeDirs(dir);
  bool ok = true;
  uint32_t shown = 0;
  for (size_t i = 0; ok && i < entries.size(); i++) {
    const Entry &e = entries[i];
    const std::string path = dir + "/" + e.name;
    if (i == 0 || hal_millis() - shown > 250) {      // the progress, a few times a second
      shown = hal_millis();
      const std::string line = std::to_string(i + 1) + " of " + std::to_string(entries.size()) + " files";
      ui::message(("Unpacking " + title).c_str(), line.c_str());
    }
    if (e.dir) { makeDirs(path); continue; }
    const size_t slash = path.find_last_of('/');
    makeDirs(path.substr(0, slash));
    ok = extract(zf, e, path);
  }
  fclose(zf);
  hal_log("zip: %s, %u entries to %s: %s", zip, (unsigned)entries.size(), dir.c_str(), ok ? "ok" : "FAILED");
  return ok;
}

} // namespace

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
  while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();   // not on FAT
  if (base.empty()) base = "zip";
  const std::string dir = sdPath(("pc-386/.unzipped/" + base).c_str());
  if ((size_t)snprintf(out, outSize, "%s", dir.c_str()) >= outSize) return -1;

  char id[512];
  snprintf(id, sizeof id, "%s\n%ld %ld\n", zip, (long)st.st_size, (long)st.st_mtime);
  if (readMarker(dir) == id) return 0;             // unpacked before, from this very zip

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
