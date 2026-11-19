/* ne2000_stub.c -- pico-386: the RP2350's PC has no network card (no radio, and the card's 48 KB of
   buffer would not fit its SRAM). pc.c still wires one at 0x300, so these stand in for ne2000.c:
   reads float high, writes vanish. */
#include "tiny386_port.h"
#if defined(PICO386_LOWMEM)
#include "ne2000.h"

void ne2000_ioport_write(void *opaque, uint32_t addr, uint32_t val) { (void)opaque; (void)addr; (void)val; }
uint32_t ne2000_ioport_read(void *opaque, uint32_t addr) { (void)opaque; (void)addr; return 0xff; }
void ne2000_reset_ioport_write(void *opaque, uint32_t addr, uint32_t val) { (void)opaque; (void)addr; (void)val; }
uint32_t ne2000_reset_ioport_read(void *opaque, uint32_t addr) { (void)opaque; (void)addr; return 0xff; }
void ne2000_asic_ioport_write(void *opaque, uint32_t addr, uint32_t val) { (void)opaque; (void)addr; (void)val; }
uint32_t ne2000_asic_ioport_read(void *opaque, uint32_t addr) { (void)opaque; (void)addr; return 0xffff; }

void ne2000_step(NE2000State *s) { (void)s; }
NE2000State *isa_ne2000_init(int base, int irq, void *pic,
                             void (*set_irq)(void *pic, int irq, int level))
{
	(void)base; (void)irq; (void)pic; (void)set_irq;
	return NULL;
}
void ne2000_set_config_file(const char *file) { (void)file; }
#endif /* PICO386_LOWMEM */
