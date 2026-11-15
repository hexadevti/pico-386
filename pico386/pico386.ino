// pico386 -- a 386/486 PC with VGA (the tiny386 emulator) for the ClockworkPi PicoCalc with a
// Waveshare ESP32-P4-WIFI6 module or a Pico 2 (and an SDL2 desktop build of the same code).
// Everything lives in src/: hal/ (the machine), frontend/ (menus, game loop) and pc/ (the PC).
#include "board.h"
#include "src/frontend/app.h"

#if defined(BOARD_PICOCALC_P4)
#include <Arduino.h>
SET_LOOP_TASK_STACK_SIZE(32 * 1024);   // the cores keep line buffers on the stack

void setup() { app_setup(); }
void loop()  { app_loop(); }

#elif defined(BOARD_PICOCALC_RP2350)
#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>

// arduino-pico's loop task has a fixed 4 KB stack, so the app gets its own task on core 0.
#define APP_STACK_WORDS (16 * 1024 / sizeof(StackType_t))
static StackType_t s_appStack[APP_STACK_WORDS];
static StaticTask_t s_appTcb;

static void appTask(void *) {
  app_setup();
  for (;;) app_loop();
}

void setup() {
  TaskHandle_t t = xTaskCreateStatic(appTask, "app", APP_STACK_WORDS, nullptr,
                                     configMAX_PRIORITIES / 2, s_appStack, &s_appTcb);
  vTaskCoreAffinitySet(t, 1u << 0);
}
void loop() { delay(1000); }

#else
void setup() { app_setup(); }
void loop()  { app_loop(); }
#endif
