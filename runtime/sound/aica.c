/* The AICA sound chip: see aica.h and docs/design.md section 16.
 *
 * Registers are 16 bits of data every 4 bytes, 0x0000-0x7fff:
 *   0x0000-0x1fff  64 channels, 0x80 bytes each
 *   0x2000-0x2047  effect output levels (the DSP's, kept only)
 *   0x2800-0x2d07  common: master volume, monitor, timers, interrupts, ARM reset
 *   0x3000-0x7fff  the DSP's program and memory (kept only: no DSP yet)
 * The main CPU reaches them at 0x00700000, the ARM at 0x00800000; the ARM
 * sees sound RAM at 0.
 *
 * Levels are in 1/16 octave (0.376 dB); the envelope runs from 0 (loudest)
 * to 0x3FF in 16.16 fixed point, a quarter of its unit per level step; the
 * rate and gain tables are tools/aica_tables.py's. No floating point: the
 * output is the same on every platform. */
#include "aica.h"
#include "arm7.h"
#include "aica_tables.h"

#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE };
#define EG_MAX (0x3ffu << 16)
#define FRAC_BITS 18

typedef struct chan {
    u8 on;                      /* producing samples */
    u8 kon;                     /* keyed on (KYONB taken by the last KYONEX) */
    u8 eg_state;
    u8 lp;                      /* passed the loop end (the monitor's LP) */
    u8 loop_saved;              /* ADPCM: the decoder's state at the loop start is kept */
    u32 eg;                     /* attenuation, 16.16, 0 .. EG_MAX */
    u32 pos, frac;              /* play position: samples, and 1/2^18 of one */
    s32 a_sig, a_step;          /* ADPCM decoder */
    u32 a_next;                 /* ADPCM: the sample the decoder gives next */
    s32 a_prev, a_cur;          /* ADPCM: samples a_next - 2 and a_next - 1 */
    s32 l_sig, l_step;          /* ADPCM: decoder state at the loop start */
    u32 noise;
} chan;

typedef struct aica {
    u16 reg[0x8000 / 4];
    chan ch[64];
    arm7 arm;
    u32 arm_running;            /* out of reset */
    u32 budget;                 /* ARM instructions per sample */
    u32 timer_pre[3];           /* samples towards the next timer count */
    u32 level;                  /* L: the ARM's interrupt level in service */
    u32 in_service;             /* an interrupt was given to the ARM and not ended (RM) */
    u64 samples;
    u64 key_ons, dma_starts;
    u32 main_irq_enabled_seen;
    u64 main_doorbells;         /* the ARM raised the main CPU's bit 5 */
    u64 key_ons_filter, key_ons_lfo, key_ons_dsp;   /* key-ons with the filter on, an LFO depth, a DSP send */
    u32 dsp_program;            /* the DSP's program has a nonzero word */
} aica;

static aica st;
static u8 *ram;

/* the audio buffer (not machine state: snapshots leave it) */
#define AUDIO_FRAMES 65536u
static s16 audio[AUDIO_FRAMES * 2];
static u32 audio_w, audio_r;    /* frame counters; the buffer holds w - r frames */

#define R(off) st.reg[(off) >> 2]

/* ---- interrupts ---- */

static void arm_irq_update(void)
{
    if (st.in_service || !st.arm_running)
        return;
    u32 p = R(0x28a0) & R(0x289c) & 0x7ff;
    if (!p)
        return;
    u32 i = (u32)__builtin_ctz(p), b = i > 7 ? 7 : i;
    st.level = ((R(0x28a8) >> b) & 1) | ((R(0x28ac) >> b) & 1) << 1 | ((R(0x28b0) >> b) & 1) << 2;
    st.in_service = 1;
    st.arm.fiq = 1;
}

/* an interrupt source: the ARM's and the main CPU's pending bits */
static void raise(u32 bit)
{
    R(0x28a0) |= (u16)(1u << bit);
    R(0x28b8) |= (u16)(1u << bit);
    arm_irq_update();
}

int aica_main_irq(void)
{
    return (R(0x28b8) & R(0x28b4) & 0x7ff) != 0;
}

/* ---- channels ---- */

#define CR(c, off) st.reg[((c) * 0x80 + (off)) >> 2]

static void key_on(int c)
{
    chan *h = &st.ch[c];
    memset(h, 0, sizeof *h);
    h->kon = 1;
    h->on = 1;
    h->eg_state = EG_ATTACK;
    h->eg = 0x280u << 16;
    h->a_step = 0x7f;
    h->noise = 1;
    st.key_ons++;
    if (!((CR(c, 0x28) >> 5) & 1))
        st.key_ons_filter++;
    if (CR(c, 0x1c) & 0xe7)
        st.key_ons_lfo++;
    if ((CR(c, 0x20) >> 4) & 15)
        st.key_ons_dsp++;
}

static void key_exec(void)
{
    for (int c = 0; c < 64; c++) {
        int kyonb = (CR(c, 0x00) >> 14) & 1;
        chan *h = &st.ch[c];
        if (kyonb && !h->kon)
            key_on(c);
        else if (!kyonb && h->kon) {
            h->kon = 0;
            if (h->on)
                h->eg_state = EG_RELEASE;
        }
    }
}

/* the effective rate of an envelope rate register (key scaling) */
static u32 eff_rate(int c, u32 rate)
{
    if (!rate)
        return 0;
    u32 e = 2 * rate, krs = (CR(c, 0x14) >> 10) & 15;
    if (krs != 15) {
        u32 pitch = CR(c, 0x18);
        s32 oct = (s32)(((pitch >> 11) & 15) ^ 8) - 8;
        e += (pitch >> 9) & 1;
        s32 k = ((s32)krs + oct) * 2;
        if (k > 0)
            e += (u32)k;
    }
    return e > 63 ? 63 : e;
}

static void eg_step(int c, chan *h)
{
    switch (h->eg_state) {
    case EG_ATTACK: {
        u32 m = aica_attack_mul[eff_rate(c, CR(c, 0x10) & 31)];
        if (m == 1)
            h->eg = 0;
        else if (m)
            h->eg = (u32)(((u64)h->eg * m) >> 32);
        int link = (CR(c, 0x14) >> 14) & 1;     /* LPSLNK: the attack ends at the loop start */
        if (h->eg < 0x10000u || (link && h->pos >= CR(c, 0x08))) {
            if (h->eg < 0x10000u)
                h->eg = 0;
            h->eg_state = EG_DECAY1;
        }
        break;
    }
    case EG_DECAY1:
        h->eg += aica_decay_inc[eff_rate(c, (CR(c, 0x10) >> 6) & 31)];
        if ((h->eg >> 16) >= (u32)((CR(c, 0x14) >> 5) & 31) << 5)
            h->eg_state = EG_DECAY2;
        if (h->eg >= EG_MAX)
            h->eg = EG_MAX;
        break;
    case EG_DECAY2:
        h->eg += aica_decay_inc[eff_rate(c, (CR(c, 0x10) >> 11) & 31)];
        if (h->eg >= EG_MAX)
            h->eg = EG_MAX;
        break;
    default:
        h->eg += aica_decay_inc[eff_rate(c, CR(c, 0x14) & 31)];
        if (h->eg >= EG_MAX) {
            h->eg = EG_MAX;
            h->on = 0;
        }
        break;
    }
}

static inline s32 adpcm_nibble(chan *h, u32 n)
{
    static const s32 quant[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    s32 d = (h->a_step * (s32)(2 * (n & 7) + 1)) >> 3;
    if (d > 0x7fff)
        d = 0x7fff;
    s32 s = (n & 8) ? h->a_sig - d : h->a_sig + d;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    h->a_sig = s;
    s32 st2 = (h->a_step * quant[n & 7]) >> 8;
    if (st2 < 0x7f) st2 = 0x7f;
    if (st2 > 0x6000) st2 = 0x6000;
    h->a_step = st2;
    return s;
}

/* ADPCM: decodes on until the samples at pos and pos + 1 are a_prev, a_cur */
static void adpcm_to(chan *h, u32 sa, u32 lsa, u32 pos)
{
    while (h->a_next < pos + 2) {
        if (h->a_next == lsa && !h->loop_saved) {
            h->l_sig = h->a_sig;
            h->l_step = h->a_step;
            h->loop_saved = 1;
        }
        u8 b = ram[(sa + (h->a_next >> 1)) & (AICA_RAM_SIZE - 1)];
        u32 n = (h->a_next & 1) ? b >> 4 : b & 15;
        h->a_prev = h->a_cur;
        h->a_cur = adpcm_nibble(h, n);
        h->a_next++;
    }
}

static inline s32 pcm_at(u32 sa, u32 fmt, u32 i)
{
    if (fmt == 0) {
        s16 v;
        memcpy(&v, ram + ((sa + 2 * i) & (AICA_RAM_SIZE - 2)), 2);
        return v;
    }
    return (s32)(int8_t)ram[(sa + i) & (AICA_RAM_SIZE - 1)] << 8;
}

static const u8 send_level[16] = { 255, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0 };

static inline u32 gain_of(u32 att)
{
    return att >= 255 ? 0 : aica_gain[att];
}

/* one sample of channel c, added to *l, *r */
static void chan_sample(int c, s32 *l, s32 *r)
{
    chan *h = &st.ch[c];
    u32 r0 = CR(c, 0x00);
    u32 sa = ((r0 & 0x7f) << 16) | CR(c, 0x04), fmt = (r0 >> 7) & 3;
    u32 lsa = CR(c, 0x08), lea = CR(c, 0x0c), loop = (r0 >> 9) & 1;
    s32 s;
    if ((r0 >> 10) & 1) {                               /* SSCTL: noise */
        h->noise = h->noise * 1103515245u + 12345u;
        s = (s32)(int16_t)(h->noise >> 16);
    } else if (fmt >= 2) {
        adpcm_to(h, sa, lsa, h->pos);
        s = h->a_prev + (s32)(((s64)(h->a_cur - h->a_prev) * h->frac) >> FRAC_BITS);
    } else {
        u32 nx = h->pos + 1;
        if (nx >= lea && loop && lea > lsa)
            nx = lsa;
        s32 a = pcm_at(sa, fmt, h->pos), b = pcm_at(sa, fmt, nx);
        s = a + (s32)(((s64)(b - a) * h->frac) >> FRAC_BITS);
    }

    /* levels: envelope, total level, direct level, pan */
    u32 pit = CR(c, 0x18);
    u32 eg_att = h->eg >> 18;
    u32 tl = CR(c, 0x28) >> 8, dl = (CR(c, 0x24) >> 8) & 15, pan = CR(c, 0x24) & 31;
    u32 full = tl + send_level[dl], side = full + send_level[~pan & 15];
    u32 al = pan & 0x10 ? full : side, ar = pan & 0x10 ? side : full;
    s32 vl = (s32)(((s64)s * gain_of(eg_att + al)) >> 17);
    s32 vr = (s32)(((s64)s * gain_of(eg_att + ar)) >> 17);
    *l += vl;
    *r += vr;

    /* on: pitch, the loop, the envelope */
    s32 oct = (s32)(((pit >> 11) & 15) ^ 8) - 8;
    u32 step = (1024u + (pit & 0x3ff)) << (oct + 8);
    h->frac += step;
    h->pos += h->frac >> FRAC_BITS;
    h->frac &= (1u << FRAC_BITS) - 1;
    if (h->pos >= lea) {
        h->lp = 1;
        if (loop && lea > lsa) {
            h->pos = lsa + (h->pos - lea) % (lea - lsa);
            if (fmt == 2 && h->loop_saved) {        /* ADPCM: from the state at the loop start */
                h->a_sig = h->l_sig;
                h->a_step = h->l_step;
                h->a_next = lsa;
                h->a_cur = h->a_sig;
            } else if (fmt == 3 && h->a_next > h->pos + 2) {
                h->a_next = h->pos;                 /* the long stream form keeps its state */
            }
        } else {
            h->on = 0;
            h->eg = EG_MAX;
            h->eg_state = EG_RELEASE;
            return;
        }
    }
    eg_step(c, h);
}

/* ---- the ARM's bus ---- */

static u32 reg_read(u32 off)
{
    off &= 0x7ffc;
    switch (off) {
    case 0x2808:
        return 0x0900;                                  /* MIDI: both buffers empty */
    case 0x2810: {
        u32 c = (R(0x280c) >> 8) & 63;
        chan *h = &st.ch[c];
        u32 v = (u32)h->lp << 15 | (u32)h->eg_state << 13;
        if (!((R(0x280c) >> 14) & 1))                  /* AFSET clear: the amplitude envelope */
            v |= h->on ? (h->eg >> 16) : 0x3ff;
        h->lp = 0;
        return v;
    }
    case 0x2814:
        return st.ch[(R(0x280c) >> 8) & 63].pos & 0xffff;
    case 0x2d00:
        st.arm.fiq = 0;                                 /* reading the level takes the interrupt */
        return st.level;
    default:
        return R(off);
    }
}

static void timer_set(int t, u32 v)
{
    R(0x2890 + 4 * t) = (u16)(v & 0x07ff);
}

static void reg_write(u32 off, u32 v, u32 mask)
{
    off &= 0x7ffc;
    u32 old = R(off), nv = (old & ~mask) | (v & mask);
    if (off < 0x2000) {
        R(off) = (u16)(nv & ((off & 0x7f) == 0 ? 0x7fff : 0xffff));
        if ((off & 0x7f) == 0 && (mask & 0x8000) && (v & 0x8000))
            key_exec();
        return;
    }
    switch (off) {
    case 0x2810:
    case 0x2814:
    case 0x2d00:
        return;
    case 0x2890:
    case 0x2894:
    case 0x2898:
        timer_set((int)(off - 0x2890) / 4, nv);
        return;
    case 0x289c:
        R(off) = (u16)(nv & 0x7ff);
        arm_irq_update();
        return;
    case 0x28a0:                                        /* only bit 5 can be raised by writing */
        if (v & mask & 0x20)
            raise(5);
        return;
    case 0x28a4:
        R(0x28a0) &= (u16)~(v & mask);
        return;
    case 0x28b4:
        R(off) = (u16)(nv & 0x7ff);
        if (nv & 0x7ff)
            st.main_irq_enabled_seen = 1;
        return;
    case 0x28b8:
        if (v & mask & 0x20) {
            R(0x28b8) |= 0x20;
            st.main_doorbells++;
        }
        return;
    case 0x28bc:
        R(0x28b8) &= (u16)~(v & mask);
        return;
    case 0x2c00:
        R(off) = (u16)nv;
        if (nv & 1)
            st.arm_running = 0;
        else if (!st.arm_running) {
            arm7_reset(&st.arm);
            st.arm_running = 1;
            st.in_service = 0;
        }
        return;
    case 0x2d04:                                        /* RM: the interrupt is over */
        if (v & mask & 1) {
            st.in_service = 0;
            st.arm.fiq = 0;
            arm_irq_update();
        }
        return;
    default:
        if (off >= 0x3400 && off < 0x3c00 && nv)
            st.dsp_program = 1;
        if (off == 0x288c && (v & mask & 1))
            st.dma_starts++;                            /* the chip's own DMA: not modelled */
        R(off) = (u16)nv;
        return;
    }
}

uint32_t aica_read(uint32_t off) { return reg_read(off); }
void aica_write(uint32_t off, uint32_t v) { reg_write(off, v, 0xffff); }

/* the ARM's view: sound RAM below 0x00800000 (arm7.c), the registers at 0x00800000 */
uint32_t arm7_bus_r32(uint32_t a)
{
    if (a >= 0x00800000u && a < 0x00808000u)
        return reg_read(a - 0x00800000u);
    return 0;
}
uint32_t arm7_bus_r16(uint32_t a)
{
    return arm7_bus_r32(a & ~3u) >> ((a & 2) ? 16 : 0) & 0xffff;
}
uint32_t arm7_bus_r8(uint32_t a)
{
    return arm7_bus_r32(a & ~3u) >> ((a & 3) * 8) & 0xff;
}
void arm7_bus_w32(uint32_t a, uint32_t v)
{
    if (a >= 0x00800000u && a < 0x00808000u)
        reg_write(a - 0x00800000u, v, 0xffff);
}
void arm7_bus_w16(uint32_t a, uint32_t v)
{
    if ((a & 2) == 0)
        arm7_bus_w32(a, v);
}
void arm7_bus_w8(uint32_t a, uint32_t v)
{
    if (a >= 0x00800000u && a < 0x00808000u && (a & 2) == 0)
        reg_write((a - 0x00800000u) & ~3u, (a & 1) ? v << 8 : v, (a & 1) ? 0xff00 : 0x00ff);
}

/* ---- running ---- */

void aica_init(uint8_t *sound_ram, unsigned arm_budget)
{
    memset(&st, 0, sizeof st);
    ram = sound_ram;
    st.budget = arm_budget ? arm_budget : 128;
    R(0x2c00) = 1;                                      /* the ARM held in reset */
    for (int c = 0; c < 64; c++)
        st.ch[c].eg = EG_MAX, st.ch[c].eg_state = EG_RELEASE;
    arm7_set_ram(sound_ram, AICA_RAM_SIZE - 1);
    audio_w = audio_r = 0;
}

void aica_run(unsigned n)
{
    while (n--) {
        if (st.arm_running)
            arm7_run(&st.arm, st.budget);
        /* timers: count every 2^TxCTL samples; the interrupt on the way from 0xFF to 0 */
        for (int t = 0; t < 3; t++) {
            u32 v = R(0x2890 + 4 * t), ctl = (v >> 8) & 7;
            if (++st.timer_pre[t] >= (1u << ctl)) {
                st.timer_pre[t] = 0;
                u32 cnt = (v & 0xff) + 1;
                if (cnt > 0xff) {
                    cnt = 0;
                    raise(6 + (u32)t);
                }
                R(0x2890 + 4 * t) = (u16)((v & 0x700) | cnt);
            }
        }
        raise(10);                                      /* one sample (taken only where enabled) */
        if (!(R(0x289c) & 0x400))
            R(0x28a0) &= (u16)~0x400;
        if (!(R(0x28b4) & 0x400))
            R(0x28b8) &= (u16)~0x400;

        s32 l = 0, r = 0;
        for (int c = 0; c < 64; c++)
            if (st.ch[c].on)
                chan_sample(c, &l, &r);
        u32 mv = gain_of(send_level[R(0x2800) & 15]);
        l = (s32)(((s64)l * mv) >> 15);
        r = (s32)(((s64)r * mv) >> 15);
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        u32 i = audio_w % AUDIO_FRAMES;
        audio[2 * i] = (s16)l;
        audio[2 * i + 1] = (s16)r;
        audio_w++;
        if (audio_w - audio_r > AUDIO_FRAMES)
            audio_r = audio_w - AUDIO_FRAMES;
        st.samples++;
    }
}

unsigned aica_audio_take(int16_t *dst, unsigned max)
{
    unsigned n = 0;
    while (n < max && audio_r != audio_w) {
        u32 i = audio_r % AUDIO_FRAMES;
        dst[2 * n] = audio[2 * i];
        dst[2 * n + 1] = audio[2 * i + 1];
        audio_r++;
        n++;
    }
    return n;
}

void aica_audio_discard(void)
{
    audio_r = audio_w;
}

void *aica_state(unsigned *size)
{
    *size = sizeof st;
    return &st;
}

void aica_stats(FILE *f)
{
    int on = 0;
    for (int c = 0; c < 64; c++)
        on += st.ch[c].on;
    fprintf(f, "  sound chip: %llu samples, ARM %s (%llu instructions, %u a sample), %llu key-ons, %d channels "
            "sounding%s%s\n", (unsigned long long)st.samples, st.arm_running ? "running" : "in reset",
            (unsigned long long)st.arm.insns, st.budget, (unsigned long long)st.key_ons, on,
            st.main_irq_enabled_seen ? "; the game enabled the chip's interrupt to the main CPU" : "",
            st.dma_starts ? "; the chip's own DMA was started (not modelled)" : "");
    fprintf(f, "  sound chip: main CPU interrupt enable %03x, pending %03x, %llu doorbells from the ARM\n",
            R(0x28b4), R(0x28b8), (unsigned long long)st.main_doorbells);
    fprintf(f, "  sound chip: key-ons with the filter on %llu, with an LFO depth %llu, sent to the DSP %llu; "
            "DSP program %s\n", (unsigned long long)st.key_ons_filter, (unsigned long long)st.key_ons_lfo,
            (unsigned long long)st.key_ons_dsp, st.dsp_program ? "loaded" : "empty");
}
