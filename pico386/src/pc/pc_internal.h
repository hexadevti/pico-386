// pc_internal.h -- what the pc_*.cpp files share (the PC core's C++ side).
#pragma once

#include <stdint.h>
#include <string>

// ---- pc_keymap.cpp: PicoCalc keys -> PS/2 keyboard and the keyboard mouse --------------------
void pckbd_reset();                          // every key up (new machine, menu, lost events)
void pckbd_event(uint8_t code, uint8_t state);
void pckbd_tick(uint32_t nowMs);             // typematic repeat and mouse movement, once a frame
void pckbd_set_mouse_mode(bool on);
bool pckbd_mouse_mode();
void pckbd_ctrl_alt_del();

// ---- pc_video.cpp: the guest screen on the 320x320 panel --------------------------------------
void pcvid_reset();                          // forget caches: the next frame is drawn in full
// Draws the current guest screen into dst (SCREEN_W x SCREEN_H). false = nothing changed, the
// frame need not be submitted.
// fb is the guest framebuffer (T386_FB_W x T386_FB_H) the machine draws graphics modes into.
bool pcvid_render(uint16_t *dst, const uint16_t *fb, uint32_t nowMs);

// ---- pc_profiles.cpp: the PC and the home menu ------------------------------------------------
// The home menu: picks the PC (or a disk image to boot) and returns the .ini to start, false
// on Esc.
bool pcprof_home(std::string &iniOut);
const char *pcprof_title();                    // what pcprof_home started ("PC", an image name), or ""
// Settings rows hooks for the home menu (implemented in app.cpp).
void pcprof_settings_menu();
// [pico] mouse=1 in the .ini: the keyboard mouse starts on.
bool pcprof_mouse_default(const char *iniPath);

// ---- pc_disks.cpp: disk images, the drive letters, Create a hard disk -------------------------
enum DiskKind { DISK_NONE, DISK_FLOPPY, DISK_HARD, DISK_CD, DISK_FOLDER };
// D: is a folder on the card, shown to the guest as a second hard disk (pc_folder.cpp).
enum { DRIVE_A, DRIVE_B, DRIVE_C, DRIVE_D, DRIVE_CD, DRIVE_COUNT };
// By size and extension: .iso = CD, up to 2.88 MB = floppy, larger = hard disk.
DiskKind pcdisk_kind(const std::string &path, long *sizeOut = nullptr);
std::string pcdisk_tag(const std::string &path);           // "Floppy 1.44 MB", "Hard disk 128 MB"
std::string pcdisk_rel(const std::string &path);           // relative to the card root, as in .ini files
std::string pcdisk_abs(const std::string &rel);
// The file browser showing only disks of one kind (DISK_NONE: every kind), each tagged.
bool pcdisk_browse(DiskKind kind, const char *title, std::string &out);
// A machine .ini's [pc] keys. Setting "" comments the key out.
std::string pcini_get(const std::string &ini, const char *key);
bool pcini_set(const std::string &ini, const char *key, const std::string &value);
// The same for any section ("pico": our own keys).
std::string pcini_get_in(const std::string &ini, const char *section, const char *key);
bool pcini_set_in(const std::string &ini, const char *section, const char *key, const std::string &value);
// The image a drive (DRIVE_*) mounts in an .ini (relative, "" = none), and changing it.
std::string pcdisk_drive(const std::string &ini, int drive);
bool pcdisk_set_drive(const std::string &ini, int drive, const std::string &image);
int pcdisk_cd_ide(const std::string &ini);                 // the CD's IDE slot: 0 (cda) or 2 (cdc)
std::string pcdisk_drive_label(const std::string &rel);    // "fdboot.img", "Empty", "x.img (missing)"
const char *pcdisk_drive_name(int drive);                  // "A: floppy"
const char *pcdisk_drive_letter(int drive);                // "A:"
// What to do with a drive: 0 = nothing, 1 = mount out (a full path; a folder for D:), 2 = eject /
// remove.
int pcdisk_choose(int drive, const std::string &current, const char *note, std::string &out);
// A machine's drives screen before it starts: true = Start, false = Esc.
bool pcdisk_machine_menu(const std::string &ini, const char *title);
void pcdisk_create_menu();
