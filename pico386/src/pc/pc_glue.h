// pc_glue.h -- the narrow C interface between the tiny386 core (C, pc_glue.c) and pico-386's C++
// side (pc_core.cpp and friends), so no C++ file has to include the core's own headers.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifndef TINY386_PICO
#define TINY386_PICO 1
#endif
#include "tiny386/vga.h"     // VGAPicoInfo

#ifdef __cplusplus
extern "C" {
#endif

// The guest's framebuffer (vga_refresh() output) and the largest picture it holds.
#define T386_FB_W 800
#define T386_FB_H 600
// The most guest RAM a machine gets; t386_start steps it down to what PSRAM (32 MB, shared with
// the framebuffer, VGA memory and the rest) has room for.
#define T386_MEM_MAX_MB 32

enum T386Error {
  T386_OK = 0,
  T386_ERR_INI,        // the profile .ini is missing or unreadable
  T386_ERR_FILE,       // a file the .ini names (BIOS, disk) is missing: see t386_error_file()
  T386_ERR_MEMORY,
  T386_ERR_ZIP,         // a zip the .ini names cannot be unpacked: see t386_error_file()
};

// Builds the machine an .ini describes (tiny386's own keys: [pc] mem_size, vga_mem_size,
// hda..hdd, cda..cdd, fda, fdb, fill_cmos; [cpu] gen, fpu). Relative paths are taken from
// sd_root. The BIOS is always bios_dir/bios.bin and bios_dir/vgabios.bin (the Settings choice):
// the .ini's bios/vga_bios keys are ignored, so .ini files written before the setting follow it.
// fb is T386_FB_W x T386_FB_H RGB565 and must outlive the machine.
int  t386_start(const char *ini_path, const char *sd_root, const char *bios_dir, uint16_t *fb);
const char *t386_error_file();
void t386_stop();                       // closes the disk images and frees the whole machine
// Before and after the in-game menu. On the RP2350 the menu's canvas shares SRAM with VRAM,
// so VRAM is saved to PSRAM meanwhile; elsewhere they do nothing. Call hal_job_wait() first.
void pcmem_park();
void pcmem_unpark();
// RP2350: SRAM lent by the pager while a PC runs (pm_lend), for the folder floppy's large
// buffers; the heap has only about 10 KB left then. NULL elsewhere, or when none is free.
// pcmem_free returns 0 for a pointer that is not the arena's (free it as usual).
void *pcmem_lend(size_t n);
int   pcmem_free(void *p);
// RP2350: a PSRAM address with PCMEM_SCRATCH_BYTES no machine uses (the zip inflater's window);
// -1 elsewhere.
#define PCMEM_SCRATCH_BYTES 32768
long  pcmem_scratch();
// RP2350: PSRAM that nothing uses, just past the most guest RAM a PC can have (PM_MAX_RAM): its
// address, and its size in *bytes. The file browser keeps its names there. -1 elsewhere.
#define PCMEM_SPARE_BYTES (512L * 1024)
long  pcmem_spare(long *bytes);
// RP2350: the rest of the PSRAM up to the scratch area, for the folder disks' FATs, directories
// and nodes (pc_folder). -1 elsewhere.
long  pcmem_folders(long *bytes);
void t386_reset();                      // the reset button (BIOS reload on the next step)
uint32_t t386_instructions();           // guest instructions run so far (wraps)
void t386_where(uint32_t *cs, uint32_t *eip);   // bring-up diagnostics
void t386_timer_info(uint32_t *count, uint32_t *behind, uint32_t *fired, uint32_t *lost);

// The guest's clock in µs (wraps). It is wall time, except that t386_clock_skip(us) takes us
// out of it: time the machine spent stopped (the menu) must not reach the guest, or its timer
// fires every missed tick at once on return.
uint32_t get_uticks();
void t386_clock_skip(uint32_t us);
void t386_clock_set(uint32_t now);      // the guest's clock reads `now` from here on (snapshots)
// Emulates until the guest's clock reaches deadline_us.
void t386_run_until(uint32_t deadline_us);
// Called after every step of t386_run_until; sound_written: the guest wrote the Adlib or the
// speaker during that step.
void t386_set_audio_hook(void (*hook)(int sound_written));
// Mixes n frames of 44.1 kHz stereo s16 (SB16 + Adlib + speaker); n <= 1024.
void t386_mix(int16_t *stereo, int n);
uint32_t t386_sb_starved();             // mixes in which the SB16 ran out of DMA data (diagnostics)

void t386_key(int down, int keycode);   // Linux input codes (1..95 = set 1, 96..111 extended)
void t386_mouse(int dx, int dy, int buttons);   // dy > 0 = down; buttons bit 0 L, 1 R, 2 M

// vga_refresh() into fb (size in VGAPicoInfo.gfx_*); 1 when any line of it was redrawn.
int  t386_refresh(int full);
// PICO386_LOWMEM: there is no framebuffer; t386_refresh hands each finished line of a graphics
// picture to f (line y of h, w pixels) instead. Set it before each refresh.
typedef void T386LineFunc(void *opaque, int y, const uint16_t *line, int w, int h);
void t386_set_line_sink(T386LineFunc *f, void *opaque);
void t386_info(VGAPicoInfo *info);

// Puts an image, a folder or a zip (as a 1.44 MB floppy) in A: (0) or B: (1); NULL or "" ejects.
// 0 on success, else a T386_FD_* code. A folder floppy taken out is written back to its folder.
enum {
  T386_FD_SIZE = -1,     // an image of a size no floppy has
  T386_FD_TOO_BIG = -2,  // a folder or zip with more than a floppy holds
  T386_FD_IN_USE = -3,   // that folder is open in the other drive or as D:
  T386_FD_UNPACK = -4,   // the zip cannot be unpacked
  T386_FD_MISSING = -5,
};
int  t386_swap_floppy(int drive, const char *path);
int  t386_floppy_dirty(int drive);      // a folder floppy the guest has written to
void t386_change_cd(int ide_drive, const char *path);  // 0..3 = primary master .. secondary slave

// Snapshots (pc_snapshot.cpp writes the file around these): the whole machine, guest RAM and
// VRAM, at the end of a step. Loading needs the machine t386_start built from the snapshot's own
// ini, before it runs. 0 on success, else a T386_SNAP_* code; a failed load leaves the machine
// half restored, so stop it.
enum {
  T386_SNAP_IO = -1,       // a short read or write (card full, file cut short)
  T386_SNAP_FORMAT = -2,   // another build's layout, or another amount of RAM
};
// The disk images open for writing: the path of slot i (0..15), NULL when that slot holds none.
const char *t386_write_file(int i);
// A snapshot was taken: the next write to each image bumps its generation again.
void t386_files_snapped(void);
// pc_snapshot.cpp: a disk (an image, or a folder written back) changed, so snapshots taken
// before now no longer match it.
void pcsnap_touch(const char *path);
int  t386_save_state(FILE *f);
int  t386_load_state(FILE *f);

// pc_platform.cpp
void   t386_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void   t386_release_all();
void   t386_flush_files();
size_t t386_heap_bytes();

#ifdef __cplusplus
}
#endif
