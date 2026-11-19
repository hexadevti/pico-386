// pc_video.cpp -- the guest screen on the 320x320 panel.
//
// Graphics modes: tiny386 draws the picture at its real size (320x200, 640x480, ...) top-left in
// its framebuffer, and it is scaled here to 320x240 (4:3) in the middle of the panel, with each
// panel pixel the average of the 2x2 source pixels nearest its footprint. 640x480 becomes an
// exact 2:1 box filter; 320x200 is a plain line stretch.
//
// Text modes are not drawn through tiny386 at all (720x400 would not survive the scaling). Text
// RAM is read directly and drawn with the guest's own font, shrunk to the cell size that fits
// the panel (4x12 for 80x25: 320x300, or 4x8 with Settings > Font size) with 4 levels of
// antialiasing; 80x25 swaps in a font drawn for that cell (pc_textfont.h, from
// tools/gen_textfont.py). The frame is skipped when neither the text, the attributes, the palette
// nor the cursor changed.
//
// PICO386_LOWMEM (the RP2350) has no framebuffer and no 320x320 back buffer: tiny386 hands over
// each finished line of the picture (lineSink), it is scaled on the spot, and 16 output rows at a
// time go straight to the panel. Text is drawn and pushed one character row at a time.
//
// The zoom (Ctrl+Caps Lock, pc_keymap.cpp): a graphics picture bigger than the panel is shown 1:1
// in the 320x310 above the FPS meter, the window centred on the guest's pointer as pc_keymap
// estimates it and stopped at the picture's edges (a window that went on past them, black there,
// was tried on 2026-11-18; the user wants it stopped). Each row is a plain copy, cheaper than the
// 2x2 box; LOWMEM also tells vga.c to decode only the window's lines and dots
// (t386_set_line_window). A window that moved draws the whole of it again.
#include <string.h>
#include <algorithm>
#include <cstdlib>
#include "pc_internal.h"
#include "pc_glue.h"
#include "../hal/hal.h"
#include "../frontend/settings.h"
#include "pc_textfont.h"

extern "C" const uint8_t vgafont16[];          // tiny386's copy of the VGA BIOS's 8x16 font

namespace {

constexpr int kOutH = SCREEN_W * 3 / 4;        // 240: every graphics mode is shown 4:3
constexpr int kMaxCellW = 8, kMaxCellH = 16;
constexpr int kZoomH = SCREEN_H - 10;          // 310: the zoom's rows, above the FPS meter

// Where the graphics picture goes on the panel: scaled to SCREEN_W x kOutH in the middle, or,
// zoomed, the SCREEN_W x kZoomH area at the top shows the picture's (vx + x, vy + y) at (x, y),
// black where that is outside the w x h picture. The window keeps the pointer in its middle and
// stops at the picture's edges; an axis that fits is centred (vx/vy negative).
struct Geo {
  bool zoom = false;
  int top = (SCREEN_H - kOutH) / 2, outH = kOutH;
  int vx = 0, vy = 0, w = 0, h = 0;
  // the picture's part of the window: columns [x0, x1) and rows [y0, y1), in window coordinates
  int x0() const { return std::max(0, -vx); }
  int x1() const { return std::min(SCREEN_W, w - vx); }
  int y0() const { return std::max(0, -vy); }
  int y1() const { return std::min(outH, h - vy); }
  bool sameLayout(const Geo &o) const { return zoom == o.zoom && top == o.top && outH == o.outH; }
  bool operator!=(const Geo &o) const {
    return !sameLayout(o) || vx != o.vx || vy != o.vy || w != o.w || h != o.h;
  }
};
Geo s_geo;                                     // the last picture's
// The pointer as last found drawn in a picture of seenW x seenH (syncPointer). The window centres
// on it rather than on the estimate, which runs ahead of what the guest has drawn by the moves it
// has not taken yet (up to 24 px on the P4), so the window jumped ahead and back every frame.
bool s_seenOk;
int s_seenX, s_seenY, s_seenW, s_seenH;

Geo geometry(int w, int h) {
  Geo g;
  if (!pckbd_zoom() || w <= 0 || h <= 0 || (w <= SCREEN_W && h <= kZoomH)) return g;
  int px, py;
  pckbd_pointer(w, h, &px, &py);
  if (s_seenOk && s_seenW == w && s_seenH == h) {
    px = s_seenX;
    py = s_seenY;
  }
  g.zoom = true;
  g.top = 0;
  g.outH = kZoomH;
  g.w = w;
  g.h = h;
  g.vx = w > SCREEN_W ? std::max(0, std::min(w - SCREEN_W, px - SCREEN_W / 2)) : (w - SCREEN_W) / 2;
  g.vy = h > kZoomH ? std::max(0, std::min(h - kZoomH, py - kZoomH / 2)) : (h - kZoomH) / 2;
  return g;
}

int s_lastMode = -1, s_lastW, s_lastH;

#if !defined(PICO386_LOWMEM)
// Zoomed, the window follows the guest's pointer. Its place summed from the moves sent drifts
// under Windows 95, which slows short moves (about 0.45x) and speeds up long ones (about 1.5x),
// so the pointer left the window on the P4 (2026-11-19). Here the standard arrow is looked for in
// the last picture near the estimate, and the estimate starts again from it. Any other shape (an
// I-beam, an hourglass) is not found, and the estimate goes on alone.
//   row 0  #           black at x
//   row 1  ##
//   row k  #o..o#      black at x, white from x+1 to x+k-1, black at x+k (k = 2..9)
bool isArrow(const uint16_t *fb, int x, int y, int w, int h) {
  if (y + 4 > h || x + 3 > w) return false;   // too little of it shows to tell
  const uint16_t *r = fb + y * T386_FB_W + x;
  if (r[T386_FB_W] != 0 || r[T386_FB_W + 1] != 0) return false;
  for (int k = 2; k <= 9 && y + k < h; k++) {
    const uint16_t *row = r + k * T386_FB_W;
    if (row[0] != 0) return false;
    for (int i = 1; i < k; i++) {
      if (x + i >= w) goto next;
      if (row[i] != 0xFFFF) return false;
    }
    if (x + k < w && row[k] != 0) return false;
  next:;
  }
  return true;
}

// The arrow's tip nearest (cx, cy) with its tip in columns [xa, xb] and rows [ya, yb], if any.
bool findArrowIn(const uint16_t *fb, int w, int h, int xa, int xb, int ya, int yb, int cx, int cy,
                 int *ox, int *oy) {
  xa = std::max(0, xa);
  xb = std::min(w - 3, xb);
  ya = std::max(0, ya);
  yb = std::min(h - 4, yb);
  int best = -1;
  for (int y = ya; y <= yb; y++) {
    const uint16_t *row = fb + y * T386_FB_W;
    for (int x = xa; x <= xb; x++) {
      if (row[x] || row[x + T386_FB_W] || row[x + T386_FB_W + 1]) continue;
      if (!isArrow(fb, x, y, w, h)) continue;
      const int d = std::abs(x - cx) + std::abs(y - cy);
      if (best < 0 || d < best) { best = d; *ox = x; *oy = y; }
    }
  }
  return best >= 0;
}

bool findArrow(const uint16_t *fb, int w, int h, int cx, int cy, int rad, int *ox, int *oy) {
  return findArrowIn(fb, w, h, cx - rad, cx + rad, cy - rad, cy + rad, cx, cy, ox, oy);
}

uint32_t s_seenMoves = ~0u, s_seenAt;            // the moves and the time of the last search
uint32_t s_movedAt;                              // the last time the moves changed
uint32_t s_wideAt;                               // the last search of the whole picture
int s_seenVX, s_seenVY;                          // how far the arrow went between two searches

// Put the estimate on the pointer drawn in the picture just decoded (fb, w x h, the guest wrote
// rows [dy0, dy1) since the last one). While the arrow is held the pointer speeds up (ours up to
// 8 px a frame, and Windows accelerates on top of that), and with 100-230 ms between frames on
// the P4 it went past any fixed search box: the window was lost until the whole-picture search.
// So, in order: around where the last step says it is now, around the estimate, the rows the
// guest just wrote (moving the pointer redraws it there), and the whole picture twice a second.
// Searched every frame while the arrows move it and for a second after (the guest catches up),
// otherwise twice a second for the guest's own moves.
void syncPointer(const uint16_t *fb, int w, int h, int dy0, int dy1, uint32_t nowMs) {
  if (!fb || !pckbd_zoom() || w <= 0 || h <= 0 || (w <= SCREEN_W && h <= kZoomH)) {
    s_seenOk = false;
    return;
  }
  const uint32_t moves = pckbd_pointer_moves();
  if (moves != s_seenMoves) s_movedAt = nowMs;
  if (moves == s_seenMoves && nowMs - s_movedAt > 1000 && nowMs - s_seenAt < 500) return;
  s_seenMoves = moves;
  s_seenAt = nowMs;
  const bool had = s_seenOk && s_seenW == w && s_seenH == h;
  int px, py, fx, fy;
  pckbd_pointer(w, h, &px, &py);
  bool found = false;
  if (had) {
    const int cx = s_seenX + s_seenVX, cy = s_seenY + s_seenVY;
    const int rad = 48 + 2 * std::max(std::abs(s_seenVX), std::abs(s_seenVY));
    found = findArrow(fb, w, h, cx, cy, rad, &fx, &fy);
  }
  if (!found) found = findArrow(fb, w, h, px, py, 64, &fx, &fy);
  if (!found && dy1 > dy0 && dy1 - dy0 <= 160)
    found = findArrowIn(fb, w, h, 0, w, dy0 - 16, dy1, px, py, &fx, &fy);
  if (!found && nowMs - s_wideAt >= 500) {
    s_wideAt = nowMs;
    found = findArrow(fb, w, h, px, py, std::max(w, h), &fx, &fy);
  }
  if (found && had) {
    s_seenVX = fx - s_seenX;
    s_seenVY = fy - s_seenY;
  } else {
    s_seenVX = s_seenVY = 0;
  }
  s_seenOk = found;
  if (!found) return;
  s_seenX = fx;
  s_seenY = fy;
  s_seenW = w;
  s_seenH = h;
  pckbd_pointer_seen(w, h, fx, fy);
}
#endif
int s_lastFontSize = -1;
int s_clearFrames;                             // both video buffers need their borders cleared
bool s_gfxFull = true;
int s_pendY0, s_pendY1 = kOutH;                // output rows the other video buffer still lacks
// Rows of the buffer the last pcvid_render drew. The bottom 10 are never pushed: the picture
// never reaches them (25 text rows end at 310), and they hold the FPS meter.
int s_spanY, s_spanH = SCREEN_H - 10;
uint32_t s_gfxGen;                             // t386_vga_gen() of the last picture drawn

// ---- graphics ----------------------------------------------------------------------------------
inline uint16_t avg2(uint16_t a, uint16_t b) {
  return (uint16_t)(((a ^ b) & 0xF7DEu) >> 1) + (a & b);
}

int16_t s_x0[SCREEN_W], s_x1[SCREEN_W];

// ---- snapshot thumbnails (pcvid_thumb) ---------------------------------------------------------
// Rows of the drawn screen are box-averaged into PCVID_THUMB_W x PCVID_THUMB_H as they go by:
// every row of the picture's box (y0, h) adds into the thumbnail row it falls in, which is sent on
// (s_th.out) once a row of a later one arrives. Only a few sums live here, so LOWMEM can catch
// the rows on their way to the panel without a frame to read them from.
struct ThumbTap {
  bool on = false;
  void (*out)(void *, const uint16_t *) = nullptr;
  void *ctx = nullptr;
  int y0 = 0, h = 1;                           // the picture's rows on the screen
  int ty = 0, n = 0;                           // the thumbnail row being summed, screen rows in it
  uint16_t r[PCVID_THUMB_W], g[PCVID_THUMB_W], b[PCVID_THUMB_W];
} s_th;

void thumbEmit() {
  constexpr int kCols = SCREEN_W / PCVID_THUMB_W;
  uint16_t row[PCVID_THUMB_W];
  const int k = s_th.n * kCols;
  for (int x = 0; x < PCVID_THUMB_W; x++)
    row[x] = k ? (uint16_t)((s_th.r[x] / k) << 11 | (s_th.g[x] / k) << 5 | s_th.b[x] / k) : 0;
  s_th.out(s_th.ctx, row);
  memset(s_th.r, 0, sizeof s_th.r);
  memset(s_th.g, 0, sizeof s_th.g);
  memset(s_th.b, 0, sizeof s_th.b);
  s_th.n = 0;
  s_th.ty++;
}

// rows screen rows from y on, stride SCREEN_W
void thumbRows(int y, const uint16_t *px, int rows) {
  if (!s_th.on) return;
  constexpr int kCols = SCREEN_W / PCVID_THUMB_W;
  for (int i = 0; i < rows; i++, px += SCREEN_W) {
    const int sy = y + i - s_th.y0;
    if (sy < 0 || sy >= s_th.h) continue;
    const int want = sy * PCVID_THUMB_H / s_th.h;
    while (s_th.ty < want) thumbEmit();
    if (s_th.ty != want) continue;             // an earlier row again: already sent
    for (int x = 0; x < PCVID_THUMB_W; x++)
      for (int c = 0; c < kCols; c++) {
        const uint16_t p = px[x * kCols + c];
        s_th.r[x] += p >> 11;
        s_th.g[x] += (p >> 5) & 63;
        s_th.b[x] += p & 31;
      }
    s_th.n++;
  }
}

void thumbEnd() {
  while (s_th.ty < PCVID_THUMB_H) thumbEmit();
  s_th.on = false;
}

#if defined(PICO386_LOWMEM)
constexpr int kBandH = 16;                     // output rows per panel push (>= the tallest text cell)
uint16_t s_band[SCREEN_W * kBandH];
int s_bandY0, s_bandRows;                      // the band's first output row, rows filled
int s_srcW = -1, s_outY;                       // the picture's width the columns are for; next output row
uint16_t s_rowCur[SCREEN_W], s_rowPend[SCREEN_W];

void flushBand() {
  if (s_bandRows) {
    hal_panel_push(0, SCREEN_Y0 + s_geo.top + s_bandY0, SCREEN_W, s_bandRows, s_band, SCREEN_W);
    thumbRows(s_geo.top + s_bandY0, s_band, s_bandRows);
  }
  s_bandY0 += s_bandRows;
  s_bandRows = 0;
}

void emitRow(const uint16_t *row) {
  memcpy(s_band + s_bandRows * SCREEN_W, row, SCREEN_W * 2);
  if (++s_bandRows == kBandH) flushBand();
}

// One source line y (of h) of the picture: filtered across, then it becomes the output rows that
// take it, averaged with the line it is paired with (the same 2x2 box as renderGraphics).
void lineSink(void *, int y, const uint16_t *line, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (w > T386_FB_W) w = T386_FB_W;
  if (s_geo.zoom) {                            // 1:1: vga.c sends only the window's lines
    const int oy = y - s_geo.vy;
    if (oy < s_geo.y0() || oy >= s_geo.y1()) return;
    if (oy == s_geo.y0()) {                    // the first: the rows above it were filled black
      s_bandY0 = oy;
      s_bandRows = 0;
    }
    const int x0 = s_geo.x0(), x1 = std::min(s_geo.x1(), w - s_geo.vx);
    if (x0 > 0) memset(s_rowCur, 0, (size_t)x0 * 2);
    if (x1 > x0) memcpy(s_rowCur + x0, line + s_geo.vx + x0, (size_t)(x1 - x0) * 2);
    if (x1 < SCREEN_W) memset(s_rowCur + std::max(x0, x1), 0, (size_t)(SCREEN_W - std::max(x0, x1)) * 2);
    emitRow(s_rowCur);
    return;
  }
  if (y == 0) {
    s_outY = 0;
    s_bandY0 = 0;
    s_bandRows = 0;
  }
  if (w != s_srcW) {
    for (int x = 0; x < SCREEN_W; x++) {
      int a = x * w / SCREEN_W, b = (x + 1) * w / SCREEN_W - 1;
      if (b < a) b = a;
      s_x0[x] = (int16_t)a;
      s_x1[x] = (int16_t)b;
    }
    s_srcW = w;
  }
  if (w == SCREEN_W) memcpy(s_rowCur, line, SCREEN_W * 2);
  else for (int x = 0; x < SCREEN_W; x++) s_rowCur[x] = avg2(line[s_x0[x]], line[s_x1[x]]);
  while (s_outY < kOutH) {
    int a = s_outY * h / kOutH, b = (s_outY + 1) * h / kOutH - 1;
    if (b < a) b = a;
    if (y < a) return;                         // a line no output row takes (h > 480)
    if (y == a && a < b) {
      memcpy(s_rowPend, s_rowCur, SCREEN_W * 2);
      return;
    }
    if (y > b) { s_outY++; continue; }         // missed (should not happen): skip the row
    if (a == b) {
      emitRow(s_rowCur);
    } else {
      uint16_t *p = s_rowPend;
      for (int x = 0; x < SCREEN_W; x++) p[x] = avg2(p[x], s_rowCur[x]);
      emitRow(p);
    }
    s_outY++;
  }
}
#endif

// Output rows [y0, y1) of the picture only.
void renderGraphics(uint16_t *dst, const uint16_t *fb, int w, int h, int y0, int y1) {
  if (w <= 0 || h <= 0) return;
  if (w > T386_FB_W) w = T386_FB_W;
  if (h > T386_FB_H) h = T386_FB_H;
  // The source span of each output column: [x0, x1], its two ends get averaged.
  for (int x = 0; x < SCREEN_W; x++) {
    int a = x * w / SCREEN_W, b = (x + 1) * w / SCREEN_W - 1;
    if (b < a) b = a;
    s_x0[x] = (int16_t)a;
    s_x1[x] = (int16_t)b;
  }
  const bool sameW = w == SCREEN_W;
  uint16_t *out = dst + ((SCREEN_H - kOutH) / 2 + y0) * SCREEN_W;
  for (int y = y0; y < y1; y++, out += SCREEN_W) {
    int a = y * h / kOutH, b = (y + 1) * h / kOutH - 1;
    if (b < a) b = a;
    const uint16_t *r0 = fb + a * T386_FB_W, *r1 = fb + b * T386_FB_W;
    if (sameW) {
      if (a == b) memcpy(out, r0, SCREEN_W * 2);
      else for (int x = 0; x < SCREEN_W; x++) out[x] = avg2(r0[x], r1[x]);
    } else if (a == b) {
      for (int x = 0; x < SCREEN_W; x++) out[x] = avg2(r0[s_x0[x]], r0[s_x1[x]]);
    } else {
      for (int x = 0; x < SCREEN_W; x++)
        out[x] = avg2(avg2(r0[s_x0[x]], r0[s_x1[x]]), avg2(r1[s_x0[x]], r1[s_x1[x]]));
    }
  }
}

// The zoom: output rows [y0, y1) of the window, copied 1:1, black outside the picture.
void renderZoom(uint16_t *dst, const uint16_t *fb, const Geo &g, int y0, int y1) {
  const int x0 = g.x0(), x1 = g.x1(), py0 = g.y0(), py1 = g.y1();
  for (int y = y0; y < y1; y++) {
    uint16_t *out = dst + (g.top + y) * SCREEN_W;
    if (y < py0 || y >= py1 || x1 <= x0) {
      memset(out, 0, SCREEN_W * 2);
      continue;
    }
    if (x0 > 0) memset(out, 0, (size_t)x0 * 2);
    memcpy(out + x0, fb + (g.vy + y) * T386_FB_W + g.vx + x0, (size_t)(x1 - x0) * 2);
    if (x1 < SCREEN_W) memset(out + x1, 0, (size_t)(SCREEN_W - x1) * 2);
  }
}

// ---- text --------------------------------------------------------------------------------------
#if defined(PICO386_LOWMEM)
// No glyph cache: it is 12 KB for 80x25 and the heap has less than that left once the PC runs
// (2026-10-28, a black screen on the device). Each cell is shrunk as it is drawn instead; the
// hand-drawn ones come straight from flash. Bit c: glyph c is a hand-drawn one.
uint32_t s_handBits[8];
#else
uint8_t *s_glyphs;                             // 256 glyphs x cellW x cellH coverage levels 0..3
#endif
int s_glyphW, s_glyphH, s_glyphCh;
uint32_t s_fontHash;
uint32_t s_textHash;

uint32_t hashBytes(uint32_t h, const uint8_t *p, int n, int step) {
  for (int i = 0; i < n; i += step) h = (h ^ p[i]) * 16777619u;
  return h;
}

// True when the guest's glyph c is still the stock VGA one, so the hand-drawn version
// stands in for it. A glyph a program loaded itself is shrunk like any other.
bool stockGlyph(const uint8_t *font, int c) {
  for (int y = 0; y < 16; y++)
    if (font[(c * 32 + y) * 4] != vgafont16[c * 16 + y]) return false;
  return true;
}

// Shrinks the VGA font (8 dots wide, cheight lines) to cellW x cellH: each output dot is the
// share of lit source dots under it, rounded up to the next of 4 levels so thin strokes survive.
// 80x25 (4x12 or 4x8 cells from the 8x16 font) uses pc_textfont.h instead wherever it has the glyph.
// Glyph c as cw x ch coverage levels 0..3, row by row, into g.
void shrinkGlyph(const uint8_t *font, int cheight, int c, int cw, int ch, bool hand, uint8_t *g) {
  if (hand) {
    const uint8_t *rows = ch == 12 ? kFont4x12[c] : kFont4x8[c];
    for (int y = 0; y < ch; y++)
      for (int x = 0; x < 4; x++) g[y * 4 + x] = (rows[y] >> (6 - 2 * x)) & 3;
    return;
  }
  for (int y = 0; y < ch; y++) {
    const int sy0 = y * cheight / ch, sy1 = (y + 1) * cheight / ch > sy0 ? (y + 1) * cheight / ch : sy0 + 1;
    for (int x = 0; x < cw; x++) {
      const int sx0 = x * 8 / cw, sx1 = (x + 1) * 8 / cw > sx0 ? (x + 1) * 8 / cw : sx0 + 1;
      int lit = 0, all = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        const uint8_t bits = font[(c * 32 + sy) * 4];
        for (int sx = sx0; sx < sx1; sx++, all++) lit += (bits >> (7 - sx)) & 1;
      }
      g[y * cw + x] = (uint8_t)((lit * 3 + all - 1) / all);
    }
  }
}

void buildGlyphs(const uint8_t *font, int cheight, int cw, int ch) {
#if !defined(PICO386_LOWMEM)
  if (!s_glyphs) s_glyphs = (uint8_t *)hal_alloc_big(256 * kMaxCellW * kMaxCellH);
  if (!s_glyphs) return;
#endif
  const bool hand = cw == 4 && cheight == 16 && (ch == 12 || ch == 8);
  const uint32_t *has = ch == 12 ? kFont4x12Has : kFont4x8Has;
  for (int c = 0; c < 256; c++) {
    const bool h = hand && (has[c >> 5] >> (c & 31) & 1) && stockGlyph(font, c);
#if defined(PICO386_LOWMEM)
    if (h) s_handBits[c >> 5] |= 1u << (c & 31);
    else s_handBits[c >> 5] &= ~(1u << (c & 31));
#else
    shrinkGlyph(font, cheight, c, cw, ch, h, s_glyphs + c * cw * ch);
#endif
  }
  s_glyphW = cw;
  s_glyphH = ch;
  s_glyphCh = cheight;
}

uint16_t mix(uint16_t bg, uint16_t fg, int k) {   // k of 3 parts fg
  if (k == 0) return bg;
  if (k == 3) return fg;
  const int r = (((bg >> 11) & 31) * (3 - k) + ((fg >> 11) & 31) * k) / 3;
  const int g = (((bg >> 5) & 63) * (3 - k) + ((fg >> 5) & 63) * k) / 3;
  const int b = ((bg & 31) * (3 - k) + (fg & 31) * k) / 3;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// LOWMEM: dst is ignored and each character row goes to the panel through s_band.
// A text mode's cell height on the panel, 0 when the mode cannot be drawn.
int textCellH(const VGAPicoInfo &in) {
  if (in.cols <= 0 || in.rows <= 0 || in.cols > 160 || in.rows > 100 || in.cheight <= 0) return 0;
  int ch = SCREEN_H / in.rows;
  const int maxCh = g_settings.fontSize == 1 ? 8 : 12;
  if (ch > maxCh) ch = maxCh;
  if (ch < 1) ch = 1;
#if defined(PICO386_LOWMEM)
  if (ch > kBandH) ch = kBandH;
#endif
  return ch;
}

bool renderText(uint16_t *dst, const VGAPicoInfo &in, uint32_t nowMs, bool force) {
  const int cols = in.cols, rows = in.rows;
  const int ch = textCellH(in);
  if (!ch) return false;
  const int cw = SCREEN_W / cols < 1 ? 1 : SCREEN_W / cols > kMaxCellW ? kMaxCellW : SCREEN_W / cols;
  const int cheight = in.cheight > 32 ? 32 : in.cheight;

  // font: rebuilt when its bits (or the cell size) change
  uint32_t fh = 2166136261u;
  for (int c = 0; c < 256; c++) fh = hashBytes(fh, in.font + c * 32 * 4, cheight * 4, 4);
  if (fh != s_fontHash || cw != s_glyphW || ch != s_glyphH || cheight != s_glyphCh) {
    buildGlyphs(in.font, cheight, cw, ch);
    s_fontHash = fh;
    force = true;
  }
#if !defined(PICO386_LOWMEM)
  if (!s_glyphs) return false;
#endif

  const uint8_t *vram = in.vram;
  const bool blinkOn = (nowMs / 267) & 1;      // VGA: the cursor blinks every 16 frames
  int curX = -1, curY = -1;
  if (!(in.cursor_start & 0x20) && in.cursor_addr >= in.start_addr && in.line_offset) {
    const uint32_t off = in.cursor_addr - in.start_addr;
    curY = (int)(off / in.line_offset);
    curX = (int)(off % in.line_offset) / 4;
  }

  // skip the frame when nothing visible changed
  uint32_t th = 2166136261u;
  for (int y = 0; y < rows; y++) {
    const uint32_t row = in.start_addr + y * in.line_offset;
    for (int x = 0; x < cols; x++) {
      const uint32_t a = (row + x * 4) & 0x3fffc;
      th = (th ^ vram[a]) * 16777619u;
      th = (th ^ vram[a + 1]) * 16777619u;
    }
  }
  th = hashBytes(th, (const uint8_t *)in.palette, sizeof in.palette, 1);
  th = (th ^ (uint32_t)(curX * 131 + curY)) * 16777619u;
  th = (th ^ (uint32_t)(blinkOn ? 1 : 2)) * 16777619u;
  th = (th ^ (uint32_t)(in.cursor_start * 256 + in.cursor_end)) * 16777619u;
  if (!force && th == s_textHash) return false;
  s_textHash = th;

  const int x0 = (SCREEN_W - cols * cw) / 2, y0 = (SCREEN_H - rows * ch) / 2;
  int cs = in.cursor_start & 0x1f, ce = in.cursor_end & 0x1f;
  if (ce >= cheight) ce = cheight - 1;
  const int curTop = cs * ch / cheight, curBot = ce * ch / cheight;

  for (int y = 0; y < rows; y++) {
    const uint32_t row = in.start_addr + y * in.line_offset;
#if defined(PICO386_LOWMEM)
    (void)dst;
    memset(s_band, 0, (size_t)SCREEN_W * ch * 2);    // the margins
    uint16_t *line = s_band + x0;
#else
    uint16_t *line = dst + (y0 + y * ch) * SCREEN_W + x0;
#endif
    for (int x = 0; x < cols; x++, line += cw) {
      const uint32_t a = (row + x * 4) & 0x3fffc;
      const uint8_t c = vram[a], attr = vram[a + 1];
      int fgi = attr & 15, bgi = attr >> 4;
      if (in.blink) {
        bgi &= 7;
        if ((attr & 0x80) && !blinkOn) fgi = bgi;
      }
      const uint16_t fg = in.palette[fgi], bg = in.palette[bgi];
      const uint16_t lut[4] = { bg, mix(bg, fg, 1), mix(bg, fg, 2), fg };
#if defined(PICO386_LOWMEM)
      uint8_t cell[kMaxCellW * kBandH];
      shrinkGlyph(in.font, cheight, c, cw, ch, s_handBits[c >> 5] >> (c & 31) & 1, cell);
      const uint8_t *g = cell;
#else
      const uint8_t *g = s_glyphs + c * cw * ch;
#endif
      uint16_t *p = line;
      for (int gy = 0; gy < ch; gy++, p += SCREEN_W, g += cw)
        for (int gx = 0; gx < cw; gx++) p[gx] = lut[g[gx]];
      if (y == curY && x == curX && blinkOn && cs <= ce) {
        uint16_t *q = line + curTop * SCREEN_W;
        for (int gy = curTop; gy <= curBot && gy < ch; gy++, q += SCREEN_W)
          for (int gx = 0; gx < cw; gx++) q[gx] = fg;
      }
    }
#if defined(PICO386_LOWMEM)
    hal_panel_push(0, SCREEN_Y0 + y0 + y * ch, SCREEN_W, ch, s_band, SCREEN_W);
    thumbRows(y0 + y * ch, s_band, ch);
#endif
  }
  return true;
}

} // namespace

void pcvid_reset() {
  s_lastMode = -1;
  s_fontHash = 0;
  s_textHash = 0;
  s_gfxFull = true;
}

void pcvid_span(int *y, int *h) {
  *y = s_spanY;
  *h = s_spanH;
}

bool pcvid_render(uint16_t *dst, const uint16_t *fb, uint32_t nowMs) {
  VGAPicoInfo in;
  t386_info(&in);
  s_spanY = 0;
  s_spanH = SCREEN_H - 10;
  bool changed = in.mode != s_lastMode || g_settings.fontSize != s_lastFontSize;
  s_lastFontSize = g_settings.fontSize;
  bool moved = false;                          // the zoom's window moved
  if (in.mode == 2) {
    // Nothing written to the VGA since the last picture: skip it. FastDoom draws each frame
    // elsewhere and copies it to VRAM a few times a second; redrawing it every 1/60 s anyway cost
    // the P4 a sixth of its speed (1.8 MIPS, 2.1-2.2 with the skip, 2026-11-14): each redraw moves
    // about 400 KB through the PSRAM, which the guest's RAM shares.
    // Not on the P4: with it, Prince of Persia's Adlib music crackled there (2026-11-14; heard on
    // the device, gone with the skip off, the desktop clean either way). The cause is not known
    // yet, so the P4 redraws every frame, as before v0.3.0.
    // The zoom turned on or off, or its window moved: a new layout or a moved one.
    const Geo geo = geometry(in.gfx_w, in.gfx_h);
    moved = geo != s_geo;
    if (!geo.sameLayout(s_geo)) changed = true;  // the borders change too: clear them
    s_geo = geo;
#if defined(PICO386_LOWMEM)
    if (geo.zoom) {
      t386_set_line_window(1, geo.vx + geo.x0(), geo.vx + geo.x1(), geo.vy + geo.y0(),
                           geo.vy + geo.y1());
      // The window's rows above and below the picture: black, as the sink sends only its lines.
      if (moved) {
        const int a = std::max(0, std::min(geo.outH, geo.y0()));
        const int b = std::max(a, std::min(geo.outH, geo.y1()));
        if (a > 0) hal_panel_fill(0, SCREEN_Y0 + geo.top, SCREEN_W, a, 0);
        if (b < geo.outH) hal_panel_fill(0, SCREEN_Y0 + geo.top + b, SCREEN_W, geo.outH - b, 0);
      }
    } else {
      t386_set_line_window(0, 0, 0, 0, 0);
    }
#endif
    const uint32_t gen = t386_vga_gen();
#if !defined(BOARD_PICOCALC_P4)
    if (!s_gfxFull && !changed && !moved && !s_clearFrames && gen == s_gfxGen &&
        in.gfx_w == s_lastW && in.gfx_h == s_lastH)
      return false;
#endif
    s_gfxGen = gen;
#if defined(PICO386_LOWMEM)
    // A new picture size: clear the panel first, or this frame's rows would be wiped after it.
    if (changed || in.gfx_w != s_lastW || in.gfx_h != s_lastH)
      hal_panel_fill(0, SCREEN_Y0, SCREEN_W, SCREEN_H - 10, 0);
    t386_set_line_sink(lineSink, nullptr);
    s_outY = kOutH;                              // nothing is emitted until line 0 arrives
#endif
#if defined(PICO386_LOWMEM)
    const int preW = in.gfx_w, preH = in.gfx_h;
#endif
#if defined(PICO386_LOWMEM)
    t386_refresh(s_gfxFull || changed || moved);
#else
    // Pictures no wider than the panel (mode 13h, the EGA games) are decoded whole every time,
    // as before the dirty lines: see the P4's note above about Prince's music.
    t386_refresh(s_gfxFull || changed || s_lastW <= SCREEN_W);
#endif
#if defined(PICO386_LOWMEM)
    flushBand();
#endif
    s_gfxFull = false;
    if (in.gfx_w != s_lastW || in.gfx_h != s_lastH) changed = true;
    // read again: the refresh is what measures the picture
    t386_info(&in);
    if (in.gfx_w != s_lastW || in.gfx_h != s_lastH) changed = true;
#if !defined(PICO386_LOWMEM)
    // Zoomed: find the pointer in the picture just decoded and place the window on it, so the
    // window shows the arrow where this very picture has it.
    if (pckbd_zoom()) {
      syncPointer(fb, in.gfx_w, in.gfx_h, in.dirty_y0, in.dirty_y1, nowMs);
      const Geo g2 = geometry(in.gfx_w, in.gfx_h);
      if (g2 != s_geo) {
        moved = true;
        if (!g2.sameLayout(s_geo)) changed = true;
        s_geo = g2;
      }
    }
#endif
#if defined(PICO386_LOWMEM)
    // The refresh found a new size and drew it over the old picture's borders: clear them now,
    // the next frame draws the picture again.
    if (in.gfx_w != preW || in.gfx_h != preH) {
      hal_panel_fill(0, SCREEN_Y0, SCREEN_W, SCREEN_H - 10, 0);
      s_gfxFull = true;                          // nothing new may be written: draw it anyway
    }
#endif
  } else {
    s_gfxFull = true;
  }
  if (changed) {
    s_lastMode = in.mode;
    s_lastW = in.gfx_w;
    s_lastH = in.gfx_h;
    s_clearFrames = 2;
    s_textHash = 0;
  }
  const bool clear = s_clearFrames > 0;
#if defined(PICO386_LOWMEM)
  // Straight to the panel: one clear is enough, and graphics were drawn by the refresh above.
  if (clear) {
    if (in.mode != 2) hal_panel_fill(0, SCREEN_Y0, SCREEN_W, SCREEN_H - 10, 0);
    s_clearFrames = 0;
  }
  if (in.mode == 2) return true;
  (void)fb;
#else
  if (clear) {
    memset(dst, 0, SCREEN_W * SCREEN_H * 2);
    s_clearFrames--;
  }

  if (in.mode == 2) {
    // The refresh decoded only the lines the guest wrote (vga.c). Scale and push only their rows,
    // plus the rows of the frame before: that one went to the other buffer, so this buffer still
    // lacks them. A full frame makes the next one full too, which fills both buffers.
    const int outH = s_geo.outH;
    int y0 = 0, y1 = 0;
    if (clear || changed || moved || in.gfx_h <= 0) {
      y1 = outH;
    } else if (in.dirty_y1 > in.dirty_y0) {
      if (s_geo.zoom) {
        y0 = in.dirty_y0 - s_geo.vy;
        y1 = in.dirty_y1 - s_geo.vy;
      } else {
        y0 = in.dirty_y0 * kOutH / in.gfx_h - 1;
        y1 = (in.dirty_y1 - 1) * kOutH / in.gfx_h + 2;
      }
      if (y0 < 0) y0 = 0;
      if (y1 > outH) y1 = outH;
    }
    int r0 = y0, r1 = y1;                      // with the last frame's rows
    if (s_pendY1 > s_pendY0) {
      if (r1 <= r0) { r0 = s_pendY0; r1 = s_pendY1; }
      else { r0 = r0 < s_pendY0 ? r0 : s_pendY0; r1 = r1 > s_pendY1 ? r1 : s_pendY1; }
    }
    if (r1 > outH) r1 = outH;                  // rows of the other layout (the zoom toggled)
    s_pendY0 = y0;
    s_pendY1 = y1;
    if (r1 <= r0) return false;                // nothing written: the panel is up to date
    if (s_geo.zoom) renderZoom(dst, fb, s_geo, r0, r1);
    else renderGraphics(dst, fb, in.gfx_w, in.gfx_h, r0, r1);
    if (r0 > 0 || r1 < outH) {
      s_spanY = s_geo.top + r0;
      s_spanH = r1 - r0;
    }
    return true;
  }
  s_pendY0 = 0;                                // text or blank next: graphics start over in full
  s_pendY1 = kOutH;
#endif
  if (in.mode == 1) {
    // the cleared buffer needs the text again even if the text did not change
    const bool drawn = renderText(dst, in, nowMs, clear);
    return drawn || clear;
  }
  return clear;
}

bool pcvid_thumb(const uint16_t *frame, void (*row)(void *, const uint16_t *), void *ctx) {
  VGAPicoInfo in;
  t386_info(&in);
  int y0, h;
  if (in.mode == 2) {
    y0 = s_geo.top;
    h = s_geo.outH;
  } else if (in.mode == 1 && textCellH(in)) {
    h = in.rows * textCellH(in);
    y0 = (SCREEN_H - h) / 2;
  } else {
    return false;                              // blank, or nothing pc_video draws
  }
#if !defined(PICO386_LOWMEM)
  if (!frame) return false;
#endif
  s_th = ThumbTap();
  s_th.out = row;
  s_th.ctx = ctx;
  s_th.y0 = y0;
  s_th.h = h;
  s_th.on = true;
#if defined(PICO386_LOWMEM)
  // No frame to read: the screen is drawn again in full and its rows caught on their way out.
  (void)frame;
  pcvid_reset();
  pcvid_render(nullptr, nullptr, hal_millis());
#else
  thumbRows(0, frame, SCREEN_H);
#endif
  thumbEnd();
  return true;
}
