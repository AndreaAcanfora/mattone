/* The picture processing unit. Timing follows the scanline: OAM scan for 80
 * dots, drawing for 172, H-blank for the rest of the 456; lines 144-153 are
 * V-blank. A whole line is rendered when drawing starts, which is enough for
 * games that change registers between lines (as dmg-acid2 does). */
#include "internal.h"

#define MODE_HBLANK 0
#define MODE_VBLANK 1
#define MODE_OAM 2
#define MODE_DRAW 3

#define DOTS_PER_LINE 456
#define DRAW_START 80
#define HBLANK_START 252

static void set_mode(gb_t *g, int mode) {
    g->io[R_STAT] = (uint8_t)((g->io[R_STAT] & ~3) | mode);
}

/* The STAT interrupt fires on a rising edge of the OR of its sources. */
void ppu_update_stat(gb_t *g) {
    uint8_t stat = g->io[R_STAT];
    if (!(g->io[R_LCDC] & 0x80)) {
        g->stat_line = 0;
        return;
    }
    int coincidence = g->io[R_LY] == g->io[R_LYC];
    stat = coincidence ? (stat | 4) : (stat & ~4);
    g->io[R_STAT] = stat;

    int mode = stat & 3;
    int line = ((stat & 0x40) && coincidence) || ((stat & 0x08) && mode == MODE_HBLANK) ||
               ((stat & 0x10) && mode == MODE_VBLANK) || ((stat & 0x20) && mode == MODE_OAM);
    if (line && !g->stat_line) g->io[R_IF] |= INT_STAT;
    g->stat_line = (uint8_t)line;
}

void ppu_reset(gb_t *g) {
    g->dot = 0;
    g->window_line = 0;
    g->io[R_LY] = 0;
    set_mode(g, MODE_OAM);
    ppu_update_stat(g);
}

void ppu_write_lcdc(gb_t *g, uint8_t v) {
    uint8_t was_on = g->io[R_LCDC] & 0x80;
    g->io[R_LCDC] = v;
    if (was_on && !(v & 0x80)) {
        g->dot = 0;
        g->io[R_LY] = 0;
        g->window_line = 0;
        set_mode(g, MODE_HBLANK);
        mem_fill(g->pixels, 0, sizeof g->pixels);
        ppu_update_stat(g);
    } else if (!was_on && (v & 0x80)) {
        g->dot = 0;
        g->io[R_LY] = 0;
        g->window_line = 0;
        set_mode(g, MODE_OAM);
        ppu_update_stat(g);
    }
}

/* Colour number (0-3) of pixel (x, y) of a tile row starting at vram offset. */
static int tile_pixel(const gb_t *g, uint16_t row_addr, int x) {
    uint8_t lo = g->vram[row_addr], hi = g->vram[row_addr + 1];
    int bit = 7 - x;
    return ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
}

static uint16_t bg_tile_row(const gb_t *g, uint16_t map, int tx, int ty, int row) {
    uint8_t tile = g->vram[map + ty * 32 + tx];
    if (g->io[R_LCDC] & 0x10) return (uint16_t)(tile * 16 + row * 2);
    return (uint16_t)(0x1000 + (int8_t)tile * 16 + row * 2);
}

static void render_line(gb_t *g) {
    uint8_t lcdc = g->io[R_LCDC];
    int ly = g->io[R_LY];
    uint8_t color[GB_WIDTH] = {0}; /* background/window colour numbers */
    uint8_t *out = g->pixels[ly];

    if (lcdc & 0x01) {
        uint16_t map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
        int y = (ly + g->io[R_SCY]) & 0xFF;
        for (int x = 0; x < GB_WIDTH; x++) {
            int px = (x + g->io[R_SCX]) & 0xFF;
            color[x] = (uint8_t)tile_pixel(g, bg_tile_row(g, map, px >> 3, y >> 3, y & 7), px & 7);
        }

        int wx = g->io[R_WX] - 7;
        if ((lcdc & 0x20) && g->io[R_WY] <= ly && wx < GB_WIDTH) {
            uint16_t wmap = (lcdc & 0x40) ? 0x1C00 : 0x1800;
            int wy = g->window_line;
            for (int x = wx < 0 ? 0 : wx; x < GB_WIDTH; x++) {
                int px = x - wx;
                color[x] = (uint8_t)tile_pixel(g, bg_tile_row(g, wmap, px >> 3, wy >> 3, wy & 7), px & 7);
            }
            g->window_line++; /* the window has its own line counter */
        }
    }

    uint8_t bgp = g->io[R_BGP];
    for (int x = 0; x < GB_WIDTH; x++) out[x] = (bgp >> (color[x] * 2)) & 3;

    if (!(lcdc & 0x02)) return;

    /* Pick the first ten sprites on this line, in OAM order. */
    int height = (lcdc & 0x04) ? 16 : 8;
    int chosen[10], count = 0;
    for (int i = 0; i < 40 && count < 10; i++) {
        int sy = g->oam[i * 4] - 16;
        if (ly >= sy && ly < sy + height) chosen[count++] = i;
    }

    /* Draw from the lowest priority up, so the winner is drawn last. A lower
     * X wins; on a tie the earlier OAM entry wins. Only the winning sprite's
     * pixel counts, even when its BG-priority flag then hides it. */
    for (int a = 1; a < count; a++) {
        int v = chosen[a], b = a - 1;
        while (b >= 0 && g->oam[chosen[b] * 4 + 1] > g->oam[v * 4 + 1]) {
            chosen[b + 1] = chosen[b];
            b--;
        }
        chosen[b + 1] = v;
    }
    int16_t sprite[GB_WIDTH]; /* -1 = none, else shade | behind_bg << 8 */
    for (int x = 0; x < GB_WIDTH; x++) sprite[x] = -1;
    for (int k = count - 1; k >= 0; k--) {
        const uint8_t *s = &g->oam[chosen[k] * 4];
        int sy = s[0] - 16, sx = s[1] - 8, tile = s[2], attr = s[3];
        int row = ly - sy;
        if (attr & 0x40) row = height - 1 - row;
        if (height == 16) tile &= 0xFE;
        uint16_t addr = (uint16_t)(tile * 16 + row * 2);
        uint8_t pal = g->io[(attr & 0x10) ? R_OBP1 : R_OBP0];
        for (int i = 0; i < 8; i++) {
            int x = sx + i;
            if (x < 0 || x >= GB_WIDTH) continue;
            int c = tile_pixel(g, addr, (attr & 0x20) ? 7 - i : i);
            if (!c) continue;
            sprite[x] = (int16_t)(((pal >> (c * 2)) & 3) | ((attr & 0x80) ? 0x100 : 0));
        }
    }
    for (int x = 0; x < GB_WIDTH; x++) {
        if (sprite[x] < 0) continue;
        if ((sprite[x] & 0x100) && color[x]) continue;
        out[x] = sprite[x] & 3;
    }
}

static void ppu_dot(gb_t *g) {
    int ly = g->io[R_LY];
    g->dot++;
    if (ly < GB_HEIGHT) {
        if (g->dot == DRAW_START) {
            set_mode(g, MODE_DRAW);
            render_line(g);
        } else if (g->dot == HBLANK_START) {
            set_mode(g, MODE_HBLANK);
            ppu_update_stat(g);
        }
    }
    if (g->dot < DOTS_PER_LINE) return;

    g->dot = 0;
    ly++;
    if (ly == GB_HEIGHT) {
        set_mode(g, MODE_VBLANK);
        g->io[R_IF] |= INT_VBLANK;
        g->frame_done = 1;
    } else if (ly == 154) {
        ly = 0;
        g->window_line = 0;
    }
    if (ly < GB_HEIGHT) set_mode(g, MODE_OAM);
    g->io[R_LY] = (uint8_t)ly;
    ppu_update_stat(g);
}

void ppu_tick(gb_t *g) {
    if (!(g->io[R_LCDC] & 0x80)) return;
    for (int i = 0; i < 4; i++) ppu_dot(g);
}
