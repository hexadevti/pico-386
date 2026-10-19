# pico-386

A 386/486 PC with VGA, Sound Blaster 16 and Adlib for the **ClockworkPi PicoCalc** fitted with a
**Waveshare ESP32-P4-WIFI6** module, plus an SDL2 desktop build of the same code for debugging.
The emulator is [tiny386](https://github.com/hchunhui/tiny386). The frontend and hardware layer
come from [pico-amiga](../pico-amiga).

**Status:** it runs on the desktop. SeaBIOS boots, FreeDOS boots from a floppy image, the
keyboard works, VGA text, 320x200 and 640x480 modes display correctly, and the PC speaker
plays. The P4 firmware builds but has not yet been run on the device.

## The SD card

```
/roms/pc/bios.bin        SeaBIOS        } from the tiny386 release tarball
/roms/pc/vgabios.bin     SeaVGABIOS     } (tiny386-*.tar.gz on its GitHub releases page)
/pc-386/pc.ini           the PC startup: written with defaults the first time, then yours to edit
/pc-386/startN.ini       the startups made with "Create new startup"
/pc-386/pico386.cfg      frontend settings (volume, FPS meter, last startup)
/pc-386/c.img            the PC's hard disk (a new pc.ini takes c.img, else dos.img or win.img)
/pc-386/*.img *.ima *.iso *.vhd    disk images to mount on a startup's drives screen
```

Disk images are raw: a hard disk is a flat sector dump (a `.vhd` must be a *fixed* VHD) and a
floppy is 160 KB to 2.88 MB. Guest writes go straight to the image on the card.

The two BIOS files can live in another folder: **Settings > BIOS folder** opens a file browser;
pick `bios.bin` there (the row says `OK` when `vgabios.bin` is next to it). Every machine uses
that folder, and any `bios`/`vga_bios` lines in the .ini files are ignored.

## Using it

The home menu has one machine, **PC**: a 486 with an FPU, SVGA (VBE) with 1 MB, and as much RAM
as the PSRAM holds. The .ini asks for 32 MB, and the PC steps down 4 MB at a time until it fits.
The CPU type only changes what programs detect, not the speed.

**Drives.** Choosing PC opens its drives screen first:

| Row | What it does |
|---|---|
| **Start** | Turns the PC on. It boots from A: if a floppy is in, otherwise from C:. |
| **A: floppy**, **B: floppy** | Put in a floppy image, a folder or a `.zip`, or eject it. |
| **C: hard disk** | Pick the hard-disk image. |
| **D: SD folder** | Pick a folder on the SD card. The PC sees it as drive D:. |
| **CD-ROM** | Put in an `.iso`, or eject it. |

The file browser shows only images that fit the drive, and labels each file with its type and
size: "Floppy 1.44 MB", "Hard disk 128 MB", "CD 650 MB". Your choices are saved in `pc.ini`.

**D: is a folder on the card.** It's the easy way to get files in and out of the PC. Put
programs or games in a folder from your computer, then pick that folder as D:.

- DOS sees the folder's files and subfolders as an ordinary hard disk. Windows 95 also sees their
  long names.
- What the PC changes on D: (new, edited, renamed or deleted files) is saved into the folder
  **when you turn the PC off** with Quit in the in-game menu, or when you change C: or D:.
  "Saving" shows while it writes. If the PicoCalc loses power first, those changes are lost.
- Files DOS creates get DOS names (`NEW.TXT`). Files that were already there keep their names.
- If no C: is set, the folder is C: instead.
- Formatting or repartitioning D: is never written back, so it cannot wipe the folder.
- The disk is sized for the folder plus 32 MB free, up to 2 GB. Above 504 MB, plain MS-DOS
  can't read it; FreeDOS and Windows 95 can.
- `/pc-386` itself can't be D:, since it holds the PC's own disks.

**A: and B: take a folder or a zip too**, as a 1.44 MB floppy. Unlike D:, it can be swapped
while DOS runs, so you can go through a folder of game zips without restarting the PC.

- A zip is unpacked once to `/pc-386/.unzipped/`, as for D:. The game then runs from that copy.
- A folder or zip holding more than 1.44 MB is refused ("Too big for a floppy (use D:)").
- What the PC writes on the floppy is saved to its folder when you eject it, swap it, or turn
  the PC off. "Saving A:" shows while it writes.
- The same folder can't be in two drives at once.

**While it runs**, the in-game menu (Ctrl-Esc) has the same drive rows:

- **A:, B: and the CD** change immediately, like swapping a real disk. DOS sees the new disk.
  This is how you feed a multi-disk installer.
- **C: and D:** restart the PC with the new disk, as a real PC would need.

**Create a hard disk** (home menu) makes a new, empty, formatted C: drive:

1. Pick a size: 32, 64, 128, 256 or 504 MB.
2. Pick a file: `c.img` or a new `diskN.img`.

The disk comes out partitioned and formatted (FAT16). Until DOS is installed on it, it shows
"This hard disk has no DOS yet". To install DOS, put a DOS install floppy in A:, then Start.

**Startups.** The home menu lists each startup: the same PC with its own drives, such as DOS on
C:, a Windows CD, or a game's zip as D:. **Create new startup** asks for a name and opens its
drives screen. That screen also has **Boot from** (C:, or the CD before C:) and **Rename**, and
every startup except PC has **Delete**. Deleting a startup removes only its `.ini`; the disk
images stay. Startups are saved as `/pc-386/startN.ini`, and the home menu shows the name in
`[pico] name`.

**Everything is remembered.** The drives (whether chosen on the drives screen or in-game), the
keyboard mouse mode and buttons, and the settings are saved to the startup as you change them, so it starts
the same way next time. A floppy left in A: boots again, as on a real PC; eject it
to boot from C:.

`pc.ini` is a tiny386 `.ini` file. Every key tiny386 knows works in it (`fda`, `fdb`,
`hda`..`hdd`, `cda`..`cdd`, `mem_size`, `vga_mem_size`, `[cpu] gen`/`fpu`), and paths are relative
to the card root. `[pico]` holds ours: `name`, `boot = cd`, `mouse` (0 arrows, 1 mouse, 2 both) and the
button keys `mouse_left`/`mouse_right` (`alt`, `rshift`, `lshift`, `ctrl`, `space`, `enter`, `tab`,
`bs`, `z`, `x` or `none`).

**Keys.** Every key goes to the PC except the frontend's Ctrl hotkeys:

- **Ctrl-Esc**: the in-game menu. It has Reset, the A:/B:/C:/D:/CD drives, the arrows' mode,
  the two mouse button keys, Ctrl+Alt+Del, volume and FPS. The FPS line also shows the PC's speed in MIPS (millions of
  instructions per second). It reads low while DOS sits idle at a prompt.
- **Both Shifts together** (or **Ctrl-Tab**): the next mode for the arrows, shown for 2 s under the
  picture:
  - **Arrows**: arrow keys, as normal.
  - **Mouse**: the arrows move the pointer, speeding up while held. The button keys click.
  - **Both**: the arrows move the pointer and are also sent as arrow keys.

  In Mouse and Both, the button keys (Alt and Right Shift unless changed in the in-game menu) are
  only mouse buttons. A DOS mouse driver (CTMOUSE) or Windows sees a PS/2 mouse.
- **Ctrl-Shift-Up**: back to esp32loader. **Ctrl-Shift-F3**: restart.
- **Ctrl+Alt+Del** restarts the PC from the keyboard, as does **Ctrl+Alt+Backspace**.
- **Caps Lock** works on letters (Shift gives lower case while it's on). The PC's own Caps Lock
  stays off.

Shifted characters are sent as Shift plus the US-layout key, so set the guest to the US keyboard
layout. Keys repeat like a real keyboard (500 ms, then 30/s).

**Screen.** 80x25 text is drawn from text RAM in a 4x12 font with 4 levels of antialiasing.
Graphics modes are scaled to 320x240: 320x200 is stretched, and 640x480 is averaged 2:1.

**Speed.** The emulator runs as fast as the P4 allows and the guest keeps real time from the
clock, so a slow stretch is just a slower CPU. The tiny386 author measured an ESP32-S3 at roughly
a fast 386SX. The P4 should be somewhat faster. DOS games and Windows 3.x are the sweet spot;
Windows 95 boots but is slow.

## Making a disk image

A FreeDOS hard disk, all on the PicoCalc:

1. **Create a hard disk**: 128 MB, `c.img`.
2. **PC**: set A: to `x86BOOT.img`, from the FreeDOS 1.3 "FloppyEdition" (1.44 MB set). Then
   Start.
3. Answer the installer. When it asks for the next diskette, press Ctrl-Esc, set A: to
   `x86DSK01.img` (then 02, 03…), and press a key. Wait for each prompt: a disk takes a minute or
   more to read.
4. At the end, eject A: and restart. It boots from C:.

Or prepare an image on a PC with QEMU (`-hda c.img`) and copy it to the card.

## Building

**PicoCalc P4.** Run the VS Code task **P4: Build** or **P4: Build & Upload** (COM9). It uses the
same Arduino-ESP32 3.x install as emu8/emu16/pico-amiga (`~/.emu6502-p4`).

**Desktop.** You need MSYS2 MinGW32 with SDL2 and CMake. Run the task **desktop: build**, or:

```sh
cmake -G "Unix Makefiles" -S . -B build -DCMAKE_BUILD_TYPE=Debug && make -C build -j8
```

The repo's `sdcard/` folder plays the SD card. The test hooks `PC_INI`, `PC_KEYS`, `PC_SHOT`,
`PC_WAV` and `PC_SD` are described in `CLAUDE.md`.

## Layout

```
pico386/pico386.ino       setup()/loop() -> src/frontend/app.cpp
pico386/board.h           pins and screen geometry (P4 and desktop)
pico386/core.h            the interface the emulator core implements
pico386/src/hal/          hal.h, picocalc/ (ESP32-P4), desktop/ (SDL2)
pico386/src/frontend/     menus, file browser, settings, game loop
pico386/src/pc/           the PC: glue, keyboard, screen, home menu
pico386/src/pc/tiny386/   the vendored emulator (see THIRD_PARTY.md)
```

## License

GPL-2.0-or-later for this project's own code (see `LICENSE`). Third-party parts, including
tiny386 (BSD-3/MIT) and the Adlib emulator (LGPL-2.1+), are listed in `THIRD_PARTY.md`.
