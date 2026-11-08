#ifndef MATTONE_INTERNAL_H
#define MATTONE_INTERNAL_H

#include "gb.h"

/* I/O registers, as offsets into gb->io (address - 0xFF00). */
enum {
    R_P1 = 0x00,
    R_SB = 0x01,
    R_SC = 0x02,
    R_DIV = 0x04,
    R_TIMA = 0x05,
    R_TMA = 0x06,
    R_TAC = 0x07,
    R_IF = 0x0F,
    R_LCDC = 0x40,
    R_STAT = 0x41,
    R_SCY = 0x42,
    R_SCX = 0x43,
    R_LY = 0x44,
    R_LYC = 0x45,
    R_DMA = 0x46,
    R_BGP = 0x47,
    R_OBP0 = 0x48,
    R_OBP1 = 0x49,
    R_WY = 0x4A,
    R_WX = 0x4B,
};

enum {
    INT_VBLANK = 1 << 0,
    INT_STAT = 1 << 1,
    INT_TIMER = 1 << 2,
    INT_SERIAL = 1 << 3,
    INT_JOYPAD = 1 << 4,
};

/* bus.c */
void gb_tick(gb_t *gb);       /* advances everything but the CPU by one M-cycle */
void gb_tick_early(gb_t *gb); /* the same without the PPU, which then needs ppu_tick() */
uint8_t gb_read(gb_t *gb, uint16_t addr);
void gb_write(gb_t *gb, uint16_t addr, uint8_t value);

/* cpu.c */
int cpu_step(gb_t *gb);

/* ppu.c */
void ppu_reset(gb_t *gb);
void ppu_tick(gb_t *gb);
void ppu_write_lcdc(gb_t *gb, uint8_t value);
void ppu_update_stat(gb_t *gb);
enum { OAM_BUG_READ, OAM_BUG_WRITE, OAM_BUG_IDU_DURING_READ };
void ppu_oam_bug(gb_t *gb, int kind);

/* apu.c */
void apu_reset(gb_t *gb);
void apu_tick(gb_t *gb);
void apu_frame_sequencer(gb_t *gb);
uint8_t apu_read(gb_t *gb, uint8_t reg);
void apu_write(gb_t *gb, uint8_t reg, uint8_t value);

/* cart.c */
int cart_init(gb_t *gb);
uint8_t cart_read(gb_t *gb, uint16_t addr);
void cart_write(gb_t *gb, uint16_t addr, uint8_t value);
void cart_tick(gb_t *gb);

static inline void mem_fill(void *dst, uint8_t value, size_t n) {
    uint8_t *p = dst;
    while (n--) *p++ = value;
}

#endif
