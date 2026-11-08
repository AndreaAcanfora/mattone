/* mattone-run: runs a ROM without a window, for tests and screenshots.
 *
 *   mattone-run [--frames N] [--serial] [--until TEXT] [--screenshot FILE]
 *               [--input FILE] [--dump DIR] [--wav FILE.wav] [--mooneye] [--dmg | --cgb] ROM
 *
 * --serial prints the text the ROM reports: what it sent through the link
 * port, or, for test ROMs that use Blargg's memory protocol (signature
 * DE B0 61 at $A001), the text at $A004. --until stops as soon as TEXT appears
 * in that output.
 * --input reads lines "FRAME BUTTONS": from that frame on the buttons
 * (comma-separated: up,down,left,right,a,b,start,select, or "-" for none)
 * are held. --dump writes every frame to DIR/00000.pgm, DIR/00001.pgm...
 * --wav records the sound as 48 kHz 16-bit stereo.
 * Screenshots and dumps are PGM (DMG) or PPM (CGB) images. --dmg and --cgb
 * choose the machine; by default CGB games run on a CGB, the rest on a DMG.
 * --mooneye runs until the ROM executes LD B,B, the end marker of the
 * mooneye test suite, then passes if B C D E H L hold 3 5 8 13 21 34.
 * The exit status is 0 unless loading fails, or --until was given and TEXT
 * never appeared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* Writes the screen: a binary PGM with the four DMG shades, or for CGB games
 * a PPM with each 5-bit channel widened as (c << 3) | (c >> 2). */
static int write_screenshot(const gb_t *gb, const char *path) {
    static const unsigned char shade[4] = {0xFF, 0xAA, 0x55, 0x00};
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "%s\n%d %d\n255\n", gb->cgb_hw ? "P6" : "P5", GB_WIDTH, GB_HEIGHT);
    for (int y = 0; y < GB_HEIGHT; y++)
        for (int x = 0; x < GB_WIDTH; x++) {
            unsigned v = gb->pixels[y][x];
            if (!gb->cgb_hw) {
                fputc(shade[v & 3], f);
                continue;
            }
            for (int ch = 0; ch < 3; ch++) {
                unsigned c = (v >> (5 * ch)) & 0x1F;
                fputc((int)(c << 3 | c >> 2), f);
            }
        }
    return fclose(f);
}

/* Everything sent through the link port, moved out of gb->serial each frame. */
static unsigned char *link_out;
static size_t link_len, link_cap;

static void drain_serial(gb_t *gb) {
    if (!gb->serial_len) return;
    if (link_len + gb->serial_len > link_cap) {
        link_cap = (link_len + gb->serial_len) * 2;
        link_out = realloc(link_out, link_cap);
        if (!link_out) {
            perror("realloc");
            exit(1);
        }
    }
    memcpy(link_out + link_len, gb->serial, gb->serial_len);
    link_len += gb->serial_len;
    gb->serial_len = 0;
}

/* The ROM's report: link-port bytes, or the NUL-terminated text at $A004. */
static const unsigned char *output(const gb_t *gb, size_t *len) {
    const unsigned char *ram = gb->cart_ram;
    if (gb->cart_ram_size >= 0x2000 && ram[1] == 0xDE && ram[2] == 0xB0 && ram[3] == 0x61) {
        size_t n = 0;
        while (4 + n < 0x2000 && ram[4 + n]) n++;
        *len = n;
        return ram + 4;
    }
    *len = link_len;
    return link_out;
}

#define WAV_RATE 48000

static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* Writes the RIFF header for frames stereo 16-bit frames at WAV_RATE. */
static void wav_header(FILE *f, uint32_t frames) {
    unsigned char h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0........\x04\0\x10\0data....";
    put32(h + 4, 36 + frames * 4);
    put32(h + 24, WAV_RATE);
    put32(h + 28, WAV_RATE * 4);
    put32(h + 40, frames * 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

static void wav_append(gb_t *gb, FILE *f, uint32_t *frames) {
    int16_t buf[GB_AUDIO_MAX * 2];
    size_t n = gb_audio_take(gb, buf, GB_AUDIO_MAX);
    for (size_t i = 0; i < n * 2; i++) {
        uint16_t v = (uint16_t)buf[i];
        fputc(v & 0xFF, f);
        fputc(v >> 8, f);
    }
    *frames += (uint32_t)n;
}

static int contains(const gb_t *gb, const char *text) {
    size_t len, n = strlen(text);
    const unsigned char *out = output(gb, &len);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(out + i, text, n) == 0) return 1;
    return 0;
}

#define MAX_INPUTS 1024
static struct { long frame; uint8_t buttons; } inputs[MAX_INPUTS];
static int input_count;

static int read_inputs(const char *path) {
    static const char *names[8] = {"right", "left", "up", "down", "a", "b", "select", "start"};
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256], list[200];
    long frame;
    while (fgets(line, sizeof line, f) && input_count < MAX_INPUTS) {
        if (line[0] == '#' || sscanf(line, "%ld %199s", &frame, list) != 2) continue;
        uint8_t buttons = 0;
        for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ","))
            for (int b = 0; b < 8; b++)
                if (!strcmp(tok, names[b])) buttons |= (uint8_t)(1 << b);
        inputs[input_count].frame = frame;
        inputs[input_count++].buttons = buttons;
    }
    fclose(f);
    return 0;
}

static void usage(void) {
    fputs("usage: mattone-run [--frames N] [--serial] [--until TEXT] [--screenshot FILE]\n"
          "                   [--input FILE] [--dump DIR] [--wav FILE.wav] [--mooneye] [--dmg | --cgb] ROM\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    long frames = 600;
    int serial = 0, mooneye = 0, model = GB_MODEL_AUTO;
    const char *until = NULL, *shot = NULL, *rom_path = NULL, *dump = NULL, *wav_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--serial")) serial = 1;
        else if (!strcmp(argv[i], "--until") && i + 1 < argc) until = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--mooneye")) mooneye = 1;
        else if (!strcmp(argv[i], "--dmg")) model = GB_MODEL_DMG;
        else if (!strcmp(argv[i], "--cgb")) model = GB_MODEL_CGB;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            if (read_inputs(argv[++i])) {
                perror(argv[i]);
                return 1;
            }
        }
        else if (argv[i][0] == '-' || rom_path) usage();
        else rom_path = argv[i];
    }
    if (!rom_path) usage();

    size_t size;
    unsigned char *rom = read_file(rom_path, &size);
    if (!rom) {
        perror(rom_path);
        return 1;
    }
    static gb_t gb;
    int err = gb_load_model(&gb, rom, size, model);
    if (err) {
        fprintf(stderr, "%s: %s\n", rom_path, err == GB_ERR_CART     ? "unsupported cartridge type"
                                    : err == GB_ERR_HEADER ? "bad header checksum"
                                                           : "bad ROM size");
        return 1;
    }

    FILE *wav = NULL;
    uint32_t wav_frames = 0;
    if (wav_path) {
        if (!(wav = fopen(wav_path, "wb"))) {
            perror(wav_path);
            return 1;
        }
        wav_header(wav, 0);
        gb_set_sample_rate(&gb, WAV_RATE);
    }

    int found = 0, next_input = 0;
    for (long f = 0; f < frames && !found; f++) {
        while (next_input < input_count && inputs[next_input].frame <= f)
            gb_set_buttons(&gb, inputs[next_input++].buttons);
        gb_run_frame(&gb);
        drain_serial(&gb);
        if (wav) wav_append(&gb, wav, &wav_frames);
        if (until) found = contains(&gb, until);
        if (mooneye && gb.breakpoint) break;
        if (dump) {
            char path[4096];
            snprintf(path, sizeof path, "%s/%05ld.%s", dump, f, gb.cgb_hw ? "ppm" : "pgm");
            if (write_screenshot(&gb, path)) {
                perror(path);
                return 1;
            }
        }
    }

    if (serial) {
        size_t len;
        const unsigned char *out = output(&gb, &len);
        fwrite(out, 1, len, stdout);
        if (len && out[len - 1] != '\n') putchar('\n');
    }
    if (wav) {
        wav_header(wav, wav_frames);
        if (fclose(wav)) {
            perror(wav_path);
            return 1;
        }
    }
    if (shot && write_screenshot(&gb, shot)) {
        perror(shot);
        return 1;
    }
    free(rom);
    if (mooneye) {
        int pass = gb.breakpoint && gb.b == 3 && gb.c == 5 && gb.d == 8 && gb.e == 13 && gb.h == 21 && gb.l == 34;
        puts(pass ? "pass" : gb.breakpoint ? "fail" : "fail (no LD B,B)");
        return pass ? 0 : 1;
    }
    return until && !found ? 1 : 0;
}
