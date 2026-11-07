// pc_internal.h -- what the pc_*.cpp files share (the PC core's C++ side).
#pragma once

#include <stdint.h>
#include <string>

// ---- pc_keymap.cpp: PicoCalc keys -> PS/2 keyboard and the keyboard mouse --------------------
void pckbd_reset();                          // every key up (new machine, menu, lost events)
void pckbd_event(uint8_t code, uint8_t state);
void pckbd_tick(uint32_t nowMs);             // typematic repeat and mouse movement, once a frame
// The keyboard mouse: the arrows are arrow keys, move the pointer, or both. Saved per startup as
// [pico] mouse = 0/1/2.
enum { MOUSE_ARROWS, MOUSE_ON, MOUSE_BOTH };
void pckbd_set_mouse_mode(int mode);
int pckbd_mouse_mode();
const char *pckbd_mouse_mode_name(int mode);   // "Arrows", "Mouse", "Both"
// The mouse button keys (button 0 left, 1 right), by their .ini names ("alt", "rshift", "z").
void pckbd_set_mouse_button(int button, const char *iniName);
const char *pckbd_mouse_button(int button);          // the .ini name
const char *pckbd_mouse_button_label(int button);    // "Right Shift"
void pckbd_next_mouse_button(int button, int dir);   // dir -1 / +1 through the choices
void pckbd_ctrl_alt_del();
// The STM32 sends Shift+Up/Down as PgUp/PgDn. On, they reach the guest as Shift+Up/Down (games
// such as Prince of Persia); off, as PgUp/PgDn (the only way to type those on the PicoCalc).
void pckbd_set_shift_arrows(bool on);
bool pckbd_shift_arrows();

// ---- pc_video.cpp: the guest screen on the 320x320 panel --------------------------------------
void pcvid_reset();                          // forget caches: the next frame is drawn in full
// Draws the current guest screen into dst (SCREEN_W x SCREEN_H). false = nothing changed, the
// frame need not be submitted.
// fb is the guest framebuffer (T386_FB_W x T386_FB_H) the machine draws graphics modes into.
bool pcvid_render(uint16_t *dst, const uint16_t *fb, uint32_t nowMs);
// A small picture of the guest screen for a snapshot: the picture's area (not the black around it)
// box-averaged to PCVID_THUMB_W x PCVID_THUMB_H, handed to row(ctx, px) a row at a time, top row
// first. frame is the last frame drawn (hal_video_front()); LOWMEM has none, so the screen is
// drawn to the panel again and its rows are caught on the way (VRAM must be in place). false =
// no picture (a blank or undrawable mode); row is then never called.
constexpr int PCVID_THUMB_W = 80, PCVID_THUMB_H = 60;
bool pcvid_thumb(const uint16_t *frame, void (*row)(void *, const uint16_t *), void *ctx);

// ---- pc_profiles.cpp: the PC and the home menu ------------------------------------------------
// The home menu: picks a startup (a machine .ini with its drives) and returns the .ini to start,
// false on Exit.
bool pcprof_home(std::string &iniOut);
const char *pcprof_title();                    // the name of the startup pcprof_home started, or ""
// Settings rows hooks for the home menu (implemented in app.cpp).
void pcprof_settings_menu();

// ---- pc_disks.cpp: disk images, the drive letters, Create a hard disk -------------------------
enum DiskKind { DISK_NONE, DISK_FLOPPY, DISK_HARD, DISK_CD, DISK_FOLDER, DISK_ZIP };
// D: is a folder on the card, shown to the guest as a second hard disk (pc_folder.cpp), or a zip,
// which is unpacked to a folder first (pc_zip.cpp).
enum { DRIVE_A, DRIVE_B, DRIVE_C, DRIVE_D, DRIVE_CD, DRIVE_COUNT };
// By size and extension: .iso = CD, .zip = zip, up to 2.88 MB = floppy, larger = hard disk.
DiskKind pcdisk_kind(const std::string &path, long *sizeOut = nullptr);
std::string pcdisk_tag(const std::string &path);           // "Floppy 1.44 MB", "Hard disk 128 MB"
std::string pcdisk_rel(const std::string &path);           // relative to the card root, as in .ini files
std::string pcdisk_abs(const std::string &rel);
// The file browser showing only disks of one kind (DISK_NONE: every kind), each tagged.
bool pcdisk_browse(DiskKind kind, const char *title, std::string &out, const char *startDir = nullptr);
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
// What to do with a drive: 0 = nothing, 1 = mount out (a full path; a folder or a zip for D:), 2 = eject /
// remove.
int pcdisk_choose(int drive, const std::string &current, const char *note, std::string &out);
// A startup's drives screen before it starts: what the user chose (MACHINE_*). canDelete adds the
// "Delete this startup" row.
enum { MACHINE_BACK, MACHINE_START, MACHINE_RENAME, MACHINE_DELETE, MACHINE_SNAPSHOT };
// MACHINE_SNAPSHOT: start from the snapshot pcsnap_set_pending was given.
int pcdisk_machine_menu(const std::string &ini, const char *title, bool canDelete);
void pcdisk_create_menu();

// ---- pc_snapshot.cpp: snapshots of a running PC, per startup -----------------------------------
#include <vector>
struct PcSnapshot { std::string path, name; };
std::vector<PcSnapshot> pcsnap_list(const std::string &ini);    // newest first
// Saves the running PC (between steps, VRAM in place). null, or why not. replace: a snapshot
// this one takes the place of, deleted only once the new one is written (it becomes the newest).
const char *pcsnap_save(const std::string &ini, const std::string &name, const std::string &replace = "");
// The save screen: "New snapshot" or one to overwrite (replace = its file, else empty), then the
// name (an overwrite starts from the old one). false = cancelled.
bool pcsnap_pick_save(const std::string &ini, std::string &replace, std::string &name);
// The list screen: Enter picks one to start from (out = its file), Delete removes one.
bool pcsnap_pick(const std::string &ini, const char *title, std::string &out);
// Whether a disk the snapshot had was written since it was taken.
bool pcsnap_disks_changed(const std::string &snap);
// The snapshot the next pc_loadRom restores, and taking it (empty when none).
void pcsnap_set_pending(const std::string &snap);
std::string pcsnap_take_pending();
// Before the machine starts: the snapshot's .ini over the startup's. After: its state into the
// machine (null, or why not; a failed restore leaves the machine to be stopped).
bool pcsnap_write_ini(const std::string &snap, const std::string &ini);
// The guest RAM the snapshot's machine had (0 if it cannot be read): it loads only into as much.
long pcsnap_ram(const std::string &snap);
const char *pcsnap_restore(const std::string &snap);
