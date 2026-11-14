// prof_rp2350.cpp -- a sampling profiler for the RP2350, driven over the log port (system_rp2350.cpp):
//   @prof <hz> <n>   sample the code address on core 0 (the app task: the PC) <n> times at <hz>
//   @profdump        print the samples as "@P" lines of hex, then "@prof end"
// A hardware alarm whose interrupt is enabled on core 0 reads the PC that the exception entry
// stacked on the interrupted task's stack (PSP + 24). A sample taken while core 0 was already in
// another interrupt (ICSR.RETTOBASE clear) is recorded as 1, since PSP then points at the task's
// frame from before that interrupt. tools/remote.py prof runs several rounds (the heap has room
// for only a few KB of samples at a time) and turns them into functions and lines with addr2line.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <hardware/timer.h>
#include "../hal.h"

static uint32_t *s_samples = nullptr;
static volatile int s_count = 0;
static int s_want = 0;
static uint32_t s_period = 1000;        // us
static int s_alarm = -1;
static volatile bool s_running = false;

static void __not_in_flash_func(onAlarm)(uint alarm) {
  uint32_t pc = 1;
  const uint32_t icsr = *(volatile uint32_t *)0xE000ED04u;
  if (icsr & (1u << 11)) {              // RETTOBASE: we interrupted thread mode, a task on PSP
    uint32_t psp;
    asm volatile("mrs %0, psp" : "=r"(psp));
    pc = ((const uint32_t *)psp)[6];
  }
  const int i = s_count;
  if (i < s_want) { s_samples[i] = pc; s_count = i + 1; }
  if (s_count < s_want && s_running) {
    // a target already passed returns true; push it on until one lands in the future
    uint32_t d = s_period;
    while (hardware_alarm_set_target(alarm, make_timeout_time_us(d))) d += s_period;
  } else {
    s_running = false;
  }
}

// The alarm's interrupt goes to the core that sets the callback, so this runs as a task on core 0.
static void startTask(void *) {
  if (s_alarm < 0) s_alarm = hardware_alarm_claim_unused(false);
  if (s_alarm >= 0) {
    hardware_alarm_set_callback((uint)s_alarm, onAlarm);
    s_running = true;
    hardware_alarm_set_target((uint)s_alarm, make_timeout_time_us(s_period));
    hal_log("@prof started %d at %u Hz", s_want, (unsigned)(1000000 / s_period));
  } else {
    hal_log("@prof no alarm");
  }
  vTaskDelete(nullptr);
}

void rp2350ProfStart(const char *args) {
  int hz = 1000, n = 1024;
  sscanf(args, "%d %d", &hz, &n);
  if (hz < 10) hz = 10;
  if (hz > 20000) hz = 20000;
  if (n < 1) n = 1;
  if (n > 2048) n = 2048;
  s_running = false;
  if (s_alarm >= 0) hardware_alarm_cancel((uint)s_alarm);
  free(s_samples);
  s_samples = (uint32_t *)malloc(n * 4);
  if (!s_samples) { hal_log("@prof no memory"); return; }
  s_count = 0;
  s_want = n;
  s_period = 1000000 / hz;
  // Created already pinned: set afterwards, it could start on core 1 first and take the alarm's
  // interrupt there (a profile of the wrong core, 2026-11-13).
  TaskHandle_t t = nullptr;
  if (xTaskCreateAffinitySet(startTask, "prof", 384, nullptr, configMAX_PRIORITIES - 1, 1u << 0, &t) != pdPASS) {
    hal_log("@prof no task");
    return;
  }
}

void rp2350ProfDump() {
  s_running = false;
  if (s_alarm >= 0) hardware_alarm_cancel((uint)s_alarm);
  const int n = s_count;
  char line[16 * 9 + 8];
  for (int i = 0; i < n; i += 16) {
    int len = snprintf(line, sizeof line, "@P");
    for (int k = i; k < n && k < i + 16; k++) len += snprintf(line + len, sizeof line - len, " %lx", (unsigned long)s_samples[k]);
    hal_log("%s", line);
  }
  hal_log("@prof end %d", n);
  free(s_samples);
  s_samples = nullptr;
  s_want = 0;
}

#endif // BOARD_PICOCALC_RP2350
