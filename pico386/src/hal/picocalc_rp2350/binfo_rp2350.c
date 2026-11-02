// binfo_rp2350.c -- the Pico SDK binary_info block: program name, version and build date.
//
// Loaders and picotool read these straight from the .uf2 (PicoMultiLoader shows them on its
// details screen before flashing, docs/apps.md there). arduino-pico's libpico is built without
// binary_info, so neither the header nor the entries are in the image unless we add them: the
// header is the five words picotool looks for, which the linker script KEEPs right after the
// vector table (".binary_info_header", in the first 256 bytes as the format requires). The entry
// list it points at is an ordinary const array. Same block as emu8's binfo_picocalc.c.
//
// The version comes from the build: tools/pml-app/package.ps1 passes -DPICO386_VERSION=<x.y.z>.

#if defined(BOARD_PICOCALC_RP2350)

#include <stdint.h>

#ifndef PICO386_VERSION
#define PICO386_VERSION dev
#endif
#define BI_STR2(x) #x
#define BI_STR(x)  BI_STR2(x)

#define BI_MARKER_START       0x7188ebf2u
#define BI_MARKER_END         0xe71aa390u
#define BI_TYPE_ID_AND_STRING 6
#define BI_TAG_RP             0x5052        // 'R','P'
#define BI_ID_PROGRAM_NAME    0x02031c86u
#define BI_ID_PROGRAM_VERSION 0x11a9bc3au
#define BI_ID_PROGRAM_DATE    0x9da22254u

typedef struct {
  uint16_t    type, tag;
  uint32_t    id;
  const char *value;
} bi_id_string_t;

static const bi_id_string_t bi_name    = { BI_TYPE_ID_AND_STRING, BI_TAG_RP, BI_ID_PROGRAM_NAME, "pico-386" };
static const bi_id_string_t bi_version = { BI_TYPE_ID_AND_STRING, BI_TAG_RP, BI_ID_PROGRAM_VERSION, BI_STR(PICO386_VERSION) };
static const bi_id_string_t bi_date    = { BI_TYPE_ID_AND_STRING, BI_TAG_RP, BI_ID_PROGRAM_DATE, __DATE__ };

static const void *const bi_entries[] = { &bi_name, &bi_version, &bi_date };
static const uint32_t bi_mapping[3] = { 0, 0, 0 };   // nothing lives in copied data: empty table

__attribute__((section(".binary_info_header"), used))
const void *const pico386_binary_info_header[5] = {
  (const void *)BI_MARKER_START,
  &bi_entries[0],
  &bi_entries[sizeof(bi_entries) / sizeof(bi_entries[0])],
  bi_mapping,
  (const void *)BI_MARKER_END,
};

#endif  // BOARD_PICOCALC_RP2350
