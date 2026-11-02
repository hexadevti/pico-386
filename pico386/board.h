// board.h -- the targets pico-386 builds for (shared with emu16).
//
//   BOARD_PICOCALC_P4      ClockworkPi PicoCalc mainboard + Waveshare ESP32-P4-WIFI6 module
//                          (arduino-cli, esp32:esp32:esp32p4, see .vscode/tasks.json)
//   BOARD_PICOCALC_RP2350  PicoCalc mainboard + a plain Pico 2 (RP2350, 520 KB SRAM, no PSRAM
//                          on the module) (arduino-cli, rp2040:rp2040:rpipico2)
//   BOARD_DESKTOP          SDL2 debug build for Windows/Linux (CMakeLists.txt)
//
// The pin maps are the ones emu8 brought up (emu8/board.h).
//
// PICO386_LOWMEM is the RP2350's memory model, and the desktop's with -DEMU_LOWMEM=ON: guest RAM
// lives in the mainboard's 8 MB PSRAM (behind a PIO, not memory-mapped) and is paged through a
// few dozen 4 KB frames of SRAM (pc_pagemem.c). VRAM, the page frames and the menu canvas share
// one SRAM arena (hal_arena), there is no 800x600 framebuffer, and video is drawn in bands.
#pragma once

#if !defined(BOARD_PICOCALC_P4) && !defined(BOARD_DESKTOP) && !defined(BOARD_PICOCALC_RP2350)
#define BOARD_PICOCALC_P4   // the P4 build passes no board flag to its C files
#endif

#if defined(BOARD_PICOCALC_RP2350) && !defined(PICO386_LOWMEM)
#define PICO386_LOWMEM 1
#endif

// --- the panel: 320x320. Unlike emu16/pico-amiga, the logical game screen is the whole panel:
// PC text mode needs 25 rows of 12 pixels (300 lines). Graphics modes draw a 320x240 window
// centred in it and submit only those rows.
#define PANEL_W           320
#define PANEL_H           320
#define SCREEN_W          320                     // logical screen
#define SCREEN_H          320
#define SCREEN_Y0         0                       // panel row of logical row 0

#if defined(BOARD_PICOCALC_P4)

#define BOARD_NAME        "PicoCalc (ESP32-P4)"

// ST7365P/ILI9488 panel on SPI2. GPIO24/25 (DC/RST) belong to the USB-Serial-JTAG PHY at reset.
#define LCD_SCK_PIN       3
#define LCD_MOSI_PIN      2
#define LCD_MISO_PIN      8
#define LCD_CS_PIN        7
#define LCD_DC_PIN        24
#define LCD_RST_PIN       25
#define LCD_SPI_HZ        80000000

// STM32 keyboard controller (also owns both backlights and the battery gauge).
#define KBD_I2C_SDA_PIN   50
#define KBD_I2C_SCL_PIN   49
#define KBD_I2C_ADDR      0x1F
#define KBD_I2C_HZ        10000
#define PICOCALC_MAINBOARD_WAIT_MS 30000

// microSD over SPI. The pins sit in an IO domain fed by internal LDO channel 4, off at boot.
#define SD_SCK_PIN        46
#define SD_MISO_PIN       48
#define SD_MOSI_PIN       33
#define SD_CS_PIN         47
#define SD_IO_LDO_CHAN    4
#define SD_SPI_HZ         20000000
#define SD_MOUNT          "/sd"

// Speakers: I2S0 PDM TX in DAC mode, left on dout, right on dout2.
#define AUDIO_L_PIN       23
#define AUDIO_R_PIN       22
#define AUDIO_OUT_RATE    44100

#elif defined(BOARD_PICOCALC_RP2350)

#define BOARD_NAME        "PicoCalc (RP2350)"

// ST7365P/ILI9488 panel on spi1, driven with the pico-sdk and one DMA channel.
#define LCD_SCK_PIN       10
#define LCD_MOSI_PIN      11
#define LCD_MISO_PIN      12
#define LCD_CS_PIN        13
#define LCD_DC_PIN        14
#define LCD_RST_PIN       15
#define LCD_SPI_HZ        62500000

// STM32 keyboard controller on i2c1 (Wire1).
#define KBD_I2C_SDA_PIN   6
#define KBD_I2C_SCL_PIN   7
#define KBD_I2C_ADDR      0x1F
#define KBD_I2C_HZ        10000
#define PICOCALC_MAINBOARD_WAIT_MS 30000

// microSD on spi0 (arduino-pico's SDFS).
#define SD_SCK_PIN        18
#define SD_MISO_PIN       16
#define SD_MOSI_PIN       19
#define SD_CS_PIN         17
#define SD_SPI_HZ         25000000
#define SD_MOUNT          "/sd"

// Speakers: PWM into the mainboard's RC filter (left 26, right 27).
#define AUDIO_L_PIN       26
#define AUDIO_R_PIN       27
#define AUDIO_OUT_RATE    22050

// The mainboard's 8 MB PSRAM (APS6404L class), plain SPI through PIO1: CS 20, SCK 21,
// SIO0 (MOSI) 2, SIO1 (MISO) 3.
#define PSRAM_CS_PIN      20
#define PSRAM_SCK_PIN     21
#define PSRAM_MOSI_PIN    2
#define PSRAM_MISO_PIN    3
#define PSRAM_BYTES       (8u << 20)

// The SRAM arena: VRAM (PM_VRAM_BYTES, 160 KB), then the guest-RAM page frames. The menu canvas
// (200 KB) reuses its start. 64 KB of frames (64 pages of 1 KB) leave about 9 KB of heap with a
// PC running; tiny386 alone takes 43 KB of it, and the interpreter in SRAM (i386.c) about 96 KB.
#define LOWMEM_ARENA_BYTES (160u * 1024 + 64u * 1024)

#elif defined(BOARD_DESKTOP)

#define BOARD_NAME        "Desktop (SDL2)"
#define AUDIO_OUT_RATE    44100
#ifndef EMU_DEFAULT_SD_DIR
#define EMU_DEFAULT_SD_DIR "sdcard"
#endif
#if defined(PICO386_LOWMEM)                // the RP2350's memory, simulated
#define PSRAM_BYTES       (8u << 20)
#define LOWMEM_ARENA_BYTES (160u * 1024 + 64u * 1024)
#endif

#endif
