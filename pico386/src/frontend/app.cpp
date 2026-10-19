// app.cpp -- the frontend (from emu16 / pico-amiga): the home menu (machine profiles, disk images,
// settings), then the machine loop with the in-game menu and the FPS meter.
//
// Keys while the machine runs: every key goes to the guest, except the frontend's Ctrl hotkeys:
//                 Ctrl-Esc = menu.
// Anywhere:       Ctrl-Shift-Up (arrives as PgUp) = esp32loader, Ctrl-Shift-F3 (arrives as F8) = restart
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <string>
#include "app.h"
#include "ui.h"
#include "browser.h"
#include "settings.h"
#include "../hal/hal.h"
#include "../pc/pc_internal.h"
#include "../../core.h"

extern const EmuCore kPC386;
const EmuCore *const kCores[] = { &kPC386 };
const int kNumCores = sizeof(kCores) / sizeof(kCores[0]);

static const char *const kFrameskipNames[] = { "Auto", "1 of 2", "1 of 3", "1 of 4" };
static const char *const kFontSizeNames[] = { "4x12", "4x8" };

// ------------------------------------------------------------------------------------------------
// keyboard -> pad (cores without keyEvent)
// ------------------------------------------------------------------------------------------------
static uint32_t padBit(uint8_t code) {
  switch (code) {
    case KEY_UP:        return PAD_BTN_UP;
    case KEY_DOWN:      return PAD_BTN_DOWN;
    case KEY_LEFT:      return PAD_BTN_LEFT;
    case KEY_RIGHT:     return PAD_BTN_RIGHT;
    case KEY_ALT:       return PAD_BTN_A;
    case KEY_CTRL:      return PAD_BTN_B;
    default:            return 0;
  }
}

// ------------------------------------------------------------------------------------------------
// settings rows shared by the home and in-game menus
// ------------------------------------------------------------------------------------------------
enum SettingRow { ROW_VOLUME, ROW_FRAMESKIP, ROW_FPS, ROW_FONT };

// Returns true when the key changed the row's setting.
static bool adjustSetting(int row, uint8_t key) {
  const int d = key == KEY_LEFT ? -1 : 1;
  switch (row) {
    case ROW_VOLUME:
      g_settings.volume += d;
      if (g_settings.volume < 0) g_settings.volume = 0;
      if (g_settings.volume > 10) g_settings.volume = 10;
      hal_audio_volume(g_settings.volume);
      return true;
    case ROW_FRAMESKIP: g_settings.frameskip = (g_settings.frameskip + d + 4) % 4; return true;
    case ROW_FPS: g_settings.showFps = !g_settings.showFps; return true;
    case ROW_FONT: g_settings.fontSize ^= 1; return true;
  }
  return false;
}

// The settings rows as shown in both menus; selfPaced cores have no frameskip row. Returns the
// count and fills rowOf[] with the SettingRow of each.
static int settingItems(const EmuCore &core, ui::Item *out, int *rowOf) {
  int n = 0;
  rowOf[n] = ROW_VOLUME;
  out[n++] = { "Volume", nullptr, ui::ICON_VOLUME, nullptr, (int8_t)g_settings.volume, true };
  if (!core.selfPaced) {
    rowOf[n] = ROW_FRAMESKIP;
    out[n++] = { "Frameskip", kFrameskipNames[g_settings.frameskip], ui::ICON_SKIP, nullptr, -1, true };
  }
  rowOf[n] = ROW_FPS;
  out[n++] = { "Show FPS", g_settings.showFps ? "On" : "Off", ui::ICON_CHART, nullptr, -1, true };
  rowOf[n] = ROW_FONT;   // 80x25 text cells; pc_video picks it up on the next frame
  out[n++] = { "Font size", kFontSizeNames[g_settings.fontSize], ui::ICON_PAD, nullptr, -1, true };
  return n;
}

// Only bios.bin is offered; the tag says whether vgabios.bin sits beside it.
static bool acceptBios(const std::string &path, std::string &tag) {
  const size_t s = path.find_last_of('/');
  if (strcasecmp(path.c_str() + s + 1, "bios.bin")) return false;
  FILE *f = fopen((path.substr(0, s) + "/vgabios.bin").c_str(), "rb");
  tag = f ? "OK" : "NO VGABIOS";
  if (f) fclose(f);
  return true;
}

// Browses for bios.bin and keeps its folder (relative to the card root) as the BIOS folder.
static void chooseBiosDir(const EmuCore &core) {
  const std::string start = sdPath(g_settings.biosDir.c_str());
  BrowseOpts o;
  o.title = "Choose bios.bin";
  o.accept = acceptBios;
  o.exts = ".bin";
  o.startDir = start.c_str();
  std::string path;
  if (!browseRom(core, path, &o)) return;
  const std::string root = hal_sd_root();
  std::string dir = path.substr(0, path.find_last_of('/'));
  dir = dir.size() > root.size() ? dir.substr(root.size() + 1) : std::string();
  g_settings.biosDir = dir;
}

static void settingsMenu(const EmuCore &core) {
  int sel = 0;
  ui::Screen scr;
  scr.title = "Settings";
  scr.subtitle = "Saved to /pc-386/pico386.cfg";
  scr.hints = "<>|Change;Esc|Back";
  for (;;) {
    ui::Item items[5];
    int rowOf[5];
    int n = settingItems(core, items, rowOf);
    // BIOS folder: home menu only, it takes effect at the next power-on
    const std::string biosShown = "/" + g_settings.biosDir;
    const int biosRow = n;
    items[n++] = { "BIOS folder", biosShown.c_str(), ui::ICON_FOLDER, nullptr, -1, false };
    const uint8_t k = ui::list(scr, items, n, &sel);
    if (k == KEY_ESC || k == KEY_BACKSPACE) break;
    if (sel == biosRow) { if (k == KEY_ENTER) chooseBiosDir(core); continue; }
    adjustSetting(rowOf[sel], k == KEY_ENTER ? KEY_RIGHT : k);
  }
  settingsSave();
}

void pcprof_settings_menu() { settingsMenu(kPC386); }

// ------------------------------------------------------------------------------------------------
// game
// ------------------------------------------------------------------------------------------------
static std::string baseNoExt(const std::string &rom) {
  const size_t s = rom.find_last_of('/');
  std::string base = s == std::string::npos ? rom : rom.substr(s + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base.erase(dot);
  return base;
}

static std::string statePath(const std::string &rom) {
  return sdPath("pc-386/states/") + baseNoExt(rom) + ".st0";
}

static bool saveState(const EmuCore &core, const std::string &rom) {
  if (!core.saveState) return false;
  makeDirs(sdPath("pc-386/states"));
  const std::string p = statePath(rom);
  FILE *f = fopen(p.c_str(), "wb");
  if (!f) return false;
  const bool ok = core.saveState(f);
  fclose(f);
  hal_log("state %s %s", ok ? "saved to" : "FAILED", p.c_str());
  return ok;
}

static bool loadState(const EmuCore &core, const std::string &rom) {
  if (!core.loadState) return false;
  FILE *f = fopen(statePath(rom).c_str(), "rb");
  if (!f) return false;
  const bool ok = core.loadState(f);
  fclose(f);
  return ok;
}

enum class MenuResult { Resume, Quit };

// The in-game menu. Rows: Resume, Reset, [Save, Load], [core rows], settings, Quit.
static MenuResult gameMenu(const EmuCore &core, const std::string &rom) {
  hal_audio_silence();
  hal_video_wait();
  ui::setBackdrop(hal_video_front());
  const std::string title = *pcprof_title() ? pcprof_title() : baseNoExt(rom);
  ui::Screen scr;
  scr.title = title.c_str();
  scr.subtitle = core.name;
  scr.hints = "Enter|Select;<>|Change;Esc|Resume";
  int sel = 0;
  MenuResult result = MenuResult::Resume;
  enum Kind : uint8_t { K_RESUME, K_RESET, K_SAVE, K_LOAD, K_CORE, K_SETTING, K_QUIT };
  for (;;) {
    ui::Item items[24];
    Kind kind[24];
    int arg[24];
    int n = 0;
    items[n] = { "Resume", nullptr, ui::ICON_PLAY }; kind[n++] = K_RESUME;
    items[n] = { "Reset", nullptr, ui::ICON_RESET }; kind[n++] = K_RESET;
    if (core.saveState && core.loadState) {
      items[n] = { "Save state", nullptr, ui::ICON_SAVE }; kind[n++] = K_SAVE;
      items[n] = { "Load state", nullptr, ui::ICON_LOAD }; kind[n++] = K_LOAD;
    }
    if (core.menuItems) {
      CoreMenuItem ci[12];
      const int m = core.menuItems(ci, 12);
      for (int i = 0; i < m; i++) {
        items[n] = { ci[i].label, ci[i].value, ui::ICON_PAD, nullptr, -1, ci[i].adjust };
        kind[n] = K_CORE; arg[n++] = i;
      }
    }
    int rowOf[4];
    const int s = settingItems(core, items + n, rowOf);
    for (int i = 0; i < s; i++) { kind[n + i] = K_SETTING; arg[n + i] = rowOf[i]; }
    n += s;
    items[n] = { "Quit machine", nullptr, ui::ICON_QUIT }; kind[n++] = K_QUIT;

    const uint8_t k = ui::list(scr, items, n, &sel);
    if (k == KEY_ESC) break;
    const bool change = k == KEY_LEFT || k == KEY_RIGHT || k == KEY_ENTER;
    if (kind[sel] == K_SETTING) {
      if (change) adjustSetting(arg[sel], k == KEY_ENTER ? KEY_RIGHT : k);
      continue;
    }
    if (kind[sel] == K_CORE) {
      if (change && core.menuAction(arg[sel], k)) break;
      ui::setBackdrop(hal_video_front());   // a core action may have opened the browser
      continue;
    }
    if (k != KEY_ENTER) continue;
    if (kind[sel] == K_RESUME) break;
    if (kind[sel] == K_RESET) { core.reset(); break; }
    if (kind[sel] == K_SAVE) { ui::message("Save state", saveState(core, rom) ? "Saved" : "Save failed", 600); break; }
    if (kind[sel] == K_LOAD) {
      if (loadState(core, rom)) break;
      ui::message("Load state", "No state for this game", 900);
      continue;
    }
    if (kind[sel] == K_QUIT) { result = MenuResult::Quit; break; }
  }
  settingsSave();
  ui::clearBackdrop();
  if (result == MenuResult::Resume) hal_panel_fill(0, 0, PANEL_W, PANEL_H, 0);
  return result;
}

// The FPS meter sits on the bottom rows of the panel, under a 4:3 picture or 25 text rows.
static void showFps(const EmuCore &core, uint32_t fps, uint32_t drawn, int skip, unsigned load) {
  char b[64];
  if (core.selfPaced && core.fpsText) snprintf(b, sizeof(b), "%u fps  %u drawn  %s", (unsigned)fps, (unsigned)drawn, core.fpsText());
  else if (core.selfPaced) snprintf(b, sizeof(b), "%u fps  %u drawn  cpu %u%%", (unsigned)fps, (unsigned)drawn, load);
  else snprintf(b, sizeof(b), "%u fps  %u drawn  skip %d  cpu %u%%", (unsigned)fps, (unsigned)drawn, skip, load);
  const int y = PANEL_H - 10;
  ui::fill(0, y, PANEL_W, 10, 0);
  ui::text(PANEL_W / 2, y + 5, b, load > 100 ? ui::kWarn : ui::kGood, 1, ui::MC);
  hal_panel_push(0, y, PANEL_W, 10, ui::canvas() + y * PANEL_W, PANEL_W);
  hal_log("%s", b);
}

static void runGame(const EmuCore &core, const std::string &rom) {
  ui::message("Starting", *pcprof_title() ? pcprof_title() : baseNoExt(rom).c_str());
  if (!core.init() || !core.loadRom(rom.c_str())) {
    ui::drawBackground();
    ui::message("Error", core.lastError ? core.lastError() : "Cannot load this ROM", 2500);
    core.shutdown();
    return;
  }
  hal_audio_volume(g_settings.volume);
  hal_panel_fill(0, 0, PANEL_W, PANEL_H, 0);

  uint32_t pad = 0;
  bool ctrl = false;
  int phase = 0, autoSkip = 0, evalCount = 0;
  uint64_t workSum = 0;
  uint32_t fpsT0 = hal_millis(), fpsFrames = 0, fpsDrawn = 0;
  uint64_t fpsWork = 0;

  for (;;) {
    // input
    KeyEvent ev;
    bool menu = false;
    while (hal_key_poll(&ev)) {
      const bool down = ev.state != KEYSTATE_RELEASED;
      if (ev.code == KEY_RELEASE_ALL) {                 // the keyboard lost an event
        pad = 0; ctrl = false;
        if (core.keyEvent) core.keyEvent(ev.code, ev.state);
        continue;
      }
      if (ev.code == KEY_CTRL) ctrl = down;             // and falls through: the guest sees Ctrl too
      if (ev.state == KEYSTATE_PRESSED && ctrl) {
        if (ev.code == KEY_ESC) { menu = true; continue; }
        if (ev.code == KEY_PGUP) hal_reboot_to_loader();
        if (ev.code == KEY_F8) hal_reboot();
      }
      if (core.keyEvent) { core.keyEvent(ev.code, ev.state); continue; }
      const uint32_t b = padBit(ev.code);
      if (down) pad |= b; else pad &= ~b;
    }
    if (menu) {
      pad = 0;
      ctrl = false;
      if (core.keyEvent) core.keyEvent(KEY_RELEASE_ALL, KEYSTATE_RELEASED);
      core.setInput(0, 0);
      hal_job_wait();                                 // a core may still be drawing on core 0
      if (gameMenu(core, rom) == MenuResult::Quit) break;
      phase = autoSkip = evalCount = 0; workSum = 0;
      fpsT0 = hal_millis(); fpsFrames = fpsDrawn = 0; fpsWork = 0;
      continue;
    }
    core.setInput(0, pad);

    // frameskip: draw 1 frame in skip+1 (self-paced cores decide for themselves)
    const int skip = core.selfPaced ? 0 : g_settings.frameskip ? g_settings.frameskip : autoSkip;
    const bool draw = phase == 0;
    if (++phase > skip) phase = 0;

    const uint32_t t0 = hal_micros(), w0 = hal_audio_wait_us();
    core.runFrame(draw);
    const uint32_t work = (hal_micros() - t0) - (hal_audio_wait_us() - w0);

    // auto: every second, compare the average work per frame to the frame period
    if (!core.selfPaced) {
      workSum += work;
      if (++evalCount >= (int)core.frameRate()) {
        const uint32_t avg = (uint32_t)(workSum / evalCount), period = 1000000 / core.frameRate();
        if (avg > period * 95 / 100 && autoSkip < 3) autoSkip++;
        else if (avg < period * 60 / 100 && autoSkip > 0) autoSkip--;
        evalCount = 0; workSum = 0;
      }
    }

    fpsFrames++;
    fpsWork += work;
    if (draw) fpsDrawn++;
    if (g_settings.showFps && hal_millis() - fpsT0 >= 1000) {
      const uint32_t ms = hal_millis() - fpsT0;
      // load: emulation time (without waiting for audio) against the real-time budget. A
      // self-paced core always fills its frame, so its load is what is left after the guest.
      const unsigned load = (unsigned)(fpsWork * core.frameRate() / 10000 / fpsFrames);
      showFps(core, fpsFrames * 1000 / ms, fpsDrawn * 1000 / ms, skip, load);
      fpsT0 = hal_millis(); fpsFrames = fpsDrawn = 0; fpsWork = 0;
    }
  }
  hal_audio_silence();
  core.shutdown();
}

// ------------------------------------------------------------------------------------------------
void app_setup() {
  hal_init();
  if (!ui::begin()) { hal_log("out of memory for the UI canvas"); for (;;) hal_delay(1000); }
  hal_panel_fill(0, 0, PANEL_W, PANEL_H, 0);
  if (hal_sd_mounted()) settingsLoad();
  hal_audio_volume(g_settings.volume);
}

void app_loop() {
  if (!hal_sd_mounted()) {
    ui::drawBackground();
    ui::message("No SD card", "Insert a card with /pc-386 and restart", 0);
    for (;;) ui::waitKey();   // its hotkeys still reach the loader / restart
  }
#if defined(BOARD_DESKTOP)
  if (const char *ini = getenv("PC_INI")) {                                           // test hook
    runGame(kPC386, ini);
    exit(0);
  }
#endif
  std::string ini;
  if (pcprof_home(ini)) runGame(kPC386, ini);
}
