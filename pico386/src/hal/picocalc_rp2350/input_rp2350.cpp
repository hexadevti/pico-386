// input_rp2350.cpp -- the PicoCalc keyboard on a Pico 2: the STM32 at I2C 0x1F on i2c1 (Wire1),
// which also owns both backlights and reads the battery gauge. A port of input_p4.cpp; the
// protocol notes are there. The poll task runs on core 1, next to the draw job, so the emulation
// core never waits on the 10 kHz bus.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#include <Wire.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "../hal.h"
#include "../keyscript.h"

#define KBD_REG_FIF   0x09
#define KBD_REG_BAT   0x0B
#define KBD_REG_BKL   0x05        // keyboard backlight
#define KBD_REG_BK2   0x0A        // LCD backlight
#define KBD_REG_WRITE 0x80
#define BAT_POLL_MS   10000

void rp2350CheckSerialCmd();      // system_rp2350.cpp
void rp2350Shot(Print &out, int rawRow);   // display_rp2350.cpp

static QueueHandle_t s_keys = nullptr;
static bool s_wireUp = false;
static bool s_selPending = false, s_batPending = false;
static volatile int  s_batPercent = -1;
static volatile bool s_batCharging = false;

static void wireBegin() {
  if (s_wireUp) return;
  Wire1.setSDA(KBD_I2C_SDA_PIN);
  Wire1.setSCL(KBD_I2C_SCL_PIN);
  Wire1.begin();
  Wire1.setClock(KBD_I2C_HZ);
  s_wireUp = true;
}

static void selectReg(uint8_t reg) {
  Wire1.beginTransmission(KBD_I2C_ADDR);
  Wire1.write(reg);
  const bool ok = Wire1.endTransmission() == 0;
  s_selPending = ok && reg == KBD_REG_FIF;
  s_batPending = ok && reg == KBD_REG_BAT;
}

static void readBat() {
  s_batPending = false;
  if (Wire1.requestFrom((uint8_t)KBD_I2C_ADDR, (size_t)2) != 2) return;
  const uint8_t r = (uint8_t)Wire1.read(), v = (uint8_t)Wire1.read();
  if (r != KBD_REG_BAT || (v & 0x7F) > 100) return;
  s_batPercent = v & 0x7F;
  s_batCharging = (v & 0x80) != 0;
}

// A lost event (failed read, full queue) becomes KEY_RELEASE_ALL, as on the P4.
static bool s_lost = false;

static void flushLost() {
  if (!s_lost) return;
  const KeyEvent all = { KEY_RELEASE_ALL, KEYSTATE_RELEASED };
  if (xQueueSend(s_keys, &all, 0) == pdTRUE) s_lost = false;
}

static void sendKey(uint8_t code, uint8_t state) {
  flushLost();
  if (s_lost) return;
  const KeyEvent ev = { code, state };
  if (xQueueSend(s_keys, &ev, 0) != pdTRUE) s_lost = true;
}

// ---- the serial remote's key scripts ("@keys", system_rp2350.cpp) -----------------------------
// The script is copied to the heap only while it runs: SRAM is short with a PC running.
static KeyScript s_script;
static char *s_scriptText = nullptr;
static Print *s_scriptPort = nullptr;

static void scriptEnd(const char *why) {
  if (s_script.held) sendKey(s_script.held, KEYSTATE_RELEASED);
  while (s_script.nmods) sendKey(s_script.mods[--s_script.nmods], KEYSTATE_RELEASED);
  free(s_scriptText);
  s_scriptText = nullptr;
  s_script.start(nullptr);
  s_scriptPort->println(why);
}

void rp2350RemoteKeys(const char *script, Print *port) {
  if (s_scriptText) scriptEnd("@keys cancelled");
  s_scriptPort = port;
  s_scriptText = strdup(script);
  if (!s_scriptText) { port->println("@keys no memory"); return; }
  s_script.start(s_scriptText);
}

static void scriptStep() {
  if (!s_scriptText) return;
  char t[16];
  if (s_script.step(millis(), uxQueueMessagesWaiting(s_keys) == 0, sendKey, t, sizeof(t))) {
    if (!strcmp(t, "shot")) rp2350Shot(*s_scriptPort, -1);
  }
  if (!s_script.busy(millis())) scriptEnd("@keys done");
}

static bool readFifo(uint8_t *state, uint8_t *code) {
  if (!s_selPending) return false;
  s_selPending = false;
  if (Wire1.requestFrom((uint8_t)KBD_I2C_ADDR, (size_t)2) != 2) { s_lost = true; return false; }
  const uint8_t s = (uint8_t)Wire1.read(), c = (uint8_t)Wire1.read();
  if (s == 0 && c == 0) return false;
  *state = s; *code = c;
  return true;
}

static bool mainboardPresent() {
  wireBegin();
  Wire1.beginTransmission(KBD_I2C_ADDR);
  return Wire1.endTransmission() == 0;
}

// The Pico is powered by the mainboard, but the STM32 may still be booting: give it a while.
bool picocalcWaitForMainboard() {
  if (mainboardPresent()) return true;
  hal_log("Waiting for the PicoCalc keyboard controller");
  const uint32_t t0 = millis();
  while (millis() - t0 < PICOCALC_MAINBOARD_WAIT_MS) {
    if (mainboardPresent()) { delay(300); return true; }
    delay(100);
  }
  hal_log("keyboard controller never answered");
  return false;
}

static bool bklWrite(uint8_t reg, uint8_t level) {
  Wire1.beginTransmission(KBD_I2C_ADDR);
  Wire1.write((uint8_t)(reg | KBD_REG_WRITE));
  Wire1.write(level);
  return Wire1.endTransmission() == 0;
}

void picocalcBacklightOn() {
  wireBegin();
  const bool a = bklWrite(KBD_REG_BK2, 0xFF);
  const bool b = bklWrite(KBD_REG_BKL, 0xFF);
  if (!a || !b) hal_log("backlight write failed");
}

static void kbdTask(void *) {
  uint32_t lastBat = 0;
  bool batOnce = false;
  selectReg(KBD_REG_FIF);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    rp2350CheckSerialCmd();
    scriptStep();
    if (s_batPending) { readBat(); selectReg(KBD_REG_FIF); continue; }
    uint8_t state, code;
    int got = 0;
    while (got < 4 && readFifo(&state, &code)) {
      sendKey(code, state);
      got++;
      selectReg(KBD_REG_FIF);
      delayMicroseconds(2000);           // STM32 turnaround after a select
    }
    flushLost();
    if (got) { selectReg(KBD_REG_FIF); continue; }
    const uint32_t now = millis();
    if (!batOnce || now - lastBat >= BAT_POLL_MS) {
      batOnce = true; lastBat = now;
      selectReg(KBD_REG_BAT);
    } else {
      selectReg(KBD_REG_FIF);
    }
  }
}

void rp2350InputInit() {
  wireBegin();
  s_keys = xQueueCreate(64, sizeof(KeyEvent));
  TaskHandle_t t = nullptr;
  xTaskCreate(kbdTask, "kbd", 1024, nullptr, configMAX_PRIORITIES / 2 + 2, &t);
  vTaskCoreAffinitySet(t, 1u << 1);
}

bool hal_key_poll(KeyEvent *ev) { return s_keys && xQueueReceive(s_keys, ev, 0) == pdTRUE; }
int  hal_battery_percent()      { return s_batPercent; }
bool hal_battery_charging()     { return s_batCharging; }

#endif // BOARD_PICOCALC_RP2350
