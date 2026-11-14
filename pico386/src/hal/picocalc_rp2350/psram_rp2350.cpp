// psram_rp2350.cpp -- the PicoCalc mainboard's 8 MB PSRAM (APS6404L class), which holds the
// guest's RAM on the RP2350 build (pc_pagemem.c pages it through SRAM 4 KB at a time).
//
// The chip sits on plain GPIOs (CS 20, SCK 21, SIO0-3 on 2-5), not on the QMI, so it is not
// memory-mapped: a PIO state machine runs the bus and two DMA channels move the data. It runs
// quad (QPI: 4 bits a clock, entered with 0x35) when that passes the self-test, else single-bit SPI
// (2026-11-13: a 1 KB page took about 120 us in SPI, and paging was a fifth of the time).
//
//  * CS and SCK are consecutive pins, so they are the program's 2-bit side-set: every
//    instruction sets both, and a stall (an empty TX FIFO, a full RX FIFO) just holds the clock.
//  * Each transaction is three words from the CPU -- write bits - 1, read bits, then the command
//    byte and 24-bit address -- followed by the data through DMA. Everything is whole 32-bit
//    words (autopull/autopush at 32, MSB first), and the DMA's byte swap puts memory order on the
//    wire, so no transaction leaves bits behind in the shift registers.
//  * SPI reads are 0x0B (fast read): the program makes the 8 wait clocks itself, then samples SIO1
//    on each falling edge, with the input synchroniser bypassed. QPI reads are 0xEB (6 wait
//    clocks, the data lines turned to inputs first) and writes 0x38, a nibble a clock.
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

extern uint8_t g_psramMinDiv;            // system_rp2350.cpp, from rp2350.ini
extern uint8_t g_psramQuad;

static uint     s_sm = 0, s_off = 0;
static int      s_dmaTx = -1, s_dmaRx = -1;
static size_t   s_size = 0;              // 0 until the chip answered and passed the test
static uint32_t s_burst = 32;            // data bytes per CS-low transaction
static bool     s_quad = false;          // the chip is in QPI mode and the quad program runs

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

// A one-byte command in QPI mode (two nibbles on SIO0-3), for leaving it: a reset (0x66, 0x99)
// puts the chip back in SPI mode. In SPI mode the chip sees only SIO0's two bits and ignores them.
static void bbQuadCmd(uint8_t cmd) {
  for (uint i = 0; i < 4; i++) { gpio_init(PSRAM_SIO0_PIN + i); gpio_set_dir(PSRAM_SIO0_PIN + i, GPIO_OUT); }
  gpio_put(PSRAM_CS_PIN, 0);
  busy_wait_us_32(1);
  for (int h = 4; h >= 0; h -= 4) {
    for (uint i = 0; i < 4; i++) gpio_put(PSRAM_SIO0_PIN + i, (cmd >> (h + i)) & 1);
    busy_wait_us_32(1);
    gpio_put(PSRAM_SCK_PIN, 1);
    busy_wait_us_32(1);
    gpio_put(PSRAM_SCK_PIN, 0);
  }
  gpio_put(PSRAM_CS_PIN, 1);
  busy_wait_us_32(1);
  for (uint i = 0; i < 4; i++) gpio_set_dir(PSRAM_SIO0_PIN + i, GPIO_IN);
  gpio_set_dir(PSRAM_MOSI_PIN, GPIO_OUT);    // bbXfer drives SIO0
  gpio_put(PSRAM_MOSI_PIN, 0);
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

// The quad program: the same shape, a nibble per clock, and the data lines turned around.
enum { Q_RLOOP = 0, Q_RMID = 1, Q_DONE = 2, Q_BEGIN = 3, Q_WLOOP = 6, Q_DLOOP = 11, Q_LEN = 14 };

static void buildQuad(uint16_t *p, uint waitClocks) {
  p[0]  = ss(pio_encode_in(pio_pins, 4), SS_LOW);              // rloop: SCK falls, sample SIO0-3
  p[1]  = ss(pio_encode_jmp_y_dec(Q_RLOOP), SS_HIGH);          // rmid:  SCK rises
  p[2]  = ss(pio_encode_nop(), SS_IDLE);                       // done:  CS high
  p[3]  = ss(pio_encode_set(pio_pindirs, 0xf), SS_IDLE);       // begin: drive SIO0-3
  p[4]  = ss(pio_encode_out(pio_x, 32), SS_IDLE);              //        x = write nibbles - 1
  p[5]  = ss(pio_encode_out(pio_y, 32), SS_IDLE);              //        y = read nibbles
  p[6]  = ss(pio_encode_out(pio_pins, 4), SS_LOW);             // wloop: data out, SCK low
  p[7]  = ss(pio_encode_jmp_x_dec(Q_WLOOP), SS_HIGH);          //        the chip latches it
  p[8]  = ss(pio_encode_jmp_not_y(Q_DONE), SS_LOW);            //        write only: done
  p[9]  = ss(pio_encode_set(pio_pindirs, 0), SS_LOW);          //        let go of SIO0-3
  p[10] = ss(pio_encode_set(pio_x, waitClocks - 1), SS_LOW);   // the read's wait clocks
  p[11] = ss(pio_encode_nop(), SS_HIGH);                       // dloop
  p[12] = ss(pio_encode_jmp_x_dec(Q_DLOOP), SS_LOW);
  p[13] = ss(pio_encode_jmp(Q_RMID), SS_LOW);                  // first nibble is out: read
}
#define PROG_MAX Q_LEN

static void smStart(float clkdiv, uint waitClocks, bool quad) {
  pio_sm_set_enabled(PSRAM_PIO, s_sm, false);
  uint16_t prog[PROG_MAX];
  const int len = quad ? Q_LEN : P_LEN;
  if (quad) buildQuad(prog, waitClocks); else buildProgram(prog, waitClocks);
  for (int i = 0; i < len; i++) {             // jumps are relative to the program's start
    uint16_t ins = prog[i];
    if ((ins & 0xe000) == 0x0000) ins = (uint16_t)(ins + s_off);   // JMP: add the load offset
    PSRAM_PIO->instr_mem[s_off + i] = ins;
  }
  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, s_off, s_off + len - 1);
  sm_config_set_sideset(&c, 2, false, false);
  sm_config_set_sideset_pins(&c, PSRAM_CS_PIN);
  const uint32_t ctl = (1u << PSRAM_CS_PIN) | (1u << PSRAM_SCK_PIN), sio = 0xfu << PSRAM_SIO0_PIN;
  if (quad) {
    sm_config_set_out_pins(&c, PSRAM_SIO0_PIN, 4);
    sm_config_set_set_pins(&c, PSRAM_SIO0_PIN, 4);
    sm_config_set_in_pins(&c, PSRAM_SIO0_PIN);
  } else {
    sm_config_set_out_pins(&c, PSRAM_MOSI_PIN, 1);
    sm_config_set_in_pins(&c, PSRAM_MISO_PIN);
  }
  sm_config_set_out_shift(&c, false, true, 32);
  sm_config_set_in_shift(&c, false, true, 32);
  sm_config_set_clkdiv(&c, clkdiv);
  pio_sm_init(PSRAM_PIO, s_sm, s_off + (quad ? Q_BEGIN : P_BEGIN), &c);
  pio_sm_set_pins_with_mask(PSRAM_PIO, s_sm, 1u << PSRAM_CS_PIN, ctl);
  pio_sm_set_pindirs_with_mask(PSRAM_PIO, s_sm, quad ? ctl | sio : ctl | (1u << PSRAM_MOSI_PIN),
                               quad ? ctl | sio : ctl | (3u << PSRAM_SIO0_PIN));
  pio_sm_clear_fifos(PSRAM_PIO, s_sm);
  pio_sm_set_enabled(PSRAM_PIO, s_sm, true);
  s_quad = quad;

  // Bursts: whole words, a power of two (so they never straddle a 1 KB page), CS low <= 8 us.
  // SPI spends about 48 clocks on the command, address and wait; quad about 16, and moves a byte
  // in 2 clocks, not 8.
  const uint32_t sck = (uint32_t)(clock_get_hz(clk_sys) / (2.0f * clkdiv));
  const uint32_t clocks = (uint32_t)((uint64_t)sck * PSRAM_CS_MAX_US / 1000000);
  const uint32_t room = quad ? (clocks > 32 ? (clocks - 16) / 2 : 4) : (clocks > 64 ? (clocks - 48) / 8 : 4);
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
  if (s_quad) {                               // x counts nibbles: 8 for the command and address
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 8 + n * 2 - 1);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 0);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 0x38000000u | a);
  } else {
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 32 + n * 8 - 1);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 0);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 0x02000000u | a);
  }
  dma_channel_transfer_from_buffer_now(s_dmaTx, src, n / 4);
  dma_channel_wait_for_finish_blocking(s_dmaTx);
}

static void burstRead(uint32_t a, void *dst, uint32_t n) {
  dma_channel_transfer_to_buffer_now(s_dmaRx, dst, n / 4);
  if (s_quad) {
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 8 - 1);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, n * 2);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 0xEB000000u | a);
  } else {
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 32 - 1);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, n * 8);
    pio_sm_put_blocking(PSRAM_PIO, s_sm, 0x0B000000u | a);
  }
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
  static uint32_t w[64], r[64];         // 256 bytes: two of the longest bursts
  static const uint32_t at[] = { 0, PSRAM_BYTES / 2 - 128, PSRAM_BYTES - sizeof w };
  for (unsigned k = 0; k < sizeof at / sizeof at[0]; k++) {
    uint32_t x = 0x9E3779B9u * (k + 1);
    for (int i = 0; i < 64; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; w[i] = x; }
    rawWrite(at[k], (const uint8_t *)w, sizeof w);
    memset(r, 0, sizeof r);
    rawRead(at[k], (uint8_t *)r, sizeof r);
    if (memcmp(w, r, sizeof w)) return false;
  }
  return true;
}

static const float kDiv[] = { 2.0f, 3.0f, 4.0f, 6.0f, 10.0f };

// Tries each clock divider (fastest first; rp2350.ini's psram_div skips the faster ones, for
// boards where the fastest passes here but fails later, g_psramMinDiv) and each wait count, in
// the mode the chip is in. Leaves the first setting that passes running.
static bool calibrate(bool quad) {
  static const uint kWaitSpi[] = { 8, 9, 7 }, kWaitQuad[] = { 6, 7, 5, 8 };
  const uint *waits = quad ? kWaitQuad : kWaitSpi;
  const int nw = quad ? 4 : 3;
  const int nd = sizeof kDiv / sizeof kDiv[0];
  for (int d = 0; d < nd; d++) {
    const float div = kDiv[d];
    if (div < g_psramMinDiv && d < nd - 1) continue;
    for (int w = 0; w < nw; w++) {
      smStart(div, waits[w], quad);
      s_size = PSRAM_BYTES;                 // selfTest goes through the raw paths
      if (selfTest()) {
        hal_log("psram: %u KB, %s, SCK %u kHz, %u wait clocks, %u-byte bursts", (unsigned)(PSRAM_BYTES >> 10),
                quad ? "quad" : "SPI", (unsigned)(clock_get_hz(clk_sys) / (2.0f * div) / 1000), waits[w],
                (unsigned)s_burst);
        return true;
      }
      s_size = 0;
    }
  }
  return false;
}

static void pioPins(bool quad) {
  pio_gpio_init(PSRAM_PIO, PSRAM_CS_PIN);
  pio_gpio_init(PSRAM_PIO, PSRAM_SCK_PIN);
  for (uint i = 0; i < (quad ? 4u : 2u); i++) pio_gpio_init(PSRAM_PIO, PSRAM_SIO0_PIN + i);
  gpio_set_slew_rate(PSRAM_SCK_PIN, GPIO_SLEW_RATE_FAST);
  for (uint i = 0; i < 4; i++) gpio_set_slew_rate(PSRAM_SIO0_PIN + i, GPIO_SLEW_RATE_FAST);
  hw_set_bits(&PSRAM_PIO->input_sync_bypass, (quad ? 0xfu : 2u) << PSRAM_SIO0_PIN);
}

void rp2350PsramInit() {
  bbInit();
  // A warm reboot (to the loader and back) can find the chip still in QPI mode: reset it in quad
  // first, which SPI mode ignores, then in SPI.
  bbQuadCmd(0x66);
  bbQuadCmd(0x99);
  busy_wait_us_32(100);
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
    uint16_t prog[PROG_MAX];
    buildQuad(prog, 6);                     // the bigger program reserves the room for both
    pio_program_t p = {};
    p.instructions = prog;
    p.length = PROG_MAX;
    p.origin = -1;
    s_off = pio_add_program(PSRAM_PIO, &p);
  }
  dmaInit();

  if (g_psramQuad) {
    static const uint8_t qpi = 0x35;        // enter QPI, sent in SPI
    bbXfer(&qpi, 1, nullptr, 0);
    pioPins(true);
    if (calibrate(true)) return;
    // No quad here (SIO2/SIO3 not wired, or too slow): reset the chip back to SPI.
    pio_sm_set_enabled(PSRAM_PIO, s_sm, false);
    bbInit();
    bbQuadCmd(0x66);
    bbQuadCmd(0x99);
    busy_wait_us_32(100);
    hal_log("psram: quad failed its test, using SPI");
  }
  pioPins(false);
  if (!calibrate(false)) hal_log("psram: read-back test failed at every speed");
}

#endif // BOARD_PICOCALC_RP2350
