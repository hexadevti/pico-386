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
#include <string.h>
#include "pc_internal.h"
#include "pc_glue.h"
#include "../hal/hal.h"
#include "../frontend/settings.h"
#include "pc_textfont.h"

extern "C" const uint8_t vgafont16[];          // tiny386's copy of the VGA BIOS's 8x16 font

namespace {

constexpr int kOutH = SCREEN_W * 3 / 4;        // 240: every graphics mode is shown 4:3
constexpr int kMaxCellW = 8, kMaxCellH = 16;

int s_lastMode = -1, s_lastW, s_lastH;
int s_lastFontSize = -1;
int s_clearFrames;                             // both video buffers need their borders cleared
bool s_gfxFull = true;

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
    hal_panel_push(0, SCREEN_Y0 + (SCREEN_H - kOutH) / 2 + s_bandY0, SCREEN_W, s_bandRows, s_band, SCREEN_W);
    thumbRows((SCREEN_H - kOutH) / 2 + s_bandY0, s_band, s_bandRows);
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

void renderGraphics(uint16_t *dst, const uint16_t *fb, int w, int h) {
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
  uint16_t *out = dst + (SCREEN_H - kOutH) / 2 * SCREEN_W;
  for (int y = 0; y < kOutH; y++, out += SCREEN_W) {
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

bool pcvid_render(uint16_t *dst, const uint16_t *fb, uint32_t nowMs) {
  VGAPicoInfo in;
  t386_info(&in);
  bool changed = in.mode != s_lastMode || g_settings.fontSize != s_lastFontSize;
  s_lastFontSize = g_settings.fontSize;
  if (in.mode == 2) {
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
    t386_refresh(s_gfxFull || changed);
#if defined(PICO386_LOWMEM)
    flushBand();
#endif
    s_gfxFull = false;
    if (in.gfx_w != s_lastW || in.gfx_h != s_lastH) changed = true;
    // read again: the refresh is what measures the picture
    t386_info(&in);
    if (in.gfx_w != s_lastW || in.gfx_h != s_lastH) changed = true;
#if defined(PICO386_LOWMEM)
    // The refresh found a new size and drew it over the old picture's borders: clear them now,
    // the next frame draws the picture again.
    if (in.gfx_w != preW || in.gfx_h != preH) hal_panel_fill(0, SCREEN_Y0, SCREEN_W, SCREEN_H - 10, 0);
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
    renderGraphics(dst, fb, in.gfx_w, in.gfx_h);
    return true;
  }
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
    y0 = (SCREEN_H - kOutH) / 2;
    h = kOutH;
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
