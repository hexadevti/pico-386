// keyscript.cpp -- runs a PC_KEYS script (see keyscript.h). No heap: the RP2350 runs it in its
// keyboard task with a 4 KB stack and a few KB of heap to spare.
#include "keyscript.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "hal.h"

static uint8_t keyCode(const char *n, size_t len) {
  static const struct { const char *n; uint8_t c; } kNames[] = {
    { "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
    { "enter", KEY_ENTER }, { "esc", KEY_ESC }, { "bs", KEY_BACKSPACE }, { "tab", KEY_TAB }, { "home", KEY_HOME },
    { "end", KEY_END }, { "pgup", KEY_PGUP }, { "pgdn", KEY_PGDN }, { "space", KEY_SPACE },
    { "ctrl", KEY_CTRL }, { "alt", KEY_ALT }, { "shift", KEY_SHIFT_L }, { "rshift", KEY_SHIFT_R }, { "del", KEY_DEL }, { "caps", KEY_CAPS },
    { "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_F3 }, { "f4", KEY_F4 }, { "f5", KEY_F5 },
    { "f6", KEY_F6 }, { "f7", KEY_F7 }, { "f8", KEY_F8 }, { "f9", KEY_F9 }, { "f10", KEY_F10 },
  };
  for (const auto &k : kNames) if (strlen(k.n) == len && !memcmp(n, k.n, len)) return k.c;
  return len == 1 ? (uint8_t)n[0] : 0;
}

bool KeyScript::step(uint32_t now, bool idle, void (*push)(uint8_t, uint8_t), char *tok, size_t n) {
  if (held && (int32_t)(now - waitUntil) >= 0) {
    push(held, KEYSTATE_RELEASED);
    while (nmods) push(mods[--nmods], KEYSTATE_RELEASED);
    held = 0;
    waitUntil = now + 60;
  }
  if (!s || !*s || held || !idle || (int32_t)(now - waitUntil) < 0) return false;
  const char *t = s, *end = strchr(s, ',');
  const size_t len = end ? (size_t)(end - s) : strlen(s);
  s = end ? end + 1 : s + len;
  if (len > 1 && t[0] == 'w' && isdigit((unsigned char)t[1])) { waitUntil = now + (uint32_t)atoi(t + 1); return false; }
  // the tokens before the last '+' are held (a lone "+" is the character)
  const char *rest = t;
  size_t rlen = len;
  uint8_t m[2];
  int nm = 0;
  for (const char *plus; rlen > 1 && nm < 2 && (plus = (const char *)memchr(rest, '+', rlen));) {
    if (const uint8_t mc = keyCode(rest, (size_t)(plus - rest))) m[nm++] = mc;
    rlen -= (size_t)(plus + 1 - rest);
    rest = plus + 1;
  }
  // "key:NNN" holds the key for NNN ms (the keyboard mouse's arrows)
  uint32_t hold = 80;
  if (const char *colon = rlen > 2 ? (const char *)memchr(rest + 1, ':', rlen - 1) : nullptr) {
    if (isdigit((unsigned char)colon[1])) { hold = (uint32_t)atoi(colon + 1); rlen = (size_t)(colon - rest); }
  }
  const uint8_t c = keyCode(rest, rlen);
  if (!c) {
    if (!n) return false;
    const size_t k = len < n - 1 ? len : n - 1;
    memcpy(tok, t, k);
    tok[k] = 0;
    return true;
  }
  for (int i = 0; i < nm; i++) { push(m[i], KEYSTATE_PRESSED); mods[nmods++] = m[i]; }
  push(c, KEYSTATE_PRESSED);
  held = c;
  waitUntil = now + hold;
  return false;
}
