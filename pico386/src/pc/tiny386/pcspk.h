#ifndef PCSPK_H
#define PCSPK_H

#include <stdint.h>
#include "i8254.h"
typedef struct PCSpkState PCSpkState;
PCSpkState *pcspk_init(PITState *pit);
void pcspk_callback(void *opaque, uint8_t *stream, int free);
uint32_t pcspk_ioport_read(void *opaque);
void pcspk_ioport_write(void *opaque, uint32_t val);
int pcspk_get_active_out(PCSpkState *s);

#ifdef TINY386_PICO
struct SnapIO;
void pcspk_snap(PCSpkState *s, struct SnapIO *io);   /* pico-386: snap.h */
#endif

#endif /* PCSPK_H */
