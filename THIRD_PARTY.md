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
- **`i386.c`:** `cpui386_get_pc` (declared in `i386.h`) reads CS and EIP without touching the CPU,
  for the RP2350's diagnostics task on the other core (`cpui386_get_state` refreshes the flags).
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
  `emulink_attach_floppy` also takes a folder: it becomes a 1.44 MB FAT12 floppy over
  `pc_folder.cpp` (`fdf[]`, read and written a sector at a time through `fdbuf`), and taking it
  out writes the guest's changes back to the folder. `emulink_floppy_folder()` (new, `misc.h`)
  returns it. Commands 0x101/0x102 also reject a drive number above 1 before using it.
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
- **The timer (2026-11-02).** Prince of Persia's 240 Hz music timer dragged and sped up on the
  RP2350. Only 140-160 IRQ0s a second got through there, and 15% of those were lost.
  - **`pc.c`:** `pc_step` calls `i8254_update_irq` every `PC_TIMER_STEP` (256) instructions
    instead of once per `PC_STEP_COUNT`, since a step can take 10 ms when guest RAM pages in.
  - **`i8254.c`, `i8259.c`:** an IRQ0 edge that found IRQ0 still pending in the PIC merged with
    it, and the tick was lost. The PIT now asks first (`i8254_set_irq_pending`,
    `i8259_irq_pending`) and keeps the tick for later, at most 50 ms (or two periods) of them.
  - **Counters for the RP2350 heartbeat:** `pit_irq0_fired`, `pic_irq0_merged` and
    `i8254_info`, read through `t386_timer_info`.
  - **Result on the device:** 450-495 IRQ0s every 2 s of the 480 due, and none lost.
- **The sound rate and the run loop (2026-11-02).**
  - **`adlib.c`, `pcspk.c`, `sb16.c`:** the fixed 44100 Hz output rate became `T386_AUDIO_RATE`
    (`tiny386_port.h`, the board's `AUDIO_OUT_RATE`). The RP2350 mixes at 22050 Hz, which halves
    the OPL synthesis. sb16.c's check of the guest's own rate (`freq >= 44100`) is unchanged.
  - **`pc.c`:** `pico_spk_writes` and `pico_adlib_writes` count guest writes to port 61h and to
    the Adlib/SB FM ports. `t386_run_until` reads them to mix right after the guest plays a note.
- **Snapshots (2026-11-03).** `snap.h` (new, ours) moves each device's state through a `SnapIO`.
  Every struct is preceded by its size, so a build with another layout refuses the file.
  - **`pc.c`:** `pc_snap` saves the `PC` fields, then the CPU, PIC, PIT, CMOS, both IDE buses,
    VGA, i8042, both DMA controllers, SB16, speaker, Adlib, i440FX and emulink. The serial port
    is not saved.
  - **`i386.c`, `fpu.c`, `i8042.c`, `i8254.c`, `i8257.c`, `i8259.c`, `ide.c`, `misc.c`, `pci.c`,
    `pcspk.c`, `sb16.c`, `vga.c`:** a `*_snap` each (under `TINY386_PICO`). They save the whole
    struct, and on load keep the fields that point into this session (other devices, callbacks,
    host buffers, open files). The CPU's TLB is emptied on load.
  - **`adlib.c`:** the OPL's state holds pointers into itself, so `adlib_write` keeps a shadow of
    the 256 registers and the address latch, and a load replays them (key-on last, timer control
    left out). Notes that were playing restart from their attack.
  - **`tiny386_port.h`:** `fwrite` goes to `t386_fwrite` (`pc_platform.cpp`), so the first write
    to a disk image after a start or a snapshot marks that disk as changed.
- **The guest's debug port on the P4 (2026-11-03).** `tiny386_port.h` sends `putchar` to
  `t386_putchar` (`pc_platform.cpp`), which writes SeaBIOS's port 402h log a line at a time through
  `hal_log`. Written straight to the UART, it cut into the serial remote's screenshot rows.

### The low-memory build (2026-10-27, for the RP2350)

`T386_PAGED` is set by `tiny386_port.h` when `board.h` selects `PICO386_LOWMEM` (the Pico 2 build,
or the desktop with `-DEMU_LOWMEM=ON`). Guest RAM then lives in the mainboard's PSRAM, which is
not memory-mapped, and is paged through 1 KB SRAM frames (`pc_pagemem.c`, ours). Under the flag:

- **`i386.c`:**
  - `pload*`/`pstore*` go through `pm_read*`/`pm_write*`.
  - The string fast paths take `PHYS_PTR` (`pm_ptr`) instead of `phys_mem + addr`.
  - Those paths cut a run at `STR_PAGE` instead of 4096 (2026-10-28): a `pm_ptr` pointer is good
    only to the end of its 1 KB frame. Without the flag `STR_PAGE` is 4096, as upstream.
  - On the RP2350 (2026-10-28) the file is built at -O2 -fno-inline-functions instead of -O3,
    and `IRAM_ATTR` puts `cpu_exec1` and the helpers in `.time_critical.i386`, which
    arduino-pico copies to SRAM. `cpu_exec1` is 90 KB that way (79 KB at -Os, 137 KB at -O3).
    From flash it ran through the 16 KB XIP cache: CheckIt gave 1.73x PC-XT, and 3.15x in SRAM
    at -Os.
  - The TLB uses the non-pointer form that upstream keeps for `BUILD_ESP32`.
  - It keeps the physical address of the PTE (`ppte`) instead of a host pointer, so setting the
    accessed and dirty bits is a paged store.
- **`i8257.c`:** DMA reads and writes go through `DMA_LOAD`/`DMA_STORE` (`pm_read8`/`pm_write8`).
- **`pc.c`:**
  - `pc_new` allocates no RAM (`t386_start` sets up the pages), and VRAM is the start of the
    SRAM arena (`pm_vram`).
  - The kernel command line is written with `pm_write`.
- **`vga.c`, `vga.h`:**
  - There is no framebuffer: `fb_data` is one 800-pixel line.
  - Each finished graphics line goes to a sink, `vga_set_line_sink()` (new).
  - The text-mode framebuffer pass is skipped, since the frontend draws text from text RAM.
- **`tiny386_port.h`, RP2350 only:** `CLOCK_MONOTONIC` is defined as `CLOCK_REALTIME`. newlib on
  arm-none-eabi has neither the constant nor `clock_gettime`; `posix_rp2350.cpp` supplies a
  `clock_gettime` that counts from boot, and `usleep`.
