# The SD card

Copy `roms/` and `pc-386/` to the root of the PicoCalc's SD card (FAT32). The desktop build uses
this folder as its SD card.

```
roms/pc/bios.bin, vgabios.bin    SeaBIOS and SeaVGABIOS (LGPL-3.0, see roms/pc/README.txt)
pc-386/disks/
  freedos/x86BOOT.img            FreeDOS 1.3 FloppyEdition (1.44 MB): the boot floppy...
  freedos/x86DSK01..05.img       ...and the five install disks
  freedos-hdd-128mb.zip          a 128 MB hard disk with FreeDOS 1.3 installed (unzip it first)
  empty-hdd-32mb.img             an empty, formatted hard disk (FAT16)
  empty-floppy-1440k.img         an empty, formatted 1.44 MB floppy (FAT12)
  empty-floppy-720k.img          an empty, formatted 720 KB floppy (FAT12)
  cdrom-driver.img               a floppy with UDVD2.SYS, a DOS CD-ROM driver
  tests/                         small test programs, booted by the .ini files in pc-386/
```

## Getting started

- **Ready to go:** unzip `freedos-hdd-128mb.zip` on your computer, copy the `.img` into `pc-386/`
  and rename it `c.img`. The PC startup boots it to `C:\>`. The install is basic: there is no
  mouse driver, package manager or CD driver on it yet.
- **Install FreeDOS yourself:** make a hard disk in the home menu (**Create a hard disk**), put
  `freedos/x86BOOT.img` in A: and press Start. When the installer asks for the next disk, press
  Ctrl-Esc and put `x86DSK01.img` (then 02, 03...) in A:.
- **Bigger empty disks:** the home menu's **Create a hard disk** makes 32 to 504 MB. On a computer,
  `python tools/make_disk.py hdd 128 c.img` makes the same disk, and
  `python tools/make_disk.py floppy 1440 a.img` makes a floppy.

## Tests

On the desktop, `PC_INI=sdcard/pc-386/<name>.ini build/pico386.exe` boots one directly:

| .ini | What it shows |
|---|---|
| `m13.ini` | a mode 13h (320x200x256) palette grid |
| `m12.ini` | 640x480x16 diagonal bars |
| `beep.ini` | a 1 kHz tone on the PC speaker |
| `cdboot.ini` | the mode 13h grid, booted from a CD (`m13.iso`, El Torito) |

`tests/sbtest/SBTEST.COM` plays a sine wave through the Sound Blaster 16 by DMA. Pick the
`sbtest` folder as D: and run `D:\SBTEST`.

## Not included

MS-DOS, Windows and games are not free to share, so they are not here. Use your own copies:
disk images go in `pc-386/`, and a game's folder or `.zip` can be used as D: directly.

## Licenses

- FreeDOS 1.3: GPL and other free licenses (see the `DOC` folders on the disks, and
  [freedos.org](https://www.freedos.org/)).
- UDVD2.SYS: by Jack R. Ellis, as distributed with FreeDOS.
- The BIOS files: LGPL-3.0.
- The empty images and the tests: made for pico-386, GPL-2.0-or-later like the rest of it.
