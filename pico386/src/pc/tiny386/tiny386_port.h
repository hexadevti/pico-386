// tiny386_port.h -- pico-386's build configuration for the vendored tiny386 core (not upstream).
//
// The Arduino build has no per-folder -D flags, so every .c file of the core includes this first.
// It replaces upstream's Makefile defines. BUILD_ESP32 is deliberately NOT defined: that path
// pulls in raw-SD, UART, FreeRTOS and wifi code meant for upstream's own boards. The generic
// path plus the TINY386_PICO patches (see THIRD_PARTY.md) is what runs on both of our targets.
//
// It also routes the core's heap and stdio through pc_platform.cpp, which keeps a list of every
// block and FILE the core opens. The core never frees anything, so that list is how a session is
// torn down (t386_release_all()), and closing the FILEs is what flushes disk-image writes.
#ifndef TINY386_PORT_H
#define TINY386_PORT_H

#define TINY386_PICO 1
#define BPP 16                       // RGB565 framebuffer, native byte order
#define I386_ENABLE_FPU 1            // built in; the profile decides whether the CPU gets one
#define USE_FMOPL 1                  // Adlib through the MAME OPL2 (fmopl.c)
#define FMOPL_USE_STATIC_TABLE 1     // fmopl_table.h (upstream fmopl.inc, from -DGENTABLE): no table allocation

// Instructions per pc_step(): between two steps the PIT, PIC, CMOS, keyboard and DMA catch up.
// Upstream uses 10240 on PCs and 512 on its ESP32 boards; 2048 keeps IRQ latency well under a
// millisecond at the P4's guest speed without the per-step overhead showing.
#ifndef PC_STEP_COUNT
#define PC_STEP_COUNT 2048
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
void *t386_malloc(size_t n);
void *t386_calloc(size_t n, size_t m);
void *t386_realloc(void *p, size_t n);
void  t386_free(void *p);
char *t386_strdup(const char *s);
FILE *t386_fopen(const char *path, const char *mode);
int   t386_fclose(FILE *f);
#ifdef __cplusplus
}
#endif

// Function-like, so identifiers that merely share the names (mixer_callback's `free` argument)
// are left alone. The system headers above are already in, so their declarations are untouched.
#define malloc(n)      t386_malloc(n)
#define calloc(n, m)   t386_calloc(n, m)
#define realloc(p, n)  t386_realloc(p, n)
#define free(p)        t386_free(p)
#define strdup(s)      t386_strdup(s)
#define fopen(p, m)    t386_fopen(p, m)
#define fclose(f)      t386_fclose(f)

#endif
