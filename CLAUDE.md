# pico-386: notes for Claude

A 386 PC emulator (tiny386) for the **ClockworkPi PicoCalc** fitted with a **Waveshare
ESP32-P4-WIFI6**, plus an SDL2 desktop build of the same code for debugging. The user (GitHub:
hexadevti) also owns **pico-amiga** (`C:\Users\lucia\repos\pico-amiga`), which this project was
started from on 2026-10-18, and **emu16** and **emu8**.

- License: GPL-2.0-or-later for our files, the same as pico-amiga.
  - Never copy emu16's `src/md/`, `src/sound/` or `src/cpu/`: they are AGPL or non-commercial.
  - Record every change to tiny386 in `THIRD_PARTY.md`.
- The plan the user approved is in `~/.claude/plans/create-a-pico-386-serene-panda.md`. The user
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
  - **`BLASTER` (2026-11-10):** the SB16 is A220 I5 D1 H5 (`sb16_new` in pc.c). FastDoom (Apogee
    Sound System) with SB sound effects and no `BLASTER` set prints "Error setup sound device: -1"
    and drops to DOS. The text is lost on the device, since the screen is cleared, so it looks like
    a hang at "Startup Sound". `SET BLASTER=A220 I5 D1 H5 P330 T6` is now in
    `freedos-hdd-32mb.img`'s FDAUTO.BAT (it had no T6, which also worked) and in the AUTOEXEC.BAT of
    the local MS-DOS images and the device's DOSHDD. With it, FastDoom has sound on the desktop and the device.
  - **SimCity Classic (2026-11-13):** the startup `simcity.ini` ("SimCity") has C: = MS-DOS 5.0
    (`msdos5-hdd-32mb.img`) and D: = `games/simcity.zip` (`/pico-386/simcity.zip` on the RP2350's
    card, which has no `games`). DOS-128 left 23 KB too little conventional memory. The game's
    AIL drivers ignore `BLASTER` and default to IRQ 7, so the digital driver failed its check
    ("Sound hardware not found") until `CLASSIC.CFG` named the port, IRQ and DMA (lines after each
    .ADV: 220, 5, 1, *). Runs on the RP2350 with music; the OPL took about 35% (690 ms of mix
    per 2 s) and, with the compact tables (`FMOPL_COMPACT`, 2026-11-14, THIRD_PARTY.md), about
    16% (335 ms). The profiler shows it as `opl_out`/`OPL_CALC_CH`, inlined into
    `YM3812UpdateOne` (4.3 KB, in flash; in SRAM it would cost 4 KB of the 10 KB of heap).
    Launch: home menu SimCity, Start, then `d:`, `cd simcity`, `simcity`.
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
    - **Fast paths (2026-11-03):** `translate`, the loads and stores and `fetch8` are inline in
      i386.c, with upstream's code as the slow path (THIRD_PARTY.md). CheckIt: 10.95x PC-XT
      (3767 Dhrystones) under MS-DOS 6.22, 8.68x under FreeDOS with JEMMEX. What is left is flat:
      instruction fetch and decode (`fetch8pf`, about 10%), lazy flags (about 6%), the dispatch's
      indirect jump, and PSRAM latency on guest RAM. Each is worth 2-3% at most.
    - **FastDoom (2026-11-10):** `DOOM.BAT` (FDOOM13H, low detail, no music) shows 4-5 fps in its
      corner counter on the device at 1.7-1.8 MIPS (33 on the desktop). About 75% of core 1 is the
      interpreter. A silent OPL took about 7% until `fmopl.c` skipped it (THIRD_PARTY.md). A full
      `-timedemo demo1` takes over 15 minutes on the device, so compare MIPS and `prof` instead.
    - **P4 pass (2026-11-14):**
      - The frontend skips drawing a graphics picture the guest has not written since the last one
        (`t386_vga_gen`, THIRD_PARTY.md): FastDoom went from 1.8 to 2.1-2.2 MIPS. The RP2350
        shares the code (on `build-lowmem`, m12/m13 draw correctly), but it is not yet tested
        on the device.
      - **The skip is off on the P4 (2026-11-15).** With it, Prince's intro music crackled on the
        device (v0.2.2 clean, v0.3.0 crackled, v0.3.0 without the skip clean; the desktop always
        clean). The cause is not known yet: maybe the panel's irregular SPI bursts reaching the PDM
        output, or core 0's timing. Read `g`/`s`/gap on the FPS line in Prince's intro with the skip
        back on before guessing. The v0.3.0 release's P4 files were replaced with this build.
      - Tried and dropped:
        - Inlining `fetch16`/`fetch32` gave 1.8 MIPS (no change). Forcing `fetch8pf` inline was
          worse, at 1.7.
        - The L2 cache can't grow. It is the first 256 KB of L2MEM, and our IRAM code sits after it.
        - Drawing 1 frame in 4 gave the same MIPS, so core 0's drawing doesn't evict the cache.
      - Readings vary between boots, so compare builds back to back with the same keys.
    - **Core counters (2026-11-15):** the P4's FPS line ends with `ipc0.60 131i 215c` (`pc_core.cpp`,
      the `cycle`/`instret` CSRs read around `t386_run_until`): IPC, then host instructions and
      cycles per guest instruction. FastDoom's demo: `1.75 MIPS ipc0.60 131i 215c`; with the
      unchanged-frame skip back on, `2.1 MIPS ipc0.71 130i 182c`. The same instruction count with
      fewer cycles means core 0's redraw (about 400 KB through the PSRAM per frame) stalls core 1's
      memory accesses. `prof` then: fetch and decode (`fetch8pf`, `fetch8`, `fetch32`, `peek*`,
      `modsib*`, part of `pload8`) about 35% of core 1.
      Each part of the drawing, cut out at run time (a TEMP switch, since removed), in cycles per guest
      instruction during the demo: all of it 213 (1.8 MIPS); no PSRAM shadow for shots 197 (1.9); only
      `vga_refresh` into the 800x600 fb 186 (2.05); no drawing at all 177 (2.2), the ceiling (+20%).
      Rendering without the push gave 205: the parts overlap, so their costs do not add up. The
      shadow is now written only when a shot asks (`s_shotWant`, display_p4.cpp).
    - **Flat fast path (2026-11-15, branch `exp-p4-draw`, THIRD_PARTY.md):** `i386_flat.h` runs flat
      32-bit code (FastDoom's DOS/32A: bases 0, paging off) with the registers in a local array.
      FastDoom: 1.75-2.1 to **3.7-4.1 MIPS**, about 100 cycles per guest instruction, 4-5 to about
      10 fps; batches average about 177 instructions. Left as misses: `REP MOVS` (f3), ADC/SBB (83
      /2 /3), port I/O and segment loads.
      - Any change to it must pass the desktop lockstep: configure a build dir with
        `-DT386_FLATCHECK -DT386_OPSTATS` (it was `build-fchk`), run `doomt8.ini` with
        `PC_KEYS='w20000,d,:,enter,d,o,o,m,enter,w60000,quit'`, and read `flatcheck N checked, 0 bad`
        and the `flatstat` misses on stderr. TEMP: `PC_NOFLAT=1` turns it off on the desktop.
      - The desktop's `doomt8.ini` (DOS-128) has no `BLASTER`: type
        `set BLASTER=A220 I5 D1 H5 T6` before `doom`, or it quits with "Error setup sound device".
        The desktop's unzipped doom folder has an `fdoom.cfg` with SB effects. With `PC_WAV`, sound
        is the same with and without the fast path, also at `PC_MIPS=4`.
      - On the P4, start Doom from the home menu with keys `d,w300,enter,...`: "d" jumps to the
      - **Silent on the device (2026-11-15):** the flat path's IRAM left too little internal RAM, and
        the I2S channel, made at the PC's first sound, failed (`PDM TX init failed (ESP_ERR_NO_MEM)`
        in the log; `g` stays 0, as nothing is queued). `p4AudioInit` now makes it in `hal_init`,
        before anything else. `t386_reserve_big` probes its 2 MB margin in 1 MB pieces, not as one
        block with the RAM, and the P4 logs `psram: no N KB block (free, largest)` on a refusal.
        "doom" startup. `down` is wrong, since the menu remembers the last startup ("PC").
    - **Profiler:** `python -I tools/remote.py --p4 prof 8` samples core 1 at 2 kHz (`prof_p4.cpp`,
      `@prof`/`@profdump`) and prints hot functions and lines through addr2line on the build's .elf.
      Names are the innermost inlined function, so `pload8` or `fetch8pf` usually means code inside
      `cpu_exec1`.
    - **FreeDOS's boot menu on the device:** typed digits are lost. It shows about 6.5 s after a
      Ctrl+Alt+Del; `w6300,shot` freezes it (a shot pauses the guest's clock), then `down,enter`.
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
        ESC O S, if Wi-Fi autostart is off), uploads `build-p4/pico386.ino.bin` to
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
      (mode 12h needs 150 KB) and `pm_map` went from pointers to 2-byte frame numbers.
      The other 96 KB (2026-11-13) are in PSRAM below the park area (`pm_vram_hi`, one 256-byte
      line cached), for planar reads and writes only. Windows 3.0's VGA driver keeps bitmaps there
      and was garbled without it (Win 3.1's isn't). The screen shows only the first 160 KB, so
      page flips above 40 KB per plane still show the wrong page. Fixed on `build-lowmem` with
      `ms5.ini` (MS-DOS 5.0 + Windows 3.0 on `msdos5-hdd-32mb.img`). Works on the device.
    - **Branch `fast-cpu` (2026-11-13/14):** CheckIt **6.72x** (2310 Dhrystones), from the inline
      ifetch through the code's frame, the real-mode `translate` exit and a hot/cold split of
      `cpu_exec1` (`i386_exec.h` compiled twice; `I386_COLD` lists the opcodes left in flash).
      All in THIRD_PARTY.md. Count opcodes with a desktop build configured with
      `-DT386_OPSTATS` (it was `build-opstats`) before moving any to the cold list.
      The split freed 23 KB of SRAM, and the app stack went from 24 to 16 KB (it uses about 3 KB),
      which pay for 96 frames instead of 64 (see the pager below). Static RAM 433 KB; with a PC
      running 14.6 KB of heap and 13 KB of app stack are free.
    - **Windows 3.1 is bound by page faults, not the interpreter.** On the desktop at a fixed
      3 MIPS (`PC_MIPS=3 PC_MIPS_AT=0`, `opst.ini`), a minute of Windows faulted 5.4M times with
      64 frames and about 0.3-0.45M with 96; its working set fits in 96 and not in 92. Random
      replacement beat the clock at 80 frames and lost badly at 64 and 96, so the clock stays.
      On the device idle Program Manager now runs at 0.29 MIPS (it was about 0.075), still with
      about 19k faults every 2 s. The user asked to leave the pager there (2026-11-14).
    - **FastDoom (2026-11-14, startup "doom", `C:\DOOM\DOOM.BAT` = FDOOM13H -low):** about 1 fps
      at 1.0-1.1 MIPS (DOOMF, potato, shows 2-4). The user wants it faster without shrinking the
      view or the detail. Done so far:
      - In mode 13h the 96 KB of VRAM above 64 KB are lent to the pager (`pm_vram_lend`,
        THIRD_PARTY.md), so 192 frames: faults per 2 s from about 12.3k to 6k. Their contents wait
        in PSRAM below `pm_vram_hi`'s area (`VLEND_BYTES`), and come back on a mode change.
      - `frameOf` checks a resident-page bitmap and a hint before searching (it was 2%).
      - A dirty victim goes out behind the new page's read (`hal_psram_swap`): the DMA streams a
        prebuilt list of burst headers and data while the guest runs, so `rawWrite` (about 5%)
        is gone. Left: about 16% waiting for page reads, about 55% the interpreter.
      - Overclock (`rp2350.ini` `mhz` up to 400): `hal_init` raises the flash's QMI divider to keep
        it at 100 MHz or less (div 4 at 360 and 400) with the other core idled. The device's ini
        now has `mhz = 400`, `vreg = 1.30`: FastDoom about 1.4 MIPS (1.28 at 360/1.25, 1.0-1.1 at
        300), 6 min of demos without a fault. The PSRAM at 60 MHz (360/3) passed the boot test but
        corrupted guest RAM (a GPF in FDOOM13H), so `calibrate` never takes a divider above
        50 MHz: 45 MHz at 360, 50 at 400.
      - **Doom quitting to `C:\DOOM>` after 1-4 min of demos was not the overclock:** it was out of
        memory ("Failed on allocation of 35120 bytes", shown on the desktop; the device's screen
        lost the text). The 4 MB guest (`PM_MAX_RAM`) left too little after DOS-128's
        `SMARTDRV.EXE`, which takes a 1 MB XMS cache by default. The device's DOS-128.img now has
        `C:\WINDOWS\SMARTDRV 256` (2026-11-14, typed in through `remote.py`; the original is
        `C:\AUTOEXEC.OLD`). The desktop's copy of DOS-128.img still has the old line.
      - Profile at 360 (2026-11-14): PSRAM page reads about 21% (`dma_channel_is_busy`, `rawRead`),
        the interpreter about 60% (`cpu_exec1` 20%, `fetch8pf` 7.5%, `translate` 6%, `fetch8` 5%,
        `modsib*` 7%, the `__GE_helper`s 5%). 2 KB and 512 B pages were both worse than 1 KB
        (pc_pagemem.h).
    - Ideas not tried yet: hot non-i386 code in SRAM (`pc_step`, the PIC/PIT, port I/O).
  - `PICO386_LOWMEM` (board.h): guest RAM lives in the mainboard's 8 MB PSRAM, which hangs off
    a PIO and is not memory-mapped. It is paged through 1 KB SRAM frames (`pc_pagemem.c`).
    - `hal_arena` holds VRAM (160 KB), then 96 KB of frames (64 until 2026-11-14). The menu canvas (200 KB) borrows the
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
        through the loader's web API to `/pico-386/pico386.ino.uf2` (the old one becomes
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
      why the frames stayed at 64 KB until the interpreter's split freed SRAM.
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
  - **`rp2350.ini` (2026-11-10, v0.2.1, the user asked):** a user's AliExpress RP2350 froze within
    seconds of boot on v0.2.0. arduino-pico leaves the RP2350's core at 1.10 V at 300 MHz (it
    raises the voltage only on the RP2040), and the PSRAM calibration takes the fastest divider
    that passes one test. The optional `/pico-386/rp2350.ini` (`mhz`, `vreg`, `psram_div`; the
    shipped one has every line commented out) is read in `hal_init` (`applyTune`) before the
    PSRAM, the panel and the SD's SPI are set up. It re-begins UART0, resets the keyboard's I2C
    clock and reloads SysTick on both cores, because FreeRTOS loaded it from F_CPU. With no file,
    nothing changes. The log shows `rp2350.ini: ...` and the MHz and mV in use. Compiled, but not
    yet run on a device: the PicoCalc was offline. Another user reported freezes from LyraCalc's
    keyboard firmware, which went away with ClockworkPi's official 1.7.
  - Test it on the desktop with the `build-lowmem` CMake dir (`-DEMU_LOWMEM=ON`), which simulates
    the PSRAM. Prince of Persia and MS-DOS 6 run there.
  - `psram_rp2350.cpp` drives the PSRAM from pio1 (CS 20, SCK 21, SIO0-3 on GP2-5). Since
    2026-11-13 (branch `fast-cpu`) it enters QPI (0x35; 0xEB reads, 0x38 writes) and falls back to
    plain SPI if quad fails its self-test; `psram_quad = 0` in rp2350.ini forces SPI. It calibrates
    itself at boot over clock dividers and read-delay clocks, and logs
    `psram: ... KB, quad|SPI, SCK ... kHz, N wait clocks, N-byte bursts`. Ask for that line first.
    The user's device: quad at 50 MHz (75 MHz fails), 128-byte bursts. Paging went from about 22%
    of the time to about 6% in Prince's intro.
  - **Profiler (2026-11-13):** `python -I tools/remote.py --hz 1000 prof 8` samples core 0
    (`prof_rp2350.cpp`, `@prof`/`@profdump`, rounds of 1024); `--raw f.txt` saves, `--from f.txt`
    reports again. Prince's intro is mostly a halted guest: `pit_ticks`, `i8254_update_irq`,
    `pc_step`, `cpui386_step` and `get_uticks` there are the idle loop, not cost. After the PIT and
    quad fixes the real work is the OPL (about 25%), the interpreter (about 12%) and paging (6%).
  - Cores: arduino-pico's FreeRTOS SMP runs setup/loop on core 0 with a 4 KB stack. So
    `pico386.ino` starts an "app" task (16 KB static stack) on core 0, and the draw job and
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
- **Zoom 1:1 (2026-11-18, branch `zoom-1to1`, the user asked):** Ctrl+Caps Lock (`pckbd_zoom`,
  `pckbd_zoom_count`; `pc_core` shows a note) shows a graphics picture over 320x310 at 1:1 in
  a 320x310 window (`kZoomH`) centred on the pointer and stopped at the picture's edges (a window
  that went on past them, black there, was rejected by the user). The pointer's place is
  estimated by summing the deltas sent (`pckbd_pointer`, clamped to the picture, recentred when
  its size changes). While zoomed, ARROWS acts as MOUSE_ON at the usual speed (a 2 px cap was too
  slow), but each frame's step is owed (`s_oweX/Y`) and `pckbd_mouse_poll` (from `audioHook`,
  during the frame) sends it as 2 px packets, one at a time (queue empty, `s_mouseInstr / 2`
  since the last): bigger packets were accelerated by Windows 3.x and the estimate fell behind.
  Queuing the 2 px packets all at once lost the pointer on the device. On the desktop at
  `PC_MIPS=1.5` the estimate matches the real pointer, also after an edge.
  Windows 95 accelerates the pointer itself (short moves about 0.45x, long ones about 1.5x at
  1.5 MIPS), so the estimate drifted and the pointer left the window on the P4 (2026-11-19). Now,
  P4/desktop only, `syncPointer` (pc_video.cpp) looks for the standard arrow (black outline, white
  fill, `isArrow`) in the last picture within 64 px of the estimate (the whole picture at most
  twice a second) while the pointer moves, and `pckbd_pointer_seen` restarts the estimate there.
  The window centres on the arrow found (`s_seenX/Y`), not on the estimate: the estimate runs up
  to 24 px ahead of what the guest has drawn (moves not taken yet), and centring on it made the
  window jump ahead and back every frame on the P4. Other shapes, or an arrow cut by the picture's
  edge, are not found, and the estimate goes on alone. Desktop Win95 at `PC_MIPS=1.5`: the
  pointer stays in the window through long and short moves and at every edge; on the P4 (2026-11-19)
  the arrow is found in nearly every frame while it moves, and shots after each move show it centred. Held 3 s, straight and diagonal, the step guess found
  it in nearly every frame; only an arrow cut by the bottom edge is missed.
  `pc_video`'s `Geo` holds the layout; P4/desktop copy rows (`renderZoom`, dirty rows map by
  `y - vy`), LOWMEM passes the window to `vga.c`
  (`t386_set_line_window`, THIRD_PARTY.md) so it decodes only that. Tested on the desktop and
  `build-lowmem` with Windows 3.1. On the P4 (2026-11-18) Ctrl+Caps arrives and MIPS look the
  same with it on.
- **Network (2026-11-18, the user asked; P4 and desktop, not the RP2350):** Settings > Network
  (`netmenu.cpp`) turns on an NE2000 at 300h, IRQ 9 (`ne2000.c`, THIRD_PARTY.md) for machines
  started after. `pc_net.cpp` is a slirp-like NAT: the guest is 10.0.2.15 by DHCP, gateway 10.0.2.2,
  DNS 10.0.2.3. TCP ends in pc_net, one host socket per connection (SYN-ACK only once the host's
  connect succeeds); UDP goes out per guest port; DNS runs on a resolver thread. The web proxy
  (Protoweb by default, its line-speed ports, Protoweb 2, or a custom host:port) takes the guest's
  port 80, rewrites requests to absolute URIs, and answers DNS with fake IPs (10.99.x.x) so names
  reach the proxy unresolved. On the P4 the Wi-Fi is Arduino's WiFi.h (`net_p4.cpp`), started in
  `app_setup` (`netApply`) while internal RAM is free; with no network chosen it joins the loader's
  (`/picomultiloader.cfg`, see below). Settings: `net`, `wifi_ssid`, `wifi_pass`, `proxy`,
  `proxy_speed`, `proxy_custom` in pico386.cfg.
  - Desktop test: `nett.ini` (C: = DOSHDD.img, D: = the local folder `nettest/` with mTCP and the
    Crynwr NE2000 packet driver). `NET.BAT` loads `NE2000 0x60 9 0x300`, runs DHCP and pings the
    gateway; `W url file` is `HTGET -o`. With proxy off, example.com and an 87 KB www.google.com
    came through intact. Run the exe with the sandbox off, or nothing gets out.
  - Protoweb answers with bare LF line ends, which htget can't parse ("Timeout reading from
    socket") although every byte arrives; test it with a real browser instead.
  - **P4 Wi-Fi (2026-11-18):**
    - Opening Settings > Network > Wi-Fi rebooted the device. ESP-Hosted 2.12.8's SDIO transport
      wants 40 KB and 47 KB of DMA RAM and asserts without them (`transport_drv_common_mempool_create`,
      `sdio_drv.c:255`). On the menu only about 47 KB of internal RAM is free, in pieces of 8 KB at
      most: the boot took 149 KB to 49 KB, 70 of it the panel's 4 x 16 KB SPI bounce buffers.
    - Now `radioOn` (net_p4.cpp) first moves the panel's bounce buffers to PSRAM
      (`p4DisplayFreeInternal`: `SPI_TRANS_DMA_USE_PSRAM` plus an `esp_cache_msync` per send). It
      also points `g_hosted_osi_funcs._h_malloc_align` at a PSRAM-first allocator: the newer
      esp-hosted has `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` for this. It refuses (no assert)
      under 72 KB free.
      - **Block (2026-11-19, the user asked):** a refusal sets `HAL_NET_NO_RAM` (`hal_net_state`
        says "no RAM: N KB free, 72 needed" until `hal_net_end`). `netApply` then returns false and
        gives pc_net `on = false`, so machines get no NE2000. Turning the card on in the Network
        menu shows the reason and leaves it Off; with `net=1` at boot, Settings and the card row
        read "No RAM". Desktop test: `PC_NET_NORAM=1` fakes the refusal.
      - Either change alone still asserted, or looped on `sdmmc_allocate_aligned_buf`.
      - The radio still takes about 56 KB of internal RAM (106 to 49 KB).
    - With 49 KB left, `hal_alloc_fast` (48 KB reserve) puts the PC's tables in PSRAM while the
      network card is on. Measured on the device (2026-11-19), Win95 opening My Computer, Control
      Panel and Multimedia three times (each round starts with a Mouse-mode click on the empty
      desktop, since nothing has focus after a boot): card off, rebooted, 1.60 MIPS and 243.5c
      (medians of 50 busy seconds); card on, 1.60 MIPS and 246c. No cost worth counting.
    - The loader's network is PicoMultiLoader's: `/picomultiloader.cfg` on the card (`wifi_ssid=`,
      `wifi_pass=`). The NVS is empty; esp32loader's NVS "loader" is only the fallback.
    - Scan, join and joining at boot work on the device (192.168.0.100). MS-DOS 5.0 boots with it on.
    - One cold join in two sat with no event at all. `hal_net_state` begins again after 15 s,
      but only the Network menu calls it (`Screen::refreshMs` = 1 s redraws Status and Traffic).
    - "Response not received for Req_GetCoprocessorFwVersion": the C6 has old firmware; harmless.
    - Windows 95 browses through Protoweb on the device (2026-11-18, the user tested it).
    - **Opening a website reset the device (2026-11-19):** `H_SDIO_DRV: sdio_write_task: Failed to
      send data: 258`, then `Unrecoverable host sdio state` and `SW_CPU_RESET`. The SDMMC driver
      (IDF 5.5) refuses a block-mode buffer whose address or size is not cache-line aligned (128
      in PSRAM, 64 internal); byte mode (under 512) bounces by itself. ESP-Hosted's TX frames are
      not aligned that way. `net_p4.cpp` now hooks `_h_sdio_write_block` and copies a misaligned
      write of 512 or more into an aligned bounce buffer (and retries a refused one through it,
      logging `wifi: SDIO write ... refused`). Win95's IE then loaded home.microsoft.com with its
      pictures over about 20 connections, with no error.
      - **Trap:** `g_hosted_osi_funcs`' layout depends on `H_USE_MEMPOOL` and `H_TRANSPORT_IN_USE`,
        set only in `port/esp/freertos/include/port_esp_hosted_host_config.h`. Without that
        include first, the table is 3 slots off, and the "write" hook landed on `_h_sdio_read_reg`
        (it broke the init). `hookHosted` checks that the slot holds `hosted_sdio_write_block`
        before hooking.
  - **Windows 95 online (2026-11-18, desktop, then the P4):** `win95net.ini` (local; C: = `win95net.img`, a
    copy of win95.img, CD = win95.iso, `net=1` in pico386.cfg). Control Panel > Network: Add >
    Adapter > Novell/Anthem > NE2000 Compatible, Resources typed in (IRQ `9`, I/O `300 - 31F`:
    the arrows do nothing, a partial I/O snaps to 220h), Add > Protocol > Microsoft > TCP/IP
    (DHCP is the default); the files come off the CD by themselves. Remove the extra clients,
    IPX and NetBEUI. After the restart, The Internet's wizard: Manual, LAN, no proxy. IE 3 then
    showed www.yahoo.com through Protoweb. Keys: `alt+n` focuses the component list, `ctrl+o` is
    IE's Open.
- **`pc_keymap.cpp`:**
  - PicoCalc characters are mapped to US-layout Linux keycodes, with Shift faked as needed.
  - It handles typematic repeat and the keyboard mouse.
  - The keyboard mouse has three modes (2026-10-19): `MOUSE_ARROWS`, `MOUSE_ON` (arrows move the
    pointer only) and `MOUSE_BOTH` (pointer and arrow keys). Both Shifts held together, or Ctrl+Tab,
    cycles them; the first Shift has already reached the guest by then.
    Pointer packets are paced by guest instructions (2026-11-13, `s_mouseInstr`, THIRD_PARTY.md):
    Windows 3.1 on the RP2350 runs at about 0.075 MIPS (7000 page faults/2 s) and drifted on for
    seconds after an arrow was let go. Works on the device: it stops on release. Then it jumped
    (40k instructions, movement added up); now 8.5k and at most 4 px a packet, never added up (7.5k drifts).
    `python -I tools/remote.py mpace "8500 4"` retunes it live on the RP2350. The RP2350
    heartbeat logs `diag: mouse` (bytes unread, packets sent). The two button keys come
    from `kButtonKeys` (`[pico] mouse_left`/`mouse_right`, default alt/rshift); outside ARROWS
    they are only buttons. `pc_core` saves the mode and keys to the ini and shows a 2 s note
    in the bottom strip (`showNote`). Test with `PC_KEYS=...,shift+rshift,...`.
  - Shift as a mouse button (2026-10-19, from the device's serial log): the STM32 sends Shift+Up/
    Down as PgUp/PgDn (0xD6/0xD7), and sends nothing at all for Shift+Left/Right. While a Shift
    that is a button is held (or any Shift, in Mouse mode), `shiftedArrowOf` turns PgUp/PgDn back
    into pointer arrows, so vertical drags work on the device. Sideways drags with a Shift button
    can't work. The desktop never sends them that way.
  - Shift with the arrows (2026-11-08, the user asked): the STM32 sends Shift+Up/Down as PgUp/PgDn
    (0xD6/0xD7) and nothing at all for Shift+Left/Right (seen on the device's log). Prince of
    Persia picks things up with Shift+Down and steps carefully with Shift+Left/Right. The in-game
    row "Shift for arrows" (`[pico] shift_key`, an ini name such as `ctrl` or a character) waits
    for a key (Esc = off; arrows, PgUp/PgDn and Caps Lock are refused). That key is then the
    guest's Left Shift and nothing else (`s_shiftKey`, filed in `s_down` in lower case), so arrows
    pressed with it are Shift+arrows. With a key set, PgUp/PgDn pressed with a real Shift also go
    as Shift+Up/Down. It replaced two options, `shift_arrows` (PgUp/PgDn) and `ctrl_arrows`; a
    startup with `ctrl_arrows = 1` and no `shift_key` gets Ctrl. Restoring a snapshot keeps the
    startup's `name`, `mouse`, `mouse_left`, `mouse_right` and `shift_key` (`pcsnap_write_ini`'s
    `kKeep`). Tested on the desktop with `popc.ini` (Ctrl+Right steps about 25 px). The
    `ctrl_arrows` version worked on the RP2350; the merged option is not yet tested on a device.
    It failed on the device until the i8042 fix for stuck mouse data (THIRD_PARTY.md): Prince
    reads Shift from the BIOS's flags, and SeaBIOS dropped every key ("found mouse data") after
    Mouse mode had queued packets that Prince never reads. Works on the device (2026-11-06).
    The in-game menu takes up to 16 core rows (`app.cpp`).
  - Dead keyboard after Ctrl+Alt+Del (2026-11-13, RP2350, DOS-128 "win 3.1"): the i8042's 1 ms
    hold made SeaBIOS's flush read the same stale byte 16 times, so it never set the keyboard up.
    The hold now starts only while IRQ 1 is in service (THIRD_PARTY.md). Works on the device. The
    RP2350 heartbeat's `diag: kbd` line shows the i8042 queues, mode and the PICs; mode 43h is
    healthy, 12h or 73h at a prompt means SeaBIOS gave up on the keyboard.
    Repro on the desktop: `popfd.ini` (FreeDOS with CTMOUSE, Prince on A:), Ctrl+Tab, arrows,
    back to Arrows, then count "mouse data" in the log.
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
    `/pico-386` with `[pico] name`. `pc.ini` comes first and is "PC" when it has no name. Other
    named .ini files follow, sorted by name. Unnamed test .ini files are not listed.
    - "Create new startup" asks for a name (`ui::editText`) and writes the first free
      `startN.ini`, which mounts only C:.
    - A startup's row opens its drives screen (`pcdisk_machine_menu`, which returns `MACHINE_*`).
      The screen has Start, the drives, "Boot from" (`[pico] boot = cd`), Rename, and Delete
      (every startup but pc.ini). Delete removes only the .ini.
    - `startup=` in pico386.cfg remembers the last startup's file for the home menu's cursor.
  - It writes `/pico-386/pc.ini` when it is missing (C: = c.img, else dos.img or win.img). It asks
    for `T386_MEM_MAX_MB` (32 MB) of RAM; `t386_start` steps down 4 MB at a time until guest
    RAM + VRAM + 2 MB fits in PSRAM.
  - Everything persists in the startup's .ini (the user asked, 2026-10-19).
    `[pico] boot = cd` becomes SeaBIOS's CMOS boot order in `t386_start` (3Dh/38h); without
    it SeaBIOS tries C: before the CD. `pc_core` writes `[pico] mouse` (0/1/2) and the
    button keys when they change. `pcini_get_in`/`pcini_set_in` edit any section.
  - Test: `sdcard/pico-386/disks/tests/m13.iso` is an El Torito CD (m13.img, floppy emulation); `cdt.ini` boots
    it ahead of `dos.img`.
  - The CD in DOS needs a guest driver. `sdcard/pico-386/cdrv.img` (`disks/cdrom-driver.img` in git) is a floppy holding FreeDOS's
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
    clusters go to `/pico-386/.pico386-dN.tmp`.
  - `pfolder_close_all()` (from `t386_release_all`, and `atexit` on the desktop) parses the final
    tree and matches nodes by first cluster, then by short name in the same parent. It stages
    new and changed files in `<folder>/.pico386-commit`, then deletes, renames and moves them in.
  - If sector 0 or 63 was rewritten (FDISK/FORMAT), nothing is applied.
  - Tested on the desktop with FreeDOS: dir, type, echo >, copy, ren, md, del, overwrite, reboot.
- **`pc_zip.cpp`, zips as D: (2026-10-19):**
  - The ini keeps the zip itself (`hdb = pico-386/games/x.zip`). `t386_start` calls `pczip_folder`,
    which unpacks the zip once to `/pico-386/.unzipped/<name>/` and hands that folder to the folder disk.
    Guest writes go to that folder, not to the zip.
  - A `.pico386-zip` marker in the folder records the zip's path, size and mtime. If they match,
    the folder is reused. If not, it is emptied and unpacked again.
  - The inflater is our own (stored and deflate, CRC checked). It has no zlib dependency.
  - The D: chooser has "Choose a zip file". (The old "Boot a disk image" also booted a zip
    holding a single image; that went with it on 2026-10-19.)
  - Tested on the desktop: all the example zips in `sdcard/pico-386/games` unpack with matching
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
  - File: `/pico-386/snapshots/<hardware>/<ini base>/sN.snp` = "P386SNP1", the name, the startup's .ini text,
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
  - Each snapshot has `sN.bmp` beside it (2026-11-06, the user asked): the guest's screen, 80x60
    RGB565 top-down (`pcvid_thumb`, box-averaged from the picture's area). The P4 and the desktop
    read `hal_video_front()`; LOWMEM draws the screen again and catches the rows on their way to
    the panel (`thumbRows`). The Load list shows it left of the name (`ui::Screen::picture`,
    `rowH`), read straight into the canvas. Tested on the desktop (mode 13h) and `build-lowmem`
    (FreeDOS text). Not yet tested on the device.
  - Saving with snapshots there (2026-11-06, the user asked) first lists "New snapshot" and then
    the snapshots to overwrite (`pcsnap_pick_save`); the name field starts from the old name.
    An overwrite writes a new `sN` and only then deletes the old one, so it moves to the top.
  - "Snapshot from another version" on the P4 (2026-11-07): the snapshots had 24 MB of RAM, and
    the restart for the load got 20 MB, because the 27 MB block was freed with each machine and
    the PSRAM fragmented. Guest RAM and VRAM now come from a block kept for the whole session
    (`t386_reserve_big`, `bigmalloc` in pc_platform.cpp). A load asks for the snapshot's RAM
    (`pcsnap_ram`, `t386_limit_mem`) and otherwise says "needs N MB". A failed `snap_check`
    logs its offset and both values. Works on the device (2026-11-07): prince 1, prince 2 and p3
    loaded one after another in-game, each at 24 MB.
  - `<hardware>` (2026-11-08, the user asked) is `p4`, `rp2350`, `desktop` or `desktop-lowmem`
    (`kHardware`), so each build lists only snapshots it can load. Older snapshots in
    `snapshots/<ini base>/` are no longer listed; moving the folder under the right hardware brings
    them back. `disks.gen` stays shared. Tested on the desktop (save and list); works on the RP2350 (2026-11-08).
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
  --build-path build-p4 pico386
```

- The core is Arduino-ESP32 **3.3.10** in `~/.emu6502-p4` (emu8's version). It was missing on
  2026-11-07 and was installed again with `arduino-cli core install esp32:esp32@3.3.10
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
- Builds go to `build-p4` and `build-rp2350` in the repo (gitignored), never to the user folder
  (the user asked, 2026-11-08). The tasks, `package.ps1`, `deploy-p4.py` and `remote.py` use them.
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
also scan from +0x3000. The SD folder is `pico-386`, which is also
the data folder since 2026-11-03 (it was `/pc-386`; the user asked). The zip also carries every
file git tracks under `sdcard/pico-386/` (BIOS, free disks, tests), copied by `package.ps1`.
Old cards need their `/pc-386` moved and the .ini drive paths edited (README says how). `app.ini` must stay under 512 bytes, ASCII with no BOM.

**Web (2026-11-24, the user asked: "a web version for phones, with emu8's techniques").** `web/`:
- `python web/build.py` (emsdk in `~/emsdk`; objects in `build-web/`, output in `web/dist/`). It builds
  the desktop sources (`BOARD_DESKTOP` + `EMU_WEB`) with `web/src/hal_web.cpp` in place of the SDL HAL:
  pthreads + `PROXY_TO_PTHREAD`, 256 MB fixed memory, WASMFS with `/sdcard` on OPFS (see the card below). Guest RAM goes up to 64 MB
  there (`T386_MEM_MAX_MB` under `EMU_WEB`, 2026-11-24, the user asked; a phone ran about 50 MIPS);
  the page's seed `pc.ini` asks for 64M and raises an old `mem_size = 32M` in `pc.ini` once. FreeDOS's
  `mem` shows 65,536K in headless Chrome. `kHardware` is `web`, the home
  menu's Exit is "Restart" (reloads the page).
- **Network (2026-11-24, the user asked: "only Protoweb").** A page has no sockets, so on the web
  `pc_net.cpp` (EMU_WEB) sends only the proxy's port-80 connections out, each as a WebSocket to the
  Worker's `/api/tcp?host=&port=` (`hal_tun_*` in hal_web.cpp, `tunOpen` in app.js). The Worker
  (`relay.js`, `connect()` from `cloudflare:sockets`) takes only `TCP_ALLOW`: Protoweb's hosts and
  ports (403 otherwise), so it is no open proxy. Binary frames are the stream; the Worker sends
  text "open" once connected, the page sends "shut" for the guest's FIN, and the proxy's EOF
  closes the WebSocket. The Worker ignores "shut": on Cloudflare (not wrangler dev) a half-close, even
  with `allowHalfOpen`, left Protoweb answering nothing (0 bytes; htget sends a FIN after its request). Workerd hands binary messages to the Worker as a Blob (`arrayBuffer()`
  first). Other TCP is refused, UDP goes nowhere, DNS answers only with fake addresses. The
  Network menu has no Wi-Fi rows there, and the proxy is Protoweb or Protoweb 2 (Off/Custom read
  as Protoweb). Dev: `localStorage['pico386-tun'] = 'ws://127.0.0.1:8787'` sends the tunnels to
  `wrangler dev` while the page comes from serve.py. Tested in headless Chrome (FreeDOS, D: =
  `nettest/`, `echo PACKETINT 0x60 > mtcp.cfg`, NET.BAT): DHCP, ping 10.0.2.2, and htget of
  www.yahoo.com brought all 8315 bytes (htget then fails on Protoweb's bare LFs, as on the desktop).
  The same run on https://386.hexadevit.com (2026-11-24) also brought 8315 bytes.
- **pico-mac's shell ported (2026-11-25, the user asked: its navigation, keyboard, portrait/landscape and
  usability).** `style.css` is pico-mac's, adapted; the layouts are `portrait`, `land`, `lgame`, `lkbd` and
  `padmode` (`gsplit`/`side`/overlay are gone). In landscape the picture fills the area left of the
  column (`fitPc`); the column is W/4, or 15% on each side with the game keys, whose toolbar is rows of 3
  that may shrink (7 buttons ran off the edge). Game keys: move sets WASD/ESDF/IJKL/ZQSD/QAOP
  (`pico386-move`); a tap on ⇄ switches the pad between arrows and the set's letters (`pico386-gswap`),
  and a 2 s hold steps the set; Click holds the left mouse button. Files > More > Log shows the page's
  console (copy button). The full-screen buttons hide where the browser has no full screen (iOS).
  Not ported: the Mac's absolute pointer, one button, page Caps Lock and Mac keyboard rows. Tested in
  headless Chrome (portrait, landscape, game keys, ⇄, back to portrait); never on a phone.
- **Screen only (2026-11-25, the user asked: for a keyboard and mouse plugged into the phone).** 🖥 in
  the landscape toolbar (`onlyOn`, `#app.land.only`) hides everything but the picture, stretched to the
  whole screen less the notch's side; ☰ and ✕ (`#only-btns`) float top-right and fade after 3 s. It
  asks for the browser's full screen too where there is one. Upright it is set aside. Also: a mouse
  with no pointer lock (iPadOS Safari may lack it or refuse it, `pointerlockerror`) steers the guest by
  its hover moves over the screen (`freeMouseDown`), where before it did nothing; and Alt/Option +
  letter or digit sends the physical key (`e.code`), as iOS turns Option+F into "ƒ". Tested in headless
  Chrome at 844x390 (FreeDOS, on and off); the mouse and the Alt key never tested on an iPhone/iPad.
- Touch: Files > More > Double tap (`pico386-tapgap` in localStorage, 200-500 ms, default 300) is the
  longest gap between a tap and the next touch; a finger's first 9 px are held back so both clicks
  land on one spot. Text selection is blocked everywhere but fields and the receive code (`selectstart`).
- `web/shell/` is the page (copied into dist): `app.js` reads the 320x320 RGBA frame from wasm memory,
  sends keys as PicoCalc codes (`web_key`; letters lower case unless Shift, Caps Lock sent, no browser
  repeats), the canvas is a touchpad (`web_mouse`: drag, tap = left, two-finger tap = right; pointer
  lock with a mouse), an on-screen PC keyboard, ☰ = Ctrl+Esc, 🖱 = a trackpad in the keyboard's place (2026-11-24, the user asked; tap, two-finger tap, double-tap-drag; none in the overlay layout, whose screen is the touchpad; Ctrl+Tab is on the on-screen keyboard), 🎮 = a game keyboard (2026-11-24, the user asked: Esc, Tab, Bs, Enter, WASD, Shift, Ctrl, Alt, Space and a 3x3 arrow pad whose corners press two arrows and whose middle is a second Down, where a finger sliding to another arrow changes the direction, big, on a 14-column grid; its modifiers are held, not latched); in landscape (`gsplit`, 2026-11-24, the user asked) the screen sits in the middle with WASD, Esc, Tab, Shift, Ctrl, Alt and Space on the left and the arrows, Enter, Backspace, a second Shift and Space on the right, a pinch on the screen zooms up to 8x (`zoom`/`zu`/`zv`: `drawPc` scales only the part in view, so it stays sharp), an AudioWorklet on the
  ring (`web_audio_ring`), Files (upload, download, delete; 2026-11-24, the user asked: the home menu's first row under EMU_WEB, `hal_web_files` waits
  until the page calls `web_files_closed`, and the screen is `#files` in `#ui`'s card style: Upload (by extension), Receive,
  the folders as rows, each with its files and Download/Delete, Language, Restart) and "receive from PC" through
  `worker/relay.js` (Cloudflare Durable Object, `wrangler.jsonc`, `/send`). Files up to 512 MB (2026-11-24, the user asked; it was 128) go into the FS a 4 MB piece at a time, sized first with `ftruncate` (`openSized`), never whole in memory; a 300 MB upload passed a reload in headless Chrome. pt/en/es in `i18n.js`.
- **The card on OPFS (2026-11-24, the user asked: a big file received from a PC reloaded the page on
  the phone).** IDBFS kept the whole card in the tab's memory, loaded every file at start and cloned a
  file again on each save (desktop Chrome's JS heap was 648 MB after a 300 MB receive). Now
  `-sWASMFS=1` and `mountCard` (hal_web.cpp) mounts `/sdcard` on the OPFS backend from main()'s
  pthread (memory backend if the browser lacks `FileSystemSyncAccessHandle`), then waits for the
  page's `webFsInit` (app.js `fsInit`: migrate, seed, pc.ini) to call `web_fs_ready`. Every write is
  on disk, so `persist()`/`syncfs` are gone. `migrateIdb` moves an old IDBFS card (database
  `/sdcard`, store `FILE_DATA`) once, a record at a time, then deletes it. The first deploy hung a
  phone on "Loading": a record is read whole, and a big one kills a phone's tab, which reloads and
  dies on it again. Now each record is deleted once copied, the one being read is kept in
  localStorage (`pico386-migrating`) and, found at the next start, is dropped unread and named on the
  boot screen (`boot.lost`). `boot.card` ("Opening the files…") shows once the page has the card:
  a hang on "Loading" with it never shown is in `mountCard`, not the page. WASMFS has no `FS.isDir`
  (`isDirMode`), and its `FS.read`/`write`/`readFile` go through the 256 MB wasm heap, so writes are
  in 4 MB pieces (`writeAll`) and a download is OPFS's own disk-backed `File` (`fileBlob`).
  Receive is flow-controlled: the phone acks the header and every 1 MB written (`{t:'ack',got}`),
  and `/send` keeps at most 8 MB unacked (`WINDOW`; a page that never acks is sent to as before).
  Tested with `wrangler dev` and a node sender: 200 MB received, byte-exact after a reload, page
  heap peak 18 MB, never more than 8 MB ahead; download and delete work; an old IDBFS card migrated
  (snapshot included) and FreeDOS booted from OPFS. Never tested on a phone. The 📁 button in the
  home menu's header was removed (the user asked); Files is the menu's first row.
  **The page never calls `Module.FS` (2026-11-24):** with OPFS, a WASMFS call from the browser's main
  thread blocks it until OPFS's thread answers, and a phone hung for good at the first `mkdir` of the
  IDBFS move ("Moving to the new storage: pico-386"); desktop Chrome never did. Every page file op
  (boot, seed, Files, upload, receive, download's fallback) goes to `fsThread` (hal_web.cpp) through
  `web_fs_req`, answered by `Module.fsDone`; app.js's `fsCall` and its `fs*` helpers are async.
  Receive handles messages in order (`recv.chain`) and acks only what is written. Pool size 6.
  Tested in headless Chrome: migration, 200 MB receive byte-exact, upload, delete, Files in-game,
  FreeDOS boot. Works on the user's phone (2026-11-24): the move finished and the menu came up.
- **Native picture and touch menus (2026-11-24, the user asked):** the page shows the PC at its own
  resolution, not the panel: `pcvid_native` (pc_video.cpp, EMU_WEB) hands graphics modes from the fb
  and draws text at 9 (or 8) dots a cell with the guest's font (720x400 for 80x25) through
  `hal_web_pc_frame`; the page fits it in a 4:3 box (`fitPc`) and scales it sharp-bilinear (`drawPc`, 2026-11-24, the user
  asked): whole-number nearest-neighbour to the first size at least the box's, then smoothly down to
  the box in device pixels, so 320x200 has even blocks with no blur at any size or `devicePixelRatio`. `ui::list`,
  `ui::message` and `ui::editText` publish JSON (`hal_web_ui_show`: list rows with label, value,
  sub, icon, meter, adjust, snapshot picture as base64 RGB565) and the page (`#ui` in app.js) draws
  them as cards; a tap answers `web_ui_reply(seq, key, row)` with ENTER, LEFT/RIGHT (◀ ▶ on adjust
  rows), ESC or BACKSPACE ("Bs|Up" in the hints), so the C++ callers are unchanged. Real keys still
  work. Screens drawn on the panel (`present`) send `{"t":"panel"}` and show the 320 canvas;
  `pc_runFrame` sends `{"t":"pc"}`. `showNote` and the FPS meter go to `hal_web_status`. Tested in
  headless Chrome: home menu, drives screen, FreeDOS in 720x400, the in-game menu, Save snapshot's
  name field, the Load list with its picture, landscape. Mode 13h (`m13.img`, a named copy of `m13.ini` written into the card) checked at dpr 1 and 3.
- A new visitor is seeded (missing files only) with the BIOS and `freedos-hdd-32mb.img` (gzipped in
  `dist/seed/`), and a `pc.ini` named FreeDOS.
- Test: `python web/serve.py [--all]` (http://localhost:8766, COOP/COEP; on localhost the page also
  copies the repo's git-tracked sdcard files, or every file up to 64 MB with `--all`). Tested in
  headless Chrome (2026-11-24): home menu, FreeDOS to `C:\>` in under 30 s, `ver` typed, the in-game
  menu, landscape layout, audio drained at 44.1 kHz, files kept across a reload. Never tested on a phone.
- Deploy: `npx wrangler deploy` in `web/` after `build.py` (the user's Cloudflare login). Live at
  https://pico-386.lucianobf.workers.dev (2026-11-24) and at https://386.hexadevit.com (the user's domain; the README links it); a fresh headless Chrome there seeds and boots FreeDOS.

**Desktop.** MSYS2 MinGW32. The login shell starts in `$HOME`, so `cd` inside the `-lc`:

```sh
MSYSTEM=MINGW32 /c/msys64/usr/bin/bash.exe -lc "cd /c/Users/lucia/repos/pico-386 && make -C build -j8"
```

`make -C build-lowmem` is the same with `-DEMU_LOWMEM=ON`, the RP2350's memory model. Run it from
the repo root, because `PC_INI` paths are relative to the cwd.

**Test hooks** (desktop):
- `PC_INI=sdcard/pico-386/x.ini` boots that machine directly, skipping the home menu.
- `PC_KEYS=` runs a comma-separated script:
  - key names (`up`, `enter`, `esc`, `f5`, `pgup`, `ctrl`, `alt`, ...), single characters, and
    chords like `ctrl+esc`
  - `wNNN` waits NNN ms; `right:1500` holds a key for 1.5 s (the keyboard mouse; also via `remote.py`)
  - `shot` saves `sdcard/shotN.bmp`
  - `quit` exits
- `PC_WAV=file` dumps the mono s16 44.1 kHz audio stream.
- `PC_SD=dir` uses another SD folder. `PC_SHOT=N` takes a screenshot after N frames.
- `bigshot` (in `PC_KEYS`) saves the guest's 800x600 framebuffer as `sdcard/shotN.bmp`, readable where `shot`'s 320x240 is not.
- `PC_KEYS_FILE=path` (2026-11-16) watches a file: when the running script is done and the file is
  there, its text is the next `PC_KEYS` script and the file is deleted. It drives a long install a
  step at a time, with a look at each shot before the next keys. `ctrl+esc` is the in-game menu,
  not the guest's Start menu; `alt+f4` on the Windows 95 desktop opens Shut Down.
- **Windows 95 OSR2 (2026-11-16, desktop only so far):** `win95.ini` ("Windows 95") boots
  `win95.img`, a 256 MB disk (local, never commit) installed on the desktop from MS-DOS 6.22 with
  `setup /is` off the CD (UDVD2 + MSCDEX). It reaches the desktop in under 100 s and shuts down
  through APM. It runs on the P4 (2026-11-17): the user finds it slow but usable, and sound works.
  - **Speed on the P4 (2026-11-17):** opening and closing Control Panel > Multimedia reads
    1.3-1.6 MIPS, 240-294 cycles per guest instruction; idle is 0.1 MIPS (HLT). `prof` is all
    interpreter (`cpu_exec1` 25%, `translate` 12%, `set_seg` 6.5%, `read_desc` 4%): 16-bit
    protected-mode code with paging, which the flat fast path does not take.
  - **Instruction mix (2026-11-17, branch `win95-speed`):** counted on the desktop with `build-eip`
    (`-DT386_OPSTATS`, not LOWMEM; `build-opstats` is LOWMEM and its translate counts mean nothing
    for the P4): `MOVSeg` 20%, `JCCb` 16%, `PUSH` 9%, `POP_DS` + `PUSH_DS` 8%; segment loads in
    protected mode are about 30% of instructions (6% reload the same selector). CR3 writes, `INVLPG`
    and task switches are rare (a few thousand), so TLB flushes don't matter; TLB refills are
    about 5% of instructions. `read_desc` now reads from RAM on a TLB hit (THIRD_PARTY.md): on the
    device `set_seg` + `read_desc` dropped from 10.7% to 2.8% of `prof`, but the load test still
    read 1.4-1.5 MIPS, 257-283c. `??` (11% in that first profile) is ROM code at 0x4fc05dxx and
    shrinks to about 2% under load: it is Windows idling. The TLB entries then went to permission
    bits and a cached dirty bit (THIRD_PARTY.md): `translate` 12.4% to 11.3%, still 1.4-1.6 MIPS,
    262-280c. `prof` under load (open/close Multimedia): `cpu_exec1` 29%, `translate` 11%, `fetch8`
    7%, `fetch8pf` 6%, `pload8` 4%. The hottest lines are loads (`pload8`'s guest RAM, the TLB
    entry's compare), so PSRAM latency on guest memory is a large share of what is left.
    To profile while keys run: send `@keys ...` without waiting, then `remote.prof` in the same
    process (only one program can hold COM9); `remote.py raw` is `@shotraw`, not a raw line.
  - **Dirty lines (2026-11-17, THIRD_PARTY.md):** for 16-colour planar pictures wider than 320
    (mode 12h), `vga.c` decodes only the lines the guest wrote, `pcvid_render` scales only their
    rows plus the previous frame's (`s_pendY0/1`: the other video buffer lacks those), and
    `drawJob` pushes only that span (`pcvid_span`); nothing written, nothing pushed. Narrower
    pictures are still decoded and pushed whole every frame on the P4 (Prince's crackle). The
    same test then read 1.4-1.6 MIPS, 240-270c: the drawing was not the limit, the interpreter
    is. The P4's shot copies every row a frame was ever pushed to (`s_vTop`/`s_vEnd`).
  - **16-bit protected-mode fast path (2026-11-17): no gain, removed** (the user agreed). It was
    `i386_pm16.h`, like `i386_flat.h` but for 16-bit code with CR0.PE (not V86), hooked in
    `i386_exec.h` after the flat path, and was never committed.
    Desktop lockstep (`build-fchk`, Control Panel + Multimedia): `pm16check 101674128 checked,
    0 bad`. But runs average only about 5.6 instructions: the misses are 66 (5.1M), 67 (3.0M),
    RETF (1.5M), CALL far (1.4M), rotates, LODSB, CLD, LOOP, POP FS/GS, SBB, STOSB, XLAT. On the
    device, A/B in one session (TEMP `@pm16 0|1`), Multimedia open/close: 1.35 MIPS off, 1.37 on,
    about 285c both. `prof` showed it taking 16% of core 1 while `cpu_exec1` took 20%: the work
    moved, its cost did not (guest-memory loads from PSRAM dominate either way).
    Its 27.7 KB of IRAM left 151 KB of internal RAM at boot, and the PC's `hal_alloc_fast` tables
    then took the rest: the job semaphores, made at the first frame, were NULL (assert in
    `xQueueSemaphoreTake` at Start). Fixed for good: `jobInit` in `hal_init`, and `hal_alloc_fast`
    leaves 48 KB of internal RAM free (`kInternalReserve`).
  - `alt+f4` through `remote.py --p4` did not reach Windows on the device because the Windows 95
    startup was in Mouse mode, where Alt is the left button. In Arrows mode (in-game menu) it
    works, and `alt+f4` on the desktop then `enter` shuts down through APM (2026-11-19).
  - Setup needs 157 MB free even for Compact, so a 128 MB disk is refused.
  - It stopped with SU0152 ("0 bytes of memory") until the CMOS memory bytes were filled
    (THIRD_PARTY.md).
  - At the restart it says a disk is in the empty floppy drive; OK goes on.
  - The key's boxes move on by themselves: type all the digits in one run.
  - With the CD mounted at boot its autorun opens, so the ini leaves the CD empty.
  - The pointer turned into an hourglass every few seconds (2026-11-18): the empty CD drive said
    ready to TEST UNIT READY and then denied a disc to READ TOC, and Windows retried each poll
    about 30 times. Fixed in `ide.c` (THIRD_PARTY.md); tested on the desktop, not yet on the P4.
- `PC_MIPS=0.07 PC_MIPS_AT=100` caps the guest at 0.07 MIPS from 100 s of guest time on (boot
  fast, then run at the RP2350's Windows speed).

Test data in `sdcard/`. It is gitignored except what may be shared (2026-11-02, the user asked):
`pico-386/roms/`, `pico-386/disks/` (FreeDOS floppies, `freedos-hdd-32mb.img`, empty
images from `tools/make_disk.py`, the tests) and the test .ini files. Never commit MS-DOS,
Windows or games (DOSHDD, Win31Disk, games/). GitHub refuses files over 100 MB.

- `sdcard/pico-386/roms/`: `bios.bin` and `vgabios.bin` (2026-11-02; it was `/roms/pc`). That folder is the
  default; `pc_core`'s `biosDir()` falls back to it and to `/roms/pc`, which v0.1.0 saved in the cfg. The user can change it in
  Settings > BIOS folder (`biosdir` in pico386.cfg). `t386_start` takes it and ignores the ini's bios keys.
- everything else in `sdcard/pico-386/`: the .ini files and the disk images (one folder for the emulator, the user's choice)
- `fdboot.img`, the FreeDOS 1.3 boot floppy (`disks/freedos/x86BOOT.img` in git)
- boot-sector tests in `disks/tests/`, each with a matching `.ini` in `sdcard/pico-386/`:
  - `m13.img`: a mode 13h palette grid
  - `m12.img`: 640x480 diagonal bars
  - `beep.img`: a 1 kHz tone

## Hardware facts

These come from pico-amiga and emu16; its CLAUDE.md has more detail.
- **CPU:** dual-core RISC-V, 400 MHz on the P4 here (`ChipVariant=postv3`), no JIT, 32 MB PSRAM.
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
