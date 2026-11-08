/* The picture processing unit. Timing follows the scanline: OAM scan for 80
 * dots, drawing for 172 or more (scrolling, the window and sprites stall it),
 * H-blank for the rest of the 456; lines 144-153 are V-blank. A whole line
 * is rendered when drawing starts, which is enough for games that change
 * registers between lines (as dmg-acid2 does); mode timings are exact to the
 * M-cycle as the mooneye tests measure them. */
#include "internal.h"

#define MODE_HBLANK 0
#define MODE_VBLANK 1
#define MODE_OAM 2
#define MODE_DRAW 3

#define DOTS_PER_LINE 456
#define DRAW_START 80

static void set_mode(gb_t *g, int mode) {
    g->io[R_STAT] = (uint8_t)((g->io[R_STAT] & ~3) | mode);
}

/* The STAT interrupt fires on a rising edge of the OR of its sources. */
void ppu_update_stat(gb_t *g) {
    uint8_t stat = g->io[R_STAT];
    /* With the LCD off the comparison clock stops: the flag and the
     * interrupt line keep their last state. */
    if (!(g->io[R_LCDC] & 0x80)) return;
    /* While LY is changing (the last M-cycle of a line) the comparison reads
     * false; it is made again when the new line starts. */
    int coincidence = g->io[R_LY] == g->io[R_LYC] && g->dot != DOTS_PER_LINE - 4;
    stat = coincidence ? (stat | 4) : (stat & ~4);
    g->io[R_STAT] = stat;

    int mode = stat & 3;
    /* On the DMG the mode 2 source also fires as line 144 (V-blank) begins. */
    int oam = mode == MODE_OAM || (mode == MODE_VBLANK && g->line == GB_HEIGHT && g->dot == 0);
    int line = ((stat & 0x40) && coincidence) || ((stat & 0x08) && mode == MODE_HBLANK) ||
               ((stat & 0x10) && mode == MODE_VBLANK) || ((stat & 0x20) && oam);
    if (line && !g->stat_line) g->io[R_IF] |= INT_STAT;
    g->stat_line = (uint8_t)line;
}

void ppu_reset(gb_t *g) {
    g->dot = 0;
    g->window_line = 0;
    g->io[R_LY] = 0;
    g->line = 0;
    set_mode(g, MODE_OAM);
    ppu_update_stat(g);
}

void ppu_write_lcdc(gb_t *g, uint8_t v) {
    uint8_t was_on = g->io[R_LCDC] & 0x80;
    g->io[R_LCDC] = v;
    if (was_on && !(v & 0x80)) {
        g->dot = 0;
        g->io[R_LY] = 0;
        g->line = 0;
        g->window_line = 0;
        set_mode(g, MODE_HBLANK);
        mem_fill(g->pixels, 0, sizeof g->pixels);
        ppu_update_stat(g);
    } else if (!was_on && (v & 0x80)) {
        /* The first line after switching on skips mode 2: it shows mode 0
         * until drawing starts. */
        g->dot = 0;
        g->io[R_LY] = 0;
        g->line = 0;
        g->window_line = 0;
        set_mode(g, MODE_HBLANK);
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

/* Extra mode 3 dots for the sprites on a line, which must be sorted from left
 * to right (Pan Docs, "OBJ penalty algorithm"). */
static int sprite_penalty(const gb_t *g, const int *sorted, int count, int window_x) {
    int penalty = 0, last_tile = 0x7FFF;
    for (int k = 0; k < count; k++) {
        int ox = g->oam[sorted[k] * 4 + 1];
        if (ox >= 168) continue; /* entirely off the right side: never fetched */
        /* The tile under the sprite's leftmost pixel, and its pixels to the
         * right. Sprites at X = 0 wait as if 7 pixels were left, whatever SCX. */
        int x = ox - 8, u = x >= window_x ? x - window_x : x + g->io[R_SCX];
        int tile = (u >> 3) + (x >= window_x ? 0x1000 : 0);
        if (ox == 0) {
            u = 0;
            tile = -0x1000;
        }
        if (tile != last_tile) {
            int right = 7 - (u & 7) - 2;
            if (right > 0) penalty += right;
            last_tile = tile;
        }
        penalty += 6;
    }
    return penalty;
}

/* Renders the current line and returns how long mode 3 lasts, in dots. */
static int render_line(gb_t *g) {
    uint8_t lcdc = g->io[R_LCDC];
    int ly = g->line;
    uint8_t color[GB_WIDTH] = {0}; /* background/window colour numbers */
    uint8_t *out = g->pixels[ly];
    int length = 172 + (g->io[R_SCX] & 7), window_x = GB_WIDTH;

    if (lcdc & 0x01) {
        uint16_t map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
        int y = (ly + g->io[R_SCY]) & 0xFF;
        uint16_t row = 0;
        for (int x = 0; x < GB_WIDTH; x++) {
            int px = (x + g->io[R_SCX]) & 0xFF;
            if (x == 0 || (px & 7) == 0) row = bg_tile_row(g, map, px >> 3, y >> 3, y & 7);
            color[x] = (uint8_t)tile_pixel(g, row, px & 7);
        }

        int wx = g->io[R_WX] - 7;
        if ((lcdc & 0x20) && g->io[R_WY] <= ly && wx < GB_WIDTH) {
            uint16_t wmap = (lcdc & 0x40) ? 0x1C00 : 0x1800;
            int wy = g->window_line;
            for (int x = wx < 0 ? 0 : wx; x < GB_WIDTH; x++) {
                int px = x - wx;
                if (x == wx || x == 0 || (px & 7) == 0) row = bg_tile_row(g, wmap, px >> 3, wy >> 3, wy & 7);
                color[x] = (uint8_t)tile_pixel(g, row, px & 7);
            }
            g->window_line++; /* the window has its own line counter */
            window_x = wx;
            length += 6; /* setting up the fetcher for the window */
        }
    }

    uint8_t bgp = g->io[R_BGP];
    for (int x = 0; x < GB_WIDTH; x++) out[x] = (bgp >> (color[x] * 2)) & 3;

    if (!(lcdc & 0x02)) return length;

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
    /* Pan Docs' sprite penalty, minus 3 dots: that offset makes mode 0 start
     * on the M-cycle the hardware does in every case of mooneye's
     * intr_2_mode0_timing_sprites. */
    int penalty = sprite_penalty(g, chosen, count, window_x);
    if (penalty > 3) length += penalty - 3;

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
    return length;
}

/* Advances one M-cycle (4 dots). Mode 0 starts on the first M-cycle at or
 * after the end of mode 3, whose length varies. */
void ppu_tick(gb_t *g) {
    if (!(g->io[R_LCDC] & 0x80)) return;
    int ly = g->line;
    g->dot += 4;
    if (ly < GB_HEIGHT) {
        if (g->dot == DRAW_START) {
            set_mode(g, MODE_DRAW);
            g->hblank_dot = (uint16_t)(DRAW_START + render_line(g));
        } else if ((g->io[R_STAT] & 3) == MODE_DRAW && g->dot >= g->hblank_dot) {
            set_mode(g, MODE_HBLANK);
            ppu_update_stat(g);
        }
    }
    if (g->dot == DOTS_PER_LINE - 4) {
        /* LY moves on one M-cycle before the line ends. */
        g->io[R_LY] = (uint8_t)(ly == 153 ? 0 : ly + 1);
        ppu_update_stat(g);
        return;
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
    g->line = (uint8_t)ly;
    ppu_update_stat(g);
}

/* ---------- the DMG OAM corruption bug ---------- */

/* The OAM row (8 bytes, one per M-cycle) the mode 2 scan is reading, or -1
 * outside it. The CPU sees the PPU as of the start of the M-cycle, by which
 * point the scan has moved on to the next row (Blargg's oam_bug timings). */
static int oam_scan_row(const gb_t *g) {
    if (!(g->io[R_LCDC] & 0x80) || (g->io[R_STAT] & 3) != MODE_OAM) return -1;
    return g->dot / 4 + 1;
}

static uint16_t oam_word(const gb_t *g, int row, int word) {
    int i = row * 8 + word * 2;
    return (uint16_t)(g->oam[i] | g->oam[i + 1] << 8);
}

static void set_oam_word(gb_t *g, int row, int word, uint16_t v) {
    int i = row * 8 + word * 2;
    g->oam[i] = v & 0xFF;
    g->oam[i + 1] = v >> 8;
}

static void copy_row(gb_t *g, int to, int from) {
    for (int i = 0; i < 8; i++) g->oam[to * 8 + i] = g->oam[from * 8 + i];
}

/* A CPU access (or a 16-bit increment of a register pointing at OAM) while
 * the PPU scans OAM garbles the row being scanned (Pan Docs, "OAM Corruption
 * Bug"). The address and the value do not matter, only the kind of access. */
void ppu_oam_bug(gb_t *g, int kind) {
    int row = oam_scan_row(g);
    if (row < 1 || row >= 20) return;
    if (kind == OAM_BUG_IDU_DURING_READ) {
        /* A read and an increment in the same M-cycle mix the rows above;
         * the plain read corruption follows with the read itself. */
        if (row >= 4 && row < 19) {
            uint16_t a = oam_word(g, row - 2, 0), b = oam_word(g, row - 1, 0);
            uint16_t c = oam_word(g, row, 0), d = oam_word(g, row - 1, 2);
            set_oam_word(g, row - 1, 0, (uint16_t)((b & (a | c | d)) | (a & c & d)));
            copy_row(g, row, row - 1);
            copy_row(g, row - 2, row - 1);
        }
        return;
    }
    uint16_t a = oam_word(g, row, 0), b = oam_word(g, row - 1, 0), c = oam_word(g, row - 1, 2);
    set_oam_word(g, row, 0, kind == OAM_BUG_WRITE ? (uint16_t)(((a ^ c) & (b ^ c)) ^ c) : (uint16_t)(b | (a & c)));
    for (int w = 1; w < 4; w++) set_oam_word(g, row, w, oam_word(g, row - 1, w));
}
