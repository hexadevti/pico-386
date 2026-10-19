// pc_glue.h -- the narrow C interface between the tiny386 core (C, pc_glue.c) and pico-386's C++
// side (pc_core.cpp and friends), so no C++ file has to include the core's own headers.
#pragma once

#include <stdint.h>
#include <stddef.h>
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
};

// Builds the machine an .ini describes (tiny386's own keys: [pc] mem_size, vga_mem_size,
// hda..hdd, cda..cdd, fda, fdb, fill_cmos; [cpu] gen, fpu). Relative paths are taken from
// sd_root. The BIOS is always bios_dir/bios.bin and bios_dir/vgabios.bin (the Settings choice):
// the .ini's bios/vga_bios keys are ignored, so .ini files written before the setting follow it.
// fb is T386_FB_W x T386_FB_H RGB565 and must outlive the machine.
int  t386_start(const char *ini_path, const char *sd_root, const char *bios_dir, uint16_t *fb);
const char *t386_error_file();
void t386_stop();                       // closes the disk images and frees the whole machine
void t386_reset();                      // the reset button (BIOS reload on the next step)
uint32_t t386_instructions();           // guest instructions run so far (wraps)

// The guest's clock in µs (wraps). It is wall time, except that t386_clock_skip(us) takes us
// out of it: time the machine spent stopped (the menu) must not reach the guest, or its timer
// fires every missed tick at once on return.
uint32_t get_uticks();
void t386_clock_skip(uint32_t us);
// Emulates until the guest's clock reaches deadline_us.
void t386_run_until(uint32_t deadline_us);
// Mixes n frames of 44.1 kHz stereo s16 (SB16 + Adlib + speaker); n <= 1024.
void t386_mix(int16_t *stereo, int n);
uint32_t t386_sb_starved();             // mixes in which the SB16 ran out of DMA data (diagnostics)

void t386_key(int down, int keycode);   // Linux input codes (1..95 = set 1, 96..111 extended)
void t386_mouse(int dx, int dy, int buttons);   // dy > 0 = down; buttons bit 0 L, 1 R, 2 M

// vga_refresh() into fb (size in VGAPicoInfo.gfx_*); 1 when any line of it was redrawn.
int  t386_refresh(int full);
void t386_info(VGAPicoInfo *info);

int  t386_swap_floppy(int drive, const char *path);   // 0 on success
void t386_change_cd(int ide_drive, const char *path);  // 0..3 = primary master .. secondary slave

// pc_platform.cpp
void   t386_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void   t386_release_all();
void   t386_flush_files();
size_t t386_heap_bytes();

#ifdef __cplusplus
}
#endif
