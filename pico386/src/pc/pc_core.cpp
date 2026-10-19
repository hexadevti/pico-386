// pc_core.cpp -- the 386 PC (tiny386) as a pico-386 core.
//
// The in-game menu holds the drives: A:, B: and the CD change while the PC runs, C: restarts it.
// Every change (and the keyboard mouse on/off) is written to the machine's .ini, so the machine
// starts with it next time too.
//
// "ROM" here is a machine .ini (see pc_profiles.cpp). The guest runs on the wall clock: tiny386's
// timers read get_uticks(), so each frame emulates until the next 1/60 s boundary, as fast as the
// CPU allows, and the sound for the real time that passed is mixed and queued. When the emulation
// is slower than a real 386 the guest simply sees a slower CPU; its clock stays right.
//
// Video is drawn on the other core (hal_job_start), only when the display has finished sending the
// last frame and the last drawing is done, so neither a slow panel nor the drawing stalls the guest
// (the PC core does its own frameskip). Drawing reads VGA memory while the guest writes it, which
// at worst tears a frame, as on upstream tiny386's ESP32 builds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "pc_internal.h"
#include "pc_glue.h"
#include "pc_folder.h"
#include "../frontend/ui.h"
#include "../frontend/browser.h"
#include "../frontend/settings.h"
#include "../hal/hal.h"
#include "../../core.h"

extern const EmuCore kPC386;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kFrameUs = 16667;
constexpr int kMixMax = 1024;

uint16_t *s_fb;
bool s_running;
char s_error[200];
uint32_t s_nextUs, s_lastAudioUs, s_lastAudioWait;
uint32_t s_speedT0, s_speedI0;     // the guest-speed meter's last sample
uint32_t s_maxGapUs;               // the longest time between two audio writes since the last meter
uint32_t s_underrunsShown;         // the output underrun count at the last meter
char s_speed[48];
uint64_t s_audioFrac;              // sample-microseconds not yet turned into samples
int16_t s_stereo[kMixMax * 2], s_mono[kMixMax];
std::string s_ini;                 // the machine's .ini: drive changes are saved to it
std::string s_drive[DRIVE_COUNT];  // what each drive holds (relative paths, "" = empty)
std::string s_driveLabel[DRIVE_COUNT];
int s_mouseSaved;                  // the keyboard mouse mode the .ini has
uint32_t s_noteUntil;              // a note in the bottom strip until then (ms), 0 = none

const char *baseName(const std::string &p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p.c_str() : p.c_str() + s + 1;
}

bool pc_loadRom(const char *ini);

bool pc_init() {
  if (!s_fb) s_fb = (uint16_t *)hal_alloc_big(T386_FB_W * T386_FB_H * 2);
  return s_fb != nullptr;
}

bool pc_loadRom(const char *ini) {
  s_error[0] = 0;
  const int r = t386_start(ini, hal_sd_root(), g_settings.biosDir.c_str(), s_fb);
  if (r != T386_OK) {
    const char *f = t386_error_file();
    const char *root = hal_sd_root();
    if (!strncmp(f, root, strlen(root))) f += strlen(root);
    switch (r) {
      case T386_ERR_INI:    snprintf(s_error, sizeof s_error, "Cannot read %s", f); break;
      case T386_ERR_FILE:   snprintf(s_error, sizeof s_error, "Missing %s", f); break;
      case T386_ERR_ZIP:    snprintf(s_error, sizeof s_error, "Cannot unpack %s", f); break;
      default:              snprintf(s_error, sizeof s_error, "Not enough memory for this machine"); break;
    }
    return false;
  }
  s_running = true;
  s_ini = ini;
  for (int d = 0; d < DRIVE_COUNT; d++) s_drive[d] = pcdisk_drive(s_ini, d);
  pckbd_reset();
  for (int b = 0; b < 2; b++) {
    const std::string k = pcini_get_in(s_ini, "pico", b ? "mouse_right" : "mouse_left");
    pckbd_set_mouse_button(b, k.empty() ? (b ? "rshift" : "alt") : k.c_str());
  }
  s_mouseSaved = atoi(pcini_get_in(s_ini, "pico", "mouse").c_str());
  pckbd_set_mouse_mode(s_mouseSaved);
  s_mouseSaved = pckbd_mouse_mode();
  s_noteUntil = 0;
  pcvid_reset();
  hal_audio_start(kRate);
  s_nextUs = get_uticks();
  s_lastAudioUs = hal_micros();
  s_lastAudioWait = hal_audio_wait_us();
  s_audioFrac = 0;
  s_speedT0 = hal_micros();
  s_speedI0 = 0;
  hal_log("tiny386: %s started, %u KB of heap", ini, (unsigned)(t386_heap_bytes() >> 10));
  return true;
}

void pc_reset() {
  pckbd_reset();
  t386_reset();
}

void pc_audio() {
  // Sound is made for the real time since the last call, minus the time the last write spent
  // blocked on a full output queue. Counting that wait would make the next write bigger, so it
  // blocks longer: once the queue filled, frames grew until the 100 ms cap (10 fps) and the PC
  // ran only in the gaps.
  const uint32_t now = hal_micros(), wait = hal_audio_wait_us();
  uint32_t elapsed = now - s_lastAudioUs;
  const uint32_t blocked = wait - s_lastAudioWait;
  elapsed = blocked < elapsed ? elapsed - blocked : 0;
  s_lastAudioUs = now;
  s_lastAudioWait = wait;
  if (elapsed > s_maxGapUs) s_maxGapUs = elapsed;
  if (elapsed > 100000) elapsed = 100000;      // after a pause: no burst of stale sound
  // Keep a cushion queued. Making exactly as much sound as the real time that passed keeps the
  // queue at whatever it started with, which is empty: then any frame a little late ran it dry,
  // and the speaker played a moment of silence (a tick). The queue's low point, here just before
  // the write, is steered to 25 ms by making a little more or less sound; at the start or after
  // a dry spell that is a few ms extra per frame, and almost nothing once it is there.
  const int32_t cushion = 25000 - (int32_t)hal_audio_queued_us();
  int32_t us = (int32_t)elapsed + cushion / 8;
  if (us < 0) us = 0;
  s_audioFrac += (uint64_t)us * kRate;
  int n = (int)(s_audioFrac / 1000000);
  s_audioFrac -= (uint64_t)n * 1000000;
  while (n > 0) {
    const int k = n > kMixMax ? kMixMax : n;
    t386_mix(s_stereo, k);
    for (int i = 0; i < k; i++) s_mono[i] = (int16_t)((s_stereo[2 * i] + s_stereo[2 * i + 1]) >> 1);
    hal_audio_write(s_mono, k);
    n -= k;
  }
}

// The drawing job (core 0 on the P4).
void drawJob(void *) {
  if (!hal_video_ready()) return;
  uint16_t *back = hal_video_back();
  // The bottom 10 rows are left alone: the picture never reaches them (25 text rows end at 310),
  // and they hold the FPS meter, which a full-height push would wipe every frame.
  if (pcvid_render(back, s_fb, hal_millis())) hal_video_submit(0, SCREEN_H - 10);
}

// A line of text in the bottom strip, under the picture (where the FPS meter goes), for 2 s;
// null clears the strip.
void showNote(const char *text) {
  const int y = PANEL_H - 10;
  ui::fill(0, y, PANEL_W, 10, 0);
  if (text) ui::text(PANEL_W / 2, y + 5, text, ui::kAccent, 1, ui::MC);
  hal_panel_push(0, y, PANEL_W, 10, ui::canvas() + y * PANEL_W, PANEL_W);
  s_noteUntil = text ? (hal_millis() + 2000) | 1 : 0;
}

void pc_runFrame(bool draw) {
  if (!s_running) return;
  uint32_t now = get_uticks();
  // Far behind the last deadline: the machine was stopped (the menu, the file browser). That
  // time is taken out of the guest's clock. Otherwise the PIT fires every missed tick back to
  // back, and Windows' DOS extender crashes or hangs on that burst (Win 3.1 setup, disk 3).
  if ((int32_t)(now - s_nextUs) > 100000) {
    t386_clock_skip(now - s_nextUs);
    now = s_nextUs;
  }
  s_nextUs += kFrameUs;
  if ((int32_t)(s_nextUs - now) > 2 * (int32_t)kFrameUs) s_nextUs = now + kFrameUs;   // ahead

  pckbd_tick(hal_millis());
  if (pckbd_mouse_mode() != s_mouseSaved) {    // both Shifts, Ctrl-Tab or the menu: kept for the next start
    s_mouseSaved = pckbd_mouse_mode();
    pcini_set_in(s_ini, "pico", "mouse", std::to_string(s_mouseSaved));
    std::string note = std::string("Arrows: ") + (s_mouseSaved == MOUSE_ARROWS ? "keys" : s_mouseSaved == MOUSE_ON ? "mouse" : "keys + mouse");
    if (s_mouseSaved != MOUSE_ARROWS)
      note += std::string("   buttons: ") + pckbd_mouse_button_label(0) + " / " + pckbd_mouse_button_label(1);
    showNote(note.c_str());
  }
  if (s_noteUntil && (int32_t)(hal_millis() - s_noteUntil) >= 0) {
    s_noteUntil = 0;
    if (!g_settings.showFps) showNote(nullptr);              // the FPS meter repaints its own
  }
  t386_run_until(s_nextUs);
  pc_audio();

  if (draw && hal_video_ready()) hal_job_start(drawJob, nullptr);   // false: still drawing the last one
}

// Millions of guest instructions per second since the last call (it reads low while the guest
// idles in HLT, so measure with something busy running), then the sound diagnostics: "g" output
// buffers that ran dry since the last meter, "s" mixes the Sound Blaster had no data for (since
// power-on), and the longest time between two sound writes since the last meter (over 70 ms = a gap).
const char *pc_fpsText() {
  const uint32_t now = hal_micros(), ins = t386_instructions();
  const uint32_t us = now - s_speedT0;
  if (us) {
    const uint32_t tenths = (uint32_t)((uint64_t)(ins - s_speedI0) * 10 / us);
    snprintf(s_speed, sizeof s_speed, "%u.%u MIPS g%u s%u %ums", (unsigned)(tenths / 10),
             (unsigned)(tenths % 10), (unsigned)(hal_audio_underruns() - s_underrunsShown), (unsigned)t386_sb_starved(),
             (unsigned)(s_maxGapUs / 1000));
    s_maxGapUs = 0;
    s_underrunsShown = hal_audio_underruns();
  }
  s_speedT0 = now;
  s_speedI0 = ins;
  return s_speed;
}

void pc_setInput(int, uint32_t) {}

uint32_t pc_frameRate() { return 60; }

// Powers the PC off. Folder disks and floppies get the guest's changes now, which can take a while.
void stopMachine() {
  hal_job_wait();
  if (pfolder_dirty()) ui::message("Saving", "Writing changes to the SD folders");
  t386_stop();
}

void pc_shutdown() {
  hal_job_wait();                              // the drawing job reads the machine and s_fb
  if (s_running) {
    pckbd_reset();
    stopMachine();                             // closes the disk images: guest writes reach the card
    s_running = false;
  }
  hal_free(s_fb);
  s_fb = nullptr;
}

void pc_keyEvent(uint8_t code, uint8_t state) { pckbd_event(code, state); }

// The in-game rows: the drives, then the keyboard mouse, its two button keys and Ctrl+Alt+Del.
enum { ROW_MOUSE, ROW_LEFT, ROW_RIGHT, ROW_CAD, ROW_EXTRA };

int pc_menuItems(CoreMenuItem *out, int max) {
  if (max < DRIVE_COUNT + ROW_EXTRA) return 0;
  for (int d = 0; d < DRIVE_COUNT; d++) {
    s_driveLabel[d] = pcdisk_drive_label(s_drive[d]);
    out[d] = { pcdisk_drive_name(d), s_driveLabel[d].c_str(), false };
  }
  out[DRIVE_COUNT + ROW_MOUSE] = { "Arrows (both Shifts)", pckbd_mouse_mode_name(pckbd_mouse_mode()), true };
  out[DRIVE_COUNT + ROW_LEFT] = { "Mouse left button", pckbd_mouse_button_label(0), true };
  out[DRIVE_COUNT + ROW_RIGHT] = { "Mouse right button", pckbd_mouse_button_label(1), true };
  out[DRIVE_COUNT + ROW_CAD] = { "Ctrl+Alt+Del", nullptr, false };
  return DRIVE_COUNT + ROW_EXTRA;
}

// A, B and the CD change at once. C: and D: cannot change under a running DOS, so the PC restarts.
bool changeDrive(int d) {
  std::string path;
  const bool hard = d == DRIVE_C || d == DRIVE_D;
  const int r = pcdisk_choose(d, s_drive[d], hard ? "The PC restarts" : nullptr, path);
  if (!r) return false;
  const std::string rel = r == 1 ? pcdisk_rel(path) : std::string();
  const std::string full = r == 1 ? path : std::string();
  if (hard) {
    if (!pcdisk_set_drive(s_ini, d, full)) {
      ui::message("Error", "Cannot write the .ini", 1500);
      return false;
    }
    const std::string letter = pcdisk_drive_letter(d);
    ui::message("Restarting", (rel.empty() ? "without " + letter : letter + " = " + pcdisk_drive_label(rel)).c_str());
    pckbd_reset();
    stopMachine();
    s_running = false;
    const std::string ini = s_ini;
    if (!pc_loadRom(ini.c_str())) ui::message("Error", s_error, 3000);
    return true;
  }
  if (d == DRIVE_CD) {
    t386_change_cd(pcdisk_cd_ide(s_ini), full.c_str());
  } else {
    // a folder floppy taken out is written back to its folder first
    if (t386_floppy_dirty(d))
      ui::message(("Saving " + std::string(pcdisk_drive_letter(d))).c_str(), "Writing changes to the SD folder");
    const int err = t386_swap_floppy(d, r == 1 ? full.c_str() : nullptr);
    if (err != 0) {
      s_drive[d].clear();
      pcdisk_set_drive(s_ini, d, "");
      ui::message(pcdisk_drive_letter(d),
                  err == T386_FD_TOO_BIG ? "Too big for a floppy (use D:)" :
                  err == T386_FD_IN_USE ? "Already in another drive" :
                  err == T386_FD_UNPACK ? "Cannot unpack this zip" :
                  err == T386_FD_MISSING ? "File not found" : "Not a floppy image size", 1800);
      return false;
    }
  }
  s_drive[d] = rel;
  pcdisk_set_drive(s_ini, d, full);
  ui::message(pcdisk_drive_letter(d), rel.empty() ? "Empty" : baseName(rel), 700);
  return true;
}

bool pc_menuAction(int i, uint8_t key) {
  if (i < DRIVE_COUNT) return key == KEY_ENTER && changeDrive(i);
  const int dir = key == KEY_LEFT ? -1 : 1;
  switch (i - DRIVE_COUNT) {
    case ROW_MOUSE: {
      const int mode = pckbd_mouse_mode();
      pckbd_set_mouse_mode((mode + (dir < 0 ? 2 : 1)) % 3);
      return false;
    }
    case ROW_LEFT:
    case ROW_RIGHT: {
      const int b = i - DRIVE_COUNT - ROW_LEFT;
      pckbd_next_mouse_button(b, dir);
      pcini_set_in(s_ini, "pico", b ? "mouse_right" : "mouse_left", pckbd_mouse_button(b));
      return false;
    }
    case ROW_CAD:
      if (key != KEY_ENTER) return false;
      pckbd_ctrl_alt_del();
      return true;
  }
  return false;
}

const char *pc_lastError() { return s_error[0] ? s_error : "Cannot start this machine"; }

} // namespace

const EmuCore kPC386 = {
  "386 PC", "pc", ".img.ima.iso.vhd",
  pc_init, pc_loadRom, pc_reset, pc_runFrame, pc_setInput, pc_frameRate,
  nullptr, nullptr, pc_shutdown,
  pc_keyEvent, pc_menuItems, pc_menuAction, pc_lastError, true, pc_fpsText,
};
