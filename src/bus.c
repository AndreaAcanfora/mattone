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
    if ((g->sysclk & 0x1000) && !(sysclk & 0x1000)) apu_frame_sequencer(g);
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
    if ((old & 0x1000) && !(g->sysclk & 0x1000)) apu_frame_sequencer(g);
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
void gb_tick_early(gb_t *g) {
    g->cycles += 4;
    timer_tick(g);
    dma_tick(g);
    apu_tick(g);
    if (g->has_rtc) cart_tick(g);
}

void gb_tick(gb_t *g) {
    gb_tick_early(g);
    ppu_tick(g);
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
    return read_raw(g, addr);
}

static uint8_t read_raw(gb_t *g, uint16_t addr) {
    if (addr < 0x8000) return cart_read(g, addr);
    if (addr < 0xA000) return vram_busy(g) ? 0xFF : g->vram[addr & 0x1FFF];
    if (addr < 0xC000) return cart_read(g, addr);
    if (addr < 0xFE00) return g->wram[addr & 0x1FFF];
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
        if (ppu_mode(g) != 3) g->vram[addr & 0x1FFF] = v;
        return;
    }
    if (addr < 0xC000) { cart_write(g, addr, v); return; }
    if (addr < 0xFE00) { g->wram[addr & 0x1FFF] = v; return; }
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
    case R_DMA:
        g->io[R_DMA] = v;
        g->dma_next = (uint16_t)(v << 8);
        g->dma_start = 2;
        break;
    }
}

int gb_load(gb_t *g, const uint8_t *rom, size_t size) {
    mem_fill(g, 0, sizeof *g);
    if (size < 0x150 || size > 0x800000) return GB_ERR_SIZE;
    uint8_t sum = 0;
    for (int i = 0x134; i <= 0x14C; i++) sum = (uint8_t)(sum - rom[i] - 1);
    if (sum != rom[0x14D]) return GB_ERR_HEADER;
    g->rom = rom;
    g->rom_size = (uint32_t)size;
    int err = cart_init(g);
    if (err) return err;

    /* Register values left by the DMG boot ROM. */
    g->a = 0x01; g->f = 0xB0;
    g->b = 0x00; g->c = 0x13;
    g->d = 0x00; g->e = 0xD8;
    g->h = 0x01; g->l = 0x4D;
    g->sp = 0xFFFE;
    g->pc = 0x0100;
    g->sysclk = 0xABC8;
    g->io[R_P1] = 0x00; /* both button groups selected */
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
