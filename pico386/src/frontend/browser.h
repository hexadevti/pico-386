// browser.h -- the ROM picker.
#pragma once

#include <string>
#include "../../core.h"

// Lists folders and the core's ROM files, starting where the browser last was (else
// /pc-386). true + the full path when a ROM was chosen, false on Esc at the top.
struct BrowseOpts {
  const char *title = nullptr;          // instead of the core's name
  // Called for each file with a matching extension: false hides it; tag (empty by default) is
  // shown at the right of the row instead of the extension.
  bool (*accept)(const std::string &path, std::string &tag) = nullptr;
  // The tag of a file, asked only for the rows on screen. For tags that need the file's size:
  // on a FAT card each stat searches the folder from its start, so stat-ing every file of a big
  // folder up front (as accept must) takes seconds.
  std::string (*tag)(const std::string &path) = nullptr;
  const char *exts = nullptr;           // instead of the core's romExts (".bin")
  // Opens here (a full path) instead of the last folder, and leaves the last folder alone.
  const char *startDir = nullptr;
  // Picks a folder instead of a file: files are hidden, and every folder but the card's root
  // has a first row "Use this folder", which returns it.
  bool pickDir = false;
};
bool browseRom(const EmuCore &core, std::string &outPath, const BrowseOpts *opts = nullptr);
