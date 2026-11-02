// audio_rp2350.cpp -- PicoCalc speakers from a Pico 2: arduino-pico's PWMAudio in stereo on
// GPIO26/27 (one PWM slice) into the mainboard's RC filter. The resampler is audio_p4.cpp's; the
// PWMAudio fixes come from emu8 (src/picocalc/audio_picocalc.cpp):
//  * Its write(sync) spins on a full ring, so writes use sync=false and sleep a tick on refusal.
//  * Its silence buffer holds an unscaled 0x80008000, past the PWM wrap: every underrun popped.
//    It is refilled with the scaled mid level.
//  * In stereo the first write only latches L; the second (R) is the one that needs room.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#define private public                   // the silence buffer and the ring's state
#include <PWMAudio.h>
#undef private
#include "../hal.h"

#define AUDIO_NBUF  12                   // 12 x 128 frames at 22050 Hz: 70 ms, as on the P4
#define AUDIO_WORDS 128

static PWMAudio s_dac(AUDIO_L_PIN, true);
static bool     s_on = false;
static uint32_t s_step = 1u << 16;       // input-to-output rate ratio, 16.16
static uint32_t s_phase = 0;
static int32_t  s_prev = 0;
static int32_t  s_gain = 256;            // 8.8
static uint32_t s_waitUs = 0;
static uint32_t s_underruns = 0;

static const int16_t kVolume[11] = { 0, 16, 32, 52, 76, 104, 136, 172, 212, 256, 320 };

void hal_audio_volume(int v) {
  if (v < 0) v = 0;
  if (v > 10) v = 10;
  s_gain = kVolume[v];
}

static void fixSilence() {
  const uint32_t mid = (0x8000u * s_dac._pwmScale) >> 16;
  AudioBufferManager *arb = s_dac._arb;
  if (!arb || !arb->_silence) return;
  for (size_t i = 0; i < arb->_wordsPerBuffer; i++) arb->_silence->buff[i] = mid | (mid << 16);
}

void hal_audio_start(uint32_t rate) {
  s_step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_OUT_RATE);
  s_phase = 0; s_prev = 0;
  if (s_on) return;
  s_dac.setBuffers(AUDIO_NBUF, AUDIO_WORDS);
  if (!s_dac.begin((long)AUDIO_OUT_RATE)) { hal_log("audio: PWMAudio begin failed"); return; }
  fixSilence();
  s_on = true;
  hal_log("audio: PWM on GPIO%d/%d at %d Hz (from %u), wrap %u", AUDIO_L_PIN, AUDIO_R_PIN,
          AUDIO_OUT_RATE, (unsigned)rate, (unsigned)s_dac._pwmScale);
}

static void putFrame(int16_t s) {
  while (!s_dac.write(s, false)) vTaskDelay(1);         // L: latches, refused only mid-frame
  if (s_dac.write(s, false)) return;
  const uint32_t t0 = micros();
  while (!s_dac.write(s, false)) vTaskDelay(1);         // R: waits for a free buffer
  s_waitUs += micros() - t0;
}

void hal_audio_write(const int16_t *mono, int n) {
  if (!s_on) return;
  if (s_dac.getUnderflow()) s_underruns++;
  for (int i = 0; i < n; i++) {
    const int32_t x = mono[i];
    while (s_phase < (1u << 16)) {
      int32_t y = s_prev + (((x - s_prev) * (int32_t)(s_phase >> 2)) >> 14);
      y = (y * s_gain) >> 8;
      if (y > 32767) y = 32767;
      if (y < -32768) y = -32768;
      putFrame((int16_t)y);
      s_phase += s_step;
    }
    s_phase -= 1u << 16;
    s_prev = x;
  }
}

// The ring plays out on its own and loops silence once it is empty; nothing to drop here.
void hal_audio_silence() {}
uint32_t hal_audio_wait_us() { return s_waitUs; }
uint32_t hal_audio_underruns() { return s_underruns; }
// Only what will still play counts. The ring's free room leaves out two things that are not
// queued sound: the frames written into the buffer being filled (it goes to the DMA only once
// full) and the part of the playing buffer already sent (it returns to the ring only at its
// end). Counted in, they read up to two buffers (11.6 ms) high: pc_audio's 25 ms cushion was
// really 13 ms, and while guest RAM paged in, the DMA found nothing queued and played a buffer of
// silence (a click) with the queue reading 11 ms.
uint32_t hal_audio_queued_us() {
  AudioBufferManager *arb = s_dac._arb;
  if (!s_on || !arb) return 0;
  const int total = AUDIO_NBUF * AUDIO_WORDS, room = s_dac.availableForWrite() / 4;
  int q = total - room - (int)arb->_userOff;
  const uint32_t n = arb->_wordsPerBuffer * (arb->_dmaSize == DMA_SIZE_16 ? 2 : 1);
  for (int i = 0; i < 2; i++) {
    const int ch = arb->_channelDMA[i];
    if (!dma_channel_is_busy(ch)) continue;
    const uint32_t left = dma_channel_hw_addr(ch)->transfer_count & 0x0fffffffu;
    if (left <= n) q -= (int)((n - left) * arb->_wordsPerBuffer / n);
    break;
  }
  return q > 0 ? (uint32_t)((uint64_t)q * 1000000 / AUDIO_OUT_RATE) : 0;
}

#endif // BOARD_PICOCALC_RP2350
