/* The audio processing unit: two square channels (the first with a frequency
 * sweep), a wave channel and a noise channel, driven by a 512 Hz frame
 * sequencer that clocks length counters, the sweep and volume envelopes.
 * Includes the DMG quirks the CPU can observe (Blargg's dmg_sound tests). */
#include "internal.h"

enum {
    NR10 = 0x10, NR11, NR12, NR13, NR14,
    NR21 = 0x16, NR22, NR23, NR24,
    NR30 = 0x1A, NR31, NR32, NR33, NR34,
    NR41 = 0x20, NR42, NR43, NR44,
    NR50 = 0x24, NR51, NR52,
    WAVE = 0x30,
};

#define CH_SQUARE1 0
#define CH_SQUARE2 1
#define CH_WAVE 2
#define CH_NOISE 3
#define CPU_HZ 4194304
#ifndef WAVE_READ_T
#define WAVE_READ_T 3
#endif

/* Each channel's registers start at 0xFF10 + 5 * channel. */
static int base(int ch) { return NR10 + 5 * ch; }

static const uint8_t duty_waves[4] = {0x01, 0x81, 0x87, 0x7E}; /* 12.5%, 25%, 50%, 75% */
static const uint8_t noise_divisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};

static uint16_t frequency(const gb_t *g, int ch) {
    return (uint16_t)(g->io[base(ch) + 3] | (g->io[base(ch) + 4] & 7) << 8);
}

/* T-cycles between two steps of a channel's waveform. */
static int32_t period(const gb_t *g, int ch) {
    if (ch == CH_NOISE) {
        uint8_t nr43 = g->io[NR43];
        return (int32_t)noise_divisors[nr43 & 7] << (nr43 >> 4);
    }
    return (2048 - frequency(g, ch)) * (ch == CH_WAVE ? 2 : 4);
}

/* ---------- frame sequencer ---------- */

static void clock_lengths(gb_t *g) {
    for (int ch = 0; ch < 4; ch++) {
        gb_channel *c = &g->apu.ch[ch];
        if ((g->io[base(ch) + 4] & 0x40) && c->length && --c->length == 0) c->enabled = 0;
    }
}

static uint16_t sweep_calc(gb_t *g) {
    gb_channel *c = &g->apu.ch[CH_SQUARE1];
    uint8_t nr10 = g->io[NR10];
    uint16_t delta = c->shadow >> (nr10 & 7), f;
    if (nr10 & 0x08) {
        f = c->shadow - delta;
        c->sweep_negated = 1;
    } else {
        f = c->shadow + delta;
    }
    if (f > 2047) c->enabled = 0;
    return f;
}

static void clock_sweep(gb_t *g) {
    gb_channel *c = &g->apu.ch[CH_SQUARE1];
    uint8_t nr10 = g->io[NR10], sweep_period = (nr10 >> 4) & 7;
    if (--c->sweep_timer) return;
    c->sweep_timer = sweep_period ? sweep_period : 8;
    if (!c->sweep_enabled || !sweep_period) return;
    uint16_t f = sweep_calc(g);
    if (f <= 2047 && (nr10 & 7)) {
        c->shadow = f;
        g->io[NR13] = f & 0xFF;
        g->io[NR14] = (uint8_t)((g->io[NR14] & ~7) | f >> 8);
        sweep_calc(g); /* checked again for overflow, not written back */
    }
}

static void clock_envelopes(gb_t *g) {
    static const int channels[3] = {CH_SQUARE1, CH_SQUARE2, CH_NOISE};
    for (int i = 0; i < 3; i++) {
        gb_channel *c = &g->apu.ch[channels[i]];
        uint8_t nrx2 = g->io[base(channels[i]) + 2], p = nrx2 & 7;
        if (!p) continue;
        if (--c->env_timer) continue;
        c->env_timer = p;
        if ((nrx2 & 0x08) && c->volume < 15) c->volume++;
        else if (!(nrx2 & 0x08) && c->volume > 0) c->volume--;
    }
}

void apu_frame_sequencer(gb_t *g) {
    if (!g->apu.power) return;
    g->apu.dirty = 1;
    int step = g->apu.fs_step;
    g->apu.fs_step = (step + 1) & 7;
    if (!(step & 1)) clock_lengths(g);
    if (step == 2 || step == 6) clock_sweep(g);
    if (step == 7) clock_envelopes(g);
}

/* ---------- channel clocks and mixing ---------- */

static void step_channel(gb_t *g, int ch) {
    gb_channel *c = &g->apu.ch[ch];
    g->apu.dirty = 1;
    switch (ch) {
    case CH_WAVE:
        c->wave_pos = (c->wave_pos + 1) & 31;
        c->sample = g->io[WAVE + c->wave_pos / 2];
        break;
    case CH_NOISE: {
        uint16_t bit = (c->lfsr ^ (c->lfsr >> 1)) & 1;
        c->lfsr = (uint16_t)(c->lfsr >> 1 | bit << 14);
        if (g->io[NR43] & 0x08) c->lfsr = (uint16_t)((c->lfsr & ~0x40) | bit << 6);
        break;
    }
    default: c->duty_pos = (c->duty_pos + 1) & 7; break;
    }
}

/* The channel's digital output, 0-15. */
static int channel_output(const gb_t *g, int ch) {
    const gb_channel *c = &g->apu.ch[ch];
    if (!c->enabled) return 0;
    switch (ch) {
    case CH_WAVE: {
        static const uint8_t shift[4] = {4, 0, 1, 2};
        int nibble = (c->wave_pos & 1) ? c->sample & 0x0F : c->sample >> 4;
        return nibble >> shift[(g->io[NR32] >> 5) & 3];
    }
    case CH_NOISE: return (c->lfsr & 1) ? 0 : c->volume;
    default: return (duty_waves[g->io[base(ch) + 1] >> 6] >> (7 - c->duty_pos) & 1) ? c->volume : 0;
    }
}

static void mix(gb_t *g, int t_cycles) {
    gb_apu *a = &g->apu;
    if (a->dirty) {
        /* The DAC maps 0..15 to +1..-1; summed in integers as 15 - 2 * d. */
        int left = 0, right = 0;
        uint8_t nr51 = g->io[NR51], nr50 = g->io[NR50];
        for (int ch = 0; ch < 4; ch++) {
            if (!a->ch[ch].dac) continue;
            int v = 15 - 2 * channel_output(g, ch);
            if (nr51 & (0x10 << ch)) left += v;
            if (nr51 & (0x01 << ch)) right += v;
        }
        a->mix_left = left * (((nr50 >> 4) & 7) + 1);
        a->mix_right = right * ((nr50 & 7) + 1);
        a->dirty = 0;
    }
    a->acc_left += a->mix_left;
    a->acc_right += a->mix_right;
    a->acc_count++;

    a->sample_clock += (uint32_t)t_cycles * a->sample_rate;
    if (a->sample_clock < CPU_HZ) return;
    a->sample_clock -= CPU_HZ;

    /* Average over the sample period (full scale is 4 channels * 15 * 8),
     * then a high-pass filter like the capacitor on the real output, which
     * removes the DC offset. */
    float scale = 1.0f / (480.0f * (float)a->acc_count);
    float l = (float)a->acc_left * scale, r = (float)a->acc_right * scale;
    a->acc_left = a->acc_right = 0;
    a->acc_count = 0;
    float out_l = l - a->cap_left, out_r = r - a->cap_right;
    a->cap_left = l - out_l * a->charge;
    a->cap_right = r - out_r * a->charge;
    if (a->audio_len < GB_AUDIO_MAX) {
        a->audio[a->audio_len * 2] = (int16_t)(out_l * 24000.0f);
        a->audio[a->audio_len * 2 + 1] = (int16_t)(out_r * 24000.0f);
        a->audio_len++;
    }
}

void apu_tick(gb_t *g, int t_cycles) {
    gb_apu *a = &g->apu;
    a->wave_read = 0;
    if (a->power) {
        for (int ch = 0; ch < 4; ch++) {
            gb_channel *c = &a->ch[ch];
            /* Without sound output only the wave channel's position can be
             * observed (through wave RAM), so the others can rest. */
            if (!c->enabled || (!a->sample_rate && ch != CH_WAVE)) continue;
            c->timer -= t_cycles;
            while (c->timer <= 0) {
                /* This step fell on T-cycle 3 + timer of the M-cycle. */
                int t_offset = t_cycles - 1 + c->timer;
                c->timer += period(g, ch);
                step_channel(g, ch);
                if (ch == CH_WAVE) a->wave_read = t_offset == WAVE_READ_T;
            }
        }
    }
    if (a->sample_rate) mix(g, t_cycles);
}

/* ---------- registers ---------- */

static void trigger(gb_t *g, int ch) {
    gb_channel *c = &g->apu.ch[ch];
    uint8_t nrx2 = g->io[base(ch) + 2];

    if (!g->cgb_hw && ch == CH_WAVE && c->enabled && c->timer <= 2) {
        /* DMG: retriggering just as the channel reads wave RAM corrupts it. */
        int pos = ((c->wave_pos + 1) & 31) / 2;
        if (pos < 4) {
            g->io[WAVE] = g->io[WAVE + pos];
        } else {
            for (int i = 0; i < 4; i++) g->io[WAVE + i] = g->io[WAVE + (pos & ~3) + i];
        }
    }

    c->enabled = 1;
    c->timer = period(g, ch);
    if (ch == CH_WAVE) {
        c->timer += 6; /* the wave channel starts a little late */
        c->wave_pos = 0;
    } else {
        c->volume = nrx2 >> 4;
        c->env_timer = (nrx2 & 7) ? (nrx2 & 7) : 8;
    }
    if (ch == CH_NOISE) c->lfsr = 0x7FFF;
    if (ch == CH_SQUARE1) {
        uint8_t nr10 = g->io[NR10], sweep_period = (nr10 >> 4) & 7;
        c->shadow = frequency(g, ch);
        c->sweep_timer = sweep_period ? sweep_period : 8;
        c->sweep_enabled = sweep_period || (nr10 & 7);
        c->sweep_negated = 0;
        if (nr10 & 7) sweep_calc(g);
    }
    if (!c->dac) c->enabled = 0;
}

static void write_nrx4(gb_t *g, int ch, uint8_t v) {
    gb_channel *c = &g->apu.ch[ch];
    uint8_t old = g->io[base(ch) + 4];
    uint16_t max = ch == CH_WAVE ? 256 : 64;
    /* The next frame sequencer step won't clock lengths: we are in the first
     * half of a length period, where enabling length clocks it once more. */
    int first_half = g->apu.fs_step & 1;
    g->io[base(ch) + 4] = v;

    if (first_half && !(old & 0x40) && (v & 0x40) && c->length) {
        if (--c->length == 0 && !(v & 0x80)) c->enabled = 0;
    }
    if (v & 0x80) {
        if (!c->length) {
            c->length = max;
            if ((v & 0x40) && first_half) c->length--;
        }
        trigger(g, ch);
    }
}

static void write_length(gb_t *g, int ch, uint8_t v) {
    g->apu.ch[ch].length = ch == CH_WAVE ? (uint16_t)(256 - v) : (uint16_t)(64 - (v & 0x3F));
}

static void power_off(gb_t *g) {
    for (int r = NR10; r < NR52; r++) g->io[r] = 0;
    for (int ch = 0; ch < 4; ch++) {
        gb_channel *c = &g->apu.ch[ch];
        c->enabled = 0;
        c->dac = 0;
        if (g->cgb_hw) c->length = 0; /* the DMG keeps its length counters */
    }
    g->apu.power = 0;
}

static void power_on(gb_t *g) {
    g->apu.power = 1;
    g->apu.fs_step = 0;
    for (int ch = 0; ch < 4; ch++) {
        g->apu.ch[ch].duty_pos = 0;
        g->apu.ch[ch].sample = 0;
    }
}

/* Is the wave channel playing, so that wave RAM belongs to it? On the DMG the
 * CPU only sees the byte being played if it reads in the same cycle. */
static int wave_busy(const gb_t *g) { return g->apu.ch[CH_WAVE].enabled; }

uint8_t apu_read(gb_t *g, uint8_t reg) {
    if (reg >= WAVE) {
        if (!wave_busy(g)) return g->io[reg];
        /* While playing, the CGB always gives the byte being played. */
        if (g->cgb_hw || g->apu.wave_read) return g->io[WAVE + g->apu.ch[CH_WAVE].wave_pos / 2];
        return 0xFF;
    }
    if (reg == NR52) {
        uint8_t v = (uint8_t)(0x70 | g->apu.power << 7);
        for (int ch = 0; ch < 4; ch++)
            if (g->apu.ch[ch].enabled) v |= (uint8_t)(1 << ch);
        return v;
    }
    return 0; /* other registers are read through gb->io and the mask table */
}

void apu_write(gb_t *g, uint8_t reg, uint8_t v) {
    g->apu.dirty = 1; /* any write can change what the mixer outputs */
    if (reg >= WAVE) {
        if (!wave_busy(g)) g->io[reg] = v;
        else if (g->cgb_hw || g->apu.wave_read) g->io[WAVE + g->apu.ch[CH_WAVE].wave_pos / 2] = v;
        return;
    }
    if (reg == NR52) {
        if (!(v & 0x80) && g->apu.power) power_off(g);
        else if ((v & 0x80) && !g->apu.power) power_on(g);
        return;
    }
    if (!g->apu.power) {
        /* With the APU off only the DMG's length counters can be written. */
        if (!g->cgb_hw && (reg == NR11 || reg == NR21 || reg == NR31 || reg == NR41)) write_length(g, (reg - NR10) / 5, v);
        return;
    }

    int ch = (reg - NR10) / 5, r = (reg - NR10) % 5;
    if (reg >= NR50) ch = -1;
    if (ch == CH_SQUARE1 && r == 0) {
        /* Leaving negate mode after a negated calculation silences the channel. */
        if ((g->io[NR10] & 0x08) && !(v & 0x08) && g->apu.ch[0].sweep_negated) g->apu.ch[0].enabled = 0;
    }
    if (ch >= 0 && r == 4) {
        write_nrx4(g, ch, v);
        return;
    }
    g->io[reg] = v;
    if (ch < 0) return;
    gb_channel *c = &g->apu.ch[ch];
    if (r == 1) write_length(g, ch, v);
    if (ch == CH_WAVE && r == 0) {
        c->dac = (v & 0x80) != 0;
        if (!c->dac) c->enabled = 0;
    } else if (ch != CH_WAVE && r == 2) {
        c->dac = (v & 0xF8) != 0;
        if (!c->dac) c->enabled = 0;
    }
}

void apu_reset(gb_t *g) {
    gb_apu *a = &g->apu;
    a->power = 1;
    a->ch[CH_SQUARE1].dac = (g->io[NR12] & 0xF8) != 0;
    a->ch[CH_SQUARE1].enabled = 1; /* the boot sound leaves it on, silent */
    a->ch[CH_SQUARE2].dac = (g->io[NR22] & 0xF8) != 0;
    a->ch[CH_WAVE].dac = (g->io[NR30] & 0x80) != 0;
    a->ch[CH_NOISE].dac = (g->io[NR42] & 0xF8) != 0;
    a->ch[CH_NOISE].lfsr = 0x7FFF;
    a->ch[CH_SQUARE1].timer = period(g, CH_SQUARE1);
}

void gb_set_sample_rate(gb_t *g, uint32_t rate) {
    gb_apu *a = &g->apu;
    a->sample_rate = rate;
    a->sample_clock = 0;
    a->dirty = 1;
    /* The real high-pass capacitor loses 0.0042% per T-cycle; this is the
     * first-order approximation of 0.999958 ^ (cycles per sample). */
    a->charge = rate ? 1.0f - 0.000042f * (float)CPU_HZ / (float)rate : 0;
}

size_t gb_audio_take(gb_t *g, int16_t *out, size_t max_frames) {
    gb_apu *a = &g->apu;
    size_t n = a->audio_len < max_frames ? a->audio_len : max_frames;
    for (size_t i = 0; i < n * 2; i++) out[i] = a->audio[i];
    for (size_t i = n * 2; i < a->audio_len * 2; i++) a->audio[i - n * 2] = a->audio[i];
    a->audio_len -= (uint32_t)n;
    return n;
}
