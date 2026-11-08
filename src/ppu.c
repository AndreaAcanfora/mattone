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
    /* The mode 2 source also fires for line 144, where V-blank begins: on the
     * DMG with it, on the CGB one M-cycle earlier (mooneye vblank_stat_intr). */
    int oam = mode == MODE_OAM ||
              (g->cgb_hw ? g->line == GB_HEIGHT - 1 && g->dot == DOTS_PER_LINE - 4
                         : mode == MODE_VBLANK && g->line == GB_HEIGHT && g->dot == 0);
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
        g->fifo.drawing = 0;
        set_mode(g, MODE_HBLANK);
        for (int y = 0; y < GB_HEIGHT; y++) /* a blank screen is white */
            for (int x = 0; x < GB_WIDTH; x++) g->pixels[y][x] = g->cgb_hw ? 0x7FFF : 0;
        ppu_update_stat(g);
    } else if (!was_on && (v & 0x80)) {
        /* The first line after switching on skips mode 2: it shows mode 0
         * until drawing starts. */
        g->fifo.lcd_just_on = 1;
        g->dot = 0;
        g->io[R_LY] = 0;
        g->line = 0;
        g->window_line = 0;
        set_mode(g, MODE_HBLANK);
        ppu_update_stat(g);
    }
}



/* ---------- mode 3: the pixel pipeline ---------- */

/* Picks the (at most ten) sprites on this line, in OAM order, then sorts them
 * by X; equal X keeps OAM order, which is also the hardware's priority. */
static void oam_scan(gb_t *g) {
    gb_fifo *f = &g->fifo;
    int height = (g->io[R_LCDC] & 0x04) ? 16 : 8;
    f->sprite_count = 0;
    for (int i = 0; i < 40 && f->sprite_count < 10; i++) {
        int sy = g->oam[i * 4] - 16;
        if (g->line >= sy && g->line < sy + height) f->sprites[f->sprite_count++] = (uint8_t)i;
    }
    for (int a = 1; a < f->sprite_count; a++) {
        uint8_t v = f->sprites[a];
        int b = a - 1;
        while (b >= 0 && g->oam[f->sprites[b] * 4 + 1] > g->oam[v * 4 + 1]) {
            f->sprites[b + 1] = f->sprites[b];
            b--;
        }
        f->sprites[b + 1] = v;
    }
    f->sprite_next = 0;
}

static void start_mode3(gb_t *g) {
    gb_fifo *f = &g->fifo;
    f->drawing = 1;
    f->early_hblank = 0;
    f->hblank_in = 0;
    oam_scan(g);
    f->bg_count = 0;
    for (int i = 0; i < 8; i++) f->obj[i] = 0;
    for (int i = 0; i < 4; i++) f->lcd[i].valid = 0;
    f->step = 0;
    f->fetched_first = 0;
    f->tile_x = 0;
    f->window = 0;
    f->window_done_at = 0xFF;
    f->window_skip = 0;
    f->lx = 0;
    f->t = 0;
    f->discard = 0;
    f->sprite_wait = 0;
    f->window_used = 0;
    f->lcd_count = 0;
    f->startup = 0;
    f->last_tile = 0x7FFF;
    f->sprite_seen = 0;
}

/* VRAM address of the current tile row's low byte. */
/* VRAM offset of the fetched tile's row (CGB: bank and vertical flip from
 * the tile's attributes). */
static uint32_t tile_row_addr(const gb_t *g) {
    const gb_fifo *f = &g->fifo;
    int row = f->fetch_window ? g->window_line & 7 : (g->line + g->io[R_SCY]) & 7;
    if (f->attr & 0x40) row = 7 - row;
    uint32_t base = (g->io[R_LCDC] & 0x10) ? (uint32_t)(f->tile * 16) : (uint32_t)(0x1000 + (int8_t)f->tile * 16);
    return (f->attr & 0x08 ? 0x2000u : 0) + base + (uint32_t)(row * 2);
}

static uint8_t reverse_bits(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

/* One dot of the background/window fetcher: tile number, low byte and high
 * byte take two dots each, then it pushes 8 pixels once the FIFO is empty. */
static void fetcher_dot(gb_t *g) {
    gb_fifo *f = &g->fifo;
    uint8_t lcdc = g->io[R_LCDC];
    switch (f->step) {
    case 1: {
        uint16_t map;
        int tx, ty;
        /* Once triggered, the window is fetched while LCDC bit 5 is set; turning
         * it off mid-line goes back to the background at the next tile. */
        if (f->window && !(lcdc & 0x20)) f->window = 0; /* needs WX to match again */
        f->fetch_window = f->window;
        if (f->fetch_window) {
            map = (lcdc & 0x40) ? 0x1C00 : 0x1800;
            tx = f->window_x;
            ty = g->window_line >> 3;
        } else {
            map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
            tx = ((g->io[R_SCX] >> 3) + f->tile_x) & 31;
            ty = ((g->line + g->io[R_SCY]) & 0xFF) >> 3;
        }
        f->tile = g->vram[map + ty * 32 + tx];
        f->attr = g->cgb ? g->vram[0x2000 + map + ty * 32 + tx] : 0;
        break;
    }
    case 3: f->lo = g->vram[tile_row_addr(g)]; break;
    case 5: f->hi = g->vram[tile_row_addr(g) + 1]; break;
    default: break;
    }
    if (f->step < 6) {
        f->step++;
        return;
    }
    if (f->bg_count) return; /* wait for the FIFO to drain */
    /* Pushing takes the dot that starts the next fetch. */
    f->step = 1;
    if (!f->fetched_first) { /* the line's first tile is fetched twice */
        f->fetched_first = 1;
        return;
    }
    f->bg_lo = f->attr & 0x20 ? reverse_bits(f->lo) : f->lo;
    f->bg_hi = f->attr & 0x20 ? reverse_bits(f->hi) : f->hi;
    f->bg_attr = f->attr;
    f->bg_count = 8;
    if (f->window_skip) {
        f->bg_lo = (uint8_t)(f->bg_lo << f->window_skip);
        f->bg_hi = (uint8_t)(f->bg_hi << f->window_skip);
        f->bg_count = (uint8_t)(8 - f->window_skip);
        f->window_skip = 0;
    }
    if (f->fetch_window) f->window_x++;
    else f->tile_x++;
}

/* Fetches a sprite's row and merges it under the sprites already queued. */
static void merge_sprite(gb_t *g, int index) {
    gb_fifo *f = &g->fifo;
    const uint8_t *s = &g->oam[index * 4];
    int height = (g->io[R_LCDC] & 0x04) ? 16 : 8;
    int row = g->line - (s[0] - 16), tile = s[2], attr = s[3];
    if (attr & 0x40) row = height - 1 - row;
    if (height == 16) tile &= 0xFE;
    row &= height - 1;
    uint32_t addr = (g->cgb && (attr & 0x08) ? 0x2000u : 0) + (uint32_t)(tile * 16 + row * 2);
    uint8_t lo = g->vram[addr], hi = g->vram[addr + 1];
    /* The DMG (and the CGB with OPRI set) gives the sprite already queued,
     * which has the lower X, priority; the CGB gives it to the lower OAM index. */
    int by_index = g->cgb && !(g->io[R_OPRI] & 1);
    for (int i = 0; i < 8; i++) {
        int x = s[1] - 8 + i;
        if (x < f->lx || x > f->lx + 7) continue;
        int bit = (attr & 0x20) ? i : 7 - i;
        int c = ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
        if (!c) continue;
        if (f->obj[x & 7] && !(by_index && index < f->obj_index[x & 7])) continue;
        f->obj[x & 7] = (uint8_t)c;
        f->obj_attr[x & 7] = (uint8_t)attr;
        f->obj_index[x & 7] = (uint8_t)index;
    }
}

/* Pixels reach the LCD, where the palettes are applied, 3 dots after they
 * leave the FIFO (mealybug m3_bgp_change). */
#define LCD_DELAY 3

static void output_pixel(gb_t *g) {
    gb_fifo *f = &g->fifo;
    uint8_t lcdc = g->io[R_LCDC];
    int c = (f->bg_hi >> 7) << 1 | f->bg_lo >> 7;
    f->bg_lo = (uint8_t)(f->bg_lo << 1);
    f->bg_hi = (uint8_t)(f->bg_hi << 1);
    f->bg_count--;
    if (f->discard) {
        f->discard--;
        return;
    }
    int slot = f->lx & 7, oc = f->obj[slot], oa = f->obj_attr[slot];
    f->obj[slot] = 0;
    /* Pick the pixel: the colour number, and the palette as 0 BGP, 1 OBP0,
     * 2 OBP1 on the DMG, or 0-7 background / 8-15 sprite on the CGB. */
    int pal, color, sprite;
    if (g->cgb) {
        /* LCDC bit 0 off: sprites always win. Otherwise colour 0 of the
         * background loses, and either priority bit makes the rest win. */
        sprite = oc && (lcdc & 0x02) && (!(lcdc & 0x01) || !c || !((f->bg_attr | oa) & 0x80));
        pal = sprite ? 8 + (oa & 7) : f->bg_attr & 7;
    } else {
        if (!(lcdc & 0x01)) c = 0; /* background and window off show colour 0 */
        sprite = oc && (lcdc & 0x02) && !((oa & 0x80) && c);
        pal = sprite ? 1 + ((oa >> 4) & 1) : 0;
    }
    color = sprite ? oc : c;
    /* lcd_dot() has already run for this dot, so lcd_t is one ahead. */
    int when = (f->lcd_t + LCD_DELAY - 1) & 3;
    f->lcd[when].x = f->lx++;
    f->lcd[when].color = (uint8_t)color;
    f->lcd[when].pal = (uint8_t)pal;
    f->lcd[when].valid = 1;
    f->lcd_count++;
}

/* Puts the pixel that has waited LCD_DELAY dots onto the screen. */
static void lcd_dot(gb_t *g) {
    gb_fifo *f = &g->fifo;
    if (f->glitch_dots && --f->glitch_dots == 0) f->pal_glitch[0] = f->pal_glitch[1] = f->pal_glitch[2] = 0;
    int now = f->lcd_t++ & 3;
    if (!f->lcd[now].valid) return;
    int color = f->lcd[now].color, pal = f->lcd[now].pal;
    if (g->cgb) {
        const uint8_t *ram = pal & 8 ? g->obj_palette : g->bg_palette;
        int i = (pal & 7) * 8 + color * 2;
        g->pixels[g->line][f->lcd[now].x] = (uint16_t)((ram[i] | ram[i + 1] << 8) & 0x7FFF);
    } else {
        /* In the dot after a palette write the DMG sees old | new. */
        uint8_t p = g->io[R_BGP + pal] | f->pal_glitch[pal];
        int shade = (p >> (color * 2)) & 3;
        if (g->cgb_hw) {
            /* Compatibility mode: the shade picks a colour of BG palette 0 or
             * OBJ palette 0/1, which the boot ROM filled in. */
            const uint8_t *ram = pal ? g->obj_palette + (pal - 1) * 8 : g->bg_palette;
            g->pixels[g->line][f->lcd[now].x] = (uint16_t)((ram[shade * 2] | ram[shade * 2 + 1] << 8) & 0x7FFF);
        } else {
            g->pixels[g->line][f->lcd[now].x] = (uint16_t)shade;
        }
    }
    f->lcd[now].valid = 0;
    f->lcd_count--;
}

/* Dots a sprite at OAM X stalls the pipeline: Pan Docs' penalty, waiting for
 * the background fetch to finish its tile (once per tile), then 6 dots. */
static int sprite_stall(const gb_t *g, int sx, int16_t *last_tile) {
    const gb_fifo *f = &g->fifo;
    int x = sx - 8, u = f->window ? x - f->window_start : x + g->io[R_SCX];
    int tile = (u >> 3) + (f->window ? 0x1000 : 0), penalty = 6;
    if (sx == 0) { /* fully off the left edge: waits as if at a tile start */
        u = 0;
        tile = -0x1000;
    }
    if (tile != *last_tile) {
        if (5 - (u & 7) > 0) penalty += 5 - (u & 7);
        *last_tile = (int16_t)tile;
    }
    return penalty;
}

/* One dot of mode 3. Returns 1 when the line's 160 pixels are out. */
static int draw_dot(gb_t *g) {
    gb_fifo *f = &g->fifo;
    uint8_t lcdc = g->io[R_LCDC];
    f->t++;
    if (f->hblank_in && --f->hblank_in == 0) f->early_hblank = 1;
    /* The low 3 bits of SCX (pixels to drop) are read 4 dots into mode 3
     * (mealybug m3_scx_low_3_bits). */
    if (f->t == 5) f->discard = g->io[R_SCX] & 7;

    if (f->sprite_wait) {
        lcd_dot(g);
        if (--f->sprite_wait == 0) merge_sprite(g, f->sprites[f->sprite_next++]);
        return 0;
    }

    /* The window takes over once the pixel position reaches WX - 7. */
    /* The window starts when the pixel position reaches WX - 7 exactly (the
     * left edge, for WX < 7) with LCDC bit 5 set. Each new start on a line
     * moves to the next row of the window. */
    int wx_match = g->io[R_WX] < 7 ? f->lx == 0 : f->lx + 7 == g->io[R_WX];
    if (!f->window && (lcdc & 0x20) && f->wy_hit && wx_match && g->io[R_WX] <= 166 && !f->discard &&
        f->lx != f->window_done_at) {
        if (f->window_used) g->window_line++;
        f->window_done_at = f->lx;
        f->window = 1;
        f->window_x = 0;
        f->window_used = 1;
        f->window_start = f->lx;
        f->bg_count = 0;
        f->step = 0;
        /* With WX < 7 the window's first pixels fall off the left edge; they
         * are dropped as the first tile is pushed, at no cost in time. */
        f->window_skip = (uint8_t)(g->io[R_WX] < 7 ? 7 - g->io[R_WX] : 0);
    }

    /* A sprite starting here stalls the pipeline (Pan Docs' penalty: wait for
     * the background fetch, then 6 dots). */
    if (f->sprite_next < f->sprite_count && (lcdc & 0x02) && !f->discard) {
        int sx = g->oam[f->sprites[f->sprite_next] * 4 + 1];
        if (sx >= 168) {
            f->sprite_next = f->sprite_count; /* sorted: the rest are off screen too */
        } else if (sx <= f->lx + 8) {
            int16_t last = f->last_tile;
            int penalty = sprite_stall(g, sx, &last);
            f->last_tile = last;
            f->sprite_seen = 1;
            f->sprite_wait = (uint8_t)penalty;
            lcd_dot(g);
            if (--f->sprite_wait == 0) merge_sprite(g, f->sprites[f->sprite_next++]);
            return 0;
        }
    }

    fetcher_dot(g);
    lcd_dot(g);
    ++f->startup;
    if (!f->bg_count) return 0;
    output_pixel(g);
    /* On a line with sprites, mode 0 shows 3 dots before the last pixel is
     * out (mooneye intr_2_mode0_timing_sprites against mealybug's sprite
     * tests). Three pixels from the end, count the stalls still to come. */
    if (f->lx == GB_WIDTH - 3) {
        int16_t last = f->last_tile;
        int pending = 0, any = f->sprite_seen;
        if (lcdc & 0x02)
            for (int k = f->sprite_next; k < f->sprite_count; k++) {
                int sx = g->oam[f->sprites[k] * 4 + 1];
                if (sx >= 168) break;
                pending += sprite_stall(g, sx, &last);
                any = 1;
            }
        if (any && !pending) f->early_hblank = 1;
        else if (any) f->hblank_in = (uint8_t)pending;
    }
    return f->lx == GB_WIDTH;
}

/* Advances one M-cycle (4 dots). */
void ppu_tick(gb_t *g) {
    if (!(g->io[R_LCDC] & 0x80)) return;
    int ly = g->line;
    int dots = g->double_speed ? 2 : 4; /* per CPU M-cycle */
    if (g->fifo.drawing) {
        int done = 0, i = 0;
        for (; i < dots && !done; i++) done = draw_dot(g);
        for (; i < dots; i++) lcd_dot(g); /* the last pixels still travel to the LCD */
        if ((done || g->fifo.early_hblank) && (g->io[R_STAT] & 3) == MODE_DRAW) {
            set_mode(g, MODE_HBLANK);
            ppu_update_stat(g);
            gb_hblank(g);
        }
        if (done) {
            g->fifo.drawing = 0;
            if (g->fifo.window_used) g->window_line++;
        }
    } else if (g->fifo.lcd_count) {
        for (int i = 0; i < dots; i++) lcd_dot(g);
    }
    g->dot = (uint16_t)(g->dot + dots);
    /* Line 0 after V-blank starts drawing one M-cycle early relative to its
     * mode 2 interrupt (mealybug's line_0_fix compensates for this). */
    if (ly < GB_HEIGHT && g->dot == (ly == 0 && !g->fifo.lcd_just_on ? DRAW_START - 4 : DRAW_START)) {
        set_mode(g, MODE_DRAW);
        start_mode3(g);
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
    } else if (ly == 1) {
        g->fifo.lcd_just_on = 0;
    } else if (ly == 154) {
        ly = 0;
        g->window_line = 0;
        g->fifo.wy_hit = 0;
    }
    if (ly < GB_HEIGHT) set_mode(g, MODE_OAM);
    g->line = (uint8_t)ly;
    if (g->io[R_WY] == ly) g->fifo.wy_hit = 1;
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
    if (g->cgb_hw) return; /* fixed on the CGB */
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

/* A palette write during mode 3: for one dot the DMG uses old | new. */
void ppu_palette_write(gb_t *g, int reg, uint8_t v) {
    if (!g->cgb_hw && (g->io[R_LCDC] & 0x80) && g->fifo.lcd_count) {
        g->fifo.pal_glitch[reg - R_BGP] = g->io[reg];
        g->fifo.glitch_dots = 2;
    }
    g->io[reg] = v;
}
