// system_rp2350.cpp -- boot order, timing, memory, SD card and the job worker on a Pico 2.
//
// Cores: arduino-pico's FreeRTOS SMP runs setup()/loop() on core 0, so the app (and with it the
// emulated CPU) lives there (pico386.ino), and the draw job and keyboard poll go to core 1.
// Memory: no PSRAM on the module. Guest RAM is in the mainboard's PIO PSRAM (psram_rp2350.cpp),
// and VRAM plus the page frames sit in s_arena; everything else is the SRAM heap.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#include <SPI.h>
#include <SDFS.h>
#include <VFS.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <malloc.h>
#include <stdarg.h>
#include <hardware/watchdog.h>
#include <hardware/clocks.h>
#include <hardware/vreg.h>
#include <hardware/structs/systick.h>
#include "../hal.h"

bool picocalcWaitForMainboard();   // input_rp2350.cpp
void rp2350InputInit();            // input_rp2350.cpp
void rp2350DisplayInit();          // display_rp2350.cpp
void rp2350PsramInit();            // psram_rp2350.cpp
void rp2350DiagReport();           // diag_rp2350.cpp
void rp2350DiagStart();            // diag_rp2350.cpp

#define LOADER_SERIAL_CMD "@uf2menu\n"  // same command the emu8 deploy tools send
#define UF2LOADER_MAGIC   0xe98cc638u   // PICOCALC_BL_MAGIC, uf2loader common/bootloader/proginfo.h
#define UF2LOADER_BOOT_SD 1u            // enum bootmode_e { BOOT_DEFAULT, BOOT_SD, ... }

static bool s_sdOk = false;
alignas(8) static uint8_t s_arena[LOWMEM_ARENA_BYTES];

uint32_t hal_millis()          { return millis(); }
uint32_t hal_micros()          { return micros(); }
void     hal_delay(uint32_t ms){ delay(ms); }
void     hal_yield()           { taskYIELD(); }

// The serial remote's screenshot rows are written in pieces, so it holds this across a row and no
// log line from another core lands inside one.
static SemaphoreHandle_t s_logLock = nullptr;
static StaticSemaphore_t s_logLockBuf;

void rp2350LogLock(bool on) {
  if (!s_logLock || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return;
  if (on) xSemaphoreTake(s_logLock, portMAX_DELAY);
  else xSemaphoreGive(s_logLock);
}

void hal_log(const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  rp2350LogLock(true);
  Serial.println(b);
  Serial1.println(b);
  rp2350LogLock(false);
}

void hal_reboot() { delay(50); rp2040.reboot(); }

// Back to PicoMultiLoader's (or pelrun's UF2 Loader's) menu: both read "boot to the SD menu" from
// the watchdog scratch registers, which survive a watchdog reboot (PicoMultiLoader docs/apps.md,
// "Getting back to the menu"; emu8's loader_picocalc.cpp). Without a loader nothing reads them
// and this is a plain reboot.
void hal_reboot_to_loader() {
  hal_log("rebooting to the loader menu");
  delay(50);
  watchdog_hw->scratch[1] = UF2LOADER_BOOT_SD;
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[0] = UF2LOADER_MAGIC;
  rp2040.reboot();                      // watchdog_reboot(0, 0, ..): scratch 0-3 are kept
}

// Commands over either serial port, one per line, polled by the keyboard task:
//   @uf2menu         back to the loader (the deploy tools send it, as on the P4)
//   @keys <script>   type a PC_KEYS script (keyscript.h); "@keys done" when it has run
//   @mpace <i> <s>   keyboard mouse: guest instructions between packets, largest step (tuning)
//   @shot            the screen, as "@S" rows (rp2350Shot); "@shotraw <y>" dumps a row's raw bytes
// tools/remote.py drives them. The answers go back to the port the command came from.
void rp2350RemoteKeys(const char *script, Print *port);   // input_rp2350.cpp
void rp2350Shot(Print &out, int rawRow);                  // display_rp2350.cpp
void rp2350ShotTest(Print &out, int khz);                 // display_rp2350.cpp

void pckbd_mouse_pace(uint32_t instr, int step);   // pc_keymap.cpp

static void serialCommand(char *line, Print *port) {
  if (!strcmp(line, "@uf2menu")) hal_reboot_to_loader();
  else if (!strncmp(line, "@keys ", 6)) rp2350RemoteKeys(line + 6, port);
  else if (!strcmp(line, "@shot")) rp2350Shot(*port, -1);
  else if (!strncmp(line, "@shotraw ", 9)) rp2350Shot(*port, atoi(line + 9));
  else if (!strncmp(line, "@shottest", 9)) rp2350ShotTest(*port, atoi(line + 9));
  else if (!strncmp(line, "@mpace ", 7)) {
    unsigned long instr = 0;
    int step = 0;
    sscanf(line + 7, "%lu %d", &instr, &step);
    pckbd_mouse_pace((uint32_t)instr, step);
    port->printf("@mpace %lu %d\n", instr, step);
  }
}

void rp2350CheckSerialCmd() {
  static char line[160];
  static uint8_t len = 0;
  static bool tooLong = false;            // a longer line is dropped whole, not cut short
  static Stream *from = nullptr;
  for (;;) {
    Stream *p = Serial.available() ? (Stream *)&Serial : Serial1.available() ? (Stream *)&Serial1 : nullptr;
    if (!p) break;
    const int c = p->read();
    if (c < 0) break;
    if (p != from) { from = p; len = 0; tooLong = false; }
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < sizeof(line) - 1) line[len++] = (char)c;
      else tooLong = true;
      continue;
    }
    line[len] = 0;
    len = 0;
    if (tooLong) { tooLong = false; p->println("@error line too long"); continue; }
    if (line[0] == '@') serialCommand(line, p);
  }
}

// One heap: the SRAM.
void *hal_alloc_big(size_t n)  { return calloc(1, n); }
void *hal_alloc_fast(size_t n) { return calloc(1, n); }
void  hal_free(void *p)        { free(p); }

uint8_t *hal_arena(size_t *size) {
  *size = sizeof(s_arena);
  return s_arena;
}

// ---- the job worker on core 1 ----------------------------------------------------------------
// Static, and made at boot: FreeRTOS allocates from the malloc heap here (heap_3a), and creating
// the task on the first frame, after tiny386 had taken its share, ran it out (2026-10-28).
// 4 KB of stack: the LOWMEM draw job's buffers are static (pc_video.cpp).
#define JOB_STACK_WORDS (4096 / sizeof(StackType_t))
static StackType_t s_jobStack[JOB_STACK_WORDS];
static StaticTask_t s_jobTcb;
static StaticSemaphore_t s_jobGoBuf, s_jobDoneBuf;
static SemaphoreHandle_t s_jobGo = nullptr, s_jobDone = nullptr;
static void (*volatile s_job)(void *) = nullptr;
static void *volatile s_jobArg = nullptr;
static volatile bool s_jobBusy = false;

static void jobTask(void *) {
  for (;;) {
    xSemaphoreTake(s_jobGo, portMAX_DELAY);
    s_job(s_jobArg);
    s_jobBusy = false;
    xSemaphoreGive(s_jobDone);
  }
}

static void jobInit() {
  s_jobGo = xSemaphoreCreateBinaryStatic(&s_jobGoBuf);
  s_jobDone = xSemaphoreCreateBinaryStatic(&s_jobDoneBuf);
  TaskHandle_t t = xTaskCreateStatic(jobTask, "job", JOB_STACK_WORDS, nullptr,
                                     configMAX_PRIORITIES / 2, s_jobStack, &s_jobTcb);
  vTaskCoreAffinitySet(t, 1u << 1);
}

bool hal_job_start(void (*job)(void *), void *arg) {
  if (s_jobBusy) return false;
  xSemaphoreTake(s_jobDone, 0);                  // drop the last job's completion
  s_job = job;
  s_jobArg = arg;
  s_jobBusy = true;
  xSemaphoreGive(s_jobGo);
  return true;
}

void hal_job_wait() {
  while (s_jobBusy) xSemaphoreTake(s_jobDone, pdMS_TO_TICKS(10));
}

bool        hal_sd_mounted() { return s_sdOk; }
const char *hal_sd_root()    { return SD_MOUNT; }

// emu8's ladder: some cards only start at a lower SPI clock.
static void sdInit() {
  SPI.setSCK(SD_SCK_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setRX(SD_MISO_PIN);
  static const uint32_t kRates[] = { SD_SPI_HZ, 12000000, 4000000, 1000000 };
  for (uint32_t hz : kRates) {
    SDFS.setConfig(SDFSConfig(SD_CS_PIN, hz, SPI));
    if (SDFS.begin()) {
      s_sdOk = true;
      VFS.map(SD_MOUNT, SDFS);
      hal_log("SD: mounted at %s, SPI %lu kHz", SD_MOUNT, (unsigned long)(hz / 1000));
      return;
    }
    SDFS.end();
    delay(50);
  }
  hal_log("SD: mount failed");
}

// ---- rp2350.ini: clock, core voltage and PSRAM speed, for boards that need them ----------------
// Every Pico 2 runs at 300 MHz (the build's freq=300), and arduino-pico leaves the RP2350's core
// at its power-on 1.10 V (it raises it only on the RP2040). Ours copes; a user's AliExpress RP2350
// froze within seconds of boot (2026-11-10). So an optional /pico-386/rp2350.ini can set
//   mhz       = 250    clk_sys, 100-300 (default 300)
//   vreg      = 1.20   the core's volts, 1.10-1.30 (default: left at 1.10)
//   psram_div = 3      the fastest PSRAM clock divider tried, 2-10 (default 2: SCK = clk_sys / 4)
// Without the file nothing changes. It is read before anything but the serial ports depends on the
// clock (the panel, the SD's SPI, the PSRAM's PIO, the audio PWM are set up after it).
uint8_t g_psramMinDiv = 2;                 // psram_rp2350.cpp

void rp2350KbdReclock();                   // input_rp2350.cpp

// FreeRTOS loaded each core's SysTick from F_CPU (configCPU_CLOCK_HZ); it counts clk_sys.
static void tickReload() { systick_hw->rvr = clock_get_hz(clk_sys) / configTICK_RATE_HZ - 1; }
static void tickCore1(void *) {
  tickReload();
  vTaskDelete(nullptr);
}

struct Tune { int mhz = 0, mv = 0, psramDiv = 0; };

// "key = value" lines; [sections], ';' and '#' comments are ignored.
static Tune readTune() {
  Tune t;
  SPI.setSCK(SD_SCK_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setRX(SD_MISO_PIN);
  SDFS.setConfig(SDFSConfig(SD_CS_PIN, 4000000, SPI));     // slow: any card, one small file
  if (!SDFS.begin()) return t;
  File f = SDFS.open("/pico-386/rp2350.ini", "r");
  if (f) {
    char line[80];
    while (f.available()) {
      const size_t n = f.readBytesUntil('\n', line, sizeof line - 1);
      line[n] = 0;
      char *p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == ';' || *p == '#' || *p == '[') continue;
      char *eq = strchr(p, '=');
      if (!eq) continue;
      char *k = p, *ke = eq;
      while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
      *ke = 0;
      const char *v = eq + 1;
      if (!strcmp(k, "mhz")) t.mhz = atoi(v);
      else if (!strcmp(k, "vreg")) t.mv = (int)(atof(v) * 1000.0 + 0.5);
      else if (!strcmp(k, "psram_div")) t.psramDiv = atoi(v);
    }
    f.close();
  }
  SDFS.end();
  return t;
}

static char s_tuneLog[120] = "";

static void applyTune() {
  const Tune t = readTune();
  char *o = s_tuneLog;
  const char *end = s_tuneLog + sizeof s_tuneLog;
  if (t.psramDiv >= 2 && t.psramDiv <= 10) {
    g_psramMinDiv = (uint8_t)t.psramDiv;
    o += snprintf(o, end - o, " psram_div %d", t.psramDiv);
  }
  // 1.10-1.30 V in 50 mV steps (VREG_VOLTAGE_1_10 .. _1_30 are consecutive); above needs unlocking.
  int vsel = -1;
  if (t.mv >= 1100 && t.mv <= 1300) vsel = VREG_VOLTAGE_1_10 + (t.mv - 1100 + 25) / 50;
  else if (t.mv) o += snprintf(o, end - o, " vreg %d mV ignored (1.10-1.30)", t.mv);
  const uint32_t khz = (t.mhz >= 100 && t.mhz <= 300) ? (uint32_t)t.mhz * 1000 : 0;
  if (t.mhz && !khz) o += snprintf(o, end - o, " mhz %d ignored (100-300)", t.mhz);
  if (vsel < 0 && (!khz || khz * 1000 == clock_get_hz(clk_sys))) return;

  // Raise the voltage before a faster clock; lower clocks are safe at any voltage, so it goes after.
  rp2350LogLock(true);
  Serial1.flush();
  if (vsel >= 0 && vsel > (int)vreg_get_voltage()) { vreg_set_voltage((vreg_voltage)vsel); busy_wait_us_32(1000); }
  if (khz && khz * 1000 != clock_get_hz(clk_sys) && !set_sys_clock_khz(khz, false)) o += snprintf(o, end - o, " %lu MHz not reachable", (unsigned long)(khz / 1000));
  if (vsel >= 0 && vsel < (int)vreg_get_voltage()) vreg_set_voltage((vreg_voltage)vsel);
  tickReload();
  Serial1.end();
  Serial1.begin(115200);
  rp2350LogLock(false);
  rp2350KbdReclock();
  TaskHandle_t h = nullptr;
  if (xTaskCreate(tickCore1, "tick1", 256, nullptr, configMAX_PRIORITIES - 1, &h) == pdPASS)
    vTaskCoreAffinitySet(h, 1u << 1);
  if (vsel >= 0) o += snprintf(o, end - o, " vreg %d mV", 1100 + (vsel - VREG_VOLTAGE_1_10) * 50);
}

void hal_init() {
  s_logLock = xSemaphoreCreateMutexStatic(&s_logLockBuf);
  Serial.begin(115200);
  // The log also goes to UART0 (GP0/GP1), which the mainboard's CH340 puts on its USB-C port: that
  // is the serial port a PicoCalc user sees, since the Pico's own USB is inside the case.
  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial1.begin(115200);
  delay(50);
  rp2350DiagReport();
  rp2350DiagStart();
  jobInit();
  const bool board = picocalcWaitForMainboard();
  if (board) applyTune();
  if (s_tuneLog[0]) hal_log("rp2350.ini:%s", s_tuneLog);
  rp2350PsramInit();
  rp2350DisplayInit();
  if (board) sdInit();
  rp2350InputInit();
  const struct mallinfo mi = mallinfo();
  hal_log("pico-386 on " BOARD_NAME " at %lu MHz, core %u mV, heap %u KB used, arena %u KB",
          (unsigned long)(clock_get_hz(clk_sys) / 1000000),
          1100u + ((unsigned)vreg_get_voltage() - VREG_VOLTAGE_1_10) * 50u,
          (unsigned)(mi.uordblks >> 10), (unsigned)(sizeof(s_arena) >> 10));
}

#endif // BOARD_PICOCALC_RP2350
