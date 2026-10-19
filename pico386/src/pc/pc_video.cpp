// pc_video.cpp -- the guest screen on the 320x320 panel.
//
// Graphics modes: tiny386 draws the picture at its real size (320x200, 640x480, ...) top-left in
// its framebuffer, and it is scaled here to 320x240 (4:3) in the middle of the panel, with each
// panel pixel the average of the 2x2 source pixels nearest its footprint. 640x480 becomes an
// exact 2:1 box filter; 320x200 is a plain line stretch.
//
// Text modes are not drawn through tiny386 at all (720x400 would not survive the scaling). Text
// RAM is read directly and drawn with the guest's own font, shrunk to the cell size that fits
// the panel (4x12 for 80x25: 320x300) with 4 levels of antialiasing. The frame is skipped when
// neither the text, the attributes, the palette nor the cursor changed.
#include <string.h>
#include "pc_internal.h"
#include "pc_glue.h"
#include "../hal/hal.h"

namespace {

constexpr int kOutH = SCREEN_W * 3 / 4;        // 240: every graphics mode is shown 4:3
constexpr int kMaxCellW = 8, kMaxCellH = 16;

int s_lastMode = -1, s_lastW, s_lastH;
int s_clearFrames;                             // both video buffers need their borders cleared
bool s_gfxFull = true;

// ---- graphics ----------------------------------------------------------------------------------
inline uint16_t avg2(uint16_t a, uint16_t b) {
  return (uint16_t)(((a ^ b) & 0xF7DEu) >> 1) + (a & b);
}

int16_t s_x0[SCREEN_W], s_x1[SCREEN_W];

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
uint8_t *s_glyphs;                             // 256 glyphs x cellW x cellH coverage levels 0..3
int s_glyphW, s_glyphH, s_glyphCh;
uint32_t s_fontHash;
uint32_t s_textHash;

uint32_t hashBytes(uint32_t h, const uint8_t *p, int n, int step) {
  for (int i = 0; i < n; i += step) h = (h ^ p[i]) * 16777619u;
  return h;
}

// Shrinks the VGA font (8 dots wide, cheight lines) to cellW x cellH: each output dot is the
// share of lit source dots under it, rounded up to the next of 4 levels so thin strokes survive.
void buildGlyphs(const uint8_t *font, int cheight, int cw, int ch) {
  if (!s_glyphs) s_glyphs = (uint8_t *)hal_alloc_big(256 * kMaxCellW * kMaxCellH);
  if (!s_glyphs) return;
  for (int c = 0; c < 256; c++) {
    uint8_t *g = s_glyphs + c * cw * ch;
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

bool renderText(uint16_t *dst, const VGAPicoInfo &in, uint32_t nowMs, bool force) {
  const int cols = in.cols, rows = in.rows;
  if (cols <= 0 || rows <= 0 || cols > 160 || rows > 100 || in.cheight <= 0) return false;
  const int cw = SCREEN_W / cols < 1 ? 1 : SCREEN_W / cols > kMaxCellW ? kMaxCellW : SCREEN_W / cols;
  int ch = SCREEN_H / rows;
  if (ch > 12) ch = 12;
  if (ch < 1) ch = 1;
  const int cheight = in.cheight > 32 ? 32 : in.cheight;

  // font: rebuilt when its bits (or the cell size) change
  uint32_t fh = 2166136261u;
  for (int c = 0; c < 256; c++) fh = hashBytes(fh, in.font + c * 32 * 4, cheight * 4, 4);
  if (fh != s_fontHash || cw != s_glyphW || ch != s_glyphH || cheight != s_glyphCh) {
    buildGlyphs(in.font, cheight, cw, ch);
    s_fontHash = fh;
    force = true;
  }
  if (!s_glyphs) return false;

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
    uint16_t *line = dst + (y0 + y * ch) * SCREEN_W + x0;
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
      const uint8_t *g = s_glyphs + c * cw * ch;
      uint16_t *p = line;
      for (int gy = 0; gy < ch; gy++, p += SCREEN_W, g += cw)
        for (int gx = 0; gx < cw; gx++) p[gx] = lut[g[gx]];
      if (y == curY && x == curX && blinkOn && cs <= ce) {
        uint16_t *q = line + curTop * SCREEN_W;
        for (int gy = curTop; gy <= curBot && gy < ch; gy++, q += SCREEN_W)
          for (int gx = 0; gx < cw; gx++) q[gx] = fg;
      }
    }
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
  bool changed = in.mode != s_lastMode;
  if (in.mode == 2) {
    t386_refresh(s_gfxFull || changed);
    s_gfxFull = false;
    if (in.gfx_w != s_lastW || in.gfx_h != s_lastH) changed = true;
    // read again: the refresh is what measures the picture
    t386_info(&in);
    if (in.gfx_w != s_lastW || in.gfx_h != s_lastH) changed = true;
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
  if (clear) {
    memset(dst, 0, SCREEN_W * SCREEN_H * 2);
    s_clearFrames--;
  }

  if (in.mode == 2) {
    renderGraphics(dst, fb, in.gfx_w, in.gfx_h);
    return true;
  }
  if (in.mode == 1) {
    // the cleared buffer needs the text again even if the text did not change
    const bool drawn = renderText(dst, in, nowMs, clear);
    return drawn || clear;
  }
  return clear;
}
