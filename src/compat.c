/* Colours for original Game Boy games on a Game Boy Color: the palettes the
 * CGB boot ROM picks from the cartridge header (Pan Docs, "Compatibility
 * palettes"). Games not published by Nintendo all get the default set.
 *
 * The tables come from SameBoy's free CGB boot ROM, BootROMs/cgb_boot.asm
 * (commit 1d7692c), leaving out its SameBoy-only palettes:
 *
 *   MIT License, Copyright (c) 2015-2021 Lior Halphon
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit
 *   persons to whom the Software is furnished to do so, subject to the
 *   following conditions: The above copyright notice and this permission
 *   notice shall be included in all copies or substantial portions of the
 *   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */
#include "internal.h"

/* Sums of the 16 title bytes. From FIRST_DUPLICATE on, the same sum appears
 * twice and the title's 4th letter (fourth_letters) tells the games apart. */
#define FIRST_DUPLICATE 65
static const uint8_t title_sums[94] = {
    0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
    0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
    0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
    0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0xC3, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
    0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
    0x5D, 0x6D, 0x67, 0x3F, 0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
    0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6,
    0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};
static const char fourth_letters[] = "BEFAARBEKEK R-URAR INAILICE R";

/* Which combination each title sum uses. */
static const uint8_t combination[94] = {
    0, 4, 5, 35, 34, 3, 31, 15, 10, 5, 19, 36,
    7, 37, 30, 44, 21, 32, 31, 20, 5, 33, 13, 14,
    5, 29, 5, 18, 9, 3, 2, 26, 25, 25, 41, 42,
    26, 45, 42, 45, 36, 38, 26, 42, 30, 41, 34, 34,
    5, 42, 6, 5, 33, 25, 42, 42, 40, 2, 16, 25,
    42, 42, 5, 0, 39, 36, 22, 25, 6, 32, 12, 36,
    11, 39, 18, 39, 24, 31, 50, 17, 46, 6, 27, 0,
    47, 41, 41, 0, 0, 19, 34, 23, 18, 29,
};

/* Combinations: offsets into colours[] for OBJ palette 0, OBJ palette 1 and
 * BG palette 0. */
static const uint8_t combinations[51][3] = {
    {16, 16, 116}, {72, 72, 72}, {80, 80, 80}, {96, 96, 96}, {36, 36, 36}, {0, 0, 0},
    {108, 108, 108}, {20, 20, 20}, {48, 48, 48}, {104, 104, 104}, {64, 32, 32}, {16, 112, 112},
    {16, 8, 8}, {12, 16, 16}, {16, 116, 116}, {112, 16, 112}, {8, 68, 8}, {64, 64, 32},
    {16, 16, 28}, {16, 16, 72}, {16, 16, 80}, {76, 76, 36}, {15, 15, 44}, {68, 68, 8},
    {16, 16, 8}, {16, 16, 12}, {112, 112, 0}, {12, 12, 0}, {0, 0, 4}, {72, 88, 72},
    {80, 88, 80}, {96, 88, 96}, {64, 88, 32}, {68, 16, 52}, {111, 0, 56}, {111, 16, 60},
    {76, 88, 36}, {64, 112, 40}, {16, 92, 112}, {68, 88, 8}, {16, 0, 8}, {16, 112, 12},
    {112, 12, 0}, {12, 112, 16}, {84, 112, 16}, {12, 112, 0}, {100, 12, 112}, {0, 112, 32},
    {16, 12, 112}, {112, 12, 24}, {16, 112, 116},
};

/* RGB555 colours, four per palette. */
static const uint16_t colours[120] = {
    0x7FFF, 0x32BF, 0x00D0, 0x0000, 0x639F, 0x4279, 0x15B0, 0x04CB,
    0x7FFF, 0x6E31, 0x454A, 0x0000, 0x7FFF, 0x1BEF, 0x0200, 0x0000,
    0x7FFF, 0x421F, 0x1CF2, 0x0000, 0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x03FF, 0x012F, 0x0000, 0x7FFF, 0x03EF, 0x01D6, 0x0000,
    0x7FFF, 0x42B5, 0x3DC8, 0x0000, 0x7E74, 0x03FF, 0x0180, 0x0000,
    0x67FF, 0x77AC, 0x1A13, 0x2D6B, 0x7ED6, 0x4BFF, 0x2175, 0x0000,
    0x53FF, 0x4A5F, 0x7E52, 0x0000, 0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0,
    0x03ED, 0x7FFF, 0x255F, 0x0000, 0x036A, 0x021F, 0x03FF, 0x7FFF,
    0x7FFF, 0x01DF, 0x0112, 0x0000, 0x231F, 0x035F, 0x00F2, 0x0009,
    0x7FFF, 0x03EA, 0x011F, 0x0000, 0x299F, 0x001A, 0x000C, 0x0000,
    0x7FFF, 0x027F, 0x001F, 0x0000, 0x7FFF, 0x03E0, 0x0206, 0x0120,
    0x7FFF, 0x7EEB, 0x001F, 0x7C00, 0x7FFF, 0x3FFF, 0x7E00, 0x001F,
    0x7FFF, 0x03FF, 0x001F, 0x0000, 0x03FF, 0x001F, 0x000C, 0x0000,
    0x7FFF, 0x033F, 0x0193, 0x0000, 0x0000, 0x4200, 0x037F, 0x7FFF,
    0x7FFF, 0x7E8C, 0x7C00, 0x0000, 0x7FFF, 0x1BEF, 0x6180, 0x0000,
};

static int palette_set(const uint8_t *rom) {
    /* Only games whose licensee is Nintendo get their own colours. */
    int nintendo = rom[0x14B] == 0x33 ? rom[0x144] == '0' && rom[0x145] == '1' : rom[0x14B] == 0x01;
    if (!nintendo) return 0;
    uint8_t sum = 0;
    for (int i = 0x134; i < 0x144; i++) sum = (uint8_t)(sum + rom[i]);
    for (int i = 0; i < (int)sizeof title_sums; i++) {
        if (title_sums[i] != sum) continue;
        if (i >= FIRST_DUPLICATE && fourth_letters[i - FIRST_DUPLICATE] != rom[0x137]) continue;
        return combination[i];
    }
    return 0;
}

static void load(uint8_t *ram, int offset) {
    for (int i = 0; i < 4; i++) {
        ram[i * 2] = colours[offset + i] & 0xFF;
        ram[i * 2 + 1] = colours[offset + i] >> 8;
    }
}

void cgb_compat_palettes(gb_t *g) {
    const uint8_t *set = combinations[palette_set(g->rom)];
    load(g->obj_palette, set[0]);
    load(g->obj_palette + 8, set[1]);
    load(g->bg_palette, set[2]);
}
