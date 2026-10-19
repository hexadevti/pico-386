// pc_keymap.cpp -- the PicoCalc keyboard as a PC keyboard (and, in mouse mode, a PS/2 mouse).
//
// The PicoCalc sends characters, not key positions: Shift+1 arrives as '!'. Each character is
// turned back into the US-layout key that types it, and Shift is pressed or released around it
// when the guest's Shift state does not already match. Modifiers, arrows and F-keys go through
// as themselves. tiny386 takes Linux input codes (1..95 = the set-1 scancode, 96.. = extended).
//
// Caps Lock is kept here and never reaches the guest. The PicoCalc's STM32 applies its own Caps
// Lock to the letters it sends (the desktop does not), so a guest Caps Lock on top of that turned
// 'A' (sent as Shift+A) back into 'a'. Instead, with Caps Lock on, a letter's case is set here
// from the Shift keys held: upper without Shift, lower with it, whatever case it arrived in.
//
// Ctrl+Alt+Del also comes from Ctrl+Alt+Backspace: on the PicoCalc, Del with Ctrl and Alt held
// did not reach the guest (2026-10-19), and Backspace sits next to it.
//
// The keyboard sends one press and one release per key, so the PC typematic repeat (500 ms, then
// 30/s) is made here, for the last key pressed, as a real keyboard does it.
//
// The keyboard mouse has three modes (MOUSE_*), cycled by pressing both Shifts together, Ctrl+Tab
// or the in-game menu: Arrows (the arrows are arrow keys), Mouse (the arrows move the pointer,
// accelerating while held) and Both (the arrows do both, for games that use the keyboard and the
// mouse). In Mouse and Both, the two button keys (Alt and Right Shift unless the startup picks
// others) are the mouse buttons and nothing else.
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
  K_F1 = 59, K_F10 = 68,
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
    case KEY_CTRL:    return { K_LCTRL, false, true };
    case KEY_ALT:     return { K_LALT, false, true };
    case KEY_SHIFT_L: return { K_LSHIFT, false, true };
    case KEY_SHIFT_R: return { K_RSHIFT, false, true };
  }
  return { 0, false, false };
}

uint8_t s_down[256];                // PicoCalc code -> the Linux code it pressed (0 = up)
bool s_lshift, s_rshift;            // what the guest believes
bool s_caps;                        // our Caps Lock (the guest's stays off)
bool s_shiftL, s_shiftR;            // the Shift keys physically held, buttons or not
uint8_t s_repeatKey;                // PicoCalc code of the key that repeats
uint32_t s_repeatAt;
bool s_needRepeatTime;

int s_mouseMode;                    // MOUSE_*
uint8_t s_button[2] = { KEY_ALT, KEY_SHIFT_R };   // the left and right button keys (0 = none)
bool s_buttonDown[2];
bool s_arrow[4];                    // the pointer's arrows held: left, right, up, down
bool s_arrowShifted[4];             // ...pressed as their Shift form (PgUp etc.)
int s_mouseButtons;
uint32_t s_mouseHeldSince;

// The keys a mouse button can be: name in the .ini, name in the menu, PicoCalc code.
struct ButtonKey { const char *ini, *label; uint8_t code; };
const ButtonKey kButtonKeys[] = {
  { "alt", "Alt", KEY_ALT },          { "rshift", "Right Shift", KEY_SHIFT_R },
  { "lshift", "Left Shift", KEY_SHIFT_L }, { "ctrl", "Ctrl", KEY_CTRL },
  { "space", "Space", ' ' },          { "enter", "Enter", KEY_ENTER },
  { "tab", "Tab", KEY_TAB },          { "bs", "Backspace", KEY_BACKSPACE },
  { "z", "Z", 'z' },                  { "x", "X", 'x' },
  { "none", "None", 0 },
};
constexpr int kNumButtonKeys = sizeof kButtonKeys / sizeof kButtonKeys[0];

int buttonKeyIndex(uint8_t code) {
  for (int i = 0; i < kNumButtonKeys; i++)
    if (kButtonKeys[i].code == code) return i;
  return kNumButtonKeys - 1;
}

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

// Back to the Shift the user really holds (the PicoCalc keys, not our temporary presses). A Shift
// that is a mouse button never reaches s_down.
void restoreShift() {
  const bool l = s_down[KEY_SHIFT_L] != 0, r = s_down[KEY_SHIFT_R] != 0;
  if (s_lshift != l) { send(l, K_LSHIFT); s_lshift = l; }
  if (s_rshift != r) { send(r, K_RSHIFT); s_rshift = r; }
}

// A character key: the press, with Shift fixed up just for it.
void pressChar(const Mapped &m) {
  setGuestShift(m.shift);
  send(true, m.code);
  restoreShift();
}

uint8_t lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Which mouse button code is (0 left, 1 right), or -1. Letters match in either case.
int buttonOf(uint8_t code) {
  for (int b = 0; b < 2; b++)
    if (s_button[b] && lower(code) == s_button[b]) return b;
  return -1;
}

int arrowOf(uint8_t code) {
  switch (code) {
    case KEY_LEFT: return 0;
    case KEY_RIGHT: return 1;
    case KEY_UP: return 2;
    case KEY_DOWN: return 3;
  }
  return -1;
}

// The STM32 sends Shift+Up/Down as PgUp/PgDn, so while a Shift held as a mouse button drags, the
// pointer's vertical arrows arrive as those. Shift+Left/Right it sends not at all (seen on the
// device's serial log, 2026-10-19): a Shift button cannot drag sideways.
int shiftedArrowOf(uint8_t code) {
  switch (code) {
    case KEY_PGUP: return 2;
    case KEY_PGDN: return 3;
  }
  return -1;
}

void mouseButtons() {
  const int b = (s_buttonDown[0] ? 1 : 0) | (s_buttonDown[1] ? 2 : 0);
  if (b != s_mouseButtons) {
    s_mouseButtons = b;
    t386_mouse(0, 0, b);
  }
}

// mapKey with our Caps Lock: a letter is upper case unless Shift is held. The key stays filed
// under the code it arrived as, since its release comes with that code.
Mapped mapKeyCaps(uint8_t code) {
  Mapped m = mapKey(code);
  if (s_caps && ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')))
    m.shift = !(s_down[KEY_SHIFT_L] || s_down[KEY_SHIFT_R]);
  return m;
}

} // namespace

void pckbd_reset() {
  if (!s_built) build();
  for (int i = 0; i < 256; i++)
    if (s_down[i]) { send(false, s_down[i]); s_down[i] = 0; }
  if (s_lshift) send(false, K_LSHIFT);
  if (s_rshift) send(false, K_RSHIFT);
  s_lshift = s_rshift = false;
  s_shiftL = s_shiftR = false;
  s_repeatKey = 0;
  memset(s_arrow, 0, sizeof s_arrow);
  memset(s_arrowShifted, 0, sizeof s_arrowShifted);
  s_buttonDown[0] = s_buttonDown[1] = false;
  if (s_mouseButtons) t386_mouse(0, 0, 0);
  s_mouseButtons = 0;
}

void pckbd_set_mouse_mode(int mode) {
  pckbd_reset();
  s_mouseMode = mode >= MOUSE_ARROWS && mode <= MOUSE_BOTH ? mode : MOUSE_ARROWS;
}

int pckbd_mouse_mode() { return s_mouseMode; }

const char *pckbd_mouse_mode_name(int mode) {
  return mode == MOUSE_ON ? "Mouse" : mode == MOUSE_BOTH ? "Both" : "Arrows";
}

void pckbd_set_mouse_button(int button, const char *iniName) {
  for (const ButtonKey &k : kButtonKeys)
    if (!strcmp(k.ini, iniName)) {
      pckbd_reset();
      s_button[button & 1] = k.code;
      return;
    }
}

const char *pckbd_mouse_button(int button) { return kButtonKeys[buttonKeyIndex(s_button[button & 1])].ini; }
const char *pckbd_mouse_button_label(int button) { return kButtonKeys[buttonKeyIndex(s_button[button & 1])].label; }

void pckbd_next_mouse_button(int button, int dir) {
  const int i = (buttonKeyIndex(s_button[button & 1]) + (dir < 0 ? kNumButtonKeys - 1 : 1)) % kNumButtonKeys;
  pckbd_reset();
  s_button[button & 1] = kButtonKeys[i].code;
}

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

  // Both Shifts together, or Ctrl+Tab: the next mouse mode. The first Shift has already gone
  // through as itself (or as a button press); the reset lets go of it.
  if (code == KEY_SHIFT_L) s_shiftL = down;
  if (code == KEY_SHIFT_R) s_shiftR = down;
  if (down && ((code == KEY_SHIFT_L && s_shiftR) || (code == KEY_SHIFT_R && s_shiftL) ||
               (code == KEY_TAB && s_down[KEY_CTRL]))) {
    pckbd_set_mouse_mode((s_mouseMode + 1) % 3);
    return;
  }

  if (s_mouseMode != MOUSE_ARROWS) {
    const int b = buttonOf(code);
    if (b >= 0) {
      s_buttonDown[b] = down;
      mouseButtons();
      return;
    }
    // PgUp/PgDn are arrows while a Shift is held that is a button (or, in Mouse, any Shift), and
    // their release always ends an arrow they started. They never reach the guest.
    const int sa = shiftedArrowOf(code);
    if (sa >= 0) {
      const bool shiftButton = (s_shiftL && buttonOf(KEY_SHIFT_L) >= 0) || (s_shiftR && buttonOf(KEY_SHIFT_R) >= 0);
      const bool shifted = s_mouseMode == MOUSE_ON ? (s_shiftL || s_shiftR) : shiftButton;
      if (down ? shifted : (shifted || s_arrowShifted[sa])) {
        if (down && !s_arrow[0] && !s_arrow[1] && !s_arrow[2] && !s_arrow[3]) s_mouseHeldSince = hal_millis();
        s_arrow[sa] = down;
        s_arrowShifted[sa] = down;
        return;
      }
    }
    const int a = arrowOf(code);
    if (a >= 0) {
      s_arrowShifted[a] = false;
      if (down && !s_arrow[0] && !s_arrow[1] && !s_arrow[2] && !s_arrow[3]) s_mouseHeldSince = hal_millis();
      s_arrow[a] = down;
      if (s_mouseMode == MOUSE_ON) return;     // Both: the arrow key too
    }
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

  if (code == KEY_CAPS) { s_caps = !s_caps; return; }
  if (s_down[code]) return;                    // already down
  Mapped m = mapKeyCaps(code);
  if (s_down[KEY_CTRL] && s_down[KEY_ALT]) {
    hal_log("kbd: Ctrl+Alt+0x%02X", code);       // what the PicoCalc sends with both held
    if (code == KEY_BACKSPACE || code == KEY_DEL) m = { K_DELETE, false, true };
  }
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
  if (code != KEY_CTRL && code != KEY_ALT && code != KEY_SHIFT_L && code != KEY_SHIFT_R) {
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
      const Mapped m = mapKeyCaps(s_repeatKey);
      if (m.raw) send(true, m.code);
      else pressChar(m);
      s_repeatAt = nowMs + 33;
    }
  }

  if (s_mouseMode != MOUSE_ARROWS) {
    const int dx = (s_arrow[1] ? 1 : 0) - (s_arrow[0] ? 1 : 0);
    const int dy = (s_arrow[3] ? 1 : 0) - (s_arrow[2] ? 1 : 0);
    if (dx || dy) {
      // 1 px a frame at first, up to 8 after about a second
      const uint32_t held = nowMs - s_mouseHeldSince;
      const int speed = held < 150 ? 1 : held > 1000 ? 8 : 1 + (int)(held - 150) * 7 / 850;
      t386_mouse(dx * speed, dy * speed, s_mouseButtons);
    }
  }
}
