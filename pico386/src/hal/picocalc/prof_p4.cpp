// prof_p4.cpp -- a sampling profiler for the P4, driven over the console UART (system_p4.cpp):
//   @prof <hz> <n>   sample the code address on core 1 (the PC) <n> times at <hz>
//   @profdump        print the samples as "@P" lines of hex, then "@prof end"
// A GP timer interrupt allocated on core 1 reads mepc, where the interrupted code was.
// tools/remote.py prof turns the samples into functions and lines with addr2line.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_P4)

#include <Arduino.h>
#include "driver/gptimer.h"
#include "esp_heap_caps.h"
#include "../hal.h"

static uint32_t *s_samples = nullptr;
static volatile int s_count = 0;
static int s_want = 0;
static gptimer_handle_t s_timer = nullptr;
static uint32_t s_hz = 1000;

static bool IRAM_ATTR onAlarm(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *) {
  uint32_t pc;
  asm volatile("csrr %0, mepc" : "=r"(pc));
  const int i = s_count;
  if (i < s_want) { s_samples[i] = pc; s_count = i + 1; }
  return false;
}

// The interrupt goes to the core that registers the callback, so this runs as a task on core 1.
static void startTask(void *) {
  if (!s_timer) {
    gptimer_config_t cfg = {};
    cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    cfg.direction = GPTIMER_COUNT_UP;
    cfg.resolution_hz = 1000000;
    if (gptimer_new_timer(&cfg, &s_timer) == ESP_OK) {
      gptimer_event_callbacks_t cbs = {};
      cbs.on_alarm = onAlarm;
      gptimer_register_event_callbacks(s_timer, &cbs, nullptr);
      gptimer_enable(s_timer);
    } else {
      s_timer = nullptr;
    }
  }
  if (s_timer) {
    gptimer_alarm_config_t al = {};
    al.alarm_count = 1000000 / s_hz;
    al.flags.auto_reload_on_alarm = true;
    gptimer_set_alarm_action(s_timer, &al);
    gptimer_set_raw_count(s_timer, 0);
    gptimer_start(s_timer);
  }
  hal_log("@prof started %d at %u Hz", s_want, (unsigned)s_hz);
  vTaskDelete(nullptr);
}

void p4ProfStart(const char *args) {
  int hz = 1000, n = 8192;
  sscanf(args, "%d %d", &hz, &n);
  if (hz < 10) hz = 10;
  if (n < 1) n = 1;
  if (n > 16384) n = 16384;
  if (s_timer) gptimer_stop(s_timer);
  free(s_samples);
  s_samples = (uint32_t *)heap_caps_malloc(n * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_samples) s_samples = (uint32_t *)heap_caps_malloc(n * 4, MALLOC_CAP_SPIRAM);
  if (!s_samples) { hal_log("@prof no memory"); return; }
  s_count = 0;
  s_want = n;
  s_hz = hz;
  xTaskCreatePinnedToCore(startTask, "prof", 4096, nullptr, 10, nullptr, 1);
}

void p4ProfDump() {
  if (s_timer) gptimer_stop(s_timer);
  const int n = s_count;
  char line[16 * 9 + 8];
  for (int i = 0; i < n; i += 16) {
    int len = snprintf(line, sizeof line, "@P");
    for (int k = i; k < n && k < i + 16; k++) len += snprintf(line + len, sizeof line - len, " %lx", (unsigned long)s_samples[k]);
    hal_log("%s", line);
  }
  hal_log("@prof end %d", n);
}

#endif // BOARD_PICOCALC_P4
