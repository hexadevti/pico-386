#ifndef VGA_H
#define VGA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FBDevice FBDevice;

typedef void SimpleFBDrawFunc(void *opaque,
                              int x, int y, int w, int h);

typedef struct VGAState VGAState;
VGAState *vga_init(char *vga_ram, int vga_ram_size,
                   uint8_t *fb, int width, int height);
void vga_set_force_8dm(VGAState *s, int v);

int vga_step(VGAState *vga);
void vga_refresh(VGAState *s,
                 SimpleFBDrawFunc *redraw_func, void *opaque, int full_update);

void vga_ioport_write(VGAState *s, uint32_t addr, uint32_t val);
uint32_t vga_ioport_read(VGAState *s, uint32_t addr);

void vbe_write(VGAState *s, uint32_t offset, uint32_t val);
uint32_t vbe_read(VGAState *s, uint32_t offset);

void vga_mem_write(VGAState *s, uint32_t addr, uint8_t val);
uint8_t vga_mem_read(VGAState *s, uint32_t addr);
void vga_mem_write16(VGAState *s, uint32_t addr, uint16_t val);
void vga_mem_write32(VGAState *s, uint32_t addr, uint32_t val);
bool vga_mem_write_string(VGAState *s, uint32_t addr, uint8_t *buf, int len);

typedef struct PCIDevice PCIDevice;
typedef struct PCIBus PCIBus;
PCIDevice *vga_pci_init(VGAState *s, PCIBus *bus,
                        void *o, void (*set_bar)(void *, int, uint32_t, bool));

#ifndef BPP
#define BPP 32
#endif

typedef struct VGATextOps {
    void (*set_cursor)(void *o, int y, int x);
    void (*put_char2)(void *o, int y, int x, int ch, int attr);
    void (*refresh)(void *o, const uint8_t *ram, int w, int h);
} VGATextOps;

void vga_set_text_ops(VGAState *s, VGATextOps *ops, void *o);

#ifdef TINY386_PICO
/* pico-386: see vga_pico_info() in vga.c */
typedef struct VGAPicoInfo {
    int mode;                 /* 0 blank, 1 text, 2 graphics */
    int gfx_w, gfx_h;         /* last graphics picture, top-left of the framebuffer */
    const uint8_t *vram;      /* planes interleaved: byte 4*a+p is plane p of address a */
    uint32_t start_addr;      /* text: vram byte of the top-left cell (char; attr at +1) */
    uint32_t line_offset;     /* text: vram bytes per row */
    int cols, rows, cheight;  /* text geometry, cheight = scanlines per character */
    const uint8_t *font;      /* glyph c row r at font[(c * 32 + r) * 4] */
    uint32_t cursor_addr;     /* vram byte of the cursor cell */
    int cursor_start, cursor_end;   /* CRTC 0x0a / 0x0b (bit 5 of start = hidden) */
    int blink;                /* attribute bit 7 blinks instead of a bright background */
    uint16_t palette[16];     /* RGB565 */
} VGAPicoInfo;
void vga_pico_info(VGAState *s, VGAPicoInfo *p);
#endif

#endif /* VGA_H */
