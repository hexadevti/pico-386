// pc_keymap.cpp -- the PicoCalc keyboard as a PC keyboard (and, in mouse mode, a PS/2 mouse).
//
// The PicoCalc sends characters, not key positions: Shift+1 arrives as '!'. Each character is
// turned back into the US-layout key that types it, and Shift is pressed or released around it
// when the guest's Shift state does not already match. Modifiers, arrows and F-keys go through
// as themselves. tiny386 takes Linux input codes (1..95 = the set-1 scancode, 96.. = extended).
//
// The keyboard sends one press and one release per key, so the PC typematic repeat (500 ms, then
// 30/s) is made here, for the last key pressed, as a real keyboard does it.
//
// Mouse mode (Ctrl+Tab, or the in-game menu): arrows move the pointer, accelerating while held,
// Alt is the left button and Right Shift the right button.
#include <string.h>
#include "pc_internal.h"
#include "pc_glue.h"
#include "../hal/hal.h"

namespace {

// Linux input codes
enum : uint8_t {
  K_ESC = 1, K_1 = 2, K_0 = 11, K_MINUS = 12, K_EQUAL = 13, K_BACKSPACE = 14, K_TAB = 15,
  K_Q = 16, K_LBRACE = 26, K_RBRACE = 27, K_ENTER = 28, K_LCTRL = 29, K_A = 30,
  K_SEMICOLON = 39, K_APOSTROPHE = 40, K_GRAVE = 41, K_LSHIFT = 42, K_BACKSLASH = 43, K_Z = 44,
  K_COMMA = 51, K_DOT = 52, K_SLASH = 53, K_RSHIFT = 54, K_LALT = 56, K_SPACE = 57,
  K_CAPS = 58, K_F1 = 59, K_F10 = 68,
  K_HOME = 102, K_UP = 103, K_PGUP = 104, K_LEFT = 105, K_RIGHT = 106, K_END = 107,
  K_DOWN = 108, K_PGDN = 109, K_INSERT = 110, K_DELETE = 111,
};

constexpr uint8_t kShift = 0x80;   // in s_ascii: the character needs Shift

uint8_t s_ascii[128];              // ASCII -> code | kShift
bool s_built;

void build() {
  auto set = [](char c, uint8_t code, bool shift) { s_ascii[(uint8_t)c] = code | (shift ? kShift : 0); };
  const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
  const uint8_t first[3] = { K_Q, K_A, K_Z };
  for (int r = 0; r < 3; r++)
    for (int i = 0; rows[r][i]; i++) {
      set(rows[r][i], first[r] + i, false);
      set(rows[r][i] - 32, first[r] + i, true);
    }
  for (int i = 0; i < 9; i++) set('1' + i, K_1 + i, false);
  set('0', K_0, false);
  const char *shifted = "!@#$%^&*()";
  for (int i = 0; i < 10; i++) set(shifted[i], K_1 + i, true);
  set('-', K_MINUS, false);      set('_', K_MINUS, true);
  set('=', K_EQUAL, false);      set('+', K_EQUAL, true);
  set('[', K_LBRACE, false);     set('{', K_LBRACE, true);
  set(']', K_RBRACE, false);     set('}', K_RBRACE, true);
  set(';', K_SEMICOLON, false);  set(':', K_SEMICOLON, true);
  set('\'', K_APOSTROPHE, false); set('"', K_APOSTROPHE, true);
  set('`', K_GRAVE, false);      set('~', K_GRAVE, true);
  set('\\', K_BACKSLASH, false); set('|', K_BACKSLASH, true);
  set(',', K_COMMA, false);      set('<', K_COMMA, true);
  set('.', K_DOT, false);        set('>', K_DOT, true);
  set('/', K_SLASH, false);      set('?', K_SLASH, true);
  set(' ', K_SPACE, false);
  set(KEY_BACKSPACE, K_BACKSPACE, false);
  set(KEY_TAB, K_TAB, false);
  set(KEY_ENTER, K_ENTER, false);
  set('\r', K_ENTER, false);
  // Ctrl+letter may arrive as the control character: ^A..^Z (minus the three that are keys)
  for (int c = 1; c <= 26; c++)
    if (c != KEY_BACKSPACE && c != KEY_TAB && c != KEY_ENTER && c != '\r') set((char)c, s_ascii['a' + c - 1], false);
  s_built = true;
}

// PicoCalc code -> (Linux code, needs Shift?, keeps the guest's Shift as it is?)
struct Mapped { uint8_t code; bool shift; bool raw; };

Mapped mapKey(uint8_t k) {
  if (k < 128) {
    const uint8_t m = s_ascii[k];
    return { (uint8_t)(m & 0x7f), (m & kShift) != 0, false };
  }
  if (k >= KEY_F1 && k <= KEY_F5) return { (uint8_t)(K_F1 + k - KEY_F1), false, true };
  if (k >= KEY_F6 && k <= KEY_F9) return { (uint8_t)(K_F1 + 5 + k - KEY_F6), false, true };
  switch (k) {
    case KEY_F10:     return { K_F10, false, true };
    case KEY_ESC:     return { K_ESC, false, true };
    case KEY_UP:      return { K_UP, false, true };
    case KEY_DOWN:    return { K_DOWN, false, true };
    case KEY_LEFT:    return { K_LEFT, false, true };
    case KEY_RIGHT:   return { K_RIGHT, false, true };
    case KEY_HOME:    return { K_HOME, false, true };
    case KEY_END:     return { K_END, false, true };
    case KEY_PGUP:    return { K_PGUP, false, true };
    case KEY_PGDN:    return { K_PGDN, false, true };
    case KEY_INSERT:  return { K_INSERT, false, true };
    case KEY_DEL:     return { K_DELETE, false, true };
    case KEY_CAPS:    return { K_CAPS, false, true };
    case KEY_CTRL:    return { K_LCTRL, false, true };
    case KEY_ALT:     return { K_LALT, false, true };
    case KEY_SHIFT_L: return { K_LSHIFT, false, true };
    case KEY_SHIFT_R: return { K_RSHIFT, false, true };
  }
  return { 0, false, false };
}

uint8_t s_down[256];                // PicoCalc code -> the Linux code it pressed (0 = up)
bool s_lshift, s_rshift;            // what the guest believes
uint8_t s_repeatKey;                // PicoCalc code of the key that repeats
uint32_t s_repeatAt;
bool s_needRepeatTime;

bool s_mouseMode;
int s_mouseButtons;
uint32_t s_mouseHeldSince;

void send(bool down, uint8_t code) {
  if (code) t386_key(down ? 1 : 0, code);
}

void setGuestShift(bool want) {
  const bool have = s_lshift || s_rshift;
  if (want == have) return;
  if (want) {
    send(true, K_LSHIFT);
    s_lshift = true;
  } else {
    if (s_lshift) send(false, K_LSHIFT);
    if (s_rshift) send(false, K_RSHIFT);
    s_lshift = s_rshift = false;
  }
}

// Back to the Shift the user really holds (the PicoCalc keys, not our temporary presses).
void restoreShift() {
  const bool l = s_down[KEY_SHIFT_L] != 0, r = s_down[KEY_SHIFT_R] != 0 && !s_mouseMode;
  if (s_lshift != l) { send(l, K_LSHIFT); s_lshift = l; }
  if (s_rshift != r) { send(r, K_RSHIFT); s_rshift = r; }
}

// A character key: the press, with Shift fixed up just for it.
void pressChar(const Mapped &m) {
  setGuestShift(m.shift);
  send(true, m.code);
  restoreShift();
}

bool mouseKey(uint8_t code) {
  return code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT ||
         code == KEY_ALT || code == KEY_SHIFT_R;
}

void mouseButtons() {
  const int b = (s_down[KEY_ALT] ? 1 : 0) | (s_down[KEY_SHIFT_R] ? 2 : 0);
  if (b != s_mouseButtons) {
    s_mouseButtons = b;
    t386_mouse(0, 0, b);
  }
}

} // namespace

void pckbd_reset() {
  if (!s_built) build();
  for (int i = 0; i < 256; i++)
    if (s_down[i]) { send(false, s_down[i]); s_down[i] = 0; }
  if (s_lshift) send(false, K_LSHIFT);
  if (s_rshift) send(false, K_RSHIFT);
  s_lshift = s_rshift = false;
  s_repeatKey = 0;
  if (s_mouseButtons) t386_mouse(0, 0, 0);
  s_mouseButtons = 0;
}

void pckbd_set_mouse_mode(bool on) {
  pckbd_reset();
  s_mouseMode = on;
}

bool pckbd_mouse_mode() { return s_mouseMode; }

void pckbd_ctrl_alt_del() {
  pckbd_reset();
  send(true, K_LCTRL);
  send(true, K_LALT);
  send(true, K_DELETE);
  send(false, K_DELETE);
  send(false, K_LALT);
  send(false, K_LCTRL);
}

void pckbd_event(uint8_t code, uint8_t state) {
  if (!s_built) build();
  if (code == KEY_RELEASE_ALL) { pckbd_reset(); return; }
  const bool down = state != KEYSTATE_RELEASED;
  if (state == KEYSTATE_HELD) return;          // the repeat is ours (pckbd_tick)

  // Ctrl+Tab: mouse mode on/off
  if (down && code == KEY_TAB && s_down[KEY_CTRL]) {
    pckbd_set_mouse_mode(!s_mouseMode);
    return;
  }

  if (s_mouseMode && mouseKey(code)) {
    if (down) {
      s_down[code] = 1;                        // marks it held; nothing goes to the keyboard
      if (code != KEY_ALT && code != KEY_SHIFT_R) s_mouseHeldSince = hal_millis();
    } else {
      s_down[code] = 0;
    }
    mouseButtons();
    return;
  }

  if (!down) {
    if (s_down[code]) {
      send(false, s_down[code]);
      if (code == KEY_SHIFT_L) s_lshift = false;
      if (code == KEY_SHIFT_R) s_rshift = false;
      s_down[code] = 0;
    }
    if (code == s_repeatKey) s_repeatKey = 0;
    return;
  }

  if (s_down[code]) return;                    // already down
  const Mapped m = mapKey(code);
  if (!m.code) return;
  s_down[code] = m.code;
  if (m.raw) {
    send(true, m.code);
    if (code == KEY_SHIFT_L) s_lshift = true;
    if (code == KEY_SHIFT_R) s_rshift = true;
  } else {
    pressChar(m);
  }
  // Modifiers do not repeat.
  if (code != KEY_CTRL && code != KEY_ALT && code != KEY_SHIFT_L && code != KEY_SHIFT_R && code != KEY_CAPS) {
    s_repeatKey = code;
    s_needRepeatTime = true;
  }
}

void pckbd_tick(uint32_t nowMs) {
  if (s_repeatKey && s_down[s_repeatKey]) {
    if (s_needRepeatTime) {
      s_repeatAt = nowMs + 500;
      s_needRepeatTime = false;
    } else if ((int32_t)(nowMs - s_repeatAt) >= 0) {
      const Mapped m = mapKey(s_repeatKey);
      if (m.raw) send(true, m.code);
      else pressChar(m);
      s_repeatAt = nowMs + 33;
    }
  }

  if (s_mouseMode) {
    const int dx = (s_down[KEY_RIGHT] ? 1 : 0) - (s_down[KEY_LEFT] ? 1 : 0);
    const int dy = (s_down[KEY_DOWN] ? 1 : 0) - (s_down[KEY_UP] ? 1 : 0);
    if (dx || dy) {
      // 1 px a frame at first, up to 8 after about a second
      const uint32_t held = nowMs - s_mouseHeldSince;
      const int speed = held < 150 ? 1 : held > 1000 ? 8 : 1 + (int)(held - 150) * 7 / 850;
      t386_mouse(dx * speed, dy * speed, s_mouseButtons);
    }
  }
}
