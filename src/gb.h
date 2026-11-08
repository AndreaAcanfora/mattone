/* mattone: a Game Boy (DMG) emulator core in C11.
 *
 * The core does no allocation and uses no libc, so it builds both natively
 * and as a freestanding WebAssembly module. The caller owns the gb_t and the
 * ROM buffer; the ROM must stay alive while the emulator runs. */
#ifndef MATTONE_GB_H
#define MATTONE_GB_H

#include <stddef.h>
#include <stdint.h>

#define GB_WIDTH 160
#define GB_HEIGHT 144
#define GB_CYCLES_PER_FRAME 70224 /* T-cycles: 154 lines * 456 dots */
#define GB_CART_RAM_MAX 0x20000
#define GB_SERIAL_MAX 4096
#define GB_AUDIO_MAX 4096 /* stereo sample frames buffered between gb_audio_take() calls */

/* Bits for gb_set_buttons(): set = pressed. */
enum {
    GB_RIGHT = 1 << 0,
    GB_LEFT = 1 << 1,
    GB_UP = 1 << 2,
    GB_DOWN = 1 << 3,
    GB_A = 1 << 4,
    GB_B = 1 << 5,
    GB_SELECT = 1 << 6,
    GB_START = 1 << 7,
};

/* Errors returned by gb_load(). */
enum {
    GB_OK = 0,
    GB_ERR_SIZE = -1,     /* smaller than the header or larger than 8 MiB */
    GB_ERR_CART = -2,     /* unsupported cartridge type */
    GB_ERR_HEADER = -3,   /* bad header checksum: the boot ROM would refuse it */
};

typedef struct {
    uint8_t enabled, dac;
    uint16_t length;
    uint8_t volume, env_timer;
    int32_t timer; /* T-cycles until the next waveform step */
    uint8_t duty_pos;
    uint16_t shadow; /* square 1: sweep */
    uint8_t sweep_timer, sweep_enabled, sweep_negated;
    uint8_t wave_pos, sample; /* wave */
    uint16_t lfsr;            /* noise */
} gb_channel;

typedef struct {
    uint8_t power, fs_step, wave_read, dirty; /* dirty: mix_left/right need recomputing */
    gb_channel ch[4];
    uint32_t sample_rate, sample_clock;
    int32_t mix_left, mix_right, acc_left, acc_right;
    float cap_left, cap_right, charge;
    uint32_t acc_count, audio_len;
    int16_t audio[GB_AUDIO_MAX * 2]; /* interleaved left, right */
} gb_apu;

typedef struct gb {
    /* CPU */
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t ime, ei_pending, halted, halt_bug;

    /* memory */
    const uint8_t *rom;
    uint32_t rom_size;
    uint8_t vram[0x2000];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    uint8_t io[0x80];
    uint8_t ie;

    /* cartridge */
    uint8_t cart_type, mbc, has_battery, has_rtc;
    uint8_t ram_enabled, mbc1_mode, bank_lo, bank_hi, ram_bank;
    uint16_t rom_bank;
    uint32_t cart_ram_size;
    uint8_t cart_ram[GB_CART_RAM_MAX];
    uint8_t rtc[5], rtc_latched[5], rtc_latch_prev;
    uint32_t rtc_cycles;

    /* timer */
    uint16_t sysclk;     /* DIV is the upper byte */
    uint8_t tima_reload;   /* M-cycles until TIMA reloads from TMA after overflow */
    uint8_t tima_reloaded; /* set during the M-cycle in which the reload happened */

    /* PPU */
    uint16_t dot;
    uint8_t window_line, stat_line, frame_done;
    uint8_t pixels[GB_HEIGHT][GB_WIDTH]; /* shades 0 (white) .. 3 (black) */

    /* OAM DMA */
    uint8_t dma_active, dma_index, dma_delay;
    uint16_t dma_source;

    gb_apu apu;

    /* joypad and serial */
    uint8_t buttons;
    uint32_t serial_len;
    uint8_t serial[GB_SERIAL_MAX]; /* bytes sent through the link port */

    uint64_t cycles; /* T-cycles since power on */
} gb_t;

/* Resets the machine to the state the boot ROM leaves it in and inserts the
 * cartridge. Returns GB_OK or a GB_ERR_* code. */
int gb_load(gb_t *gb, const uint8_t *rom, size_t size);

/* Runs one instruction (or one interrupt dispatch / halted M-cycle).
 * Returns the number of T-cycles it took. */
int gb_step(gb_t *gb);

/* Runs until the PPU finishes a frame (or one frame's worth of cycles while
 * the LCD is off). */
void gb_run_frame(gb_t *gb);

void gb_set_buttons(gb_t *gb, uint8_t pressed);

/* Turns on audio output at the given sample rate (0 = off, the default after
 * gb_load). Samples are signed 16-bit stereo. */
void gb_set_sample_rate(gb_t *gb, uint32_t rate);

/* Moves up to max_frames buffered stereo frames into out (2 * max_frames
 * values) and returns how many were moved. Call it every frame: once
 * GB_AUDIO_MAX frames are waiting, new samples are dropped. */
size_t gb_audio_take(gb_t *gb, int16_t *out, size_t max_frames);

/* The cartridge title from the header, NUL terminated, max 16 chars. */
void gb_title(const gb_t *gb, char out[17]);

#endif
