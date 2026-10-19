// pc_disks.cpp -- the PC's drives: what kind of disk an image is, the drive letters a machine
// .ini mounts (A:, B:, C:, CD), the drive picker, and the "Create a hard disk" tool.
//
// A new hard disk is ready for DOS: an MBR with boot code and one active FAT16 partition, already
// formatted. Its boot sector only prints "no DOS yet" until DOS's SYS (or an installer) replaces
// it. Sizes stop at 504 MB, the most a disk can have with the 16-head, 63-sector geometry
// tiny386 gives raw images and still need no BIOS translation, so the partition table, the BIOS
// and every DOS agree on the CHS layout.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include "pc_internal.h"
#include "../frontend/ui.h"
#include "../frontend/browser.h"
#include "../frontend/settings.h"
#include "../hal/hal.h"
#include "../../core.h"

extern const EmuCore kPC386;

namespace {

long fileSize(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

bool hasExt(const std::string &p, const char *ext) {
  const size_t n = strlen(ext);
  return p.size() > n && !strcasecmp(p.c_str() + p.size() - n, ext);
}

std::string baseName(const std::string &p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

// "1.44 MB", "720 KB", "128 MB"
std::string sizeText(long bytes) {
  char b[24];
  if (bytes == 1474560) return "1.44 MB";
  if (bytes == 2949120) return "2.88 MB";
  if (bytes == 1228800) return "1.2 MB";
  if (bytes < 1024 * 1024) snprintf(b, sizeof b, "%ld KB", bytes >> 10);
  else snprintf(b, sizeof b, "%ld MB", (bytes + (1 << 19)) >> 20);
  return b;
}

std::string kindTag(DiskKind k, long size) {
  switch (k) {
    case DISK_FLOPPY: return "Floppy " + sizeText(size);
    case DISK_HARD:   return "Hard disk " + sizeText(size);
    case DISK_CD:     return "CD " + sizeText(size);
    case DISK_ZIP:    return "Zip " + sizeText(size);
    default:          return "";
  }
}

// The browser hook: only disks of the wanted kind, tagged with what they are. Floppies and hard
// disks differ only in size, so choosing one of them stats every image of the folder.
DiskKind s_wanted;

bool acceptDisk(const std::string &path, std::string &tag) {
  long size;
  const DiskKind k = pcdisk_kind(path, &size);
  if (k == DISK_NONE || (s_wanted != DISK_NONE && k != s_wanted)) return false;
  tag = kindTag(k, size);
  return true;
}

// ---- the .ini [pc] keys ------------------------------------------------------------------------

std::vector<std::string> readLines(const std::string &path) {
  std::vector<std::string> lines;
  FILE *f = fopen(path.c_str(), "r");
  if (!f) return lines;
  char buf[256];
  while (fgets(buf, sizeof buf, f)) {
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    lines.push_back(buf);
  }
  fclose(f);
  return lines;
}

// "key = value" in line: the value (trimmed) when the key matches. commented: a leading ';'.
bool matchKey(const std::string &line, const char *key, bool commented, std::string *value) {
  const char *s = line.c_str();
  while (*s == ' ' || *s == '\t') s++;
  if (commented) {
    if (*s != ';') return false;
    s++;
    while (*s == ' ' || *s == '\t') s++;
  }
  const size_t n = strlen(key);
  if (strncasecmp(s, key, n)) return false;
  s += n;
  while (*s == ' ' || *s == '\t') s++;
  if (*s != '=') return false;
  s++;
  while (*s == ' ' || *s == '\t') s++;
  if (value) {
    *value = s;
    while (!value->empty() && (value->back() == ' ' || value->back() == '\t')) value->pop_back();
  }
  return true;
}

// A section's line range [first, end); both are lines.size() when it is missing.
void iniSection(const std::vector<std::string> &lines, const char *name, size_t &first, size_t &end) {
  const std::string head = std::string("[") + name + "]";
  first = end = lines.size();
  for (size_t i = 0; i < lines.size(); i++) {
    const char *s = lines[i].c_str();
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '[') continue;
    if (first != lines.size()) { end = i; return; }
    if (!strncasecmp(s, head.c_str(), head.size())) first = i + 1;
  }
}

void pcSection(const std::vector<std::string> &lines, size_t &first, size_t &end) {
  iniSection(lines, "pc", first, end);
}

// ---- making a hard disk ------------------------------------------------------------------------

// Boot code, assembled from small 16-bit sources kept in comments here.
//
// MBR (loaded at 0:7C00): moves itself to 0:0600, finds the active partition, reads its first
// sector to 0:7C00 with INT 13h AH=42h (CHS AH=02h as a fallback), checks 55AA and jumps to it with
// DL = the boot drive and DS:SI = the partition entry. Errors print "No active partition",
// "Disk read error" or "Partition not bootable", wait for a key and INT 19h.
const uint8_t kMbrCode[] = {
  0xfa, 0x31, 0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0x8e, 0xd8, 0x8e, 0xc0, 0xfb, 0xfc, 0xbe, 0x00,
  0x7c, 0xbf, 0x00, 0x06, 0xb9, 0x00, 0x01, 0xf3, 0xa5, 0xea, 0x1e, 0x06, 0x00, 0x00, 0xbe, 0xbe,
  0x07, 0xb9, 0x04, 0x00, 0xf6, 0x04, 0x80, 0x75, 0x0a, 0x83, 0xc6, 0x10, 0xe2, 0xf6, 0xbe, 0x91,
  0x06, 0xeb, 0x3a, 0x89, 0xf5, 0x66, 0x8b, 0x44, 0x08, 0x66, 0xa3, 0x89, 0x06, 0xbe, 0x81, 0x06,
  0xb4, 0x42, 0xcd, 0x13, 0x73, 0x15, 0x8a, 0x76, 0x01, 0x8b, 0x4e, 0x02, 0xbb, 0x00, 0x7c, 0xb8,
  0x01, 0x02, 0xcd, 0x13, 0x73, 0x05, 0xbe, 0xa5, 0x06, 0xeb, 0x12, 0x81, 0x3e, 0xfe, 0x7d, 0x55,
  0xaa, 0x75, 0x07, 0x89, 0xee, 0xea, 0x00, 0x7c, 0x00, 0x00, 0xbe, 0xb5, 0x06, 0xac, 0x84, 0xc0,
  0x74, 0x09, 0xb4, 0x0e, 0xbb, 0x07, 0x00, 0xcd, 0x10, 0xeb, 0xf2, 0x31, 0xc0, 0xcd, 0x16, 0xcd,
  0x19, 0x10, 0x00, 0x01, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x4e, 0x6f, 0x20, 0x61, 0x63, 0x74, 0x69, 0x76, 0x65, 0x20, 0x70, 0x61, 0x72, 0x74, 0x69,
  0x74, 0x69, 0x6f, 0x6e, 0x00, 0x44, 0x69, 0x73, 0x6b, 0x20, 0x72, 0x65, 0x61, 0x64, 0x20, 0x65,
  0x72, 0x72, 0x6f, 0x72, 0x00, 0x50, 0x61, 0x72, 0x74, 0x69, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x6e,
  0x6f, 0x74, 0x20, 0x62, 0x6f, 0x6f, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x00,
};

// The partition's boot sector code (at offset 3Eh, after the FAT16 BPB): prints the message
// below, waits for a key and INT 19h, so the BIOS boots again (from A: if a floppy is in).
const uint8_t kVbrCode[] = {
  0xfa, 0x31, 0xc0, 0x8e, 0xd8, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0xfb, 0xfc, 0xbe, 0x61, 0x7c, 0xac,
  0x84, 0xc0, 0x74, 0x09, 0xb4, 0x0e, 0xbb, 0x07, 0x00, 0xcd, 0x10, 0xeb, 0xf2, 0x31, 0xc0, 0xcd,
  0x16, 0xcd, 0x19,
  // "\r\nThis hard disk has no DOS yet.\r\nBoot a DOS floppy (A:) and install DOS on C:.\r\n
  //  Press a key to try again.\r\n"
  0x0d, 0x0a, 0x54, 0x68, 0x69, 0x73, 0x20, 0x68, 0x61, 0x72, 0x64, 0x20, 0x64,
  0x69, 0x73, 0x6b, 0x20, 0x68, 0x61, 0x73, 0x20, 0x6e, 0x6f, 0x20, 0x44, 0x4f, 0x53, 0x20, 0x79,
  0x65, 0x74, 0x2e, 0x0d, 0x0a, 0x42, 0x6f, 0x6f, 0x74, 0x20, 0x61, 0x20, 0x44, 0x4f, 0x53, 0x20,
  0x66, 0x6c, 0x6f, 0x70, 0x70, 0x79, 0x20, 0x28, 0x41, 0x3a, 0x29, 0x20, 0x61, 0x6e, 0x64, 0x20,
  0x69, 0x6e, 0x73, 0x74, 0x61, 0x6c, 0x6c, 0x20, 0x44, 0x4f, 0x53, 0x20, 0x6f, 0x6e, 0x20, 0x43,
  0x3a, 0x2e, 0x0d, 0x0a, 0x50, 0x72, 0x65, 0x73, 0x73, 0x20, 0x61, 0x20, 0x6b, 0x65, 0x79, 0x20,
  0x74, 0x6f, 0x20, 0x74, 0x72, 0x79, 0x20, 0x61, 0x67, 0x61, 0x69, 0x6e, 0x2e, 0x0d, 0x0a, 0x00,
};

constexpr int kHeads = 16, kSpt = 63;      // tiny386's geometry for raw images (ide.c)
constexpr uint32_t kPartStart = kSpt;      // the partition starts on the second track, as DOS does

void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// A partition table CHS triple for an LBA.
void putChs(uint8_t *p, uint32_t lba) {
  const uint32_t c = lba / (kHeads * kSpt), h = lba / kSpt % kHeads, s = lba % kSpt + 1;
  p[0] = (uint8_t)h;
  p[1] = (uint8_t)(s | ((c >> 2) & 0xC0));
  p[2] = (uint8_t)c;
}

bool writeAt(FILE *f, uint32_t lba, const uint8_t *data, size_t bytes) {
  const bool ok = fseek(f, (long)lba * 512, SEEK_SET) == 0 && fwrite(data, 1, bytes, f) == bytes;
  if (!ok) hal_log("hard disk: write of %u bytes at sector %u failed", (unsigned)bytes, (unsigned)lba);
  return ok;
}

// Writes the whole image. sizeMB is rounded down to whole cylinders.
bool makeHardDisk(const std::string &path, int sizeMB) {
  const uint32_t cyl = (uint32_t)sizeMB * 2048 / (kHeads * kSpt);
  const uint32_t total = cyl * kHeads * kSpt;
  const uint32_t part = total - kPartStart;

  // FAT16 with the cluster sizes DOS's FORMAT picks
  const uint32_t spc = part <= 32680 ? 2 : part <= 262144 ? 4 : part <= 524288 ? 8 : 16;
  const uint32_t rsvd = 1, rootEnt = 512, rootSecs = rootEnt * 32 / 512, nFats = 2;
  const uint32_t tmp2 = 256 * spc + nFats;
  const uint32_t fatSz = (part - rsvd - rootSecs + tmp2 - 1) / tmp2;

  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  std::vector<uint8_t> zero(512 * kPartStart, 0);   // the largest run of zeros: the gap after the MBR
  uint8_t s[512];
  bool ok = true;

  // MBR
  memset(s, 0, sizeof s);
  memcpy(s, kMbrCode, sizeof kMbrCode);
  const uint32_t sig = hal_micros() ^ (uint32_t)sizeMB * 2654435761u;
  put32(s + 0x1B8, sig);                               // disk signature
  uint8_t *pe = s + 0x1BE;
  pe[0] = 0x80;                                        // active
  putChs(pe + 1, kPartStart);
  pe[4] = part < 65536 ? 0x04 : 0x06;                  // FAT16 under / over 32 MB
  putChs(pe + 5, total - 1);
  put32(pe + 8, kPartStart);
  put32(pe + 12, part);
  s[510] = 0x55; s[511] = 0xAA;
  ok = ok && writeAt(f, 0, s, 512);
  ok = ok && writeAt(f, 1, zero.data(), (kPartStart - 1) * 512);

  // the partition's boot sector, with the BPB
  memset(s, 0, sizeof s);
  s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
  memcpy(s + 3, "PICO386 ", 8);
  put16(s + 11, 512);
  s[13] = (uint8_t)spc;
  put16(s + 14, rsvd);
  s[16] = (uint8_t)nFats;
  put16(s + 17, rootEnt);
  put16(s + 19, part < 65536 ? part : 0);
  s[21] = 0xF8;                                        // fixed disk
  put16(s + 22, fatSz);
  put16(s + 24, kSpt);
  put16(s + 26, kHeads);
  put32(s + 28, kPartStart);                           // hidden sectors
  put32(s + 32, part < 65536 ? 0 : part);
  s[36] = 0x80;                                        // drive number
  s[38] = 0x29;                                        // extended boot signature
  put32(s + 39, sig ^ 0x5A5A1234);                     // volume serial
  memcpy(s + 43, "NO NAME    ", 11);
  memcpy(s + 54, "FAT16   ", 8);
  memcpy(s + 62, kVbrCode, sizeof kVbrCode);
  s[510] = 0x55; s[511] = 0xAA;
  ok = ok && writeAt(f, kPartStart, s, 512);

  // both FATs and the root directory: zeros, with the two reserved FAT entries
  const uint32_t fat0 = kPartStart + rsvd;
  for (uint32_t lba = fat0; ok && lba < fat0 + nFats * fatSz + rootSecs; lba += 32) {
    const uint32_t n = fat0 + nFats * fatSz + rootSecs - lba;
    ok = writeAt(f, lba, zero.data(), (n < 32 ? n : 32) * 512);
  }
  memset(s, 0, sizeof s);
  s[0] = 0xF8; s[1] = 0xFF; s[2] = 0xFF; s[3] = 0xFF;
  for (uint32_t i = 0; ok && i < nFats; i++) ok = writeAt(f, fat0 + i * fatSz, s, 512);

  // The data area is never read before DOS writes it, so the file is only extended to its full
  // size (FAT on the card allocates the clusters without writing them).
  ok = ok && writeAt(f, total - 1, zero.data(), 512);
  if (fclose(f) != 0) ok = false;
  if (!ok) remove(path.c_str());
  hal_log("hard disk %s: %u sectors, %u-sector clusters, FAT %u sectors, %s", path.c_str(),
          (unsigned)total, (unsigned)spc, (unsigned)fatSz, ok ? "ok" : "FAILED");
  return ok;
}

bool confirm(const char *title, const char *question) {
  ui::Item items[2] = { { "No", nullptr, ui::ICON_EXIT }, { "Yes", nullptr, ui::ICON_PLAY, question } };
  ui::Screen scr;
  scr.title = title;
  scr.hints = "Enter|Select;Esc|Back";
  int sel = 0;
  return ui::list(scr, items, 2, &sel) == KEY_ENTER && sel == 1;
}

const char *const kLetters[DRIVE_COUNT] = { "A:", "B:", "C:", "D:", "CD" };
const char *const kDriveNames[DRIVE_COUNT] = {
  "A: floppy", "B: floppy", "C: hard disk", "D: SD folder", "CD-ROM" };
const DiskKind kDriveKind[DRIVE_COUNT] = { DISK_FLOPPY, DISK_FLOPPY, DISK_HARD, DISK_FOLDER, DISK_CD };

} // namespace

// ---- what a disk image is ----------------------------------------------------------------------

DiskKind pcdisk_kind(const std::string &path, long *sizeOut) {
  const long size = fileSize(path);
  if (sizeOut) *sizeOut = size;
  if (size <= 0) return DISK_NONE;
  if (hasExt(path, ".iso")) return DISK_CD;
  if (hasExt(path, ".zip")) return DISK_ZIP;
  // tiny386 takes the floppy formats up to 2.88 MB; anything larger is a hard disk
  if (size <= 2949120) return DISK_FLOPPY;
  return DISK_HARD;
}

std::string pcdisk_tag(const std::string &path) {
  long size;
  const DiskKind k = pcdisk_kind(path, &size);
  return kindTag(k, size);
}

std::string pcdisk_rel(const std::string &path) {
  const std::string root = hal_sd_root();
  if (!path.compare(0, root.size(), root) && path.size() > root.size() && path[root.size()] == '/')
    return path.substr(root.size() + 1);
  return path;
}

std::string pcdisk_abs(const std::string &rel) {
  if (rel.empty() || rel[0] == '/' || (rel.size() > 1 && rel[1] == ':')) return rel;
  return sdPath(rel.c_str());
}

bool pcdisk_browse(DiskKind kind, const char *title, std::string &out, const char *startDir) {
  s_wanted = kind;
  BrowseOpts o;
  o.title = title;
  o.startDir = startDir;
  // Any disk, a CD or a zip is known by its extension alone: the sizes are read only for the rows
  // on screen, so a folder of hundreds of games opens at once.
  if (kind == DISK_NONE || kind == DISK_CD || kind == DISK_ZIP) {
    o.exts = kind == DISK_CD ? ".iso" : kind == DISK_ZIP ? ".zip" : ".img.ima.iso.vhd.zip";
    o.tag = pcdisk_tag;
  } else {
    o.exts = ".img.ima.vhd";
    o.accept = acceptDisk;
  }
  return browseRom(kPC386, out, &o);
}

// ---- drive letters in a machine .ini ----------------------------------------------------------

const char *pcdisk_drive_key(const std::string &ini, int drive) {
  switch (drive) {
    case DRIVE_A: return "fda";
    case DRIVE_B: return "fdb";
    case DRIVE_C: return "hda";
    case DRIVE_D: return "hdb";                                          // the primary slave
    default:      return pcini_get(ini, "cda").empty() ? "cdc" : "cda";   // a hand-written .ini may use cda
  }
}

int pcdisk_cd_ide(const std::string &ini) { return pcini_get(ini, "cda").empty() ? 2 : 0; }

std::string pcini_get(const std::string &ini, const char *key) {
  const std::vector<std::string> lines = readLines(ini);
  size_t first, end;
  pcSection(lines, first, end);
  std::string v;
  for (size_t i = first; i < end; i++)
    if (matchKey(lines[i], key, false, &v)) return v;
  return "";
}

bool pcini_set(const std::string &ini, const char *key, const std::string &value) {
  return pcini_set_in(ini, "pc", key, value);
}

std::string pcini_get_in(const std::string &ini, const char *section, const char *key) {
  const std::vector<std::string> lines = readLines(ini);
  size_t first, end;
  iniSection(lines, section, first, end);
  std::string v;
  for (size_t i = first; i < end; i++)
    if (matchKey(lines[i], key, false, &v)) return v;
  return "";
}

bool pcini_set_in(const std::string &ini, const char *section, const char *key, const std::string &value) {
  std::vector<std::string> lines = readLines(ini);
  size_t first, end;
  iniSection(lines, section, first, end);
  if (first == lines.size()) {                         // no such section yet
    if (!lines.empty() && !lines.back().empty()) lines.push_back("");
    lines.push_back(std::string("[") + section + "]");
    first = end = lines.size();
  }
  // the active line, else a commented-out example to reuse, else a new line at the section's end
  size_t at = end;
  for (size_t i = first; i < end && at == end; i++)
    if (matchKey(lines[i], key, false, nullptr)) at = i;
  for (size_t i = first; i < end && at == end; i++)
    if (matchKey(lines[i], key, true, nullptr)) at = i;
  const std::string line = value.empty() ? std::string(";") + key + " =" : std::string(key) + " = " + value;
  if (at == end) {
    while (at > first && lines[at - 1].empty()) at--;    // before the blank line closing the section
    lines.insert(lines.begin() + at, line);
  } else {
    lines[at] = line;
  }
  FILE *f = fopen(ini.c_str(), "w");
  if (!f) return false;
  for (const std::string &l : lines) fprintf(f, "%s\n", l.c_str());
  return fclose(f) == 0;
}

std::string pcdisk_drive(const std::string &ini, int drive) {
  return pcini_get(ini, pcdisk_drive_key(ini, drive));
}

bool pcdisk_set_drive(const std::string &ini, int drive, const std::string &image) {
  return pcini_set(ini, pcdisk_drive_key(ini, drive), image.empty() ? "" : pcdisk_rel(image));
}

std::string pcdisk_drive_label(const std::string &rel) {
  if (rel.empty()) return "Empty";
  if (isDir(pcdisk_abs(rel))) return rel[0] == '/' ? rel : "/" + rel;
  std::string b = baseName(rel);
  if (fileSize(pcdisk_abs(rel)) < 0) b += " (missing)";
  return b;
}

const char *pcdisk_drive_name(int drive) { return kDriveNames[drive]; }
const char *pcdisk_drive_letter(int drive) { return kLetters[drive]; }

int pcdisk_choose(int drive, const std::string &current, const char *note, std::string &out) {
  const bool loaded = !current.empty();
  std::string title = std::string(kDriveNames[drive]);
  const char *what = drive == DRIVE_C ? "hard disk" : drive == DRIVE_CD ? "CD image" : "floppy";
  const std::string pick = std::string("Choose a ") + what + " image";
  const std::string letter = kLetters[drive];
  // D: is a folder or a zip (a game as it was downloaded), unpacked to a folder when the PC
  // starts. A: and B: take those too, as a 1.44 MB floppy, besides their images.
  const bool floppy = drive == DRIVE_A || drive == DRIVE_B;
  enum { ROW_IMAGE, ROW_FOLDER, ROW_ZIP, ROW_EJECT };
  int rows[4], n = 0;
  if (drive != DRIVE_D) rows[n++] = ROW_IMAGE;
  if (floppy || drive == DRIVE_D) { rows[n++] = ROW_FOLDER; rows[n++] = ROW_ZIP; }
  if (loaded) rows[n++] = ROW_EJECT;
  ui::Item items[4];
  for (int i = 0; i < n; i++) {
    switch (rows[i]) {
      case ROW_IMAGE:  items[i] = { pick.c_str(), nullptr, ui::ICON_FOLDER, note }; break;
      case ROW_FOLDER: items[i] = { "Choose a folder", nullptr, ui::ICON_FOLDER, note }; break;
      case ROW_ZIP:    items[i] = { "Choose a zip file", nullptr, ui::ICON_LOAD, note }; break;
      default:         items[i] = { drive >= DRIVE_C ? "Remove it" : "Eject", nullptr, ui::ICON_EXIT, note }; break;
    }
  }
  int sel = 0;
  const std::string sub = loaded ? "Now: " + pcdisk_drive_label(current) : std::string("Now: empty");
  ui::Screen scr;
  scr.title = title.c_str();
  scr.subtitle = sub.c_str();
  scr.hints = "Enter|Select;Esc|Back";
  for (;;) {
    const uint8_t k = ui::list(scr, items, n, &sel);
    if (k == KEY_ESC || k == KEY_BACKSPACE) return 0;
    if (k != KEY_ENTER) continue;
    const int row = rows[sel];
    if (row == ROW_EJECT) { out.clear(); return 2; }
    // the browsers open beside what the drive holds now, so the next game is a key away
    std::string start = loaded ? pcdisk_abs(current) : std::string();
    if (loaded) start = start.substr(0, start.find_last_of('/'));
    if (row == ROW_ZIP) {
      if (pcdisk_browse(DISK_ZIP, ("Zip file for " + letter).c_str(), out, isDir(start) ? start.c_str() : nullptr))
        return 1;
      continue;
    }
    if (row == ROW_FOLDER) {
      if (!isDir(start)) start = hal_sd_root();
      BrowseOpts o;
      const std::string t = "Folder for " + letter;
      o.title = t.c_str();
      o.pickDir = true;
      o.startDir = start.c_str();
      if (!browseRom(kPC386, out, &o)) continue;
      // C: and the other images are open while the PC runs; that folder must not be shared too
      if (out == sdPath("pc-386")) {
        ui::message("Not this folder", "It holds the PC's own disks", 1800);
        continue;
      }
      return 1;
    }
    if (pcdisk_browse(kDriveKind[drive], title.c_str(), out)) return 1;
  }
}

// ---- the drives screen of a machine, before it starts -----------------------------------------

int pcdisk_machine_menu(const std::string &ini, const char *title, bool canDelete) {
  // Start, the drives, the boot order, then the startup's own rows
  enum { ROW_START = 0, ROW_DRIVES = 1, ROW_BOOT = ROW_DRIVES + DRIVE_COUNT, ROW_RENAME, ROW_DELETE };
  int sel = 0;
  for (;;) {
    std::string vals[DRIVE_COUNT];
    ui::Item items[ROW_DELETE + 1];
    const bool cdFirst = pcini_get_in(ini, "pico", "boot") == "cd";
    items[ROW_START] = { "Start", nullptr, ui::ICON_PLAY,
                         cdFirst ? "Boot order: A: (if a floppy is in), CD, C:"
                                 : "Boots from A: if a floppy is in, else from C:" };
    for (int d = 0; d < DRIVE_COUNT; d++) {
      const std::string rel = pcdisk_drive(ini, d);
      vals[d] = pcdisk_drive_label(rel);
      items[ROW_DRIVES + d] = { kDriveNames[d], vals[d].c_str(),
                                d == DRIVE_C ? ui::ICON_SAVE : d == DRIVE_D ? ui::ICON_FOLDER : ui::ICON_LOAD };
    }
    items[ROW_BOOT] = { "Boot from", cdFirst ? "CD, then C:" : "C:", ui::ICON_SKIP };
    items[ROW_BOOT].adjust = true;
    items[ROW_RENAME] = { "Rename", nullptr, ui::ICON_GEAR };
    items[ROW_DELETE] = { "Delete this startup", nullptr, ui::ICON_EXIT, "Only the settings; the disks stay" };
    ui::Screen scr;
    scr.title = title;
    scr.subtitle = "Pick the disks, then Start";
    scr.hints = "Enter|Select;Esc|Back";
    const uint8_t k = ui::list(scr, items, canDelete ? ROW_DELETE + 1 : ROW_DELETE, &sel);
    if (k == KEY_ESC || k == KEY_BACKSPACE) return MACHINE_BACK;
    if (sel == ROW_BOOT && (k == KEY_ENTER || k == KEY_LEFT || k == KEY_RIGHT)) {
      // SeaBIOS's CMOS boot order (pc_glue.c): without it a CD only boots when C: cannot
      if (!pcini_set_in(ini, "pico", "boot", cdFirst ? "" : "cd")) ui::message("Error", "Cannot write the .ini", 1500);
      continue;
    }
    if (k != KEY_ENTER) continue;
    if (sel == ROW_RENAME) return MACHINE_RENAME;
    if (sel == ROW_DELETE) {
      if (confirm(("Delete " + std::string(title) + "?").c_str(), "The disk images are kept")) return MACHINE_DELETE;
      continue;
    }
    if (sel == ROW_START) {
      const std::string c = pcdisk_drive(ini, DRIVE_C);
      if (!c.empty() && fileSize(pcdisk_abs(c)) < 0) {
        ui::message("C: is missing", (baseName(c) + ": make it with Create a hard disk").c_str(), 2200);
        continue;
      }
      const std::string dd = pcdisk_drive(ini, DRIVE_D);
      if (!dd.empty() && !isDir(pcdisk_abs(dd)) && fileSize(pcdisk_abs(dd)) < 0) {
        ui::message("D: is missing", ((hasExt(dd, ".zip") ? "No zip " : "No folder ") + dd).c_str(), 2200);
        continue;
      }
      return MACHINE_START;
    }
    const int d = sel - ROW_DRIVES;
    std::string path;
    const int r = pcdisk_choose(d, pcdisk_drive(ini, d), nullptr, path);
    if (r && !pcdisk_set_drive(ini, d, path)) ui::message("Error", "Cannot write the .ini", 1500);
  }
}

// ---- Create a hard disk ------------------------------------------------------------------------

void pcdisk_create_menu() {
  struct Size { int mb; const char *label, *sub; };
  static const Size kSizes[] = {
    { 32,  "32 MB",  "DOS and a few programs" },
    { 64,  "64 MB",  "DOS and some games" },
    { 128, "128 MB", "DOS and many games (good for the DOS machine)" },
    { 256, "256 MB", "Windows 3.1 or a big DOS collection" },
    { 504, "504 MB", "Windows 95 (the largest size)" },
  };
  constexpr int kNumSizes = sizeof kSizes / sizeof kSizes[0];

  int sizeSel = 2;
  for (;;) {
    ui::Item items[kNumSizes];
    for (int i = 0; i < kNumSizes; i++) items[i] = { kSizes[i].label, nullptr, ui::ICON_SAVE, kSizes[i].sub };
    ui::Screen scr;
    scr.title = "Create a hard disk";
    scr.subtitle = "1. Size (formatted FAT16, ready for DOS)";
    scr.hints = "Enter|Select;Esc|Back";
    const uint8_t k = ui::list(scr, items, kNumSizes, &sizeSel);
    if (k == KEY_ESC || k == KEY_BACKSPACE) return;
    if (k != KEY_ENTER) continue;

    // the file: the PC's C: drive, or the first free diskN.img
    std::string spare;
    for (int i = 1; i < 100; i++) {
      spare = "disk" + std::to_string(i) + ".img";
      if (fileSize(sdPath(("pc-386/" + spare).c_str())) < 0) break;
    }
    const std::string names[2] = { "c.img", spare };
    const char *const subs[2] = { "C: of the PC (when its .ini is new)", "A spare disk, to mount as C: or D:" };
    std::string vals[2], subText[2];
    ui::Item nitems[2];
    for (int i = 0; i < 2; i++) {
      const bool exists = fileSize(sdPath(("pc-386/" + names[i]).c_str())) >= 0;
      vals[i] = exists ? "replace" : "new";
      subText[i] = subs[i];
      nitems[i] = { names[i].c_str(), vals[i].c_str(), ui::ICON_SAVE, subText[i].c_str() };
    }
    ui::Screen nscr;
    nscr.title = "Create a hard disk";
    nscr.subtitle = "2. File in /pc-386";
    nscr.hints = "Enter|Create;Esc|Back";
    int nameSel = fileSize(sdPath("pc-386/c.img")) < 0 ? 0 : 1;
    const uint8_t nk = ui::list(nscr, nitems, 2, &nameSel);
    if (nk != KEY_ENTER) continue;

    const std::string path = sdPath(("pc-386/" + names[nameSel]).c_str());
    if (fileSize(path) >= 0 &&
        !confirm(("Replace " + names[nameSel] + "?").c_str(), "Every file on that disk is lost"))
      continue;

    makeDirs(sdPath("pc-386"));
    ui::message("Creating", (names[nameSel] + ", " + kSizes[sizeSel].label + "... please wait").c_str());
    if (!makeHardDisk(path, kSizes[sizeSel].mb)) {
      ui::message("Error", "Cannot write the image (card full?)", 2500);
      continue;
    }
    ui::drawBackground();
    ui::message(("Created " + names[nameSel]).c_str(),
                "To install DOS: put a DOS floppy in A:, then Start", 3500);
    return;
  }
}
