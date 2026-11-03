# pico-386: notes for Claude

A 386 PC emulator (tiny386) for the **ClockworkPi PicoCalc** fitted with a **Waveshare
ESP32-P4-WIFI6**, plus an SDL2 desktop build of the same code for debugging. The user (GitHub:
hexadevti) also owns **pico-amiga** (`C:\Users\lucia\repos\pico-amiga`), which this project was
started from on 2026-10-18, and **emu16** and **emu8**.

- License: GPL-2.0-or-later for our files, the same as pico-amiga.
  - Never copy emu16's `src/md/`, `src/sound/` or `src/cpu/`: they are AGPL or non-commercial.
  - Record every change to tiny386 in `THIRD_PARTY.md`.
- The plan the user approved is in `~/.claude/plans/create-a-pc-386-serene-panda.md`. The user
  chose an Arduino-cli build like pico-amiga's. The DOS and Windows profiles were merged into
  one PC on 2026-10-19 at the user's request: a 386 with an FPU, and the most RAM that fits. Describe it only as a 386 (the user,
  2026-11-02): tiny386 emulates a 386; the ini's `gen = 4` only changes what CPUID reports.

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
    - With all 115 volumes in one folder as D:, `cd \freedos` and
      `slicerex /voxf d:\freedos.saf /g 486` extract everything (1310 files, about 7 min on
      the desktop). So the failure comes from the swaps. The repo's
      FreeDOS image was repaired this way on 2026-11-02 and boots with no errors. The repo ships
      it as `disks/freedos-hdd-32mb.img` (xcopied to a 32 MB disk; the 128 MB zip was dropped
      on 2026-11-03, the user asked). A local MS-DOS 6.22 disk, `msdos622-hdd-32mb.img`
      (from DOSHDD, no Windows), sits beside it and is kept out by `.git/info/exclude`.
    - Its FDCONFIG.SYS now defaults (1 s menu) to HIMEMX + `LBACACHE 1024` (in FDAUTO.BAT).
      JEMMEX/JEMM386 (options 2 and 3) run DOS in V86 mode, where every INT traps: with
      `SPEED.COM` (a loop count per second) INT 21h was 2.3x, INT 10h 2.5x and REP MOVSW 2.8x
      slower under JEMMEX on the desktop. On the RP2350 V86 also means guest paging through the
      pager. MS-DOS 6 on DOSHDD felt faster for that reason (and SMARTDRV).
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
    queue never built a cushion. `pc_audio` now steers the queue (`hal_audio_queued_us`,
    which mirrors the I2S driver's free-buffer queue on the P4; see the RP2350's Prince notes for
    today's rule), and `g` counts per second.
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
    - **Slow boot (2026-11-03):** DOS took over a minute to boot, at 0.0-0.3 MIPS. The audio writes
      blocked for 1-1.9 s of every 2 s. `hal_audio_queued_us` counted frames written against
      buffers sent, and sent buffers of silence counted as played data. So it read 0-22 ms with
      the queue full, and `pc_audio` kept topping the queue up. `audio_p4.cpp` now mirrors the
      driver's free-buffer queue (`s_free`, `tookFrames`). Now 0 blocks, about 40 ms queued, `g0`,
      and MS-DOS 6.22 reaches `C:\>` in about 25 s (HIMEM's test of 24 MB reads 0.1 MIPS: each
      REP counts as one instruction).
    - **Driving the device yourself (2026-11-03, the user asked for it).** The P4 stays on COM9
      (console UART, open it with DTR and RTS both high before `open()`, or it resets), with
      esp32loader on Wi-Fi at 192.168.0.100. Never flash it over serial.
      - `python -I tools/deploy-p4.py` sends `@uf2menu`, waits for the loader's web server (F4,
        ESC O S, if Wi-Fi autostart is off), uploads `~/.pico386-p4-build/pico386.ino.bin` to
        `/pico-386/pico386-p4.bin` (keeping `pico386-p4.prev.bin`) and flashes it. About 15 s.
      - `python -I tools/remote.py --p4 [--out prefix] keys "..."` and `remote.py --p4 shot x.png`
        work as on the RP2350 (options before the command). The commands are parsed in
        `system_p4.cpp`. Send keys only once the home menu is up: a `@keys` sent during boot is lost.
      - A shot reads a PSRAM shadow of the panel (`display_p4.cpp`, written with every push), not
        the panel. SeaBIOS's port 402h log goes through `hal_log` a line at a time, under the
        log lock, so it no longer cuts into the shot's rows.

- **RP2350 (a plain Pico 2 in the PicoCalc, 2026-10-27):** the user chose the Pico 2 without
  PSRAM. It builds (546 KB of flash, 436 KB of static RAM; 10.3 KB of heap free with a PC
  running).
  - **Speed (CheckIt on the device, 2026-10-28):** 1.73x PC-XT with the interpreter in flash;
    3.15x with i386.c in SRAM at -Os; **5.07x** at -O2 -fno-inline-functions and 300 MHz
    (1.1M guest instructions a second, no page faults while it runs). See THIRD_PARTY.md.
    - From flash, `cpu_exec1` ran through the 16 KB XIP cache, which core 1 shares.
    - SRAM is full. The 90 KB interpreter fits only because VRAM went from 256 KB to 160 KB
      (mode 12h needs 150 KB; EGA/VGA games that keep pages above that lose them) and `pm_map`
      went from pointers to 2-byte frame numbers.
    - Ideas not tried yet: hot non-i386 code in SRAM (`pc_step`, the PIC/PIT, port I/O).
  - `PICO386_LOWMEM` (board.h): guest RAM lives in the mainboard's 8 MB PSRAM, which hangs off
    a PIO and is not memory-mapped. It is paged through 1 KB SRAM frames (`pc_pagemem.c`).
    - `hal_arena` holds VRAM (160 KB), then 64 KB of frames. The menu canvas (200 KB) borrows the
      start of it, so `app.cpp` parks that much (`PARK_BYTES`, the first frames too) in PSRAM
      around the menus.
    - Pages were 4 KB at first (16 frames), and DOS crawled: about 40k guest instructions a
      second, nearly all of the time spent faulting (about 1.8 ms each). 1 KB pages (2026-10-28)
      cut desktop FreeDOS-boot faults from 313k to 49k (2 KB: 276k, 512 B: 41k), and the
      device went to 0.25-1.4M instructions a second, with no faults at the DOS prompt. The page
      map costs 2 bytes a page, so guest RAM is capped at 4 MB (`PM_MAX_RAM`). i386.c's string
      paths stop at `STR_PAGE`.
    - The device's log is UART0 on COM6. `diag_rp2350.cpp` halts and prints on a crash, and
      prints a heartbeat every 2 s (instructions, CS:EIP, faults, write-backs, stack, heap), and
      a `diag: timer` line: the PIT rate, IRQ0s raised and lost, and how many periods behind,
      and a `diag: audio` line: mix time, buffers that ran dry, the queue's low point, the longest
      gap between writes, and speaker/Adlib port writes.
    - **Prince's music (2026-11-02):** it wavered in speed and clicked. Fixed on the device: the
      timer (THIRD_PARTY.md), and the sound path:
      - The mixer runs at `AUDIO_OUT_RATE` (`T386_AUDIO_RATE`: 22050 Hz here, 44100 on the P4 and
        the desktop), which halves the OPL cost.
      - `t386_run_until` calls `audioHook` after each `pc_step`, so sound is made during the frame
        (1 ms after an Adlib/speaker write, or every 8 ms), not only at its end.
      - It is mixed and queued 128 samples at a time (`kMixMax`).
      - `hal_audio_queued_us` on the RP2350 leaves out the buffer being filled and what the DMA
        has already played; it read up to 11.6 ms high. The ring must still hold two buffers at
        each write, since the DMA picks the next buffer a whole buffer early.
      - `pc_audio` steers the queue after each write to 30 ms plus the longest recent gap (fading
        over 2 s), at most 62 ms.
      Result: 1 dry buffer in 80 s of Prince's intro and first level (it was 5-6 every 2 s).
    - **Driving the device yourself (2026-11-02, the user asked for it).** The PicoCalc stays on COM6,
      with PicoMultiLoader on Wi-Fi at 192.168.0.70. Nothing here needs the user's hands:
      - `python -I tools/deploy-rp2350.py` sends `@uf2menu`, uploads `build-rp2350/pico386.ino.uf2`
        through the loader's web API to `/pc-386/pico386.ino.uf2` (the old one becomes
        `pico386.prev.uf2`) and flashes it. Boot takes about 20 s.
      - `python -I tools/remote.py keys "enter,w800,shot"` types a PC_KEYS script (`keyscript.cpp`,
        shared with the desktop's `PC_KEYS`); each `shot` saves `sdcard/rshotN.png` (`--out`).
        `remote.py shot x.png` takes one shot, and `remote.py log 10` prints the log.
      - The firmware side is `rp2350CheckSerialCmd` (`@keys`, `@shot`, `@shotraw y`,
        `@shottest kHz`). The script lives on the heap only while it runs.
      - A shot reads the panel back (RAMRD, 4 MHz on MISO): a dummy byte, then RGB565 as written.
        Reads are exact up to 16 MHz, and at 25 MHz each 1 bit smears into the next. CS has to go
        high after a read, or the panel keeps sending and garbles the next commands. The panel
        and log locks are held for the whole shot, so the PC pauses for it (2-30 s, depending on
        how well the rows compress).
      - Only one program can have COM6 open, so close any serial monitor before using these.
    - There is no 800x600 framebuffer: `vga.c` hands finished lines to `pc_video`, which pushes
      bands straight to the panel (`hal_panel_push`).
    - tiny386 itself takes 43 KB of heap. 26 frames left too little heap, and on the
      device (2026-10-28) 18 ran out at the PC's first frame, when FreeRTOS (whose heap is malloc's,
      `heap_3a`) made the draw task. The draw task is now static and made at boot. That is
      why the frames stay at 64 KB.
    - A zip chosen for A: in-game hung the device (2026-11-01). With a PC running, only about
      10 KB of heap is free, and the unzip wanted 64 KB for the zip's tail plus a 32 KB window.
      The folder floppy then needed 12 KB for its FAT and root directory. Now:
      - `pc_zip` scans the tail in 1 KB steps and reads the central directory entry by entry.
      - The inflater keeps 2 KB in SRAM and a 32 KB ring in PSRAM (`pcmem_scratch`, just below
        the park area).
      - A folder floppy's FAT and root directory use frames the pager lends (`pm_lend`, through
        `pcmem_lend`). Lent frames come only from above the menu canvas, because the floppy is
        built while the menu is drawn. The pager keeps at least 16 frames.
      Tested on the desktop with `build-lowmem` (Crossfire to Mine Shaft in-game). Not yet tested
      on the device.
    - The device still hit `abort()` (a HardFault in `__breakpoint`, lr in `_exit`) as soon as
      "Choose a zip file" opened. The file browser's vectors of strings and Items (about 150
      bytes a file) ran out of heap. `browser.cpp` now packs each folder's names into the PSRAM
      that nothing uses (`pcmem_spare`, 4 MB up to the scratch area; the heap on other builds),
      and keeps only a 4-byte offset per file in SRAM, sized by a first counting pass and lent
      by the pager when it can. `ui::list` takes rows from `Screen::itemAt` when `items` is null.
      Tested on the desktop with `build-lowmem` (527 zips in `games`). Not yet tested on the device.
    - The device's PSRAM moves whole words by DMA. Until 2026-11-01 `hal_psram_read`/`write`
      dropped any address, length or buffer not a multiple of 4 ("psram: bad read" in the log),
      and the desktop's simulated PSRAM never did, so the browser's names and the zip ring
      worked only on the desktop. Unaligned calls now go through a 64-byte bounce buffer
      (a read-modify-write for writes).
    - Mounting some zips on A: still hit `abort()` (2026-11-01): about 200 bytes of heap a file
      (the folder disk's nodes with a path, a name and a vector of children each, plus the
      unzip's list of entries), so roughly 40 files used up the 10 KB. Now:
      - `pc_folder`'s `Node` is 56 bytes, with no path (`hostOf` rebuilds it), names in one pool
        and children as an index range.
      - For a floppy, `countTree` sizes the nodes and names up front, and they are lent.
      - `pc_zip` walks the central directory twice (`eachEntry`) and keeps no list, and
        `removeTree` deletes one entry at a time.
    - Freezes on every drive (2026-11-01; a peer session also saw `abort()` in `pc_new` on a D:
      restart with 9.5 KB free). A D: folder's FAT, root and directories were on the heap too.
      Now **nothing of a folder disk lives in SRAM on the RP2350**:
      - Its tables are `Store`s in the PSRAM (`pcmem_folders`: the PSRAM after the browser's
        512 KB spare, up to the scratch area, carved by `psAlloc`). These are the FAT (read through
        a 512-byte cache, `fatSync`), the root, the directories, the nodes (`nodeAt`/`setNode`),
        the names (`nameOf`/`addName`, sized by `countTree`) and the spill map (2 bytes a cluster).
        Desktop builds keep them in vectors.
      - Clusters are handed out in node order, and a node with none gets the next one's `first`,
        so `ownerOf` binary-searches the nodes themselves (no index). Its entry writes 0
        (`entryFirst`).
      - `scanDir` writes children straight to the node store. SRAM holds 13 bytes a child while
        they are sorted (an order, the nodes moved along its cycles) and given short names.
      - Sectors written outside the FAT (`extra`) are capped at 4. The commit's folder list is a
        lent vector, not a `vector<bool>` of every cluster.
      - The build and the commit catch `bad_alloc`. The disk is refused (logged) or its changes
        are lost, instead of `abort()`. The desktop's `PC_HEAP` budget now returns NULL too.
      - `t386_start` frees the floppy probe's EMULINK, which sat in the middle of the heap.
        `t386_release_all` closes the folders last.
      Tested on the desktop with `build-lowmem` and `PC_HEAP=55`. The heap peaked at 46-50 KB
      (43-45 KB is the PC itself) for all of these: a 436-file zip on D:, 1802 entries on D:
      (1502 in one folder), 1000 files on a floppy, zips on A:+B:+D: together, a 429-file
      zip swapped onto B: in-game (54.6 KB before), D: zip changed twice in-game (restarts), and
      writes on D: written back. Not yet tested on the device.
  - Test it on the desktop with the `build-lowmem` CMake dir (`-DEMU_LOWMEM=ON`), which simulates
    the PSRAM. Prince of Persia and MS-DOS 6 run there.
  - `psram_rp2350.cpp` drives the PSRAM as plain SPI from pio1 (CS 20, SCK 21, SIO0 2, SIO1 3).
    It calibrates itself at boot over clock dividers and read-delay clocks, and logs
    `psram: ... KB, SCK ... kHz, N wait clocks, N-byte bursts`. Ask for that line first.
  - Cores: arduino-pico's FreeRTOS SMP runs setup/loop on core 0 with a 4 KB stack. So
    `pico386.ino` starts an "app" task (24 KB static stack) on core 0, and the draw job and
    keyboard poll run on core 1. The panel has a mutex, because both cores push to it.
  - The POSIX gaps are filled in `posix_rp2350.cpp` and `include/dirent.h`: mkdir, rmdir,
    rename, readdir over SDFS, clock_gettime and usleep. `VFS.map("/sd", SDFS)` covers the
    rest. stat has no mtime there, so the zip marker always sees mtime 0.
  - `hal_reboot_to_loader` (Ctrl+PgUp, home menu Exit) writes PicoMultiLoader's watchdog request
    (scratch 0 = 0xe98cc638, scratch 1 = 1), as emu8 does. Without a loader it is a plain reboot.

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
  - Test: `sdcard/pc-386/disks/tests/m13.iso` is an El Torito CD (m13.img, floppy emulation); `cdt.ini` boots
    it ahead of `dos.img`.
  - The CD in DOS needs a guest driver. `sdcard/pc-386/cdrv.img` (`disks/cdrom-driver.img` in git) is a floppy holding FreeDOS's
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
  - Each kind of choice (floppy, hard-disk or CD image, zip, folder) opens in the folder it was last
    made in (2026-11-02, the user asked). These are `dir_floppy`, `dir_hdd`, `dir_cd`, `dir_zip`
    and `dir_folder` in pico386.cfg, relative to the card root. `rememberPick` writes the cfg as
    soon as the choice is made. Without one, the chooser opens beside what the drive holds.
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
  - Zips of floppy images (2026-11-01; the Sierra sets: `(Disk N) [!].ima`). A zip chosen for A:/B:
    is unpacked, and if it holds .ima/.img/.vfd files up to 2.88 MB (`pczip_images`, sorted by the
    "Disk N" in the name), the drive gets the image itself: the only one, or the one picked. The ini
    then names the image under `.unzipped`. A drive holding one disk of a set gets a "Change disk"
    row first, with the next disk selected. A zip named in the ini by hand mounts its first disk
    (`pczip_floppy` in `floppy_source`). Zips without images are still folder floppies.
    Names over 64 bytes (zip folders and entries) are shortened on unpacking to their first 20
    bytes, `~hash~` and their last 36 (`shortName`), which keeps "(Disk 2) [!].ima". The full
    paths went past Windows' 260 characters and the 256-byte ini lines. Old unpacked folders with
    longer names are orphaned. Tested on the desktop and `build-lowmem` with `zimg.ini` (B: = Police
    Quest, 5 disks), Jones in the Fast Lane (1.2 MB) and Space Quest II (360 KB). Never tested on
    the device.
  - The CD goes through `ide_change_cd`. Every machine has an empty secondary-master CD.
  - C: and D: rewrite the ini and restart the machine (`stopMachine` shows "Saving D:").
    A live D: swap was tried on 2026-10-19 and dropped, because it failed on the device. DOS has no
    media change for a fixed disk: it keeps the BPB from boot, and SMARTDRV serves the old
    sectors (on the desktop it only worked after `SMARTDRV /R`). The user chose the restart.
  - DOS sees floppy swaps; this was tested by copying from B: before and after a swap.
    Emulink itself has no change line.
- **Snapshots (`pc_snapshot.cpp`, 2026-11-03, the user asked):**
  - In-game Save/Load snapshot rows, and a Snapshots row on the startup's drives screen
    (`MACHINE_SNAPSHOT`). The save asks for a name with an empty field (the user wanted names
    that say what the snapshot is, not "Snapshot N").
  - File: `/pc-386/snapshots/<ini base>/sN.snp` = "P386SNP1", the name, the startup's .ini text,
    the disks with their generations, then `t386_save_state` (`snap_run` in pc_glue.c: "T386", RAM
    and VRAM sizes, the guest clock, `pc_snap` (THIRD_PARTY.md), RAM in 1 KB pages, VRAM).
  - Restoring writes the snapshot's ini over the startup's (keeping `[pico] name`), starts the
    machine and loads the state (`pcsnap_set_pending`, then `pc_loadRom`). The in-game save and
    load both run in `pc_runFrame` after the menu has closed (the RP2350's menu parks VRAM).
  - `disks.gen` counts each disk's writes: `t386_fwrite` bumps a disk on its first write after a
    start or a snapshot, and so do a dirty folder commit and a zip unpacked again. A snapshot
    whose disks moved on gets a warning in the list. A dirty folder disk refuses the save.
  - Desktop: `PC_SNAP=path.snp` with `PC_INI` restores at start. A `PC_KEYS` run now ignores real
    keys: the SDL window takes the focus, and typing meant for another window reached the guest
    (stray "da"/"rama" at the prompt looked like a snapshot bug).
  - Tested on the desktop, `build-lowmem` and the device (2026-11-03): save at the FreeDOS prompt,
    `cls`, Load snapshot brought the boot screen back in about 1 s, and `ver` worked.
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

**RP2350.** Use the VS Code task **RP2350: Build** / **RP2350: Build & Upload** (COM7). They use
arduino-pico 6.1.1 from the default arduino-cli data dir, with `arch=arm,os=freertos,freq=300`, and
write `build-rp2350/pico386.ino.uf2`. The board flag and the `include/` path must reach both the C
and the C++ flags.

**PicoMultiLoader app** (2026-10-27). `tools/pml-app/` holds `app.ini`, `icon.bmp` and
`make_icon.py`, which draws the icon (32x32, 4-bit, pixel values are the loader's palette roles;
edit its grid and rerun it). The task **PicoMultiLoader: Package**, or
`tools/pml-app/package.ps1 -Version x.y.z`, builds both firmwares and writes
`build-pml/pico-386/` and a zip. The version goes into app.ini and the RP2350's binary_info
(`binfo_rp2350.c`, `-DPICO386_VERSION`, emu8's block). The P4 .bin's descriptor version comes
from the precompiled core and can't be set.
The header lands at 0x10003124, behind arduino-pico's 12 KB OTA stub, and PicoMultiLoader's
`read_binary_info` (rp2xxx/src/uf2_image.c) scans only 0x500 bytes from the image start, so its
details screen doesn't see it yet. emu8 has the same problem. The fix belongs in PicoMultiLoader:
also scan from +0x3000. The SD folder is `pico-386`, never `pc-386`, which is
the data folder (FAT ignores case). `app.ini` must stay under 512 bytes, ASCII with no BOM.

**Desktop.** MSYS2 MinGW32. The login shell starts in `$HOME`, so `cd` inside the `-lc`:

```sh
MSYSTEM=MINGW32 /c/msys64/usr/bin/bash.exe -lc "cd /c/Users/lucia/repos/pico-386 && make -C build -j8"
```

`make -C build-lowmem` is the same with `-DEMU_LOWMEM=ON`, the RP2350's memory model. Run it from
the repo root, because `PC_INI` paths are relative to the cwd.

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

Test data in `sdcard/`. It is gitignored except what may be shared (2026-11-02, the user asked):
`pc-386/roms/`, `pc-386/disks/` (FreeDOS floppies, `freedos-hdd-32mb.img`, empty
images from `tools/make_disk.py`, the tests) and the test .ini files. Never commit MS-DOS,
Windows or games (DOSHDD, Win31Disk, games/). GitHub refuses files over 100 MB.

- `sdcard/pc-386/roms/`: `bios.bin` and `vgabios.bin` (2026-11-02; it was `/roms/pc`). That folder is the
  default; `pc_core`'s `biosDir()` falls back to it and to `/roms/pc`, which v0.1.0 saved in the cfg. The user can change it in
  Settings > BIOS folder (`biosdir` in pico386.cfg). `t386_start` takes it and ignores the ini's bios keys.
- everything else in `sdcard/pc-386/`: the .ini files and the disk images (one folder for the emulator, the user's choice)
- `fdboot.img`, the FreeDOS 1.3 boot floppy (`disks/freedos/x86BOOT.img` in git)
- boot-sector tests in `disks/tests/`, each with a matching `.ini` in `sdcard/pc-386/`:
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
