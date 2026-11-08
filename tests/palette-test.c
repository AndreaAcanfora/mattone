/* Checks the colours the CGB boot ROM gives original Game Boy games (src/compat.c)
 * on synthetic headers: the default set, a Nintendo title, and two Nintendo
 * titles with the same checksum told apart by their 4th letter. */
#include <stdio.h>
#include <string.h>

#include "../src/gb.h"

static unsigned char rom[0x8000];
static gb_t gb;

static int check(const char *title, int nintendo, unsigned bg1, unsigned bg2, unsigned obj1_1) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x134, title, strlen(title));
    rom[0x14B] = nintendo ? 0x01 : 0x00; /* old licensee code: Nintendo or none */
    unsigned char sum = 0;
    for (int i = 0x134; i <= 0x14C; i++) sum = (unsigned char)(sum - rom[i] - 1);
    rom[0x14D] = sum;
    gb_load_model(&gb, rom, sizeof rom, GB_MODEL_CGB);
    unsigned got1 = gb.bg_palette[2] | gb.bg_palette[3] << 8, got2 = gb.bg_palette[4] | gb.bg_palette[5] << 8;
    unsigned got_obj = gb.obj_palette[10] | gb.obj_palette[11] << 8;
    int ok = got1 == bg1 && got2 == bg2 && got_obj == obj1_1;
    printf("  %s  %-13s BG %04X %04X, OBJ1 %04X\n", ok ? "pass" : "FAIL", title, got1, got2, got_obj);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= check("HOMEBREW", 0, 0x1BEF, 0x6180, 0x421F);     /* not Nintendo: the default set */
    ok &= check("TETRIS", 0, 0x1BEF, 0x6180, 0x421F);       /* the title alone is not enough */
    ok &= check("TETRIS", 1, 0x03FF, 0x001F, 0x03FF);
    ok &= check("POKEMON BLUE", 1, 0x7E8C, 0x7C00, 0x7E8C); /* same title sum as VEGAS STAKES */
    ok &= check("VEGAS STAKES", 1, 0x1BEF, 0x0200, 0x7E8C);
    return ok ? 0 : 1;
}
