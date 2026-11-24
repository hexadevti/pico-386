// hal_web.cpp -- the browser HAL (Emscripten, -sPROXY_TO_PTHREAD), in place of src/hal/desktop.
//
// main() runs on a worker thread, so the app blocks exactly as on the desktop: setup(), then
// loop() forever. The page (web/shell/app.js, the browser's main thread) owns the canvas, the
// AudioContext and the touch UI, and talks to us only through shared wasm memory and the
// EMSCRIPTEN_KEEPALIVE exports below (the techniques of emu8's web build):
//   * display: the 320x320 RGB565 panel is kept here, and every push converts its rectangle into
//     one RGBA copy and bumps a frame counter; the page's requestAnimationFrame paints that copy
//     when the counter moves. A frame may tear between two pushes, which nobody sees.
//   * keys: a lock-free queue of PicoCalc key codes (the page sends what the device's STM32
//     would: printable keys as the character typed), drained by hal_key_poll.
//   * mouse: the screen is a touchpad; its moves add up here and go to the guest's PS/2 mouse
//     from hal_key_poll, on the app's own thread, one packet at a time.
//   * audio: a mono float ring the page's AudioWorklet drains (header layout in audio-worklet.js).
//     Writes block while about 80 ms are queued, which paces the PC at real speed, as SDL's queue
//     does on the desktop.
//   * the PC's picture goes at its own resolution to a second buffer (hal_web_pc_frame), and the
//     menus go out as JSON for the page to draw as touch screens (hal_web_ui_show; ui.cpp).
//   * the SD card is /sdcard, an IndexedDB-backed folder the page mounts before main() starts and
//     saves from time to time.
#include "../../pico386/board.h"

#if defined(EMU_WEB)

#include <emscripten.h>
#include <emscripten/threading.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include "../../pico386/src/hal/hal.h"
#include "../../pico386/src/pc/pc_glue.h"

void setup();
void loop();

// ------------------------------------------------------------------------------------------------
// page visibility: hidden (tab switched, phone locked) parks the app wherever it next asks for time
// ------------------------------------------------------------------------------------------------
static std::atomic<int> s_hidden{0};
static void webHiddenWait() { while (s_hidden.load(std::memory_order_relaxed)) emscripten_thread_sleep(100); }

// ------------------------------------------------------------------------------------------------
// system
// ------------------------------------------------------------------------------------------------
static double s_t0 = emscripten_get_now();
uint32_t hal_millis() { return (uint32_t)(int64_t)(emscripten_get_now() - s_t0); }
uint32_t hal_micros() { return (uint32_t)(int64_t)((emscripten_get_now() - s_t0) * 1000.0); }
void hal_yield() { webHiddenWait(); }
void hal_delay(uint32_t ms) { webHiddenWait(); emscripten_thread_sleep(ms); }

void hal_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}

// The page saves /sdcard to IndexedDB and reloads itself (app.js webReboot); nothing comes back.
static void reloadPage() {
  fflush(nullptr);
  MAIN_THREAD_ASYNC_EM_ASM({ if (Module.webReboot) Module.webReboot(); else location.reload(); });
  for (;;) emscripten_thread_sleep(1000);
}
void hal_reboot()           { hal_log("reboot requested - reloading the page"); reloadPage(); }
void hal_reboot_to_loader() { hal_log("exit requested - reloading the page"); reloadPage(); }

void *hal_alloc_big(size_t n)  { return calloc(1, n); }
void *hal_alloc_fast(size_t n) { return calloc(1, n); }
void  hal_free(void *p)        { free(p); }
bool  hal_job_start(void (*job)(void *), void *arg) { job(arg); return true; }
void  hal_job_wait()           {}
int   hal_battery_percent()    { return -1; }
bool  hal_battery_charging()   { return false; }
bool        hal_sd_mounted()   { return true; }
const char *hal_sd_root()      { return EMU_DEFAULT_SD_DIR; }

// ------------------------------------------------------------------------------------------------
// display
// ------------------------------------------------------------------------------------------------
static uint16_t s_panel[PANEL_W * PANEL_H];
static uint8_t  s_rgba[PANEL_W * PANEL_H * 4];
static std::atomic<uint32_t> s_frameNo{0};
static uint16_t *s_vbuf[2];
static int s_back = 0;
static uint8_t s_lut5[32], s_lut6[64];

// RGB565 -> RGBA for one rectangle of the panel, then tell the page
static void present(int x, int y, int w, int h) {
  for (int r = y; r < y + h; r++) {
    const uint16_t *s = &s_panel[r * PANEL_W + x];
    uint8_t *d = &s_rgba[(r * PANEL_W + x) * 4];
    for (int i = 0; i < w; i++, d += 4) {
      const uint16_t c = s[i];
      d[0] = s_lut5[c >> 11]; d[1] = s_lut6[(c >> 5) & 0x3f]; d[2] = s_lut5[c & 0x1f]; d[3] = 255;
    }
  }
  s_frameNo.fetch_add(1, std::memory_order_release);
}

void hal_panel_fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  if (w <= 0 || h <= 0) return;
  for (int r = 0; r < h; r++)
    for (int i = 0; i < w; i++) s_panel[(y + r) * PANEL_W + x + i] = c;
  present(x, y, w, h);
}

void hal_display_fill(int x, int y, int w, int h, uint16_t c) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  hal_panel_fill(x, y + SCREEN_Y0, w, h, c);
}

void hal_panel_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  if (x < 0) { px -= x; w += x; x = 0; }
  if (y < 0) { px -= y * stride; h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  if (w <= 0 || h <= 0) return;
  for (int r = 0; r < h; r++)
    memcpy(&s_panel[(y + r) * PANEL_W + x], px + r * stride, w * 2);
  present(x, y, w, h);
}

void hal_display_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  if (y < 0) { px -= y * stride; h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  hal_panel_push(x, y + SCREEN_Y0, w, h, px, stride);
}

uint16_t *hal_video_back() { return s_vbuf[s_back]; }
const uint16_t *hal_video_front() { return s_vbuf[s_back ^ 1]; }

uint16_t *hal_video_submit(int y, int h) {
  webHiddenWait();
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (h > 0) {
    memcpy(&s_panel[(SCREEN_Y0 + y) * PANEL_W], s_vbuf[s_back] + y * SCREEN_W, (size_t)h * SCREEN_W * 2);
    present(0, SCREEN_Y0 + y, PANEL_W, h);
  }
  s_back ^= 1;
  return s_vbuf[s_back];
}

void hal_video_wait() {}
bool hal_video_ready() { return true; }

// ------------------------------------------------------------------------------------------------
// the PC's own picture, the status lines and the menus (hal.h's EMU_WEB part)
// ------------------------------------------------------------------------------------------------
// The guest's picture at its own size, RGBA, for the page's second canvas. Written by the app's
// thread only; the page copies it when web_pc_frame_no moves (a tear between two frames is
// possible and invisible, as with the panel).
static const int kPcMaxW = 1280, kPcMaxH = 1024;
static uint8_t *s_pcRgba;
static std::atomic<uint32_t> s_pcFrameNo{0};
static std::atomic<int> s_pcW{0}, s_pcH{0};

void hal_web_pc_frame(const uint16_t *px, int w, int h, int stride) {
  if (w <= 0 || h <= 0 || w > kPcMaxW || h > kPcMaxH) return;
  if (!s_pcRgba) s_pcRgba = (uint8_t *)malloc((size_t)kPcMaxW * kPcMaxH * 4);
  if (!s_pcRgba) return;
  webHiddenWait();
  uint8_t *d = s_pcRgba;
  for (int r = 0; r < h; r++) {
    const uint16_t *s = px + (size_t)r * stride;
    for (int i = 0; i < w; i++, d += 4) {
      const uint16_t c = s[i];
      d[0] = s_lut5[c >> 11]; d[1] = s_lut6[(c >> 5) & 0x3f]; d[2] = s_lut5[c & 0x1f]; d[3] = 255;
    }
  }
  s_pcW.store(w, std::memory_order_relaxed);
  s_pcH.store(h, std::memory_order_relaxed);
  s_pcFrameNo.fetch_add(1, std::memory_order_release);
}

// Two status lines, each with a counter the page polls.
static char s_status[2][128];
static std::atomic<uint32_t> s_statusNo[2];
void hal_web_status(int slot, const char *text) {
  if (slot < 0 || slot > 1) return;
  snprintf(s_status[slot], sizeof s_status[slot], "%s", text ? text : "");
  s_statusNo[slot].fetch_add(1, std::memory_order_release);
}

// Menus: the newest screen's JSON and its sequence number. Two buffers, written in turn, and the
// page reads like a seqlock (number, text, number again), so a screen replaced while it reads is
// read again. The answer is one (seq, key, row) the page stores and the app takes once.
static std::string s_uiJson[2];
static std::atomic<uint32_t> s_uiSeq{0};
static bool s_uiIsPc = false;
static std::atomic<int> s_repKey{0}, s_repSel{0};
static std::atomic<uint32_t> s_repSeq{0};
static char s_uiText[256];

uint32_t hal_web_ui_show(const char *json) {
  const uint32_t seq = s_uiSeq.load(std::memory_order_relaxed) + 1;
  s_uiJson[seq & 1] = json;
  s_uiIsPc = false;
  s_uiSeq.store(seq, std::memory_order_release);
  return seq;
}

bool hal_web_ui_take(uint32_t seq, int *key, int *sel) {
  if (s_repSeq.load(std::memory_order_acquire) != seq) return false;
  *key = s_repKey.load(std::memory_order_relaxed);
  *sel = s_repSel.load(std::memory_order_relaxed);
  s_repSeq.store(0, std::memory_order_relaxed);
  return true;
}

const char *hal_web_ui_text() { return s_uiText; }

void hal_web_ui_pc() {
  if (s_uiIsPc) return;
  hal_web_ui_show("{\"t\":\"pc\"}");
  s_uiIsPc = true;
}

// ------------------------------------------------------------------------------------------------
// input
// ------------------------------------------------------------------------------------------------
// One producer (the page's thread, web_key) and one consumer (the app, hal_key_poll).
static const uint32_t kQN = 256;
static KeyEvent s_q[kQN];
static std::atomic<uint32_t> s_qW{0}, s_qR{0};

// The touchpad's moves since the last packet, and its buttons (bit 0 left, 1 right).
static std::atomic<int> s_mdx{0}, s_mdy{0}, s_mbtn{0};

static void mousePump() {
  static int sentBtn = 0;
  const int btn = s_mbtn.load(std::memory_order_relaxed);
  int dx = s_mdx.load(std::memory_order_relaxed), dy = s_mdy.load(std::memory_order_relaxed);
  if (!dx && !dy && btn == sentBtn) return;
  // One packet at a time, as pc_keymap paces the keyboard mouse: Windows accelerates big packets,
  // and a queue full of them leaves the pointer drifting after the finger stops.
  if (t386_mouse_busy()) return;
  const int sx = dx > 24 ? 24 : dx < -24 ? -24 : dx;
  const int sy = dy > 24 ? 24 : dy < -24 ? -24 : dy;
  s_mdx.fetch_sub(sx, std::memory_order_relaxed);
  s_mdy.fetch_sub(sy, std::memory_order_relaxed);
  t386_mouse(sx, sy, btn);
  sentBtn = btn;
}

bool hal_key_poll(KeyEvent *ev) {
  webHiddenWait();
  mousePump();
  const uint32_t r = s_qR.load(std::memory_order_relaxed);
  if (r == s_qW.load(std::memory_order_acquire)) return false;
  *ev = s_q[r % kQN];
  s_qR.store(r + 1, std::memory_order_release);
  return true;
}

// ------------------------------------------------------------------------------------------------
// audio
// ------------------------------------------------------------------------------------------------
static const uint32_t kRingN = 32768;        // a power of two
struct AudioRing {
  std::atomic<uint32_t> w;                    // [0] written (samples, wraps)
  std::atomic<uint32_t> r;                    // [1] read by the worklet
  std::atomic<uint32_t> rate;                 // [2] the samples' rate
  uint32_t              size;                 // [3] kRingN
  std::atomic<uint32_t> beat;                 // [4] bumped by the worklet every block
  std::atomic<uint32_t> dry;                  // [5] blocks the worklet found empty
  uint32_t              pad[2];
  float                 data[kRingN];
};
static AudioRing s_ring;
static const int16_t kVolume[11] = { 0, 16, 32, 52, 76, 104, 136, 172, 212, 256, 320 };
static int32_t  s_gain = 256;
static uint32_t s_audioWaitUs = 0;

// Until the page's AudioContext runs (browsers want a tap first) nothing drains the ring; this
// plays the part of the consumer at the real rate, so the PC still runs at real speed.
static void virtualConsume() {
  static uint32_t lastBeat = 0, lastBeatUs = 0, lastUs = 0;
  const uint32_t now = hal_micros();
  const uint32_t b = s_ring.beat.load(std::memory_order_relaxed);
  if (b != lastBeat) { lastBeat = b; lastBeatUs = now; lastUs = now; return; }
  if (now - lastBeatUs < 150000) { lastUs = now; return; }   // the worklet is alive
  const uint32_t n = (uint32_t)((uint64_t)(now - lastUs) * s_ring.rate.load() / 1000000);
  if (!n) return;
  lastUs = now;
  const uint32_t r = s_ring.r.load(), w = s_ring.w.load();
  const uint32_t avail = w - r;
  s_ring.r.store(r + (n < avail ? n : avail));
}

void hal_audio_start(uint32_t rate) {
  s_ring.size = kRingN;
  s_ring.rate.store(rate);
}

void hal_audio_volume(int v) {
  if (v < 0) v = 0;
  if (v > 10) v = 10;
  s_gain = kVolume[v];
}

void hal_audio_write(const int16_t *mono, int n) {
  const uint32_t rate = s_ring.rate.load();
  if (!rate) return;
  const uint32_t limit = rate * 8 / 100;      // 80 ms
  const float k = (float)s_gain / (256.0f * 32768.0f);
  const uint32_t t0 = hal_micros();
  for (int i = 0; i < n;) {
    for (;;) {
      webHiddenWait();
      virtualConsume();
      if (s_ring.w.load(std::memory_order_relaxed) - s_ring.r.load(std::memory_order_acquire) < limit) break;
      emscripten_thread_sleep(1);
    }
    uint32_t w = s_ring.w.load(std::memory_order_relaxed);
    int chunk = n - i;
    if (chunk > 128) chunk = 128;
    for (int j = 0; j < chunk; j++) {
      float v = mono[i + j] * k;
      s_ring.data[(w + j) & (kRingN - 1)] = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
    }
    s_ring.w.store(w + chunk, std::memory_order_release);
    i += chunk;
  }
  s_audioWaitUs += hal_micros() - t0;
}

// Dropping what is queued means moving the read index, which the worklet owns; it only ever stores
// what it read plus what it played, so the worst case is a few samples played twice.
void hal_audio_silence() { s_ring.r.store(s_ring.w.load()); }
uint32_t hal_audio_wait_us() { return s_audioWaitUs; }
uint32_t hal_audio_underruns() { return s_ring.dry.load(std::memory_order_relaxed); }
uint32_t hal_audio_queued_us() {
  const uint32_t rate = s_ring.rate.load();
  if (!rate) return 25000;
  virtualConsume();
  const uint32_t fill = s_ring.w.load() - s_ring.r.load();
  return (uint32_t)((uint64_t)fill * 1000000 / rate);
}

// ------------------------------------------------------------------------------------------------
// network: pc_net needs a host's sockets, which a page doesn't have
// ------------------------------------------------------------------------------------------------
bool hal_net_available() { return false; }
void hal_net_begin(const char *, const char *) {}
void hal_net_end() {}
int  hal_net_state(char *info, size_t n) { if (n) snprintf(info, n, "not in the browser"); return HAL_NET_OFF; }
int  hal_net_scan(char (*)[33], int) { return -1; }

// ------------------------------------------------------------------------------------------------
void hal_init() {
  for (int i = 0; i < 32; i++) s_lut5[i] = (uint8_t)((i << 3) | (i >> 2));
  for (int i = 0; i < 64; i++) s_lut6[i] = (uint8_t)((i << 2) | (i >> 4));
  for (int i = 0; i < 2; i++) s_vbuf[i] = (uint16_t *)calloc(SCREEN_W * SCREEN_H, 2);
  s_ring.size = kRingN;
  present(0, 0, PANEL_W, PANEL_H);
  hal_log("pico-386 on " BOARD_NAME ", SD folder: %s", hal_sd_root());
}

// ------------------------------------------------------------------------------------------------
// what the page calls
// ------------------------------------------------------------------------------------------------
extern "C" {
// code = a PicoCalc key code (hal.h), down = 1 pressed / 0 released
EMSCRIPTEN_KEEPALIVE void web_key(int code, int down) {
  const uint32_t w = s_qW.load(std::memory_order_relaxed);
  if (w - s_qR.load(std::memory_order_acquire) >= kQN) return;    // full: drop
  s_q[w % kQN] = { (uint8_t)code, (uint8_t)(down ? KEYSTATE_PRESSED : KEYSTATE_RELEASED) };
  s_qW.store(w + 1, std::memory_order_release);
}
// touchpad: a move in guest pixels (dy > 0 = down) and the buttons held (bit 0 L, 1 R)
EMSCRIPTEN_KEEPALIVE void web_mouse(int dx, int dy, int buttons) {
  s_mdx.fetch_add(dx, std::memory_order_relaxed);
  s_mdy.fetch_add(dy, std::memory_order_relaxed);
  s_mbtn.store(buttons, std::memory_order_relaxed);
}
EMSCRIPTEN_KEEPALIVE void web_hidden(int h) { s_hidden.store(h ? 1 : 0); }
EMSCRIPTEN_KEEPALIVE uint32_t web_frame_no() { return s_frameNo.load(std::memory_order_acquire); }
EMSCRIPTEN_KEEPALIVE uint8_t *web_frame_ptr() { return s_rgba; }
EMSCRIPTEN_KEEPALIVE void *web_audio_ring() { return &s_ring; }
// the PC's picture: its counter, its RGBA pixels and its size
EMSCRIPTEN_KEEPALIVE uint32_t web_pc_frame_no() { return s_pcFrameNo.load(std::memory_order_acquire); }
EMSCRIPTEN_KEEPALIVE uint8_t *web_pc_frame_ptr() { return s_pcRgba; }
EMSCRIPTEN_KEEPALIVE int web_pc_w() { return s_pcW.load(std::memory_order_relaxed); }
EMSCRIPTEN_KEEPALIVE int web_pc_h() { return s_pcH.load(std::memory_order_relaxed); }
// the status lines (0 the note, 1 the FPS meter): a counter that moves with each, and the text
EMSCRIPTEN_KEEPALIVE uint32_t web_status_no(int slot) { return slot == 1 ? s_statusNo[1].load() : s_statusNo[0].load(); }
EMSCRIPTEN_KEEPALIVE const char *web_status(int slot) { return s_status[slot == 1 ? 1 : 0]; }
// the menus: the screen's number (0 = none yet), its JSON, and the answer to it
EMSCRIPTEN_KEEPALIVE uint32_t web_ui_seq() { return s_uiSeq.load(std::memory_order_acquire); }
EMSCRIPTEN_KEEPALIVE const char *web_ui_json(uint32_t seq) { return s_uiJson[seq & 1].c_str(); }
EMSCRIPTEN_KEEPALIVE void web_ui_reply(uint32_t seq, int key, int sel) {
  s_repKey.store(key, std::memory_order_relaxed);
  s_repSel.store(sel, std::memory_order_relaxed);
  s_repSeq.store(seq, std::memory_order_release);
}
// where the page writes an edit's text (UTF-8, 255 bytes at most) before answering it
EMSCRIPTEN_KEEPALIVE char *web_ui_textbuf() { return s_uiText; }
// 1 while a PC is running (the page shows the mouse buttons then)
EMSCRIPTEN_KEEPALIVE int web_pc_running() { return t386_mem_bytes() > 0; }
}

int main(int, char **) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setup();
  for (;;) loop();
}

#endif // EMU_WEB
