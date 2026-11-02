// keyscript.h -- the PC_KEYS script format, shared by the desktop's test hook (hal_sdl.cpp) and the
// RP2350's serial remote (input_rp2350.cpp, "@keys ...").
//
// A script is comma-separated tokens. A key name (up down left right enter esc bs tab del caps
// f1..f10 home end pgup pgdn space ctrl alt shift rshift) or a single character is pressed and
// released; "ctrl+esc" holds the first keys while tapping the last ("ctrl+alt+del" holds two);
// wNNN waits NNN ms. Any other token (shot, quit) goes back to the caller.
#pragma once

#include <stdint.h>
#include <stddef.h>

struct KeyScript {
  const char *s = nullptr;       // the rest of the script; the caller owns the text
  uint32_t waitUntil = 0;
  uint8_t held = 0, mods[2] = {}, nmods = 0;

  void start(const char *script) { s = script; held = nmods = 0; waitUntil = 0; }
  bool busy(uint32_t now) const { return held || (s && (*s || (int32_t)(now - waitUntil) < 0)); }

  // Call it often. `idle`: the key queue is empty, so the last key has been read. push(code,
  // state) queues a key. Returns true with a token that is not a key, NUL-terminated in tok.
  bool step(uint32_t now, bool idle, void (*push)(uint8_t code, uint8_t state), char *tok, size_t n);
};
