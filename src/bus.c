/* Memory map, I/O registers, timer, OAM DMA and the machine clock. */
#include "internal.h"

/* Which bit of the system clock drives TIMA, by TAC & 3. */
static const uint16_t timer_bit[4] = {1 << 9, 1 << 3, 1 << 5, 1 << 7};

/* Unused bits of each I/O register read back as 1. */
static const uint8_t io_mask[0x80] = {
    [0x00] = 0xC0, [0x02] = 0x7E, [0x03] = 0xFF, [0x07] = 0xF8,
    [0x08] = 0xFF, [0x09] = 0xFF, [0x0A] = 0xFF, [0x0B] = 0xFF, [0x0C] = 0xFF, [0x0D] = 0xFF, [0x0E] = 0xFF,
    [0x0F] = 0xE0,
    [0x10] = 0x80, [0x11] = 0x3F, [0x13] = 0xFF, [0x14] = 0xBF, [0x15] = 0xFF,
    [0x16] = 0x3F, [0x18] = 0xFF, [0x19] = 0xBF,
    [0x1A] = 0x7F, [0x1B] = 0xFF, [0x1C] = 0x9F, [0x1D] = 0xFF, [0x1E] = 0xBF,
    [0x1F] = 0xFF, [0x20] = 0xFF, [0x23] = 0xBF, [0x26] = 0x70,
    [0x27] = 0xFF, [0x28] = 0xFF, [0x29] = 0xFF, [0x2A] = 0xFF, [0x2B] = 0xFF, [0x2C] = 0xFF, [0x2D] = 0xFF,
    [0x2E] = 0xFF, [0x2F] = 0xFF,
    [0x41] = 0x80,
    [0x4C] = 0xFF, [0x4D] = 0xFF, [0x4E] = 0xFF, [0x4F] = 0xFF, [0x50] = 0xFF, [0x51] = 0xFF, [0x52] = 0xFF,
    [0x53] = 0xFF, [0x54] = 0xFF, [0x55] = 0xFF, [0x56] = 0xFF, [0x57] = 0xFF, [0x58] = 0xFF, [0x59] = 0xFF,
    [0x5A] = 0xFF, [0x5B] = 0xFF, [0x5C] = 0xFF, [0x5D] = 0xFF, [0x5E] = 0xFF, [0x5F] = 0xFF,
    [0x60] = 0xFF, [0x61] = 0xFF, [0x62] = 0xFF, [0x63] = 0xFF, [0x64] = 0xFF, [0x65] = 0xFF, [0x66] = 0xFF,
    [0x67] = 0xFF, [0x68] = 0xFF, [0x69] = 0xFF, [0x6A] = 0xFF, [0x6B] = 0xFF, [0x6C] = 0xFF, [0x6D] = 0xFF,
    [0x6E] = 0xFF, [0x6F] = 0xFF, [0x70] = 0xFF, [0x71] = 0xFF, [0x72] = 0xFF, [0x73] = 0xFF, [0x74] = 0xFF,
    [0x75] = 0xFF, [0x76] = 0xFF, [0x77] = 0xFF, [0x78] = 0xFF, [0x79] = 0xFF, [0x7A] = 0xFF, [0x7B] = 0xFF,
    [0x7C] = 0xFF, [0x7D] = 0xFF, [0x7E] = 0xFF, [0x7F] = 0xFF,
};

static int timer_signal(const gb_t *g) {
    uint8_t tac = g->io[R_TAC];
    return (tac & 4) && (g->sysclk & timer_bit[tac & 3]);
}

static void tima_increment(gb_t *g) {
    if (++g->io[R_TIMA] == 0) g->tima_reload = 1;
}

/* Changing DIV or TAC can drop the timer signal, which counts as a tick. */
static void timer_set(gb_t *g, uint16_t sysclk, uint8_t tac) {
    int before = timer_signal(g);
    /* The APU frame sequencer steps on a falling edge of DIV bit 4. */
    uint16_t fs_bit = g->double_speed ? 0x2000 : 0x1000;
    if ((g->sysclk & fs_bit) && !(sysclk & fs_bit)) apu_frame_sequencer(g);
    g->sysclk = sysclk;
    g->io[R_TAC] = tac;
    if (before && !timer_signal(g)) {
        tima_increment(g);
        /* The register write lands mid M-cycle, so the overflow delay ends in
         * time for the CPU's next interrupt check (mooneye rapid_toggle). */
        if (g->tima_reload) g->io[R_IF] |= INT_TIMER;
    }
}

/* With the internal clock the link port shifts one bit out (and, with nothing
 * plugged in, a 1 in) every 512 T-cycles, in step with the system clock. */
static void serial_tick(gb_t *g) {
    if ((g->io[R_SC] & 0x81) != 0x81) return;
    g->io[R_SB] = (uint8_t)(g->io[R_SB] << 1 | 1);
    if (++g->serial_bits < 8) return;
    g->serial_bits = 0;
    g->io[R_SC] &= 0x7F;
    g->io[R_IF] |= INT_SERIAL;
}

static void timer_tick(gb_t *g) {
    /* After an overflow TIMA reads 0 for one M-cycle, then reloads. */
    g->tima_reloaded = 0;
    if (g->tima_reload && --g->tima_reload == 0) {
        g->io[R_TIMA] = g->io[R_TMA];
        g->tima_reloaded = 1;
        g->io[R_IF] |= INT_TIMER;
    }
    int before = timer_signal(g);
    uint16_t old = g->sysclk;
    g->sysclk += 4;
    if (before && !timer_signal(g)) tima_increment(g);
    /* In double speed the frame sequencer follows the next bit of DIV, so it
     * keeps stepping at 512 Hz. */
    uint16_t fs_bit = g->double_speed ? 0x2000 : 0x1000;
    if ((old & fs_bit) && !(g->sysclk & fs_bit)) apu_frame_sequencer(g);
    /* The serial clock's edge comes one M-cycle before bit 8 of the system
     * clock falls (mooneye boot_sclk_align). */
    if (((old + 4) & 0x0100) && !((g->sysclk + 4) & 0x0100)) serial_tick(g);
}

static uint8_t read_raw(gb_t *g, uint16_t addr);

/* A transfer starts two M-cycles after the write to DMA (a new request
 * replaces a running one only then), copies one byte per M-cycle and keeps
 * the OAM busy for exactly 160 M-cycles. */
static void dma_tick(gb_t *g) {
    if (g->dma_start && --g->dma_start == 0) {
        g->dma_active = 1;
        g->dma_index = 0;
        g->dma_source = g->dma_next;
    }
    if (!g->dma_active) return;
    if (g->dma_index == 0xA0) {
        g->dma_active = 0;
        return;
    }
    uint16_t src = (uint16_t)(g->dma_source + g->dma_index);
    if (src >= 0xE000) src -= 0x2000; /* above the echo RAM the DMG reads WRAM */
    g->dma_byte = read_raw(g, src);
    g->oam[g->dma_index++] = g->dma_byte;
}

/* During a transfer the CPU shares the bus the DMA reads from: it sees the
 * byte being copied instead of memory. VRAM is on its own bus. */
static int dma_conflict(const gb_t *g, uint16_t addr) {
    if (!g->dma_active || addr >= 0xFE00) return 0;
    int src_vram = g->dma_source >= 0x8000 && g->dma_source < 0xA000;
    int addr_vram = addr >= 0x8000 && addr < 0xA000;
    return src_vram == addr_vram;
}

/* One M-cycle of everything but the PPU. */
/* One CPU M-cycle: 4 T-cycles, or 2 in the CGB's double speed mode, where the
 * CPU, timer, serial and OAM DMA run twice as fast as the PPU and APU. */
void gb_tick_early(gb_t *g) {
    int t = g->double_speed ? 2 : 4;
    g->cycles += (uint64_t)t;
    if (!g->speed_switch_wait) timer_tick(g);
    dma_tick(g);
    apu_tick(g, t);
    if (g->has_rtc) cart_tick(g, t);
}

void gb_tick(gb_t *g) {
    gb_tick_early(g);
    ppu_tick(g);
}

static int ppu_mode(const gb_t *g);

/* WRAM: 0xC000-0xCFFF is bank 0, 0xD000-0xDFFF bank 1 or, on the CGB, the
 * bank SVBK selects; 0xE000-0xFDFF mirrors both. */
static uint32_t wram_index(const gb_t *g, uint16_t addr) {
    if (!(addr & 0x1000)) return addr & 0x0FFF;
    int bank = g->cgb && g->wram_bank ? g->wram_bank : 1;
    return (uint32_t)(bank << 12 | (addr & 0x0FFF));
}

/* Copies one 16-byte block of a CGB VRAM DMA. The CPU waits 8 M-cycles at
 * normal speed (16 in double speed) for each. */
static void hdma_block(gb_t *g) {
    for (int i = 0; i < 16; i++) {
        uint8_t v = read_raw(g, g->hdma_source++);
        g->vram[g->vram_bank << 13 | (g->hdma_dest++ & 0x1FFF)] = v;
    }
    g->hdma_dest &= 0x1FFF;
    g->cpu_stall = (uint8_t)(g->cpu_stall + (g->double_speed ? 16 : 8));
    if (--g->hdma_blocks == 0) g->hdma_hblank = 0;
}

void gb_hblank(gb_t *g) {
    if (g->hdma_hblank) hdma_block(g);
}

static void palette_write(uint8_t *ram, uint8_t *index, uint8_t v) {
    ram[*index & 0x3F] = v;
    if (*index & 0x80) *index = (uint8_t)(0x80 | ((*index + 1) & 0x3F));
}

static void cgb_write(gb_t *g, uint8_t reg, uint8_t v) {
    switch (reg) {
    case R_KEY1: g->io[R_KEY1] = v & 1; break; /* armed: the next STOP switches speed */
    case R_VBK: g->vram_bank = v & 1; break;
    case R_HDMA1: g->hdma_source = (uint16_t)((g->hdma_source & 0x00FF) | v << 8); break;
    case R_HDMA2: g->hdma_source = (uint16_t)((g->hdma_source & 0xFF00) | (v & 0xF0)); break;
    case R_HDMA3: g->hdma_dest = (uint16_t)((g->hdma_dest & 0x00FF) | (v & 0x1F) << 8); break;
    case R_HDMA4: g->hdma_dest = (uint16_t)((g->hdma_dest & 0xFF00) | (v & 0xF0)); break;
    case R_HDMA5:
        if (g->hdma_hblank && !(v & 0x80)) { /* stops an H-blank transfer */
            g->hdma_hblank = 0;
            break;
        }
        g->hdma_blocks = (uint8_t)((v & 0x7F) + 1);
        if (v & 0x80) {
            g->hdma_hblank = 1;
            if (ppu_mode(g) == 0 && (g->io[R_LCDC] & 0x80)) hdma_block(g);
        } else {
            while (g->hdma_blocks) hdma_block(g); /* general-purpose: all at once */
        }
        break;
    case R_BCPS: g->bcps = v & 0xBF; break;
    case R_OCPS: g->ocps = v & 0xBF; break;
    case R_BCPD: if (ppu_mode(g) != 3) palette_write(g->bg_palette, &g->bcps, v); else if (g->bcps & 0x80) g->bcps = (uint8_t)(0x80 | ((g->bcps + 1) & 0x3F)); break;
    case R_OCPD: if (ppu_mode(g) != 3) palette_write(g->obj_palette, &g->ocps, v); else if (g->ocps & 0x80) g->ocps = (uint8_t)(0x80 | ((g->ocps + 1) & 0x3F)); break;
    case R_OPRI: g->io[R_OPRI] = v & 1; break;
    case R_SVBK: g->wram_bank = v & 7; break;
    default: break;
    }
}

static uint8_t joypad_read(const gb_t *g) {
    uint8_t sel = g->io[R_P1], low = 0x0F;
    if (!(sel & 0x10)) low &= (uint8_t) ~(g->buttons & 0x0F);
    if (!(sel & 0x20)) low &= (uint8_t) ~(g->buttons >> 4);
    return 0xC0 | (sel & 0x30) | low;
}

static int ppu_mode(const gb_t *g) { return g->io[R_STAT] & 3; }

/* Reads and writes are cut off at slightly different times (mooneye
 * lcdon_timing and lcdon_write_timing). Drawing claims VRAM for reads one
 * M-cycle before mode 3 shows in STAT; writes only fail in mode 3 proper. */
static int vram_busy(const gb_t *g) {
    int mode = ppu_mode(g);
    return mode == 3 || (mode == 2 && g->dot >= 76);
}

/* For reads, the OAM scan of the next visible line claims the OAM from the
 * M-cycle in which LY changes, one before mode 2 shows in STAT. */
static int oam_busy(const gb_t *g) {
    if (ppu_mode(g) >= 2) return 1;
    return (g->io[R_LCDC] & 0x80) && g->dot >= 452 && (g->line < GB_HEIGHT - 1 || g->line == 153);
}

/* Writes still go through then, and in the last M-cycle of mode 2. */
static int oam_write_busy(const gb_t *g) {
    int mode = ppu_mode(g);
    return mode == 3 || (mode == 2 && g->dot != 76);
}

uint8_t gb_read(gb_t *g, uint16_t addr) {
    if (dma_conflict(g, addr)) return g->dma_byte;
    if (addr >= 0xFE00 && addr < 0xFF00) ppu_oam_bug(g, OAM_BUG_READ);
    return read_raw(g, addr);
}

static uint8_t read_raw(gb_t *g, uint16_t addr) {
    if (addr < 0x8000) return cart_read(g, addr);
    if (addr < 0xA000) return vram_busy(g) ? 0xFF : g->vram[g->vram_bank << 13 | (addr & 0x1FFF)];
    if (addr < 0xC000) return cart_read(g, addr);
    if (addr < 0xFE00) return g->wram[wram_index(g, addr)];
    if (addr < 0xFEA0) {
        if (g->dma_active) return 0xFF;
        return oam_busy(g) ? 0xFF : g->oam[addr - 0xFE00];
    }
    if (addr < 0xFF00) return 0xFF;
    if (addr == 0xFFFF) return g->ie;
    if (addr >= 0xFF80) return g->hram[addr - 0xFF80];

    uint8_t reg = addr & 0x7F;
    switch (reg) {
    case R_P1: return joypad_read(g);
    case R_DIV: return g->sysclk >> 8;
    case R_LY: return (g->io[R_LCDC] & 0x80) ? g->io[R_LY] : 0;
    case 0x26: return apu_read(g, reg);
    /* CGB registers; in compatibility mode most are locked (mooneye boot_hwio-C). */
    case R_KEY1: return g->cgb ? (uint8_t)(0x7E | g->double_speed << 7 | (g->io[R_KEY1] & 1)) : 0xFF;
    case R_VBK: return g->cgb ? (uint8_t)(0xFE | g->vram_bank) : g->cgb_hw ? 0xFE : 0xFF;
    case R_HDMA5: return g->cgb ? (uint8_t)(g->hdma_hblank ? g->hdma_blocks - 1 : 0xFF) : 0xFF;
    case R_BCPS: return g->cgb_hw ? (uint8_t)(g->bcps | 0x40) : 0xFF;
    case R_OCPS: return g->cgb_hw ? (uint8_t)(g->ocps | 0x40) : 0xFF;
    case R_BCPD: return g->cgb && ppu_mode(g) != 3 ? g->bg_palette[g->bcps & 0x3F] : 0xFF;
    case R_OCPD: return g->cgb && ppu_mode(g) != 3 ? g->obj_palette[g->ocps & 0x3F] : 0xFF;
    case R_OPRI: return g->cgb ? (uint8_t)(0xFE | (g->io[R_OPRI] & 1)) : 0xFF;
    case R_SVBK: return g->cgb ? (uint8_t)(0xF8 | g->wram_bank) : 0xFF;
    case 0x72: return g->cgb_hw ? g->cgb_undoc[0] : 0xFF;
    case 0x73: return g->cgb_hw ? g->cgb_undoc[1] : 0xFF;
    case 0x74: return g->cgb ? g->io[0x74] : 0xFF;
    case 0x75: return g->cgb_hw ? (uint8_t)(g->cgb_undoc[2] | 0x8F) : 0xFF;
    case 0x76: case 0x77: return g->cgb_hw ? 0x00 : 0xFF; /* PCM amplitudes: not modelled */
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        return apu_read(g, reg);
    default: return g->io[reg] | io_mask[reg];
    }
}

void gb_write(gb_t *g, uint16_t addr, uint8_t v) {
    if (dma_conflict(g, addr)) return;
    if (addr < 0x8000) { cart_write(g, addr, v); return; }
    if (addr < 0xA000) {
        if (ppu_mode(g) != 3) g->vram[g->vram_bank << 13 | (addr & 0x1FFF)] = v;
        return;
    }
    if (addr < 0xC000) { cart_write(g, addr, v); return; }
    if (addr < 0xFE00) { g->wram[wram_index(g, addr)] = v; return; }
    if (addr < 0xFF00) ppu_oam_bug(g, OAM_BUG_WRITE); /* the address is >= 0xFE00 here */
    if (addr < 0xFEA0) {
        if (!g->dma_active && !oam_write_busy(g)) g->oam[addr - 0xFE00] = v;
        return;
    }
    if (addr < 0xFF00) return;
    if (addr == 0xFFFF) { g->ie = v; return; }
    if (addr >= 0xFF80) { g->hram[addr - 0xFF80] = v; return; }

    uint8_t reg = addr & 0x7F;
    switch (reg) {
    case R_P1: g->io[R_P1] = v & 0x30; break;
    case R_SC:
        g->io[R_SC] = v;
        if ((v & 0x81) == 0x81) {
            /* No link partner: the byte goes out bit by bit, 0xFF comes back. */
            if (g->serial_len < GB_SERIAL_MAX) g->serial[g->serial_len++] = g->io[R_SB];
            g->serial_bits = 0;
        }
        break;
    case R_DIV: timer_set(g, 0, g->io[R_TAC]); break;
    case R_TIMA:
        /* A write during the reload delay cancels the reload. */
        if (g->tima_reload == 1) g->tima_reload = 0;
        /* A write in the cycle of the reload loses against TMA. */
        if (!g->tima_reloaded) g->io[R_TIMA] = v;
        break;
    case R_TMA:
        g->io[R_TMA] = v;
        if (g->tima_reloaded) g->io[R_TIMA] = v;
        break;
    case R_TAC: timer_set(g, g->sysclk, v & 7); break;
    case R_IF: g->io[R_IF] = v & 0x1F; break;
    case R_LCDC: ppu_write_lcdc(g, v); break;
    case R_STAT:
        g->io[R_STAT] = (g->io[R_STAT] & 0x07) | (v & 0x78);
        ppu_update_stat(g);
        break;
    case R_LY: break;
    default:
        if (reg >= 0x10 && reg < 0x40) apu_write(g, reg, v);
        else g->io[reg] = v;
        break;
    case R_LYC:
        g->io[R_LYC] = v;
        ppu_update_stat(g);
        break;
    case R_BGP: case R_OBP0: case R_OBP1: ppu_palette_write(g, reg, v); break;
    case R_KEY1: case R_VBK: case R_HDMA1: case R_HDMA2: case R_HDMA3: case R_HDMA4: case R_HDMA5:
    case R_BCPS: case R_BCPD: case R_OCPS: case R_OCPD: case R_OPRI: case R_SVBK:
        if (g->cgb) cgb_write(g, reg, v);
        break;
    case 0x72: g->cgb_undoc[0] = v; break;
    case 0x73: g->cgb_undoc[1] = v; break;
    case 0x74: if (g->cgb) g->io[0x74] = v; break;
    case 0x75: g->cgb_undoc[2] = v & 0x70; break;
    case R_DMA:
        g->io[R_DMA] = v;
        g->dma_next = (uint16_t)(v << 8);
        g->dma_start = 2;
        break;
    }
}

/* The DMG boot ROM leaves the scrolled-in logo in VRAM: the cartridge's logo
 * bitmap with every pixel doubled (tiles 1-24), the (R) mark (tile 25) and the
 * map entries that showed them. Some programs and tests draw these tiles. */
static void boot_vram(gb_t *g) {
    static const uint8_t mark[8] = {0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5, 0x42, 0x3C};
    uint8_t *tile = &g->vram[0x10];
    for (int i = 0; i < 48; i++) {
        uint8_t b = g->rom[0x104 + i];
        for (int half = 0; half < 2; half++) {
            uint8_t nibble = half ? b & 0x0F : b >> 4, wide = 0;
            for (int bit = 3; bit >= 0; bit--) wide = (uint8_t)(wide << 2 | ((nibble >> bit) & 1) * 3);
            tile[0] = wide; /* two rows, low bitplane only */
            tile[2] = wide;
            tile += 4;
        }
    }
    for (int i = 0; i < 8; i++) g->vram[0x190 + i * 2] = mark[i];
    for (int i = 0; i < 12; i++) {
        g->vram[0x1904 + i] = (uint8_t)(1 + i);
        g->vram[0x1924 + i] = (uint8_t)(13 + i);
    }
    g->vram[0x1910] = 0x19;
}

int gb_load(gb_t *g, const uint8_t *rom, size_t size) { return gb_load_model(g, rom, size, GB_MODEL_AUTO); }

int gb_load_model(gb_t *g, const uint8_t *rom, size_t size, int model) {
    mem_fill(g, 0, sizeof *g);
    if (size < 0x150 || size > 0x800000) return GB_ERR_SIZE;
    uint8_t sum = 0;
    for (int i = 0x134; i <= 0x14C; i++) sum = (uint8_t)(sum - rom[i] - 1);
    if (sum != rom[0x14D]) return GB_ERR_HEADER;
    g->rom = rom;
    g->rom_size = (uint32_t)size;
    int err = cart_init(g);
    if (err) return err;

    /* Register values left by the boot ROM. */
    g->cgb_hw = model == GB_MODEL_CGB || (model == GB_MODEL_AUTO && (rom[0x143] & 0x80));
    g->cgb = g->cgb_hw && (rom[0x143] & 0x80);
    if (g->cgb_hw && !g->cgb) {
        /* A DMG cartridge on a CGB: compatibility mode. */
        g->a = 0x11; g->f = 0x80;
        g->b = 0x00; g->c = 0x00;
        g->d = 0x00; g->e = 0x08;
        g->h = 0x00; g->l = 0x7C;
        g->bcps = 0x88; /* where the boot ROM left the palette indexes */
        g->ocps = 0x90;
        cgb_compat_palettes(g);
    } else if (g->cgb) {
        g->a = 0x11; g->f = 0x80;
        g->b = 0x00; g->c = 0x00;
        g->d = 0xFF; g->e = 0x56;
        g->h = 0x00; g->l = 0x0D;
        g->wram_bank = 1;
        mem_fill(g->bg_palette, 0xFF, sizeof g->bg_palette); /* white */
        mem_fill(g->obj_palette, 0xFF, sizeof g->obj_palette);
    } else {
        g->a = 0x01; g->f = 0xB0;
        g->b = 0x00; g->c = 0x13;
        g->d = 0x00; g->e = 0xD8;
        g->h = 0x01; g->l = 0x4D;
    }
    g->sp = 0xFFFE;
    g->pc = 0x0100;
    /* System clock phase at 0x100 (mooneye boot_div-dmgABCmgb and, for the
     * CGB, boot_div-cgbABCDE, which runs in compatibility mode). */
    g->sysclk = g->cgb_hw ? 0x2674 : 0xABC8;
    g->io[R_P1] = g->cgb_hw ? 0x30 : 0x00; /* the DMG boot ROM leaves both button groups selected */
    g->io[R_SC] = 0x7E;
    g->io[R_TAC] = 0x00;
    g->io[R_IF] = 0x01;
    static const uint8_t sound[0x17] = {0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF,
                                        0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1};
    for (int i = 0; i < 0x17; i++) g->io[0x10 + i] = sound[i];
    g->io[R_LCDC] = 0x91;
    g->io[R_BGP] = 0xFC;
    g->io[R_OBP0] = 0xFF;
    g->io[R_OBP1] = 0xFF;
    if (!g->cgb_hw) boot_vram(g);
    ppu_reset(g);
    apu_reset(g);
    return GB_OK;
}

int gb_step(gb_t *g) { return cpu_step(g); }

void gb_run_frame(gb_t *g) {
    uint64_t limit = g->cycles + GB_CYCLES_PER_FRAME;
    g->frame_done = 0;
    while (!g->frame_done && g->cycles < limit) cpu_step(g);
}

void gb_set_buttons(gb_t *g, uint8_t pressed) {
    uint8_t newly = pressed & ~g->buttons;
    g->buttons = pressed;
    if (newly) g->io[R_IF] |= INT_JOYPAD;
}

void gb_title(const gb_t *g, char out[17]) {
    int n = 0;
    for (; n < 16; n++) {
        uint8_t ch = g->rom[0x134 + n];
        if (ch == 0 || ch >= 0x80) break;
        out[n] = (char)ch;
    }
    out[n] = 0;
}
