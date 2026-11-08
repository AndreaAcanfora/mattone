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
static int16_t audio[GB_AUDIO_MAX * 2];

EXPORT("rom_buffer") uint8_t *rom_buffer(void) { return rom; }
EXPORT("rom_capacity") uint32_t rom_capacity(void) { return ROM_MAX; }

/* model: 0 automatic, 1 DMG, 2 CGB (GB_MODEL_*). */
EXPORT("load") int load(uint32_t size, uint32_t model) {
    int err = gb_load_model(&gb, rom, size, (int)model);
    if (!err) gb_title(&gb, title);
    return err;
}

/* Converts the last frame to RGBA: DMG shades through the current palette,
 * CGB colours from RGB555 with each channel widened as (c << 3) | (c >> 2). */
EXPORT("render") void render(void) {
    for (int y = 0; y < GB_HEIGHT; y++)
        for (int x = 0; x < GB_WIDTH; x++) {
            uint32_t v = gb.pixels[y][x];
            if (!gb.cgb) {
                screen[y * GB_WIDTH + x] = palette[v & 3];
                continue;
            }
            uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            r = r << 3 | r >> 2;
            g = g << 3 | g >> 2;
            b = b << 3 | b >> 2;
            screen[y * GB_WIDTH + x] = 0xFF000000u | b << 16 | g << 8 | r;
        }
}

EXPORT("is_cgb") uint32_t is_cgb(void) { return gb.cgb; }

EXPORT("run_frame") void run_frame(void) {
    gb_run_frame(&gb);
    render();
}

EXPORT("screen") uint32_t *screen_ptr(void) { return screen; }
EXPORT("palette") uint32_t *palette_ptr(void) { return palette; }
EXPORT("set_buttons") void set_buttons(uint32_t pressed) { gb_set_buttons(&gb, (uint8_t)pressed); }
EXPORT("title") const char *title_ptr(void) { return title; }

/* Sound: after set_sample_rate(), each audio_take() moves the samples made
 * since the last call into audio_buffer() (interleaved 16-bit stereo) and
 * returns how many stereo frames it moved. */
EXPORT("set_sample_rate") void set_sample_rate(uint32_t rate) { gb_set_sample_rate(&gb, rate); }
EXPORT("audio_buffer") int16_t *audio_buffer(void) { return audio; }
EXPORT("audio_take") uint32_t audio_take(void) { return (uint32_t)gb_audio_take(&gb, audio, GB_AUDIO_MAX); }

/* Battery-backed cartridge RAM, so the page can keep saves in localStorage. */
EXPORT("save_ram") uint8_t *save_ram(void) { return gb.cart_ram; }
EXPORT("save_size") uint32_t save_size(void) { return gb.has_battery ? gb.cart_ram_size : 0; }
