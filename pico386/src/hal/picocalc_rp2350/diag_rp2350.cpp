// diag_rp2350.cpp -- crash capture and a heartbeat, for bringing the Pico 2 up.
//
// A HardFault, a FreeRTOS stack overflow or a failed pvPortMalloc would otherwise end in a reset
// with nothing in the log. Rebooting with a record does not work here: the SDK's watchdog_reboot
// writes scratch 4-7 itself, and PicoMultiLoader runs (and uses the RAM) before the app comes back.
// So these halt the chip and repeat the report on UART0 by polling, which needs no interrupts or
// FreeRTOS; the mainboard's CH340 shows it on the USB-C port.
// The heartbeat on core 1 shows whether the guest still runs and how much stack and heap are left.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_RP2350)

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <hardware/watchdog.h>
#include <hardware/uart.h>
#include <hardware/sync.h>
#include <hardware/structs/powman.h>
#include "../hal.h"
#include "../../pc/pc_glue.h"
#include "../../pc/pc_pagemem.h"

static void diagPuts(const char *s) {
  while (*s) uart_putc_raw(uart0, *s++);
}

static void diagHex(uint32_t v) {
  char b[11] = "0x";
  for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
  b[10] = 0;
  diagPuts(b);
}

static void diagHalt(const char *what, const char *name, uint32_t pc, uint32_t lr) {
  (void)save_and_disable_interrupts();
  watchdog_disable();
  for (;;) {
    diagPuts("\r\n*** diag: ");
    diagPuts(what);
    if (name) { diagPuts(" in task '"); diagPuts(name); diagPuts("'"); }
    if (pc || lr) { diagPuts(", pc "); diagHex(pc); diagPuts(", lr "); diagHex(lr); }
    diagPuts(", core ");
    diagHex(get_core_num());
    diagPuts(" (halted)\r\n");
    busy_wait_us_32(2000000);
  }
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) {
  diagHalt("stack overflow", name, 0, 0);
}

extern "C" void vApplicationMallocFailedHook(void) {
  diagHalt("failed FreeRTOS allocation", nullptr, 0, 0);
}

// The exception frame is on whichever stack was in use: PSP in a task, MSP in an interrupt.
// Its words 5 and 6 are LR and PC.
extern "C" void diagFault(uint32_t *frame) { diagHalt("HardFault", nullptr, frame[6], frame[5]); }

extern "C" __attribute__((naked)) void isr_hardfault(void) {
  __asm volatile(
    "tst lr, #4      \n"
    "ite eq          \n"
    "mrseq r0, msp   \n"
    "mrsne r0, psp   \n"
    "b diagFault     \n");
}

// POWMAN's record of the last chip-level reset: bit 16 POR, 17 BOR, 18 RUN pin, 19 debugger,
// 21 rescue, 22-24 and 28 watchdog, 25 SWCORE power-down, 26 glitch, 27 SYSRESETREQ/lockup.
// A plain watchdog reboot (PicoMultiLoader's) leaves it unchanged.
void rp2350DiagReport() {
  hal_log("diag: chip reset %08lx, watchdog reason %08lx", (unsigned long)powman_hw->chip_reset,
          (unsigned long)watchdog_hw->reason);
}

void pc_audioStats(uint32_t *mixUs, uint32_t *lowQueueUs, uint32_t *gapUs);   // pc_core.cpp
extern "C" uint32_t pico_spk_writes, pico_adlib_writes;                       // tiny386 pc.c

static void heartbeat(void *) {
  uint32_t lastUnder = 0, lastSpk = 0, lastAdl = 0;
  TaskHandle_t app = nullptr;
  uint32_t last = 0, lastFaults = 0, lastWb = 0, lastFired = 0, lastLost = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!app) app = xTaskGetHandle("app");
    const uint32_t ins = t386_instructions(), faults = pm_faults(), wb = pm_writebacks();
    uint32_t cs, eip;
    t386_where(&cs, &eip);
    hal_log("diag: guest +%lu instr at %04lx:%08lx, +%lu page faults, +%lu writebacks, app stack "
            "free %u B, heap free %d B",
            (unsigned long)(ins - last), (unsigned long)cs, (unsigned long)eip,
            (unsigned long)(faults - lastFaults), (unsigned long)(wb - lastWb),
            app ? (unsigned)(uxTaskGetStackHighWaterMark(app) * sizeof(StackType_t)) : 0u,
            rp2040.getFreeHeap());
    uint32_t count, behind, fired, lost;
    t386_timer_info(&count, &behind, &fired, &lost);
    hal_log("diag: timer %lu Hz, +%lu IRQ0, +%lu lost, %lu behind", count ? 1193182ul / count : 0ul,
            (unsigned long)(fired - lastFired), (unsigned long)(lost - lastLost), (unsigned long)behind);
    uint32_t mixUs, lowQ, gap;
    pc_audioStats(&mixUs, &lowQ, &gap);
    const uint32_t under = hal_audio_underruns(), spk = pico_spk_writes, adl = pico_adlib_writes;
    hal_log("diag: audio mix %lu ms, +%lu dry, queue low %lu ms, gap %lu ms, +%lu spk, +%lu adlib",
            (unsigned long)(mixUs / 1000), (unsigned long)(under - lastUnder), (unsigned long)(lowQ / 1000),
            (unsigned long)(gap / 1000), (unsigned long)(spk - lastSpk), (unsigned long)(adl - lastAdl));
    lastUnder = under; lastSpk = spk; lastAdl = adl;
    int mq;
    uint32_t mev;
    t386_mouse_info(&mq, &mev);
    static uint32_t lastMev;
    hal_log("diag: mouse %d bytes unread, +%lu events", mq, (unsigned long)(mev - lastMev));
    lastMev = mev;
    int kq, kmq;
    unsigned kb;
    uint8_t pr[6];
    t386_kbd_info(&kq, &kmq, &kb, pr);
    hal_log("diag: kbd q%d mq%d st%02x mode%02x pend%x hold%u, pic m %02x/%02x/%02x s %02x/%02x/%02x",
            kq, kmq, kb & 0xff, (kb >> 8) & 0xff, (kb >> 16) & 0xff, kb >> 24,
            pr[0], pr[1], pr[2], pr[3], pr[4], pr[5]);
    last = ins; lastFaults = faults; lastWb = wb; lastFired = fired; lastLost = lost;
  }
}

void rp2350DiagStart() {
  TaskHandle_t t;
  xTaskCreate(heartbeat, "diag", 1024, nullptr, configMAX_PRIORITIES / 2, &t);
  vTaskCoreAffinitySet(t, 1u << 1);
}

#endif // BOARD_PICOCALC_RP2350
