# Third-party code in pico-386

pico-386 is distributed under the GNU General Public License, version 2 or (at your option) any
later version (see `LICENSE`). Everything vendored here is GPL-compatible.

| Component | Path | Origin | License / terms |
|---|---|---|---|
| UI bitmap fonts | `pico386/src/frontend/fonts/uifont.h` | X11 misc-fixed 6x10 / 7x13 (via FabGL, via emu8) | Public domain |
| Frontend, HAL, P4 display/keyboard/audio bring-up | `pico386/src/frontend/`, `pico386/src/hal/` | pico-amiga / emu16 / emu8 | Same author as pico-386 |
| tiny386 PC emulator (i386 CPU + x87, PC chipset, VGA/VBE, IDE, i8042, SB16, Adlib, PC speaker) | `pico386/src/pc/tiny386/` | [hchunhui/tiny386](https://github.com/hchunhui/tiny386) @ `43b4b85412563915e61e69e3f740d326edaba912`, by Chunhui He; device models derived from QEMU | BSD-3-Clause (`tiny386/LICENSE`); the QEMU-derived files carry their MIT headers |
| OPL2 emulator (Adlib) | `pico386/src/pc/tiny386/fmopl.c`, `fmopl.h`, `fmopl_table.h` | The MAME fmopl 0.37a by Tatsuyuki Satoh, as relicensed in DOSBox/QEMU and carried by tiny386 | LGPL-2.1-or-later (header of `fmopl.c`) |
| ini parser | `pico386/src/pc/tiny386/ini.c`, `ini.h` | inih by Ben Hoyt, via tiny386 | BSD-3-Clause (headers) |

**Not in the firmware:** `bios.bin` (SeaBIOS) and `vgabios.bin` (SeaVGABIOS) are LGPL-3. They
are taken unmodified from the tiny386 release tarball and go on the SD card. The user supplies
them; they are not in this repo.

## Core selection (2026-10-18)

- **tiny386: chosen.** It is the only small emulator that has a real i386 (protected mode,
  paging, optional x87) together with VGA/VBE and SB16. It is flat C99 with five platform hooks,
  and it already runs Windows 9x on an ESP32-S3. Its licence is BSD-3/MIT, plus fmopl under
  LGPL-2.1+.
- **Rejected:**
  - FabGL PCEmulator, Faux86 and 8086tiny: 8086 with CGA only.
  - pico-286: built around the RP2040.
  - DOSBox, PCem and 86Box: far too big, and their dynarecs have no RISC-V backend.

## Changes to tiny386

The vendored copy is the upstream `src/` core. These were left out:

- the platform front ends (`main.c`, `win32.c`, `esp/`, `wasm/`, `android/`)
- `ne2000.c`, which needs a host network. `ne2000_stub.c` (new) gives `pc.c` an inert card.
- `adlib_nuked_opl3.h`, the alternative OPL3 that is not used (`USE_NUKED_OPL3` stays unset).
- the Makefile

Changes, all made on 2026-10-18 and all marked `pico-386` or `TINY386_PICO` in the source:

- **`tiny386_port.h` (new).** It replaces the Makefile defines: `BPP 16`, `I386_ENABLE_FPU`,
  `USE_FMOPL`, `FMOPL_USE_STATIC_TABLE` and `PC_STEP_COUNT 2048`. It also routes `malloc`/`free`/
  `fopen`/`fclose` to tracked versions in `pc_platform.cpp`, so a session can be torn down. Every
  `.c` file includes it first.
- **Renames.** Arduino only copies known source extensions:
  - `fmopl.inc` → `fmopl_table.h`, which was generated once with upstream's `make prepare`
    (`-DGENTABLE`)
  - `i386ins.def` → `i386ins_def.h`
  - `simd.inc` → `simd_inc.h`

  The `#include` lines in `fmopl.c`, `i386.c` and `fpu.c` follow.
- **`i386.c`, `pc.c`, `vga.c`:** `#pragma GCC optimize("O3")`, as upstream builds these at -O3.
  `i386.c` no longer `usleep(1)`s on HLT, because the frame loop paces itself.
- **`i386.c`:** when `ESP_PLATFORM` is defined but `BUILD_ESP32` isn't (our P4 build), it includes
  `esp_attr.h` and puts `cpu_exec1` in IRAM (`IRAM_ATTR_CPU_EXEC1`) along with the functions
  already marked `IRAM_ATTR`. The flag getters, `refresh_flags`, `read_desc`, `set_seg`,
  `enter_helper`, the LSEG/XLAT helpers, `check_ioperm`, `pmcall`, `pmret`, `cpui386_step` and
  `cpu_getflags` gained `IRAM_ATTR`, and `parity_tab` gained `DRAM_ATTR`.
- **`pc.c`:** `PC_STEP_COUNT` can be overridden.
- **`vga.c`, `vga.h`:**
  - Under `TINY386_PICO`, graphics modes draw at their real size, top-left in the framebuffer.
    Line-doubled 320x200 is written as 320x200, not 640x400, and there is no `SCALE_*`/`SWAPXY`.
    The frontend scales the picture.
  - The size of the last picture is recorded.
  - `vga_pico_info()` (new) reports the mode, the text-mode registers, the text RAM, the font
    and the palette, for the frontend's own text renderer.
- **`misc.c`:** no host-console code (`termios`/`CaptureKeyboardInput` become no-ops).
  Emulink command 0x100 always reports two 1.44 MB floppy drives, so a disk can be put in A: or
  B: while the PC runs, even if the drive was empty at power-on.
- **`ide.c`:** a CD drive given an empty path starts empty instead of `exit(1)`, and
  `block_device_reinit` with an empty path ejects the disc. This gives every machine a CD drive
  that can be filled later.
  A hard disk whose path is a folder becomes a block device over `pc_folder.cpp` (a FAT16 disk
  built from that folder; see the D: drive in the README).
- **`pc.c`:** the IDE attach loop skips an empty path only for hard disks, so an empty CD drive
  is still created.
- **`fmopl.h`:** single-precision `FLOAT` under `TINY386_PICO`, as on upstream's ESP32 builds (the P4 FPU is single precision).
- **`sb16.c`:** the six `resample_*` functions are replaced by one resampler (`sb16_read` plus
  the loop in `sb16_audio_callback`, state in `rs_frac`/`rs_prev`/`rs_cur`). Upstream's restarted
  its rate counters on every mixer call and rounded the rate ratio to a fraction with a
  denominator of at most 8, so each call dropped or repeated a sample (a 60 Hz buzz) and rates like
  8000 Hz played off pitch. The new one keeps its 16.16 position between calls, interpolates
  linearly, and also plays signed 8-bit, which upstream left silent. When the DMA data runs out
  it holds the last value instead of dropping to silence, and counts it in `sb16_starved`.
