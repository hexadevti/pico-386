// audio_p4.cpp -- PicoCalc speakers from an ESP32-P4: I2S0 in PDM TX, DAC line mode, which turns
// 16-bit PCM into a sigma-delta bitstream on two pins (left on dout, right on dout2) straight into
// the mainboard's RC filter + amp. From emu8 (src/picocalc/audio_picocalc_p4.cpp).
//
// The PDM side always runs at AUDIO_OUT_RATE; the core's rate (53267 Hz for the Mega Drive) is
// brought to it by linear interpolation in 16.16 fixed point -- the same loop handles up- and
// down-sampling (it emits zero or more output frames per input sample). Writes block on the DMA,
// which is what paces the emulator at real speed.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_P4)

#include <Arduino.h>
#include "driver/i2s_pdm.h"
#include "../hal.h"

static i2s_chan_handle_t s_tx = nullptr;
static uint32_t s_step = 1u << 16;     // input-to-output rate ratio, 16.16
static uint32_t s_phase = 0;
static int32_t  s_prev = 0;
static int32_t  s_gain = 256;          // 8.8
static int16_t  s_out[256];
static int      s_outN = 0;
static uint32_t s_waitUs = 0;
static volatile uint32_t s_underruns = 0;
static const int kDmaDescs = 12, kDmaFrames = 256;
// What is queued, from a mirror of the driver's free-buffer queue (esp_driver_i2s i2s_common.c):
// every buffer the DMA finishes is cleared and goes on that queue (11 deep; when full, the oldest
// is dropped and on_send_q_ovf fires), and i2s_channel_write takes buffers from it, waiting when
// it is empty. s_free mirrors its length, s_fill how much of the buffer being written is filled.
// Counting frames written against buffers sent read about 40 ms low (2026-11-03): sent buffers
// that were silence (ahead of the first data after a quiet spell, or the half the driver leaves
// when it moves on early) counted as played data. pc_audio then saw an empty queue at 22 ms,
// kept topping it up, and every write blocked: the guest got 5-50% of the time, and DOS took over
// a minute to boot.
static volatile int s_free = 0;
static int s_fill = kDmaFrames;
static bool s_cur = false;

static bool IRAM_ATTR onSent(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  if (s_free < kDmaDescs - 1) s_free++;
  return false;
}

// Every buffer is free: the DMA plays cleared (silent) ones, heard as a tick.
static bool IRAM_ATTR onSendEmpty(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  s_underruns++;
  return false;
}

// Frames written and not yet played: the buffers taken from the free queue, less the one being
// filled, plus what that one holds (the buffer the DMA is playing is left out).
static int queuedFrames() {
  if (!s_cur) return 0;
  const int taken = kDmaDescs - 1 - s_free;
  return taken > 0 ? (taken - 1) * kDmaFrames + s_fill : s_fill;
}

// After a write: the buffers it took, by the driver's rule (a new one when the current one is
// full, or when at most one slot of the free queue is left, i.e. nearly everything has played).
// The driver took them during the write, after on_sent had freed any it waited for.
static void tookFrames(int n) {
  while (n > 0) {
    if (!s_cur || s_fill == kDmaFrames || s_free >= kDmaDescs - 2) {
      int f = s_free;     // on_sent may add one meanwhile
      while (f > 0 && !__atomic_compare_exchange_n((int *)&s_free, &f, f - 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
      s_cur = true;
      s_fill = 0;
    }
    const int k = n < kDmaFrames - s_fill ? n : kDmaFrames - s_fill;
    s_fill += k;
    n -= k;
  }
}

static const int16_t kVolume[11] = { 0, 16, 32, 52, 76, 104, 136, 172, 212, 256, 320 };

void hal_audio_volume(int v) {
  if (v < 0) v = 0;
  if (v > 10) v = 10;
  s_gain = kVolume[v];
}

void hal_audio_start(uint32_t rate) {
  s_step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_OUT_RATE);
  s_phase = 0; s_prev = 0; s_outN = 0;
  if (s_tx) return;

  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // 70 ms of queue. The PC writes ~17 ms at the end of each frame; with the old 23 ms (8 x 128)
  // any frame that ran 6 ms long (an SD read, a mode switch) emptied it and the gap clicked.
  cc.dma_desc_num  = 12;
  cc.dma_frame_num = kDmaFrames;
  cc.auto_clear    = true;             // an underrun plays silence, not the last buffer again
  esp_err_t e = i2s_new_channel(&cc, &s_tx, nullptr);

  i2s_pdm_tx_config_t pc = {
    .clk_cfg  = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(AUDIO_OUT_RATE),
    .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .clk  = I2S_GPIO_UNUSED,
      .dout = (gpio_num_t)AUDIO_L_PIN,
      .dout2 = (gpio_num_t)AUDIO_R_PIN,
      .invert_flags = { .clk_inv = 0 },
    },
  };
  if (e == ESP_OK) e = i2s_channel_init_pdm_tx_mode(s_tx, &pc);
  if (e == ESP_OK) {
    i2s_event_callbacks_t cbs = {};
    cbs.on_sent = onSent;
    cbs.on_send_q_ovf = onSendEmpty;
    e = i2s_channel_register_event_callback(s_tx, &cbs, nullptr);
  }
  if (e == ESP_OK) e = i2s_channel_enable(s_tx);
  if (e != ESP_OK) {
    hal_log("audio: PDM TX init failed (%s)", esp_err_to_name(e));
    if (s_tx) { i2s_del_channel(s_tx); s_tx = nullptr; }
    return;
  }
  hal_log("audio: PDM on GPIO%d/%d at %d Hz (from %u)", AUDIO_L_PIN, AUDIO_R_PIN, AUDIO_OUT_RATE,
          (unsigned)rate);
}

static void flushOut() {
  size_t wrote;
  const uint32_t t0 = micros();
  if (s_tx && s_outN) {
    i2s_channel_write(s_tx, s_out, (size_t)s_outN * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    tookFrames(s_outN);
  }
  s_waitUs += micros() - t0;
  s_outN = 0;
}

static inline void put(int32_t x) {
  while (s_phase < (1u << 16)) {
    int32_t y = s_prev + (((x - s_prev) * (int32_t)(s_phase >> 2)) >> 14);   // fits int32 for any two samples
    y = (y * s_gain) >> 8;
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    s_out[s_outN * 2] = (int16_t)y; s_out[s_outN * 2 + 1] = (int16_t)y;
    if (++s_outN == 128) flushOut();
    s_phase += s_step;
  }
  s_phase -= 1u << 16;
  s_prev = x;
}

void hal_audio_write(const int16_t *mono, int n) {
  for (int i = 0; i < n; i++) put(mono[i]);
  flushOut();
}

// auto_clear plays silence once the queued DMA buffers run out; nothing to drop here.
void hal_audio_silence() { s_outN = 0; }
uint32_t hal_audio_wait_us() { return s_waitUs; }
uint32_t hal_audio_underruns() { return s_underruns; }
uint32_t hal_audio_queued_us() { return (uint32_t)((uint64_t)queuedFrames() * 1000000 / AUDIO_OUT_RATE); }

#endif // BOARD_PICOCALC_P4
