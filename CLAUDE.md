# pico-386: notes for Claude

A 386/486 PC emulator (tiny386) for the **ClockworkPi PicoCalc** fitted with a **Waveshare
ESP32-P4-WIFI6**, plus an SDL2 desktop build of the same code for debugging. The user (GitHub:
hexadevti) also owns **pico-amiga** (`C:\Users\lucia\repos\pico-amiga`), which this project was
started from on 2026-10-18, and **emu16** and **emu8**.

- License: GPL-2.0-or-later for our files, the same as pico-amiga.
  - Never copy emu16's `src/md/`, `src/sound/` or `src/cpu/`: they are AGPL or non-commercial.
  - Record every change to tiny386 in `THIRD_PARTY.md`.
- The plan the user approved is in `~/.claude/plans/create-a-pc-386-serene-panda.md`. The user
  chose an Arduino-cli build like pico-amiga's. The DOS and Windows profiles were merged into
  one PC on 2026-10-19 at the user's request: 486 + FPU, and the most RAM that fits.

## State right now (2026-10-18)

- Not a git repo yet, and nothing has been committed.
- **Desktop, working:**
  - SeaBIOS boots.
  - FreeDOS 1.3 boots from a floppy into its installer, and keys reach it.
  - The text renderer is readable: colours, blink and cursor work.
  - Mode 13h and mode 12h (640x480) scale correctly.
  - The PC speaker gives a clean tone.
  - The home menu, file browser and in-game menu work.
  - Create a hard disk works, and FreeDOS's installer accepts the disk it makes.
  - Booting with A: empty falls through to C:.
  - Swapping A:/B: in-game works, and DOS reads the new disk.
  - A scripted FreeDOS install swaps x86DSK01-05 in-game. Each disk takes about 80 s on the
    desktop, so swap only when it asks.
    - The resulting `dos.img` boots to `C:\>`.
    - The last step, extracting `FREEDOS.SAF`, fails with a garbled filename, so CTMOUSE,
      FDNPKG and the CD driver are missing. The cause is not yet known; it may be FreeDOS
      reading a file opened on disk 1 after the swap.
- **Never tested:**
  - the keyboard mouse with a real driver
  - the Adlib. The SB16 was tested on the desktop on 2026-10-19 with a DMA sine-test .COM: after
    the resampler fix (THIRD_PARTY.md) it is clean to the 8-bit limit (about 47 dB). The P4's I2S
    queue went from 23 ms to 70 ms in the same pass, to stop underrun clicks.
    The user still heard random ticks on the device after that. The FPS line now shows
    `g` (I2S buffers that ran dry, `on_send_q_ovf`; it also counts while the menu is open),
    `s` (mixes the SB16 had no DMA data for, `sb16_starved`) and the longest gap between sound
    writes (over 70 ms = a gap). Prince of Persia on the device read `g836 s0 17ms`, so the
    output ran dry with no long gaps: sound was made at exactly the rate it was played, so the
    queue never built a cushion. `pc_audio` now steers the queue's low point to 25 ms
    (`hal_audio_queued_us`, which counts `on_sent` on the P4), and `g` counts per second.
    **Fixed on the device (2026-10-19): `g0 s0 17ms`, no ticks.** The desktop was always clean. Test: `pop.ini` (C: =
    DOSHDD.img, `C:\PRINCE\PRINCE.EXE`).
  - VBE modes and Windows
  - writes to a hard-disk image
  - a D: folder disk above 504 MB (FAT16 LBA, type 0Eh), and D: with Windows 95 long names
- **P4:**
  - The firmware compiles: 1059 KB of flash, 37 KB of static RAM.
  - It runs on the device. The first build was as slow as a PC-XT in CheckIt.
  - First speed pass (2026-10-18). CheckIt went from 0.25k (an XT is 0.3k) to **8.6x PC-XT**:
    - `cpu_exec1` (135 KB) and the other `IRAM_ATTR` functions in i386.c are in IRAM under
      `ESP_PLATFORM`. About 260 KB of internal heap is left.
    - Drawing runs on core 0 through `hal_job_start` (pc_core `drawJob`). The app calls
      `hal_job_wait` before the in-game menu, and `stopMachine`/`pc_shutdown` call it too.
    - The FPS meter shows guest MIPS (`EmuCore::fpsText`, `t386_instructions`).
      `drawJob` pushes only rows 0-309: a full-height push wiped the meter every frame.
    - The meter then showed `10 fps 10 drawn 0.2 MIPS`: the audio write blocked on a full queue,
      and `pc_audio` counted that wait as time to make sound for, so frames grew to the 100 ms
      cap. `pc_audio` now subtracts the blocked time (`hal_audio_wait_us`). After that fix the
      device showed `60 fps 60 drawn 1.9 MIPS`, and CheckIt 8.5x PC-XT.
    - Next, all hot i386.c helpers went to IRAM too. i386.c compiled three ways, measured with
      CheckIt on the device:
      - -O3 (`cpu_exec1` 130 KB): 9.4x XT, 2.0 MIPS. **Kept.**
      - -O2 (118 KB): 8.5x, 1.9 MIPS.
      - -Os (91 KB): 6.5x, 1.4 MIPS.
      Smaller code is slower, so the L1 instruction cache is not the limit any more.

## How it fits together

- **`src/pc/pc_glue.c`** (C), the only file that touches tiny386's structs:
  - `t386_start` parses the .ini (tiny386's `[pc]`/`[cpu]` keys plus our `[pico]`), resolves paths
    against the SD root, checks the files and calls `pc_new`.
  - `t386_run_until(us)` loops `pc_step` + `vga_step`.
  - It also covers mixing, keys, the mouse, floppy and CD swap, and `vga_pico_info`.
- **`tiny386/tiny386_port.h`** is included first by every core file:
  - It holds the defines and routes `malloc`/`fopen` to the tracked versions in `pc_platform.cpp`.
  - tiny386 never frees anything; `t386_release_all()` frees the whole session and closes the
    images, which is what flushes guest writes.
  - `BUILD_ESP32` is deliberately **not** defined. Upstream's ESP path brings raw-SD, UART and
    wifi code.
- **`pc_core.cpp`, the EmuCore (`selfPaced`):**
  - The guest's clock (`get_uticks`) is wall time minus the time the machine was stopped
    (`t386_clock_skip`, when a frame starts more than 100 ms late). Without that, after the menu
    the PIT fired every missed tick at once and Windows 3.1 setup crashed at the disk 3 swap.
  - Each frame emulates until the next 1/60 s of wall clock, mixes the audio for the real time that
    passed, and draws only if `hal_video_ready()`.
  - When it is slower than real time, the guest just sees a slower CPU.
- **`pc_video.cpp`:**
  - Graphics are box-filtered from the 800x600 framebuffer (tiny386 draws at native size,
    top-left) to 320x240 at y=40.
  - Text is rendered from text RAM with the guest font shrunk to 4x12 and 2-bit antialiasing.
    Unchanged frames are skipped by hashing.
  - 80x25 uses the hand-drawn fonts in `pc_textfont.h` instead (2026-10-19): ASCII, accents, box
    drawing and blocks. They cover only glyphs that still match `vgafont16`, so a font a program
    loads itself is still shrunk. Edit `tools/gen_textfont.py` (`--preview x.png` draws both)
    and rerun it; never edit the header.
  - Settings > Font size (`fontsize` in pico386.cfg, also in the in-game menu) picks 4x12 (fills
    300 lines) or 4x8 (200 lines, centred). It caps the cell height in `renderText`.
- **`pc_keymap.cpp`:**
  - PicoCalc characters are mapped to US-layout Linux keycodes, with Shift faked as needed.
  - It handles typematic repeat and the keyboard mouse.
  - The keyboard mouse has three modes (2026-10-19): `MOUSE_ARROWS`, `MOUSE_ON` (arrows move the
    pointer only) and `MOUSE_BOTH` (pointer and arrow keys). Both Shifts held together, or Ctrl+Tab,
    cycles them; the first Shift has already reached the guest by then. The two button keys come
    from `kButtonKeys` (`[pico] mouse_left`/`mouse_right`, default alt/rshift); outside ARROWS
    they are only buttons. `pc_core` saves the mode and keys to the ini and shows a 2 s note
    in the bottom strip (`showNote`). Test with `PC_KEYS=...,shift+rshift,...`.
  - Shift as a mouse button (2026-10-19, from the device's serial log): the STM32 sends Shift+Up/
    Down as PgUp/PgDn (0xD6/0xD7), and sends nothing at all for Shift+Left/Right. While a Shift
    that is a button is held (or any Shift, in Mouse mode), `shiftedArrowOf` turns PgUp/PgDn back
    into pointer arrows, so vertical drags work on the device. Sideways drags with a Shift button
    can't work. The desktop never sends them that way.
  - Caps Lock (2026-10-19) is kept in `s_caps` and never sent to the guest. The STM32 applies
    its own Caps Lock to letters and the desktop doesn't, so a guest Caps Lock as well turned
    'A' back into 'a' on the device. `mapKeyCaps` sets a letter's Shift from the Shift keys held.
  - Ctrl+Alt+Del from the keyboard did not reach the guest on the device. Ctrl+Alt+Backspace is
    now also sent as Ctrl+Alt+Del, and every key pressed with Ctrl+Alt held is logged
    (`kbd: Ctrl+Alt+0xNN`), so the serial log shows what the STM32 sends. Never tested on the
    device. On the desktop both combinations reboot SeaBIOS.
  - `PC_KEYS` chords take two held keys (`ctrl+alt+del`); `del`, `caps` and `shift` are names.
  - tiny386's `ps2_put_keycode` takes Linux codes: below 96 is the set-1 scancode, 96 and above
    are extended keys.
  - The guest's own reads drain the PS/2 queue. A prompt only reacts once the guest polls, so
    a scripted key needs the guest to be ready (FreeDOS needs about 15 s to reach its first
    prompt).
- **`pc_profiles.cpp`:**
  - Startups (2026-10-19, replacing "Boot a disk image"): the home menu lists each startup, then
    "Create new startup", "Create a hard disk", Settings and Exit. A startup is a machine .ini in
    `/pc-386` with `[pico] name`. `pc.ini` comes first and is "PC" when it has no name. Other
    named .ini files follow, sorted by name. Unnamed test .ini files are not listed.
    - "Create new startup" asks for a name (`ui::editText`) and writes the first free
      `startN.ini`, which mounts only C:.
    - A startup's row opens its drives screen (`pcdisk_machine_menu`, which returns `MACHINE_*`).
      The screen has Start, the drives, "Boot from" (`[pico] boot = cd`), Rename, and Delete
      (every startup but pc.ini). Delete removes only the .ini.
    - `startup=` in pico386.cfg remembers the last startup's file for the home menu's cursor.
  - It writes `/pc-386/pc.ini` when it is missing (C: = c.img, else dos.img or win.img). It asks
    for `T386_MEM_MAX_MB` (32 MB) of RAM; `t386_start` steps down 4 MB at a time until guest
    RAM + VRAM + 2 MB fits in PSRAM.
  - Everything persists in the startup's .ini (the user asked, 2026-10-19).
    `[pico] boot = cd` becomes SeaBIOS's CMOS boot order in `t386_start` (3Dh/38h); without
    it SeaBIOS tries C: before the CD. `pc_core` writes `[pico] mouse` (0/1/2) and the
    button keys when they change. `pcini_get_in`/`pcini_set_in` edit any section.
  - Test: `sdcard/pc-386/m13.iso` is an El Torito CD (m13.img, floppy emulation); `cdt.ini` boots
    it ahead of `dos.img`.
  - The CD in DOS needs a guest driver. `sdcard/pc-386/cdrv.img` is a floppy holding FreeDOS's
    `UDVD2.SYS`. Add `DEVICE=C:\DOS\UDVD2.SYS /D:CDROM1 /UX` to CONFIG.SYS, and
    `C:\DOS\MSCDEX.EXE /D:CDROM1 /L:E` to AUTOEXEC.BAT ahead of SMARTDRV. This works on
    DOSHDD (MS-DOS 6 + Win 3.11) on the desktop and on the device (2026-10-19).
- **`pc_disks.cpp`:**
  - It classifies images: .iso is a CD, up to 2.88 MB is a floppy, larger is a hard disk.
  - The browser filter labels each file with its type and size.
  - Big folders (2026-10-19, 500 game zips): on the card's FAT, each `stat` searches the folder
    from its start. So the browser takes folders from `d_type` (the P4 has it; MinGW falls back to
    stat), and for any disk, CD or zip it reads sizes only for the rows on screen
    (`BrowseOpts::tag` + `ui::Screen::prepare`). The floppy and hard-disk choosers still stat
    every file, because only the size tells those two apart. "Reading the folder" progress shows
    after 250 ms. `ui::list` jumps to rows by typed letters (several letters within 1 s add up).
  - It edits the ini's drive keys (`pcini_get`/`pcini_set`, where "" comments the key out).
  - It provides the per-drive chooser and the drives screen.
  - `makeHardDisk` writes an MBR, a FAT16 partition at LBA 63 and the format, using 16 heads,
    63 sectors, at most 504 MB. The boot code is placeholder text until DOS's `sys` replaces it.
- **`pc_folder.cpp`, the D: folder disk (`hdb` = a directory):**
  - `ide.c`'s `block_device_init` hands a directory to it. It builds a virtual FAT16 disk: MBR,
    a partition at LBA 63, the FAT and directories (with VFAT long names) in RAM. Files are
    allocated contiguously and read straight from the host files.
  - Guest writes never touch the folder while it runs. FAT and directory writes stay in RAM; data
    clusters go to `/pc-386/.pico386-dN.tmp`.
  - `pfolder_close_all()` (from `t386_release_all`, and `atexit` on the desktop) parses the final
    tree and matches nodes by first cluster, then by short name in the same parent. It stages
    new and changed files in `<folder>/.pico386-commit`, then deletes, renames and moves them in.
  - If sector 0 or 63 was rewritten (FDISK/FORMAT), nothing is applied.
  - Tested on the desktop with FreeDOS: dir, type, echo >, copy, ren, md, del, overwrite, reboot.
- **`pc_zip.cpp`, zips as D: (2026-10-19):**
  - The ini keeps the zip itself (`hdb = pc-386/games/x.zip`). `t386_start` calls `pczip_folder`,
    which unpacks the zip once to `/pc-386/.unzipped/<name>/` and hands that folder to the folder disk.
    Guest writes go to that folder, not to the zip.
  - A `.pico386-zip` marker in the folder records the zip's path, size and mtime. If they match,
    the folder is reused. If not, it is emptied and unpacked again.
  - The inflater is our own (stored and deflate, CRC checked). It has no zlib dependency.
  - The D: chooser has "Choose a zip file". (The old "Boot a disk image" also booted a zip
    holding a single image; that went with it on 2026-10-19.)
  - Tested on the desktop: all the example zips in `sdcard/pc-386/games` unpack with matching
    CRCs. `zipt.ini` (C: = DOSHDD.img, D: = Alley Cat) runs the game from D:. Never tested on the device.
- **Drives in-game (`pc_core.cpp`):**
  - A:/B: swap through emulink.
  - A:/B: also take a folder or a zip (2026-10-19), as a 1.44 MB FAT12 floppy
    (`pfolder_open_floppy`, emulink's `fdf[]` in misc.c). `t386_swap_floppy` maps a zip through
    `pczip_folder` and returns `T386_FD_*` codes: too big, in use (other drive or D:), unpack.
    A swap or eject commits the old folder (`pfolder_close`). Each floppy gets a new serial, so
    DOS sees the change even with SMARTDRV caching A:. Tested on the desktop with `fdzip.ini`:
    Crossfire to Mine Shaft in-game, and `echo > a:\t.txt` was committed on the swap. Never
    tested on the device. Live swap of D: was tried and dropped: DOS has no media change for
    fixed disks.
  - The CD goes through `ide_change_cd`. Every machine has an empty secondary-master CD.
  - C: and D: rewrite the ini and restart the machine (`stopMachine` shows "Saving D:").
    A live D: swap was tried on 2026-10-19 and dropped, because it failed on the device. DOS has no
    media change for a fixed disk: it keeps the BPB from boot, and SMARTDRV serves the old
    sectors (on the desktop it only worked after `SMARTDRV /R`). The user chose the restart.
  - DOS sees floppy swaps; this was tested by copying from B: before and after a swap.
    Emulink itself has no change line.
- **`core.h`:** the EmuCore gained `keyEvent`, `menuItems`/`menuAction` (core rows in the in-game
  menu), `lastError` and `selfPaced`. `app.cpp` builds the in-game menu from these.
- **The mixer's quirk:** `mixer_callback` *adds* into the stream, and the SB16 leaves it untouched
  while idle. `t386_mix` zeroes the buffer first; without that, the PC speaker saturated.

## Builds

**P4.** Use the VS Code task **P4: Build** / **P4: Build & Upload** (COM9), or:

```sh
export ARDUINO_DIRECTORIES_DATA=~/.emu6502-p4/data ARDUINO_DIRECTORIES_USER=~/.emu6502-p4/user
arduino-cli compile --fqbn esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,ChipVariant=postv3,PartitionScheme=huge_app,DebugLevel=error \
  --build-property compiler.optimization_flags=-O2 \
  --build-property compiler.cpp.extra_flags=-DBOARD_PICOCALC_P4 \
  --build-path ~/.pico386-p4-build pico386
```

- Arduino copies only known source extensions into the build tree, so vendored `.inc`/`.def` files
  must be renamed `.h`.
- `compiler.cpp.extra_flags` doesn't reach the C files; `tiny386_port.h` must not depend on it.

**Desktop.** MSYS2 MinGW32. The login shell starts in `$HOME`, so `cd` inside the `-lc`:

```sh
MSYSTEM=MINGW32 /c/msys64/usr/bin/bash.exe -lc "cd /c/Users/lucia/repos/pico-386 && make -C build -j8"
```

**Test hooks** (desktop):
- `PC_INI=sdcard/pc-386/x.ini` boots that machine directly, skipping the home menu.
- `PC_KEYS=` runs a comma-separated script:
  - key names (`up`, `enter`, `esc`, `f5`, `pgup`, `ctrl`, `alt`, ...), single characters, and
    chords like `ctrl+esc`
  - `wNNN` waits NNN ms
  - `shot` saves `sdcard/shotN.bmp`
  - `quit` exits
- `PC_WAV=file` dumps the mono s16 44.1 kHz audio stream.
- `PC_SD=dir` uses another SD folder. `PC_SHOT=N` takes a screenshot after N frames.

Local test data in the gitignored `sdcard/`:
- `sdcard/roms/pc/`: `bios.bin` and `vgabios.bin`. That folder is the default, and the user can change it in
  Settings > BIOS folder (`biosdir` in pico386.cfg). `t386_start` takes it and ignores the ini's bios keys.
- everything else in `sdcard/pc-386/`: the .ini files and the disk images (one folder for the emulator, the user's choice)
- `fdboot.img`, the FreeDOS 1.3 boot floppy
- boot-sector tests, each with a matching `.ini` in `sdcard/pc-386/`:
  - `m13.img`: a mode 13h palette grid
  - `m12.img`: 640x480 diagonal bars
  - `beep.img`: a 1 kHz tone

## Hardware facts

These come from pico-amiga and emu16; its CLAUDE.md has more detail.
- **CPU:** dual-core RISC-V, 360 MHz, no JIT, 32 MB PSRAM.
  - The emulator runs on core 1 with a 32 KB stack.
  - The keyboard and video tasks run on core 0.
- **Panel:** 320x320 SPI. `hal_video_*` is double-buffered.
- **Keyboard:** I2C STM32. It sends characters with one press and one release, so repeat is
  emulated here. `KEY_RELEASE_ALL` means events may have been lost.
- **`char` is unsigned** on the P4, and the desktop builds with `-funsigned-char`.
- **PSRAM per machine:** the guest RAM, the VGA RAM and the 800x600x2 framebuffer. The PC asks
  for 32 MB of RAM and gets what fits; check the serial log's "KB RAM" line on the device.

## Ideas for the performance pass (after the first on-device run)

- The CPU state and TLB are already internal: `track()` sends blocks of 64 KB or less to
  `hal_alloc_fast`.
- Upstream's P4 sdkconfig uses a 512 KB L2 cache; Arduino's precompiled libs have 256 KB.
- Tune `PC_STEP_COUNT`.
- Give VGA dirty tracking, since `vga_refresh` redraws everything each time.
- Use a larger IDE transfer, or a write-back cache, for SD speed.

## Conventions

- Match the existing style: a `// file.cpp -- what it is` header and comments that explain *why*.
  C++17 for the glue, C for tiny386.
- Commit only when the user asks.
- The user tests on the real PicoCalc. Ask for on-device results when it matters.
