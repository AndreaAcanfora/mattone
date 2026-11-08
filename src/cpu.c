/* The Sharp SM83 CPU. Every memory access costs one M-cycle (gb_tick), and
 * instructions add the internal cycles the hardware spends on top of that, so
 * timers and the PPU see accesses at the right moment. */
#include "internal.h"

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

/* The CPU sees the timer as it is at the end of the M-cycle but the PPU as it
 * was at its start, so the PPU advances after the access. */
static uint8_t rd(gb_t *g, uint16_t addr) {
    gb_tick_early(g);
    uint8_t v = gb_read(g, addr);
    ppu_tick(g);
    return v;
}

static void wr(gb_t *g, uint16_t addr, uint8_t v) {
    gb_tick_early(g);
    gb_write(g, addr, v);
    ppu_tick(g);
}

static uint8_t fetch(gb_t *g) {
    uint8_t v = rd(g, g->pc);
    if (g->halt_bug)
        g->halt_bug = 0; /* the byte after HALT is read twice */
    else
        g->pc++;
    return v;
}

static uint16_t fetch16(gb_t *g) {
    uint8_t lo = fetch(g);
    return (uint16_t)(lo | fetch(g) << 8);
}

static uint16_t hl(const gb_t *g) { return (uint16_t)(g->h << 8 | g->l); }
static void set_hl(gb_t *g, uint16_t v) { g->h = v >> 8; g->l = v & 0xFF; }

static uint16_t get_rr(const gb_t *g, int i) {
    switch (i) {
    case 0: return (uint16_t)(g->b << 8 | g->c);
    case 1: return (uint16_t)(g->d << 8 | g->e);
    case 2: return hl(g);
    default: return g->sp;
    }
}

static void set_rr(gb_t *g, int i, uint16_t v) {
    switch (i) {
    case 0: g->b = v >> 8; g->c = v & 0xFF; break;
    case 1: g->d = v >> 8; g->e = v & 0xFF; break;
    case 2: set_hl(g, v); break;
    default: g->sp = v; break;
    }
}

static uint8_t get_r(gb_t *g, int i) {
    switch (i) {
    case 0: return g->b;
    case 1: return g->c;
    case 2: return g->d;
    case 3: return g->e;
    case 4: return g->h;
    case 5: return g->l;
    case 6: return rd(g, hl(g));
    default: return g->a;
    }
}

static void set_r(gb_t *g, int i, uint8_t v) {
    switch (i) {
    case 0: g->b = v; break;
    case 1: g->c = v; break;
    case 2: g->d = v; break;
    case 3: g->e = v; break;
    case 4: g->h = v; break;
    case 5: g->l = v; break;
    case 6: wr(g, hl(g), v); break;
    default: g->a = v; break;
    }
}

static void push(gb_t *g, uint16_t v) {
    gb_tick(g);
    wr(g, --g->sp, v >> 8);
    wr(g, --g->sp, v & 0xFF);
}

static uint16_t pop(gb_t *g) {
    uint8_t lo = rd(g, g->sp++);
    return (uint16_t)(lo | rd(g, g->sp++) << 8);
}

static int cond(const gb_t *g, int cc) {
    switch (cc) {
    case 0: return !(g->f & FZ);
    case 1: return (g->f & FZ) != 0;
    case 2: return !(g->f & FC);
    default: return (g->f & FC) != 0;
    }
}

/* ADD ADC SUB SBC AND XOR OR CP */
static void alu(gb_t *g, int op, uint8_t v) {
    unsigned a = g->a, carry = (g->f & FC) ? 1 : 0, r;
    switch (op) {
    case 0: carry = 0; /* fall through */
    case 1:
        r = a + v + carry;
        g->f = ((r & 0xFF) ? 0 : FZ) | (((a & 0xF) + (v & 0xF) + carry) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
        g->a = (uint8_t)r;
        break;
    case 2:
    case 7:
        carry = 0; /* fall through */
    case 3:
        r = a - v - carry;
        g->f = FN | ((r & 0xFF) ? 0 : FZ) | ((a & 0xF) < (v & 0xF) + carry ? FH : 0) | (a < v + carry ? FC : 0);
        if (op != 7) g->a = (uint8_t)r;
        break;
    case 4:
        g->a &= v;
        g->f = (g->a ? 0 : FZ) | FH;
        break;
    case 5:
        g->a ^= v;
        g->f = g->a ? 0 : FZ;
        break;
    default:
        g->a |= v;
        g->f = g->a ? 0 : FZ;
        break;
    }
}

/* RLC RRC RL RR SLA SRA SWAP SRL; sets Z from the result. */
static uint8_t shift(gb_t *g, int op, uint8_t v) {
    unsigned carry_in = (g->f & FC) ? 1 : 0, out, r;
    switch (op) {
    case 0: out = v >> 7; r = (unsigned)(v << 1) | out; break;
    case 1: out = v & 1; r = (unsigned)(v >> 1) | out << 7; break;
    case 2: out = v >> 7; r = (unsigned)(v << 1) | carry_in; break;
    case 3: out = v & 1; r = (unsigned)(v >> 1) | carry_in << 7; break;
    case 4: out = v >> 7; r = (unsigned)(v << 1); break;
    case 5: out = v & 1; r = (unsigned)(v >> 1) | (v & 0x80); break;
    case 6: out = 0; r = (unsigned)(v >> 4 | v << 4); break;
    default: out = v & 1; r = v >> 1; break;
    }
    r &= 0xFF;
    g->f = (r ? 0 : FZ) | (out ? FC : 0);
    return (uint8_t)r;
}

static void exec_cb(gb_t *g) {
    uint8_t op = fetch(g);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = get_r(g, z);
    switch (x) {
    case 0: set_r(g, z, shift(g, y, v)); break;
    case 1: g->f = (g->f & FC) | FH | ((v >> y) & 1 ? 0 : FZ); break; /* BIT */
    case 2: set_r(g, z, v & ~(1 << y)); break;                       /* RES */
    default: set_r(g, z, v | 1 << y); break;                          /* SET */
    }
}

static uint16_t add_sp_e(gb_t *g) {
    int8_t e = (int8_t)fetch(g);
    unsigned sp = g->sp, u = (uint8_t)e;
    g->f = (((sp & 0xF) + (u & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + u) > 0xFF ? FC : 0);
    return (uint16_t)(sp + e);
}

static void daa(gb_t *g) {
    unsigned a = g->a;
    uint8_t f = g->f;
    if (!(f & FN)) {
        if ((f & FC) || a > 0x99) { a += 0x60; f |= FC; }
        if ((f & FH) || (a & 0xF) > 9) a += 6;
    } else {
        if (f & FC) a -= 0x60;
        if (f & FH) a -= 6;
    }
    g->a = (uint8_t)a;
    g->f = (f & (FN | FC)) | (g->a ? 0 : FZ);
}

static void halt(gb_t *g) {
    if (!g->ime && (g->ie & g->io[R_IF] & 0x1F))
        g->halt_bug = 1;
    else
        g->halted = 1;
}

static void exec(gb_t *g, uint8_t op) {
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1;

    if (x == 1) {
        if (op == 0x76) halt(g);
        else set_r(g, y, get_r(g, z)); /* LD r,r' */
        if (op == 0x40) g->breakpoint = 1;
        return;
    }
    if (x == 2) {
        alu(g, y, get_r(g, z));
        return;
    }

    switch (op) {
    case 0x00: break; /* NOP */
    case 0x10:        /* STOP: the second byte is skipped; DIV resets */
        fetch(g);
        g->sysclk = 0;
        break;
    case 0x08: { /* LD (nn),SP */
        uint16_t a = fetch16(g);
        wr(g, a, g->sp & 0xFF);
        wr(g, (uint16_t)(a + 1), g->sp >> 8);
        break;
    }
    case 0x18: { /* JR e */
        int8_t e = (int8_t)fetch(g);
        gb_tick(g);
        g->pc = (uint16_t)(g->pc + e);
        break;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: { /* JR cc,e */
        int8_t e = (int8_t)fetch(g);
        if (cond(g, y - 4)) {
            gb_tick(g);
            g->pc = (uint16_t)(g->pc + e);
        }
        break;
    }
    case 0x01: case 0x11: case 0x21: case 0x31: set_rr(g, p, fetch16(g)); break; /* LD rr,nn */
    case 0x09: case 0x19: case 0x29: case 0x39: { /* ADD HL,rr */
        unsigned a = hl(g), v = get_rr(g, p), r = a + v;
        gb_tick(g);
        g->f = (g->f & FZ) | (((a & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0);
        set_hl(g, (uint16_t)r);
        break;
    }
    case 0x02: wr(g, get_rr(g, 0), g->a); break;
    case 0x12: wr(g, get_rr(g, 1), g->a); break;
    case 0x22: wr(g, hl(g), g->a); set_hl(g, hl(g) + 1); break;
    case 0x32: wr(g, hl(g), g->a); set_hl(g, hl(g) - 1); break;
    case 0x0A: g->a = rd(g, get_rr(g, 0)); break;
    case 0x1A: g->a = rd(g, get_rr(g, 1)); break;
    case 0x2A: g->a = rd(g, hl(g)); set_hl(g, hl(g) + 1); break;
    case 0x3A: g->a = rd(g, hl(g)); set_hl(g, hl(g) - 1); break;
    case 0x03: case 0x13: case 0x23: case 0x33: /* INC rr */
        gb_tick(g);
        set_rr(g, p, get_rr(g, p) + 1);
        break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: /* DEC rr */
        gb_tick(g);
        set_rr(g, p, get_rr(g, p) - 1);
        break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: { /* INC r */
        uint8_t v = get_r(g, y), r = v + 1;
        g->f = (g->f & FC) | (r ? 0 : FZ) | ((v & 0xF) == 0xF ? FH : 0);
        set_r(g, y, r);
        break;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: { /* DEC r */
        uint8_t v = get_r(g, y), r = v - 1;
        g->f = (g->f & FC) | FN | (r ? 0 : FZ) | ((v & 0xF) == 0 ? FH : 0);
        set_r(g, y, r);
        break;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E: /* LD r,n */
        set_r(g, y, fetch(g));
        break;
    case 0x07: g->a = shift(g, 0, g->a); g->f &= FC; break; /* RLCA */
    case 0x0F: g->a = shift(g, 1, g->a); g->f &= FC; break; /* RRCA */
    case 0x17: g->a = shift(g, 2, g->a); g->f &= FC; break; /* RLA */
    case 0x1F: g->a = shift(g, 3, g->a); g->f &= FC; break; /* RRA */
    case 0x27: daa(g); break;
    case 0x2F: g->a = ~g->a; g->f |= FN | FH; break;            /* CPL */
    case 0x37: g->f = (g->f & FZ) | FC; break;                  /* SCF */
    case 0x3F: g->f = (g->f & FZ) | ((g->f & FC) ^ FC); break;  /* CCF */

    case 0xC0: case 0xC8: case 0xD0: case 0xD8: /* RET cc */
        gb_tick(g);
        if (cond(g, y)) {
            g->pc = pop(g);
            gb_tick(g);
        }
        break;
    case 0xC9: g->pc = pop(g); gb_tick(g); break;              /* RET */
    case 0xD9: g->pc = pop(g); gb_tick(g); g->ime = 1; break;  /* RETI */
    case 0xC1: case 0xD1: case 0xE1: set_rr(g, p, pop(g)); break;
    case 0xF1: { /* POP AF */
        uint16_t v = pop(g);
        g->a = v >> 8;
        g->f = v & 0xF0;
        break;
    }
    case 0xC5: case 0xD5: case 0xE5: push(g, get_rr(g, p)); break;
    case 0xF5: push(g, (uint16_t)(g->a << 8 | g->f)); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: { /* JP cc,nn */
        uint16_t a = fetch16(g);
        if (cond(g, y)) {
            gb_tick(g);
            g->pc = a;
        }
        break;
    }
    case 0xC3: { /* JP nn */
        uint16_t a = fetch16(g);
        gb_tick(g);
        g->pc = a;
        break;
    }
    case 0xE9: g->pc = hl(g); break; /* JP HL */
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: { /* CALL cc,nn */
        uint16_t a = fetch16(g);
        if (cond(g, y)) {
            push(g, g->pc);
            g->pc = a;
        }
        break;
    }
    case 0xCD: { /* CALL nn */
        uint16_t a = fetch16(g);
        push(g, g->pc);
        g->pc = a;
        break;
    }
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF: /* RST */
        push(g, g->pc);
        g->pc = (uint16_t)(y * 8);
        break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE: /* ALU A,n */
        alu(g, y, fetch(g));
        break;
    case 0xCB: exec_cb(g); break;
    case 0xE0: wr(g, (uint16_t)(0xFF00 | fetch(g)), g->a); break;      /* LDH (n),A */
    case 0xF0: g->a = rd(g, (uint16_t)(0xFF00 | fetch(g))); break;     /* LDH A,(n) */
    case 0xE2: wr(g, (uint16_t)(0xFF00 | g->c), g->a); break;          /* LD (C),A */
    case 0xF2: g->a = rd(g, (uint16_t)(0xFF00 | g->c)); break;         /* LD A,(C) */
    case 0xEA: wr(g, fetch16(g), g->a); break;
    case 0xFA: g->a = rd(g, fetch16(g)); break;
    case 0xE8: { /* ADD SP,e */
        uint16_t r = add_sp_e(g);
        gb_tick(g);
        gb_tick(g);
        g->sp = r;
        break;
    }
    case 0xF8: { /* LD HL,SP+e */
        uint16_t r = add_sp_e(g);
        gb_tick(g);
        set_hl(g, r);
        break;
    }
    case 0xF9: gb_tick(g); g->sp = hl(g); break; /* LD SP,HL */
    case 0xF3: g->ime = 0; g->ei_pending = 0; break; /* DI */
    case 0xFB: g->ei_pending = 1; break;             /* EI */
    default:
        /* D3 DB DD E3 E4 EB EC ED F4 FC FD lock up the real CPU. */
        g->halted = 1;
        g->ime = 0;
        g->ie = 0;
        break;
    }
}

static void dispatch_interrupt(gb_t *g) {
    g->ime = 0;
    gb_tick(g);
    gb_tick(g);
    wr(g, --g->sp, g->pc >> 8);
    /* Pushing the high byte can overwrite IE, which changes the target. */
    uint8_t pending = g->ie & g->io[R_IF] & 0x1F;
    wr(g, --g->sp, g->pc & 0xFF);
    if (pending) {
        int n = 0;
        while (!(pending & 1 << n)) n++;
        g->io[R_IF] &= (uint8_t) ~(1 << n);
        g->pc = (uint16_t)(0x40 + n * 8);
    } else {
        g->pc = 0;
    }
    gb_tick(g);
}

int cpu_step(gb_t *g) {
    uint64_t start = g->cycles;
    uint8_t pending = g->ie & g->io[R_IF] & 0x1F;

    if (g->halted) {
        if (!pending) {
            gb_tick(g);
            return (int)(g->cycles - start);
        }
        g->halted = 0;
    }
    if (g->ime && pending) {
        dispatch_interrupt(g);
        return (int)(g->cycles - start);
    }
    if (g->ei_pending) {
        g->ei_pending = 0;
        g->ime = 1;
    }
    exec(g, fetch(g));
    return (int)(g->cycles - start);
}
