/* The AICA sound chip: see aica.h and docs/design.md section 16.
 *
 * Registers are 16 bits of data every 4 bytes, 0x0000-0x7fff:
 *   0x0000-0x1fff  64 channels, 0x80 bytes each
 *   0x2000-0x2047  effect output levels and pans (EFSDL, EFPAN)
 *   0x2800-0x2d07  common: master volume, ring buffer, monitor, timers,
 *                  interrupts, ARM reset
 *   0x3000-0x45c7  the effect DSP: coefficients, memory addresses, the
 *                  program, its registers (see "the effect DSP" below)
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
#include "../machine/mem_track.h"   /* recomp: rollback (docs/design.md section 55) */

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

/* one DSP step, decoded when its registers are written */
typedef struct dstep {
    u8 tra, twt, twa, xsel, ysel, ira, iwt, iwa, table, mwt, mrd, ewt;
    u8 ewa, adrl, frcl, shift, yrl, negb, zero, bsel, nofl, masa, adreb, nxadr;
} dstep;

/* the effect DSP's working state */
typedef struct dsp {
    dstep prog[128];
    s32 temp[128];              /* 24-bit, addressed relative to dec */
    s32 mems[32];               /* 24-bit */
    s32 mixs[16];               /* 20-bit: this sample's channel sends */
    s32 efreg[16];              /* 16-bit outputs */
    s32 acc;                    /* 26-bit accumulator */
    s32 memval[2];              /* reads in flight: a read is stored by IWT two steps later */
    u32 frc, yreg, adrs;
    u32 dec;                    /* MDEC_CT: down by one each sample */
    u64 saturated;              /* SHIFT 0/1 results that hit the 24-bit limit */
    u32 peak;                   /* the largest output seen */
} dsp;

typedef struct aica {
    u16 reg[0x8000 / 4];
    dsp d;
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
    u64 key_ons_isel[16];       /* key-ons with a DSP send, by send (ISEL) */
    u32 dsp_program;            /* the DSP's program has a nonzero word */
} aica;

static aica st;
static u8 *ram;

/* the audio buffer (not machine state: snapshots leave it) */
#define AUDIO_FRAMES 65536u
static s16 audio[AUDIO_FRAMES * 2];
static u32 audio_w, audio_r;    /* frame counters; the buffer holds w - r frames */
static int audio_drop;          /* aica_audio_drop: re-run frames' samples are not kept */
static u32 audio_dropped;

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
    if ((CR(c, 0x20) >> 4) & 15) {
        st.key_ons_dsp++;
        st.key_ons_isel[CR(c, 0x20) & 15]++;
    }
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

/* attenuations for a level (0-15) and pan (0-31): the louder side gets
 * the level, the other side the level plus the pan's */
static inline void pan_split(u32 base, u32 pan, u32 *al, u32 *ar)
{
    u32 side = base + send_level[~pan & 15];
    *al = pan & 0x10 ? base : side;
    *ar = pan & 0x10 ? side : base;
}

/* one sample of channel c: added to *l, *r and its DSP send, all at 20
 * bits (a full-scale 16-bit sample is 2^19) */
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
    u32 al, ar;
    pan_split(tl + send_level[dl], pan, &al, &ar);
    *l += (s32)(((s64)s * gain_of(eg_att + al)) >> 11);
    *r += (s32)(((s64)s * gain_of(eg_att + ar)) >> 11);
    u32 sendr = CR(c, 0x20), imxl = (sendr >> 4) & 15;     /* the DSP send: ISEL, IMXL */
    if (imxl)
        st.d.mixs[sendr & 15] += (s32)(((s64)s * gain_of(eg_att + tl + send_level[imxl])) >> 11);

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

/* ---- the effect DSP ----
 *
 * 128 steps a sample, each a 64-bit instruction in four registers at
 * 0x3400 + 16 * step (the first holds bits 63-48):
 *   63-57 TRA  56 TWT  55-49 TWA  47 XSEL  46-45 YSEL  44-39 IRA  38 IWT
 *   37-33 IWA  31 TABLE  30 MWT  29 MRD  28 EWT  27-24 EWA  23 ADRL
 *   22 FRCL  21-20 SHIFT  19 YRL  18 NEGB  17 ZERO  16 BSEL  15 NOFL
 *   14-9 MASA  8 ADREB  7 NXADR
 * A step multiplies X (24 bits: TEMP or the input) by Y (13 bits: a
 * coefficient, FRC or Y_REG), shifts it right 12 and adds B (TEMP or
 * the accumulator, negated or zero). What a step stores (TEMP, the ring
 * buffer, an output, FRC, ADRS) is the previous step's accumulator after
 * SHIFT. A read from the ring buffer is stored into MEMS by IWT two steps
 * later. The game's program keeps to that: its reads are on odd steps,
 * each one stored two steps on.
 *
 * The ring buffer is 16-bit words in sound RAM from RBP * 2 KiB, 8K to 64K
 * words (RBL), addressed relative to dec, which counts down every sample.
 * Words are a 16-bit float (sign, 4-bit exponent, 11 bits) unless NOFL. */

static inline s32 sx(s32 v, int bits)
{
    return (s32)((u32)v << (32 - bits)) >> (32 - bits);
}

static inline s32 clamp24(s32 v, u64 *count)
{
    if (v > 0x7fffff) { ++*count; return 0x7fffff; }
    if (v < -0x800000) { ++*count; return -0x800000; }
    return v;
}

/* 16-bit float to 24 bits: the sign, the exponent's copies of it, a
 * leading bit (unless the exponent is 12 or more), the 11 bits */
static s32 dsp_unpack(u16 w)
{
    u32 sign = w >> 15, e = (w >> 11) & 15, m = w & 0x7ff;
    u32 v = m << 11 | sign << 23;
    if (e > 11) {
        e = 11;
        v |= sign << 22;
    } else {
        v |= (sign ^ 1) << 22;
    }
    return sx((s32)v, 24) >> e;
}

/* 24 bits to the 16-bit float: the low bits that do not fit are dropped */
static u16 dsp_pack(s32 v)
{
    u32 u = (u32)v & 0xffffff, sign = u >> 23, e = 0;
    while (e < 12 && ((u >> (22 - e)) & 1) == sign)
        e++;
    u32 m = e < 12 ? ((u << e) >> 11) & 0x7ff : u & 0x7ff;
    return (u16)(sign << 15 | e << 11 | m);
}

static void dsp_decode(u32 step)
{
    const u16 *w = &st.reg[(0x3400 + 16 * step) >> 2];
    u64 v = (u64)w[0] << 48 | (u64)w[1] << 32 | (u64)w[2] << 16 | w[3];
    dstep *p = &st.d.prog[step];
    p->tra = (u8)(v >> 57) & 0x7f; p->twt = (u8)(v >> 56) & 1; p->twa = (u8)(v >> 49) & 0x7f;
    p->xsel = (u8)(v >> 47) & 1; p->ysel = (u8)(v >> 45) & 3; p->ira = (u8)(v >> 39) & 0x3f;
    p->iwt = (u8)(v >> 38) & 1; p->iwa = (u8)(v >> 33) & 0x1f;
    p->table = (u8)(v >> 31) & 1; p->mwt = (u8)(v >> 30) & 1; p->mrd = (u8)(v >> 29) & 1;
    p->ewt = (u8)(v >> 28) & 1; p->ewa = (u8)(v >> 24) & 15; p->adrl = (u8)(v >> 23) & 1;
    p->frcl = (u8)(v >> 22) & 1; p->shift = (u8)(v >> 20) & 3; p->yrl = (u8)(v >> 19) & 1;
    p->negb = (u8)(v >> 18) & 1; p->zero = (u8)(v >> 17) & 1; p->bsel = (u8)(v >> 16) & 1;
    p->nofl = (u8)(v >> 15) & 1; p->masa = (u8)(v >> 9) & 0x3f; p->adreb = (u8)(v >> 8) & 1;
    p->nxadr = (u8)(v >> 7) & 1;
}

static void dsp_sample(void)
{
    dsp *d = &st.d;
    u32 ring = R(0x2804), mask = (0x2000u << ((ring >> 13) & 3)) - 1, base = (ring & 0xfff) << 11;
    s32 acc = d->acc;
    for (u32 i = 0; i < 16; i++)
        d->mixs[i] = d->mixs[i] > 0x7ffff ? 0x7ffff : d->mixs[i] < -0x80000 ? -0x80000 : d->mixs[i];
    for (u32 step = 0; step < 128; step++) {
        const dstep *p = &d->prog[step];
        s32 in = p->ira < 0x20 ? d->mems[p->ira] : p->ira < 0x30 ? d->mixs[p->ira - 0x20] * 16 : 0;    /* EXTS: nothing */
        s32 shifted;
        switch (p->shift) {
        case 0: shifted = clamp24(acc, &d->saturated); break;
        case 1: shifted = clamp24(acc * 2, &d->saturated); break;
        case 2: shifted = sx(acc * 2, 24); break;
        default: shifted = sx(acc, 24); break;
        }
        if (p->iwt)
            d->mems[p->iwa] = d->memval[step & 1];

        s32 t = d->temp[(p->tra + d->dec) & 127];
        s32 b = p->zero ? 0 : p->bsel ? acc : t;
        if (p->negb)
            b = -b;
        s32 x = p->xsel ? in : t, y;
        switch (p->ysel) {
        case 0: y = sx((s32)d->frc, 13); break;
        case 1: y = (s16)R(0x3000 + 4 * step) >> 3; break;
        case 2: y = sx((s32)(d->yreg >> 11), 13); break;
        default: y = (s32)((d->yreg >> 4) & 0xfff); break;
        }
        if (p->yrl)
            d->yreg = (u32)in & 0xffffff;
        acc = sx((s32)(((s64)x * y) >> 12) + b, 26);

        if (p->twt)
            d->temp[(p->twa + d->dec) & 127] = shifted;
        if (p->frcl)
            d->frc = p->shift == 3 ? (u32)shifted & 0xfff : ((u32)shifted >> 11) & 0x1fff;
        if (p->adrl)
            d->adrs = p->shift == 3 ? ((u32)shifted >> 12) & 0xfff : ((u32)in >> 16) & 0xfff;
        if (p->mrd || p->mwt) {
            u32 a = R(0x3200 + 4 * p->masa);
            if (!p->table)
                a += d->dec;
            if (p->adreb)
                a += d->adrs;
            a += p->nxadr;
            a &= p->table ? 0xffff : mask;
            u32 at = (base + 2 * a) & (AICA_RAM_SIZE - 2);
            if (p->mrd) {
                u16 m;
                memcpy(&m, ram + at, 2);
                d->memval[step & 1] = p->nofl ? sx((s32)m << 8, 24) : dsp_unpack(m);
            }
            if (p->mwt) {
                u16 m = p->nofl ? (u16)(shifted >> 8) : dsp_pack(shifted);
                mem_track_host(ram + at, 2);        /* recomp */
                memcpy(ram + at, &m, 2);
            }
        }
        if (p->ewt)
            d->efreg[p->ewa] = shifted >> 8;
    }
    d->acc = acc;
    d->dec--;
    memset(d->mixs, 0, sizeof d->mixs);
}

/* the DSP's registers as the processors see them: TEMP and MEMS in two
 * registers each (bits 7-0, then 23-8), MIXS likewise (3-0, 19-4), EFREG */
static int dsp_reg(u32 off, s32 **p, int *hi, int *bits)
{
    if (off >= 0x4000 && off < 0x4400) *p = &st.d.temp[(off - 0x4000) >> 3], *bits = 24;
    else if (off >= 0x4400 && off < 0x4500) *p = &st.d.mems[(off - 0x4400) >> 3], *bits = 24;
    else if (off >= 0x4500 && off < 0x4580) *p = &st.d.mixs[(off - 0x4500) >> 3], *bits = 20;
    else if (off >= 0x4580 && off < 0x45c0) { *p = &st.d.efreg[(off - 0x4580) >> 2]; *bits = 16; *hi = -1; return 1; }
    else return 0;
    *hi = (off >> 2) & 1;
    return 1;
}

static u32 dsp_reg_read(s32 *p, int hi, int bits)
{
    u32 lo_bits = bits - 16u;
    if (hi < 0)
        return (u32)*p & 0xffff;
    return hi ? ((u32)*p >> lo_bits) & 0xffff : (u32)*p & ((1u << lo_bits) - 1);
}

static void dsp_reg_write(s32 *p, int hi, int bits, u32 v)
{
    u32 lo_bits = bits - 16u, u = (u32)*p;
    if (hi < 0)
        u = v & 0xffff;
    else if (hi)
        u = (u & ((1u << lo_bits) - 1)) | (v & 0xffff) << lo_bits;
    else
        u = (u & ~((1u << lo_bits) - 1)) | (v & ((1u << lo_bits) - 1));
    *p = sx((s32)u, bits);
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
    default: {
        s32 *p;
        int hi, bits;
        if (off >= 0x4000 && dsp_reg(off, &p, &hi, &bits))
            return dsp_reg_read(p, hi, bits);
        return R(off);
    }
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
    default: {
        s32 *p;
        int hi, bits;
        if (off >= 0x4000 && dsp_reg(off, &p, &hi, &bits)) {
            dsp_reg_write(p, hi, bits, nv);
            return;
        }
        if (off >= 0x3400 && off < 0x3c00 && nv)
            st.dsp_program = 1;
        if (off == 0x288c && (v & mask & 1))
            st.dma_starts++;                            /* the chip's own DMA: not modelled */
        R(off) = (u16)nv;
        if (off >= 0x3400 && off < 0x3c00)
            dsp_decode((off - 0x3400) >> 4);
        return;
    }
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
    audio_drop = 0;
    audio_dropped = 0;
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
        dsp_sample();
        for (u32 e = 0; e < 16; e++) {                  /* the DSP's outputs: EFSDL, EFPAN */
            s32 ev = st.d.efreg[e];
            u32 lv = R(0x2000 + 4 * e), efsdl = (lv >> 8) & 15;
            if (!ev || !efsdl)
                continue;
            u32 a = (u32)(ev < 0 ? -ev : ev), al, ar;
            if (a > st.d.peak)
                st.d.peak = a;
            pan_split(send_level[efsdl], lv & 31, &al, &ar);
            l += (s32)(((s64)ev * gain_of(al)) >> 11);
            r += (s32)(((s64)ev * gain_of(ar)) >> 11);
        }
        /* master volume, and 2^-6: the sum is at 20 bits, the mix level an
         * estimate (docs/design.md section 17) */
        u32 mv = gain_of(send_level[R(0x2800) & 15]);
        l = (s32)(((s64)l * mv) >> 21);
        r = (s32)(((s64)r * mv) >> 21);
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        st.samples++;
        if (audio_drop) {
            audio_dropped++;
            continue;
        }
        u32 i = audio_w % AUDIO_FRAMES;
        audio[2 * i] = (s16)l;
        audio[2 * i + 1] = (s16)r;
        audio_w++;
        if (audio_w - audio_r > AUDIO_FRAMES)
            audio_r = audio_w - AUDIO_FRAMES;
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

unsigned aica_audio_drop(int on)
{
    unsigned n = audio_dropped;
    audio_drop = on;
    audio_dropped = 0;
    return n;
}

uint32_t aica_audio_kept(void)
{
    return audio_w;
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
    fprintf(f, "  sound chip: DSP sends by ISEL:");
    for (int i = 0; i < 16; i++)
        if (st.key_ons_isel[i])
            fprintf(f, " %d:%llu", i, (unsigned long long)st.key_ons_isel[i]);
    fprintf(f, "; outputs peak %u of 32767, %llu saturated results\n", st.d.peak,
            (unsigned long long)st.d.saturated);
}
