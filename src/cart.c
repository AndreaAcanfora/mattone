/* Cartridges: plain ROM, MBC1, MBC2, MBC3 (with the real-time clock) and MBC5. */
#include "internal.h"

#define RTC_S 0
#define RTC_M 1
#define RTC_H 2
#define RTC_DL 3
#define RTC_DH 4
#define CPU_HZ 4194304

int cart_init(gb_t *g) {
    static const uint32_t ram_sizes[6] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    uint8_t type = g->rom[0x147], ram_code = g->rom[0x149];
    g->cart_type = type;

    switch (type) {
    case 0x00: case 0x08: case 0x09: g->mbc = 0; break;
    case 0x01: case 0x02: case 0x03: g->mbc = 1; break;
    case 0x05: case 0x06: g->mbc = 2; break;
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: g->mbc = 3; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: g->mbc = 5; break;
    default: return GB_ERR_CART;
    }
    g->has_battery = type == 0x03 || type == 0x06 || type == 0x09 || type == 0x0F || type == 0x10 ||
                     type == 0x13 || type == 0x1B || type == 0x1E;
    g->has_rtc = type == 0x0F || type == 0x10;
    g->cart_ram_size = g->mbc == 2 ? 0x200 : (ram_code < 6 ? ram_sizes[ram_code] : 0);
    g->rom_bank = 1;
    g->bank_lo = 1;
    /* MBC1M multicarts (1 MiB, a second Nintendo logo at bank 0x10) wire only
     * four bits of the low bank register. */
    if (g->mbc == 1 && g->rom_size == 0x100000) {
        int same = 1;
        for (int i = 0x104; i < 0x134 && same; i++) same = g->rom[i] == g->rom[0x40000 + i];
        g->mbc1_multicart = (uint8_t)same;
    }
    mem_fill(g->cart_ram, 0xFF, sizeof g->cart_ram);
    return GB_OK;
}

static uint8_t rom_at(const gb_t *g, uint32_t bank, uint16_t addr) {
    return g->rom[(bank * 0x4000 + (addr & 0x3FFF)) % g->rom_size];
}

static int ram_offset(const gb_t *g, uint16_t addr) {
    if (!g->ram_enabled || !g->cart_ram_size) return -1;
    if (g->mbc == 2) return addr & 0x1FF;
    uint32_t bank = g->ram_bank;
    if (g->mbc == 1 && !g->mbc1_mode) bank = 0;
    return (int)((bank * 0x2000 + (addr & 0x1FFF)) % g->cart_ram_size);
}

uint8_t cart_read(gb_t *g, uint16_t addr) {
    if (addr < 0x4000) {
        uint32_t bank = (g->mbc == 1 && g->mbc1_mode) ? (uint32_t)g->bank_hi << (g->mbc1_multicart ? 4 : 5) : 0;
        return rom_at(g, bank, addr);
    }
    if (addr < 0x8000) return rom_at(g, g->rom_bank, addr);

    if (g->mbc == 3 && g->ram_enabled && g->ram_bank >= 8) {
        return g->ram_bank <= 0x0C ? g->rtc_latched[g->ram_bank - 8] : 0xFF;
    }
    if (g->mbc == 0 && g->cart_ram_size) return g->cart_ram[(addr & 0x1FFF) % g->cart_ram_size];
    int off = ram_offset(g, addr);
    if (off < 0) return 0xFF;
    return g->mbc == 2 ? (g->cart_ram[off] | 0xF0) : g->cart_ram[off];
}

static void mbc1_update(gb_t *g) {
    if (g->mbc1_multicart) g->rom_bank = (uint16_t)(g->bank_hi << 4 | (g->bank_lo & 0x0F));
    else g->rom_bank = (uint16_t)(g->bank_hi << 5 | g->bank_lo);
    g->ram_bank = g->bank_hi;
}

void cart_write(gb_t *g, uint16_t addr, uint8_t v) {
    if (addr >= 0xA000) {
        if (g->mbc == 3 && g->ram_enabled && g->ram_bank >= 8) {
            if (g->ram_bank <= 0x0C) {
                g->rtc[g->ram_bank - 8] = v;
                g->rtc_latched[g->ram_bank - 8] = v;
                if (g->ram_bank == 8) g->rtc_cycles = 0;
            }
            return;
        }
        if (g->mbc == 0) {
            if (g->cart_ram_size) g->cart_ram[(addr & 0x1FFF) % g->cart_ram_size] = v;
            return;
        }
        int off = ram_offset(g, addr);
        if (off >= 0) g->cart_ram[off] = g->mbc == 2 ? (v & 0x0F) : v;
        return;
    }

    switch (g->mbc) {
    case 1:
        if (addr < 0x2000) g->ram_enabled = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) { g->bank_lo = v & 0x1F ? v & 0x1F : 1; mbc1_update(g); }
        else if (addr < 0x6000) { g->bank_hi = v & 3; mbc1_update(g); }
        else g->mbc1_mode = v & 1;
        break;
    case 2:
        if (addr < 0x4000) {
            if (addr & 0x100) g->rom_bank = v & 0x0F ? v & 0x0F : 1;
            else g->ram_enabled = (v & 0x0F) == 0x0A;
        }
        break;
    case 3:
        if (addr < 0x2000) g->ram_enabled = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) g->rom_bank = v & 0x7F ? v & 0x7F : 1;
        else if (addr < 0x6000) g->ram_bank = v;
        else {
            if (g->rtc_latch_prev == 0 && v == 1)
                for (int i = 0; i < 5; i++) g->rtc_latched[i] = g->rtc[i];
            g->rtc_latch_prev = v;
        }
        break;
    case 5:
        if (addr < 0x2000) g->ram_enabled = (v & 0x0F) == 0x0A;
        else if (addr < 0x3000) g->rom_bank = (uint16_t)((g->rom_bank & 0x100) | v);
        else if (addr < 0x4000) g->rom_bank = (uint16_t)((g->rom_bank & 0xFF) | (v & 1) << 8);
        else if (addr < 0x6000) g->ram_bank = v & 0x0F;
        break;
    default: break;
    }
}

/* The MBC3 clock counts in emulated time, one second per 4194304 T-cycles. */
void cart_tick(gb_t *g) {
    if (!g->has_rtc || (g->rtc[RTC_DH] & 0x40)) return;
    g->rtc_cycles += 4;
    if (g->rtc_cycles < CPU_HZ) return;
    g->rtc_cycles -= CPU_HZ;
    if (++g->rtc[RTC_S] != 60) return;
    g->rtc[RTC_S] = 0;
    if (++g->rtc[RTC_M] != 60) return;
    g->rtc[RTC_M] = 0;
    if (++g->rtc[RTC_H] != 24) return;
    g->rtc[RTC_H] = 0;
    if (++g->rtc[RTC_DL] != 0) return;
    if (g->rtc[RTC_DH] & 1) g->rtc[RTC_DH] |= 0x80; /* day counter overflow */
    g->rtc[RTC_DH] ^= 1;
}
