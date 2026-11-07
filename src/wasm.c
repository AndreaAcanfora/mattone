/* WebAssembly entry points for the browser front end (web/). The module has no
 * imports: JavaScript copies the ROM into rom_buffer(), calls run_frame() and
 * reads the RGBA screen straight out of the module's memory. */
#include "gb.h"

#define EXPORT(name) __attribute__((export_name(name)))
#define ROM_MAX 0x800000

static gb_t gb;
static uint8_t rom[ROM_MAX];
static uint32_t screen[GB_HEIGHT * GB_WIDTH];
static uint32_t palette[4] = {0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000}; /* ABGR in memory: RGBA bytes */
static char title[17];

EXPORT("rom_buffer") uint8_t *rom_buffer(void) { return rom; }
EXPORT("rom_capacity") uint32_t rom_capacity(void) { return ROM_MAX; }

EXPORT("load") int load(uint32_t size) {
    int err = gb_load(&gb, rom, size);
    if (!err) gb_title(&gb, title);
    return err;
}

/* Converts the shades of the last frame to RGBA with the current palette. */
EXPORT("render") void render(void) {
    for (int y = 0; y < GB_HEIGHT; y++)
        for (int x = 0; x < GB_WIDTH; x++) screen[y * GB_WIDTH + x] = palette[gb.pixels[y][x] & 3];
}

EXPORT("run_frame") void run_frame(void) {
    gb_run_frame(&gb);
    render();
}

EXPORT("screen") uint32_t *screen_ptr(void) { return screen; }
EXPORT("palette") uint32_t *palette_ptr(void) { return palette; }
EXPORT("set_buttons") void set_buttons(uint32_t pressed) { gb_set_buttons(&gb, (uint8_t)pressed); }
EXPORT("title") const char *title_ptr(void) { return title; }

/* Battery-backed cartridge RAM, so the page can keep saves in localStorage. */
EXPORT("save_ram") uint8_t *save_ram(void) { return gb.cart_ram; }
EXPORT("save_size") uint32_t save_size(void) { return gb.has_battery ? gb.cart_ram_size : 0; }
