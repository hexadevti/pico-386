// display_rp2350.cpp -- the PicoCalc's ST7365P/ILI9488 320x320 SPI panel behind a Pico 2.
//
// Transport ported from emu8 (src/picocalc/display_picocalc.cpp, the RP2350 path):
//  * spi1 with one DMA channel. CS is a plain GPIO held low for good: the panel is alone on the
//    bus, and no CS edge can then land between a RAMWR and its pixel data.
//  * Pixels go out in 16-bit SPI frames, which send a uint16_t MSB first: native RGB565 needs no
//    byte swap. Commands and their arguments use 8-bit frames; switching needs an idle shifter.
//  * clk_peri is moved onto clk_sys, or spi1 is capped at 24 MHz.
//
// PICO386_LOWMEM has no game frame buffer: the PC pushes bands with hal_panel_push from the draw
// job on core 1 while the app core may push the bottom strip, so a mutex guards the bus.
// hal_video_back()/front() are null and submit() does nothing.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include <hardware/spi.h>
#include <hardware/dma.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/uart.h>
#include "../hal.h"

void picocalcBacklightOn();              // input_rp2350.cpp: the backlight lives on the STM32

#define LCD_SPI spi1

static int  s_dma = -1;
static dma_channel_config s_cfgInc;      // a pixel buffer
static dma_channel_config s_cfgSolid;    // one colour repeated
static bool s_fmt16 = false;
static uint16_t s_solid = 0;             // the solid fill's DMA source
static SemaphoreHandle_t s_lock = nullptr;

static inline void dcCmd()  { gpio_put(LCD_DC_PIN, 0); }
static inline void dcData() { gpio_put(LCD_DC_PIN, 1); }

// Wait for the shifter to go idle and drain the RX side; spi_set_format() needs it.
static void spiIdle() {
  while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
  while (spi_is_busy(LCD_SPI)) tight_loop_contents();
  while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
  spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;
}

static inline void txWait() { dma_channel_wait_for_finish_blocking(s_dma); }

static void fmt16() {
  if (s_fmt16) return;
  txWait(); spiIdle();
  spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = true;
}
static void fmt8() {
  if (!s_fmt16) return;
  txWait(); spiIdle();
  spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = false;
}

static void txBuf(const uint16_t *src, uint32_t n) {
  dma_channel_configure(s_dma, &s_cfgInc, &spi_get_hw(LCD_SPI)->dr, src, n, true);
}
static void txSolid(uint16_t c, uint32_t n) {
  s_solid = c;
  dma_channel_configure(s_dma, &s_cfgSolid, &spi_get_hw(LCD_SPI)->dr, &s_solid, n, true);
}

static void wrCmd(uint8_t c) {
  fmt8();
  dcCmd();
  spi_write_blocking(LCD_SPI, &c, 1);    // returns once the byte has shifted out
  dcData();
}
static void wrArgs(const uint8_t *d, size_t n) {
  if (n) spi_write_blocking(LCD_SPI, d, n);
}

// CASET / RASET / RAMWR in raw panel coordinates (inclusive bounds), then 16-bit frames.
static void panelWindow(int x0, int y0, int x1, int y1) {
  uint8_t b[4];
  wrCmd(0x2A);
  b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0; b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
  wrArgs(b, 4);
  wrCmd(0x2B);
  b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0; b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
  wrArgs(b, 4);
  wrCmd(0x2C);
  fmt16();
}

static void lcdHwInit() {
  gpio_init(LCD_CS_PIN);  gpio_set_dir(LCD_CS_PIN,  GPIO_OUT); gpio_put(LCD_CS_PIN,  1);
  gpio_init(LCD_DC_PIN);  gpio_set_dir(LCD_DC_PIN,  GPIO_OUT); gpio_put(LCD_DC_PIN,  1);
  gpio_init(LCD_RST_PIN); gpio_set_dir(LCD_RST_PIN, GPIO_OUT); gpio_put(LCD_RST_PIN, 1);

  // clk_peri runs from the 48 MHz USB PLL by default, which caps SPI at 24 MHz. From clk_sys the
  // panel gets LCD_SPI_HZ; the SD card's Arduino SPI computes its divider from clk_peri anyway.
  // The UARTs run from clk_peri too: uart0 (the log, on the mainboard's USB-C) already has its
  // divider, so set its baud again, or everything after this comes out about 5x too fast.
  if (clock_get_hz(clk_peri) != clock_get_hz(clk_sys)) {
    uart_tx_wait_blocking(uart0);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
    uart_set_baudrate(uart0, 115200);
  }
  const uint hz = spi_init(LCD_SPI, LCD_SPI_HZ);
  spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  s_fmt16 = false;
  gpio_set_function(LCD_SCK_PIN,  GPIO_FUNC_SPI);
  gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
  gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);
  hal_log("LCD: SPI at %u kHz", hz / 1000);

  s_dma = dma_claim_unused_channel(true);
  s_cfgInc = dma_channel_get_default_config(s_dma);
  channel_config_set_transfer_data_size(&s_cfgInc, DMA_SIZE_16);
  channel_config_set_dreq(&s_cfgInc, spi_get_dreq(LCD_SPI, true));
  channel_config_set_read_increment(&s_cfgInc, true);
  channel_config_set_write_increment(&s_cfgInc, false);
  s_cfgSolid = s_cfgInc;
  channel_config_set_read_increment(&s_cfgSolid, false);

  // ClockworkPi reset timings: 10 ms low, 200 ms settle.
  gpio_put(LCD_RST_PIN, 1); delay(10);
  gpio_put(LCD_RST_PIN, 0); delay(10);
  gpio_put(LCD_RST_PIN, 1); delay(200);
  gpio_put(LCD_CS_PIN, 0);               // asserted from here on, for good
}

// ST7365P/ILI9488 init: {cmd, nargs, args...}. Same table as the P4 (display_p4.cpp).
static const uint8_t kInit[] = {
  0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
  0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
  0xC0, 2, 0x17, 0x15,
  0xC1, 1, 0x41,
  0xC5, 3, 0x00, 0x12, 0x80,
  0x36, 1, 0x48,
  0x3A, 1, 0x55,
  0xB0, 1, 0x00,
  0xB1, 1, 0xA0,
  0x21, 0,
  0xB4, 1, 0x02,
  0xB6, 3, 0x02, 0x02, 0x3B,
  0xB7, 1, 0xC6,
  0xE9, 1, 0x00,
  0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,
};

static void panelFillLocked(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  panelWindow(x, y, x + w - 1, y + h - 1);
  txSolid(c, (uint32_t)w * (uint32_t)h);
  txWait();
}

// ---- video: no frame buffer under LOWMEM ------------------------------------------------------
uint16_t *hal_video_back() { return nullptr; }
const uint16_t *hal_video_front() { return nullptr; }
uint16_t *hal_video_submit(int, int) { return nullptr; }
void hal_video_wait() {}
bool hal_video_ready() { return true; }

// ---- synchronous pushes -----------------------------------------------------------------------
void hal_panel_fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  panelFillLocked(x, y, w, h, c);
  xSemaphoreGive(s_lock);
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
  xSemaphoreTake(s_lock, portMAX_DELAY);
  panelWindow(x, y, x + w - 1, y + h - 1);
  if (w == stride) {
    txBuf(px, (uint32_t)w * (uint32_t)h);
    txWait();
  } else {
    for (int r = 0; r < h; r++, px += stride) { txBuf(px, (uint32_t)w); txWait(); }
  }
  xSemaphoreGive(s_lock);
}

void hal_display_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  if (y < 0) { px -= y * stride; h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  hal_panel_push(x, y + SCREEN_Y0, w, h, px, stride);
}

// ---- screenshots for the serial remote (system_rp2350.cpp, tools/remote.py) -------------------
// There is no frame buffer to copy, so the panel's own memory is read back: RAMRD (0x2E) on MISO,
// with SCK slowed to SHOT_SPI_HZ (the panel's reads are specified far slower than its writes).
// The bus and the log stay locked for the whole shot, so the PC freezes while it goes out (its
// clock skips the gap) and the picture is one moment.
// The panel answers a dummy byte, then the pixels as written: RGB565, 2 bytes each, high byte
// first (seen on the device, 2026-11-02; the ILI9488's datasheet promises 18-bit reads).
// "@shotraw <y>" dumps a row's start, to check that. A shot is "@shot 320 320", a line per row,
// "@S <y> <base64> <sum>", and "@shot end". A row's bytes are runs of one RGB565 colour,
// [count-1][hi][lo], so each run is 4 base64 characters; sum is their 16-bit byte sum in hex.
#define SHOT_SPI_HZ 4000000

void rp2350LogLock(bool on);             // system_rp2350.cpp

void rp2350Shot(Print &out, int rawRow);

// A read only ends when CS goes high: with CS held low as usual, the panel went on sending after
// RAMRD and garbled the commands that followed (seen on the device, 2026-11-02).
static void csPulse() {
  gpio_put(LCD_CS_PIN, 1);
  busy_wait_us_32(2);
  gpio_put(LCD_CS_PIN, 0);
}

// "@shottest <kHz>": draws a pattern at the start of the last row and dumps it read back at that
// SCK. On the device (2026-11-02) reads were exact up to 16 MHz; at 25 MHz each 1 bit smeared
// into the next.
static uint32_t s_shotHz = SHOT_SPI_HZ;
static bool s_shotTest = false;

void rp2350ShotTest(Print &out, int khz) {
  s_shotHz = khz > 0 ? (uint32_t)khz * 1000 : SHOT_SPI_HZ;
  s_shotTest = true;
  rp2350Shot(out, PANEL_H - 1);
  s_shotTest = false;
  s_shotHz = SHOT_SPI_HZ;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void rp2350Shot(Print &out, int rawRow) {
  if (rawRow >= PANEL_H) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  rp2350LogLock(true);
  if (s_shotTest) {                      // a known pattern at the start of the row, read back below
    static const uint16_t kPat[8] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0x1234, 0xABCD, 0x5555 };
    panelWindow(0, rawRow, 7, rawRow);
    txBuf(kPat, 8);
    txWait();
  }
  const int y0 = rawRow < 0 ? 0 : rawRow, y1 = rawRow < 0 ? PANEL_H - 1 : rawRow;
  uint8_t b[4] = { 0, 0, (uint8_t)((PANEL_W - 1) >> 8), (uint8_t)(PANEL_W - 1) };
  wrCmd(0x2A);
  wrArgs(b, 4);
  b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0; b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
  wrCmd(0x2B);
  wrArgs(b, 4);
  wrCmd(0x2E);
  spiIdle();
  spi_set_baudrate(LCD_SPI, s_shotHz);
  uint8_t dummy;
  spi_read_blocking(LCD_SPI, 0, &dummy, 1);
  char line[80];
  if (rawRow >= 0) {
    uint8_t px[2 * 24];
    spi_read_blocking(LCD_SPI, 0, px, sizeof(px));
    int n = snprintf(line, sizeof(line), "@raw %d %02x:", rawRow, dummy);
    out.write(line, n);
    for (size_t i = 0; i < sizeof(px); i++) { n = snprintf(line, sizeof(line), " %02x", px[i]); out.write(line, n); }
    out.write("\r\n", 2);
  } else {
    int n = snprintf(line, sizeof(line), "@shot %d %d\r\n", PANEL_W, PANEL_H);
    out.write(line, n);
    for (int y = 0; y < PANEL_H; y++) {
      n = snprintf(line, sizeof(line), "@S %d ", y);
      uint16_t sum = 0, run = 0;
      int count = 0;
      auto emit = [&]() {
        const uint8_t t[3] = { (uint8_t)(count - 1), (uint8_t)(run >> 8), (uint8_t)run };
        sum += t[0] + t[1] + t[2];
        const uint32_t v = (uint32_t)t[0] << 16 | (uint32_t)t[1] << 8 | t[2];
        for (int k = 18; k >= 0; k -= 6) line[n++] = kB64[(v >> k) & 63];
        if (n > 64) { out.write(line, n); n = 0; }
      };
      for (int x = 0; x < PANEL_W; x++) {
        uint8_t p[2];
        spi_read_blocking(LCD_SPI, 0, p, 2);
        const uint16_t c = (uint16_t)(p[0] << 8 | p[1]);
        if (count && (c != run || count == 256)) { emit(); count = 0; }
        run = c;
        count++;
      }
      emit();
      n += snprintf(line + n, sizeof(line) - n, " %04x\r\n", sum);
      out.write(line, n);
    }
    out.write("@shot end\r\n", 11);
  }
  spiIdle();
  spi_set_baudrate(LCD_SPI, LCD_SPI_HZ);
  csPulse();                             // the panel keeps sending until CS goes high
  rp2350LogLock(false);
  xSemaphoreGive(s_lock);
}

void rp2350DisplayInit() {
  s_lock = xSemaphoreCreateMutex();
  lcdHwInit();
  for (size_t i = 0; i < sizeof(kInit);) {
    const uint8_t cmd = kInit[i], n = kInit[i + 1];
    wrCmd(cmd);
    wrArgs(&kInit[i + 2], n);
    i += 2 + n;
  }
  wrCmd(0x11); delay(120);               // SLPOUT
  wrCmd(0x29); delay(120);               // DISPON
  panelFillLocked(0, 0, PANEL_W, PANEL_H, 0x0000);
  picocalcBacklightOn();
}

#endif // BOARD_PICOCALC_RP2350
