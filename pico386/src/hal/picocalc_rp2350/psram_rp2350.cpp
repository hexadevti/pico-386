// psram_rp2350.cpp -- the PicoCalc mainboard's 8 MB PSRAM (APS6404L class), which holds the
// guest's RAM on the RP2350 build (pc_pagemem.c pages it through SRAM 4 KB at a time).
//
// The chip sits on four plain GPIOs (CS 20, SCK 21, SIO0 2, SIO1 3), not on the QMI, so it is
// not memory-mapped: a PIO state machine runs single-bit SPI and two DMA channels move the data.
//
//  * CS and SCK are consecutive pins, so they are the program's 2-bit side-set: every
//    instruction sets both, and a stall (an empty TX FIFO, a full RX FIFO) just holds the clock.
//  * Each transaction is three words from the CPU -- write bits - 1, read bits, then the command
//    byte and 24-bit address -- followed by the data through DMA. Everything is whole 32-bit
//    words (autopull/autopush at 32, MSB first), and the DMA's byte swap puts memory order on the
//    wire, so no transaction leaves bits behind in the shift registers.
//  * Reads are 0x0B (fast read): the program makes the 8 wait clocks itself, then samples SIO1
//    on each falling edge, with the input synchroniser bypassed.
//  * The chip wraps at 1 KB pages, and must not be held selected for more than 8 us (tCEM: its
//    refresh runs while CS is high), so transfers are split into bursts that respect both.
//
// The clock divider and the number of wait clocks are found at start-up: a pattern is written and
// read back with each setting until one matches. The serial log names the one that won.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>
#include <hardware/dma.h>
#include <hardware/clocks.h>
#include <string.h>
#include "../hal.h"

#define PSRAM_PIO      pio1
#define PSRAM_CS_MAX_US 8                // tCEM
#define PSRAM_PAGE     1024u             // the chip's wrap-around page

static uint     s_sm = 0, s_off = 0;
static int      s_dmaTx = -1, s_dmaRx = -1;
static size_t   s_size = 0;              // 0 until the chip answered and passed the test
static uint32_t s_burst = 32;            // data bytes per CS-low transaction

// ---- bit-banged SPI, only for the reset and ID read before the PIO takes the pins ----------
static void bbInit() {
  const uint pins[] = { PSRAM_CS_PIN, PSRAM_SCK_PIN, PSRAM_MOSI_PIN, PSRAM_MISO_PIN };
  for (uint p : pins) gpio_init(p);
  gpio_set_dir(PSRAM_CS_PIN, GPIO_OUT);   gpio_put(PSRAM_CS_PIN, 1);
  gpio_set_dir(PSRAM_SCK_PIN, GPIO_OUT);  gpio_put(PSRAM_SCK_PIN, 0);
  gpio_set_dir(PSRAM_MOSI_PIN, GPIO_OUT); gpio_put(PSRAM_MOSI_PIN, 0);
  gpio_set_dir(PSRAM_MISO_PIN, GPIO_IN);
}

static void bbXfer(const uint8_t *out, int nout, uint8_t *in, int nin) {
  gpio_put(PSRAM_CS_PIN, 0);
  busy_wait_us_32(1);
  for (int i = 0; i < nout; i++)
    for (int b = 7; b >= 0; b--) {
      gpio_put(PSRAM_MOSI_PIN, (out[i] >> b) & 1);
      busy_wait_us_32(1);
      gpio_put(PSRAM_SCK_PIN, 1);
      busy_wait_us_32(1);
      gpio_put(PSRAM_SCK_PIN, 0);
    }
  for (int i = 0; i < nin; i++) {
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) {
      busy_wait_us_32(1);
      gpio_put(PSRAM_SCK_PIN, 1);
      v = (uint8_t)(v << 1 | gpio_get(PSRAM_MISO_PIN));
      busy_wait_us_32(1);
      gpio_put(PSRAM_SCK_PIN, 0);
    }
    in[i] = v;
  }
  gpio_put(PSRAM_CS_PIN, 1);
  busy_wait_us_32(1);
}

// ---- the PIO program --------------------------------------------------------------------------
// side-set bit 0 = CS, bit 1 = SCK
enum { SS_IDLE = 1, SS_LOW = 0, SS_HIGH = 2 };
enum { P_RLOOP = 0, P_RMID = 1, P_DONE = 2, P_BEGIN = 3, P_WLOOP = 5, P_DUMMY = 8, P_DLOOP = 9, P_LEN = 12 };

static inline uint16_t ss(uint16_t ins, uint v) { return (uint16_t)(ins | pio_encode_sideset(2, v)); }

static void buildProgram(uint16_t *p, uint waitClocks) {
  p[0]  = ss(pio_encode_in(pio_pins, 1), SS_LOW);              // rloop: SCK falls, sample SIO1
  p[1]  = ss(pio_encode_jmp_y_dec(P_RLOOP), SS_HIGH);          // rmid:  SCK rises
  p[2]  = ss(pio_encode_nop(), SS_IDLE);                       // done:  CS high
  p[3]  = ss(pio_encode_out(pio_x, 32), SS_IDLE);              // begin: x = write bits - 1
  p[4]  = ss(pio_encode_out(pio_y, 32), SS_IDLE);              //        y = read bits
  p[5]  = ss(pio_encode_out(pio_pins, 1), SS_LOW);             // wloop: data out, SCK low
  p[6]  = ss(pio_encode_jmp_x_dec(P_WLOOP), SS_HIGH);          //        the chip latches it
  p[7]  = ss(pio_encode_jmp_not_y(P_DONE), SS_LOW);            //        write only: done
  p[8]  = ss(pio_encode_set(pio_x, waitClocks - 1), SS_LOW);   // the fast read's wait clocks
  p[9]  = ss(pio_encode_nop(), SS_HIGH);                       // dloop
  p[10] = ss(pio_encode_jmp_x_dec(P_DLOOP), SS_LOW);
  p[11] = ss(pio_encode_jmp(P_RMID), SS_LOW);                  // first data bit is out: read
}

static void smStart(float clkdiv, uint waitClocks) {
  pio_sm_set_enabled(PSRAM_PIO, s_sm, false);
  uint16_t prog[P_LEN];
  buildProgram(prog, waitClocks);
  for (int i = 0; i < P_LEN; i++) {           // jumps are relative to the program's start
    uint16_t ins = prog[i];
    if ((ins & 0xe000) == 0x0000) ins = (uint16_t)(ins + s_off);   // JMP: add the load offset
    PSRAM_PIO->instr_mem[s_off + i] = ins;
  }
  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, s_off, s_off + P_LEN - 1);
  sm_config_set_sideset(&c, 2, false, false);
  sm_config_set_sideset_pins(&c, PSRAM_CS_PIN);
  sm_config_set_out_pins(&c, PSRAM_MOSI_PIN, 1);
  sm_config_set_in_pins(&c, PSRAM_MISO_PIN);
  sm_config_set_out_shift(&c, false, true, 32);
  sm_config_set_in_shift(&c, false, true, 32);
  sm_config_set_clkdiv(&c, clkdiv);
  pio_sm_init(PSRAM_PIO, s_sm, s_off + P_BEGIN, &c);
  pio_sm_set_pins_with_mask(PSRAM_PIO, s_sm, 1u << PSRAM_CS_PIN, (1u << PSRAM_CS_PIN) | (1u << PSRAM_SCK_PIN));
  pio_sm_set_pindirs_with_mask(PSRAM_PIO, s_sm,
                               (1u << PSRAM_CS_PIN) | (1u << PSRAM_SCK_PIN) | (1u << PSRAM_MOSI_PIN),
                               (1u << PSRAM_CS_PIN) | (1u << PSRAM_SCK_PIN) | (1u << PSRAM_MOSI_PIN) |
                               (1u << PSRAM_MISO_PIN));
  pio_sm_clear_fifos(PSRAM_PIO, s_sm);
  pio_sm_set_enabled(PSRAM_PIO, s_sm, true);

  // Bursts: whole words, a power of two (so they never straddle a 1 KB page), CS low <= 8 us.
  const uint32_t sck = (uint32_t)(clock_get_hz(clk_sys) / (2.0f * clkdiv));
  const uint32_t bits = (uint32_t)((uint64_t)sck * PSRAM_CS_MAX_US / 1000000);
  const uint32_t room = bits > 64 ? (bits - 48) / 8 : 4;
  s_burst = 4;
  while (s_burst * 2 <= room && s_burst * 2 <= PSRAM_PAGE) s_burst *= 2;
}

static void dmaInit() {
  s_dmaTx = dma_claim_unused_channel(true);
  s_dmaRx = dma_claim_unused_channel(true);
  dma_channel_config t = dma_channel_get_default_config(s_dmaTx);
  channel_config_set_transfer_data_size(&t, DMA_SIZE_32);
  channel_config_set_bswap(&t, true);                // memory order first on the wire
  channel_config_set_read_increment(&t, true);
  channel_config_set_write_increment(&t, false);
  channel_config_set_dreq(&t, pio_get_dreq(PSRAM_PIO, s_sm, true));
  dma_channel_configure(s_dmaTx, &t, &PSRAM_PIO->txf[s_sm], nullptr, 0, false);
  dma_channel_config r = dma_channel_get_default_config(s_dmaRx);
  channel_config_set_transfer_data_size(&r, DMA_SIZE_32);
  channel_config_set_bswap(&r, true);
  channel_config_set_read_increment(&r, false);
  channel_config_set_write_increment(&r, true);
  channel_config_set_dreq(&r, pio_get_dreq(PSRAM_PIO, s_sm, false));
  dma_channel_configure(s_dmaRx, &r, nullptr, &PSRAM_PIO->rxf[s_sm], 0, false);
}

// One CS-low transaction each. n is a multiple of 4, and the range stays inside one chip page.
static void burstWrite(uint32_t a, const void *src, uint32_t n) {
  pio_sm_put_blocking(PSRAM_PIO, s_sm, 32 + n * 8 - 1);
  pio_sm_put_blocking(PSRAM_PIO, s_sm, 0);
  pio_sm_put_blocking(PSRAM_PIO, s_sm, 0x02000000u | a);
  dma_channel_transfer_from_buffer_now(s_dmaTx, src, n / 4);
  dma_channel_wait_for_finish_blocking(s_dmaTx);
}

static void burstRead(uint32_t a, void *dst, uint32_t n) {
  dma_channel_transfer_to_buffer_now(s_dmaRx, dst, n / 4);
  pio_sm_put_blocking(PSRAM_PIO, s_sm, 32 - 1);
  pio_sm_put_blocking(PSRAM_PIO, s_sm, n * 8);
  pio_sm_put_blocking(PSRAM_PIO, s_sm, 0x0B000000u | a);
  dma_channel_wait_for_finish_blocking(s_dmaRx);
}

// The pager moves whole 4 KB pages and the menu parks 256 KB; both are word-aligned.
static void rawWrite(uint32_t a, const uint8_t *src, size_t n) {
  while (n) {
    uint32_t k = s_burst - (a & (s_burst - 1));
    if (k > n) k = (uint32_t)n;
    burstWrite(a, src, k);
    a += k; src += k; n -= k;
  }
}

static void rawRead(uint32_t a, uint8_t *dst, size_t n) {
  while (n) {
    uint32_t k = s_burst - (a & (s_burst - 1));
    if (k > n) k = (uint32_t)n;
    burstRead(a, dst, k);
    a += k; dst += k; n -= k;
  }
}

extern "C" size_t hal_psram_size(void) { return s_size; }

// The bursts move whole words by DMA. The pager and the park area are word-aligned, but the
// browser's names and the zip inflater's window are not: those go through this buffer, a few
// words at a time, and a write reads the words it only partly covers first.
static uint32_t s_bounce[16];

extern "C" void hal_psram_read(uint32_t a, void *dst, size_t n) {
  if (!s_size || a + n > s_size) { hal_log("psram: bad read %lx+%u", (unsigned long)a, (unsigned)n); return; }
  if (!((a | n | (uintptr_t)dst) & 3)) { rawRead(a, (uint8_t *)dst, n); return; }
  uint8_t *d = (uint8_t *)dst;
  while (n) {
    const uint32_t off = a & 3, k = n < sizeof s_bounce - off ? (uint32_t)n : sizeof s_bounce - off;
    rawRead(a - off, (uint8_t *)s_bounce, (off + k + 3) & ~3u);
    memcpy(d, (uint8_t *)s_bounce + off, k);
    a += k; d += k; n -= k;
  }
}

extern "C" void hal_psram_write(uint32_t a, const void *src, size_t n) {
  if (!s_size || a + n > s_size) { hal_log("psram: bad write %lx+%u", (unsigned long)a, (unsigned)n); return; }
  if (!((a | n | (uintptr_t)src) & 3)) { rawWrite(a, (const uint8_t *)src, n); return; }
  const uint8_t *p = (const uint8_t *)src;
  while (n) {
    const uint32_t off = a & 3, k = n < sizeof s_bounce - off ? (uint32_t)n : sizeof s_bounce - off;
    const uint32_t span = (off + k + 3) & ~3u;
    if (off || (k & 3)) rawRead(a - off, (uint8_t *)s_bounce, span);
    memcpy((uint8_t *)s_bounce + off, p, k);
    rawWrite(a - off, (const uint8_t *)s_bounce, span);
    a += k; p += k; n -= k;
  }
}

// A pattern at both ends of the chip and across a page boundary, written then read back.
static bool selfTest() {
  static uint32_t w[256], r[256];
  static const uint32_t at[] = { 0, PSRAM_BYTES / 2 - 512, PSRAM_BYTES - sizeof w };
  for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
    uint32_t x = 0x9E3779B9u * (k + 1);
    for (int i = 0; i < 256; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; w[i] = x; }
    rawWrite(at[k], (const uint8_t *)w, sizeof w);
    memset(r, 0, sizeof r);
    rawRead(at[k], (uint8_t *)r, sizeof r);
    if (memcmp(w, r, sizeof w)) return false;
  }
  return true;
}

void rp2350PsramInit() {
  bbInit();
  static const uint8_t rsten = 0x66, rst = 0x99;
  bbXfer(&rsten, 1, nullptr, 0);
  bbXfer(&rst, 1, nullptr, 0);
  busy_wait_us_32(100);
  static const uint8_t rdid[4] = { 0x9F, 0, 0, 0 };
  uint8_t id[2] = {};
  bbXfer(rdid, 4, id, 2);
  hal_log("psram: ID %02X, KGD %02X (%s)", id[0], id[1], id[1] == 0x5D ? "good" : "no chip?");
  if (id[1] != 0x5D) return;

  s_sm = (uint)pio_claim_unused_sm(PSRAM_PIO, true);
  {
    uint16_t prog[P_LEN];
    buildProgram(prog, 8);
    pio_program_t p = {};
    p.instructions = prog;
    p.length = P_LEN;
    p.origin = -1;
    s_off = pio_add_program(PSRAM_PIO, &p);
  }
  const uint pins[] = { PSRAM_CS_PIN, PSRAM_SCK_PIN, PSRAM_MOSI_PIN, PSRAM_MISO_PIN };
  for (uint pin : pins) pio_gpio_init(PSRAM_PIO, pin);
  gpio_set_slew_rate(PSRAM_SCK_PIN, GPIO_SLEW_RATE_FAST);
  gpio_set_slew_rate(PSRAM_MOSI_PIN, GPIO_SLEW_RATE_FAST);
  hw_set_bits(&PSRAM_PIO->input_sync_bypass, 1u << PSRAM_MISO_PIN);
  dmaInit();

  // Fastest first: SCK = clk_sys / (2 * div).
  static const float kDiv[] = { 2.0f, 3.0f, 4.0f, 6.0f, 10.0f };
  static const uint  kWait[] = { 8, 9, 7 };
  for (float div : kDiv)
    for (uint wc : kWait) {
      smStart(div, wc);
      s_size = PSRAM_BYTES;                 // selfTest goes through the raw paths
      if (selfTest()) {
        hal_log("psram: %u KB, SCK %u kHz, %u wait clocks, %u-byte bursts", (unsigned)(PSRAM_BYTES >> 10),
                (unsigned)(clock_get_hz(clk_sys) / (2.0f * div) / 1000), wc, (unsigned)s_burst);
        return;
      }
      s_size = 0;
    }
  hal_log("psram: read-back test failed at every speed");
}

#endif // BOARD_PICOCALC_RP2350
