/* SH-4 interpreter.
 *
 * Written from the processor's public documentation (the instruction
 * descriptions of the SH-4 software manual) and from what the decompilation
 * shows the game uses: the integer set, the FPU with FPSCR.PR/SZ/FR, fsca,
 * fsrra, ftrv, fipr, frchg/fschg, the store queues behind pref, mac.l/mac.w
 * with the S bit, delay slots, the register banks and exception entry.
 * No emulator code was consulted or copied.
 *
 * Known departures from the hardware, all deliberate:
 *   - no MMU, no caches (ocbi/ocbp/ocbwb/movca.l act as plain stores or
 *     nothing);
 *   - fsca and fsrra give the correctly rounded host result, where the chip
 *     reads a table (the last bit can differ); fipr and ftrv are plain
 *     single-precision sums, where the chip's result is approximate; fmac
 *     is an exact fused multiply-add;
 *   - with FPSCR.DN set, denormal inputs and results are zero (host DAZ/FTZ).
 *
 * FPU exceptions (the manual's section on the FPU and the operation text of
 * each instruction; docs/hle.md, "The FPU's special cases", says what is
 * the manual's and what is inferred): every result that is not a number is
 * the chip's own quiet NaN (7FBFFFFF, or 7FF7FFFF FFFFFFFF in double
 * precision), whatever the operands were; each arithmetic instruction
 * clears the cause field of FPSCR and sets the cause and flag bits of what
 * happened (invalid operation, division by zero, overflow, underflow,
 * inexact, taken from the host's own flags, which are cleared before every
 * such instruction); an exception whose enable bit is set, and an "FPU
 * error" (a denormalized operand with DN clear), enters the exception
 * handler with EXPEVT 0x120 and the destination unchanged.
 */
#include "sh4.h"
#include "mem.h"
#include "dispatch.h"
#include "observe.h"
#include "machine.h"

#include <math.h>
#include <string.h>
#include <xmmintrin.h>

#define RN (c->r[(op >> 8) & 15])
#define RM (c->r[(op >> 4) & 15])
#define N ((op >> 8) & 15)
#define M ((op >> 4) & 15)
#define IMM8S ((s32)(s8)(op & 0xff))
#define IMM8U (op & 0xff)

const char *sh4_stop_name(int code)
{
    static const char *const names[] = {
        "none", "event", "return", "illegal instruction", "pc outside code",
        "break", "fatal", "limit", "sleep with nothing scheduled"
    };
    if (code < 0 || code >= (int)(sizeof names / sizeof names[0]))
        return "?";
    return names[code];
}

static void swap_bank(sh4 *c)
{
    for (int i = 0; i < 8; i++) {
        u32 t = c->r[i];
        c->r[i] = c->r_bank[i];
        c->r_bank[i] = t;
    }
}

u32 sh4_get_sr(const sh4 *c)
{
    return (c->sr & ~SR_T) | (c->t & 1);
}

void sh4_set_sr(sh4 *c, u32 v)
{
    u32 old = c->sr;
    v &= SR_VALID;
    /* r0-r7 come from bank 1 only in privileged mode with RB set */
    int old_b = (old & SR_MD) && (old & SR_RB);
    int new_b = (v & SR_MD) && (v & SR_RB);
    if (old_b != new_b)
        swap_bank(c);
    c->sr = v & ~SR_T;
    c->t = v & 1;
    c->next_event = 0;      /* the interrupt mask may have opened: recheck */
}

u32 sh4_diag;
u64 sh4_nan_results, sh4_fpu_traps;

/* the host's MXCSR for the guest's rounding mode and DN bit, with every
 * exception masked and no flag set: loaded before each arithmetic
 * instruction of the FPU, so that the flags read after it are its own */
static u32 fp_mx = 0x1f80;
/* FPSCR bits an arithmetic instruction keeps, and what "inexact" adds */
static u32 fp_keep = ~FPSCR_CAUSE, fp_i_bits = FPC_I | (FPC_I >> 10);
/* set by --diag nofpflags: the cause and flag bits are not kept, and the two MXCSR instructions around
 * every arithmetic instruction are left out (which, measured, saves nothing) */
static int fp_noflags;
#define FP_CLEAR() do { if (__builtin_expect(!fp_noflags, 1)) _mm_setcsr(fp_mx); } while (0)
#define FP_FLAGS() (__builtin_expect(!fp_noflags, 1) ? _mm_getcsr() : 0u)

static void set_mxcsr_for(u32 fpscr)
{
    u32 m = 0x1f80;                      /* all exceptions masked, nearest */
    switch (fpscr & FPSCR_RM) {
    case 1: m |= 0x6000; break;          /* toward zero */
    default: break;                      /* 0 nearest; 2, 3 reserved */
    }
    if (fpscr & FPSCR_DN)
        m |= 0x8040;                     /* FTZ | DAZ */
    fp_mx = m;
    fp_noflags = (sh4_diag & DIAG_NO_FP_FLAGS) != 0;
    if (fp_noflags)
        fp_keep = ~0u, fp_i_bits = 0;
    else
        fp_keep = ~FPSCR_CAUSE, fp_i_bits = FPC_I | (FPC_I >> 10);
    _mm_setcsr(m);
}

void sh4_set_fpscr(sh4 *c, u32 v)
{
    v &= FPSCR_VALID;
    if ((v ^ c->fpscr) & FPSCR_FR) {
        fpreg t[16];
        memcpy(t, c->fr, sizeof t);
        memcpy(c->fr, c->xf, sizeof t);
        memcpy(c->xf, t, sizeof t);
    }
    c->fpscr = v;
    set_mxcsr_for(v);
}

void sh4_reset(sh4 *c, u32 pc)
{
    memset(c, 0, sizeof *c);
    c->sr = SR_MD | SR_BL | SR_RB | SR_IMASK;
    c->pc = pc;
    c->vbr = 0;
    c->fpscr = 0x00040001;
}

void sh4_exception(sh4 *c, u32 vector_offset)
{
    c->ssr = sh4_get_sr(c);
    c->spc = c->pc;
    c->sgr = c->r[15];
    sh4_set_sr(c, sh4_get_sr(c) | SR_MD | SR_BL | SR_RB);
    c->pc = c->vbr + vector_offset;
    disp_enter_exception(c, c->pc);
}

/* ---- helpers ---- */

static void illegal(sh4 *c, u32 op)
{
    c->pc -= 2;
    machine_stop(c, STOP_ILLEGAL, "word %04x at %08x%s is not an instruction this interpreter knows", op, c->pc,
                 c->in_slot ? " (in a delay slot)" : "");
    c->stop_arg = op;
}

static inline u64 get_dr(sh4 *c, int n)
{
    return ((u64)c->fr[n & 14].u << 32) | c->fr[(n & 14) + 1].u;
}
static inline void set_dr(sh4 *c, int n, u64 v)
{
    c->fr[n & 14].u = (u32)(v >> 32);
    c->fr[(n & 14) + 1].u = (u32)v;
}
static inline double get_d(sh4 *c, int n)
{
    u64 v = get_dr(c, n);
    double d;
    memcpy(&d, &v, 8);
    return d;
}
static inline void set_d(sh4 *c, int n, double d)
{
    u64 v;
    memcpy(&v, &d, 8);
    set_dr(c, n, v);
}

/* ftrc: truncation. Out of range is an invalid operation: the largest
 * value for a positive operand, the most negative one for a negative
 * operand and for every NaN (the manual's ftrc_single_type_of and
 * ftrc_invalid). *bad says so. */
static inline s32 float_to_int(double v, int *bad)
{
    *bad = 1;
    if (v != v)
        return (s32)0x80000000;
    if (v >= 2147483648.0)
        return 0x7fffffff;
    if (v < -2147483648.0)
        return (s32)0x80000000;
    *bad = 0;
    return (s32)v;
}

/* ---- FPU exceptions ---- */

static inline int nan_s(u32 u) { return (u & 0x7fffffffu) > 0x7f800000u; }
/* the chip's signaling NaN has the top bit of the fraction SET (the reverse
 * of the host's): data_type_of in the manual */
static inline int snan_s(u32 u) { return (u & 0x7fffffffu) >= 0x7fc00000u; }
static inline int den_s(u32 u) { return !(u & 0x7f800000u) && (u & 0x007fffffu); }
static inline int nan_d(u64 u) { return (u & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
static inline int snan_d(u64 u) { return (u & 0x7fffffffffffffffull) >= 0x7ff8000000000000ull; }
static inline int den_d(u64 u) { return !(u & 0x7ff0000000000000ull) && (u & 0x000fffffffffffffull); }

/* the host's flags (ZE 4, OE 8, UE 0x10, PE 0x20) as cause bits Z O U I */
static inline u32 host_cause(u32 x)
{
    return ((x & 4) << 13) | ((x & 8) << 11) | ((x & 0x10) << 9) | ((x & 0x20) << 7);
}

/* makes the optimizer treat a value as changed here, so that an operation
 * on it cannot be moved across the MXCSR load before it or the read after */
#define FP_HERE(v) __asm__ volatile("" : "+x"(v))

static void fp_trap(sh4 *c)
{
    sh4_fpu_traps++;
    if (c->in_slot) {
        c->pc -= 2;
        machine_stop(c, STOP_ILLEGAL, "an FPU exception that traps (FPSCR %08x) in the delay slot at %08x: not modelled",
                     c->fpscr, c->pc);
        return;
    }
    c->pc -= 2;                         /* SPC: the instruction itself */
    machine_fpu_exception(c);
}

/* Sets the cause field to what an instruction raised, and the flags of the
 * same exceptions. Returns 1 when it traps: an exception whose enable bit
 * is set, or an FPU error; the instruction must then leave its destination
 * as it was. */
static int fp_raise(sh4 *c, u32 cause)
{
    if (sh4_diag & DIAG_NO_FP_FLAGS)
        return 0;
    c->fpscr = (c->fpscr & ~FPSCR_CAUSE) | cause | ((cause >> 10) & FPSCR_FLAG);
    if ((cause & FPC_E) || (c->fpscr & (cause >> 5) & FPSCR_ENABLE)) {
        fp_trap(c);
        return 1;
    }
    return 0;
}

/* a NaN came out, from operands of which some were NaNs (q) and some
 * signaling ones (s): the cause. A quiet NaN going in is no exception; a
 * signaling one, or no NaN at all (inf - inf, 0 / 0, 0 * inf, the root of a
 * negative number), is an invalid operation. */
static inline u32 nan_cause(int q, int s)
{
    return (s || !q) ? FPC_V : 0;
}

/* the uncommon endings of a single-precision instruction: a host flag
 * other than "inexact" is up, or the result has every exponent bit set.
 * a, b, d: the operands' bit patterns (0 for one that is not there). */
static __attribute__((noinline)) int fp_special_s(sh4 *c, u32 x, fpreg *r, u32 a, u32 b, u32 d)
{
    if (nan_s(r->u)) {
        if (fp_raise(c, nan_cause(nan_s(a) | nan_s(b) | nan_s(d), snan_s(a) | snan_s(b) | snan_s(d))))
            return 1;
        if (!(sh4_diag & DIAG_HOST_NAN)) {
            sh4_nan_results += r->u != SH4_QNAN_S;
            r->u = SH4_QNAN_S;
        }
        return 0;
    }
    /* a denormalized operand with DN clear is an FPU error and nothing else:
     * the operation is not carried out */
    return fp_raise(c, ((x & 2) && !(c->fpscr & FPSCR_DN)) ? FPC_E : host_cause(x));
}

static __attribute__((noinline)) int fp_special_d(sh4 *c, u32 x, u64 *r, u64 a, u64 b)
{
    if (nan_d(*r)) {
        if (fp_raise(c, nan_cause(nan_d(a) | nan_d(b), snan_d(a) | snan_d(b))))
            return 1;
        if (!(sh4_diag & DIAG_HOST_NAN)) {
            sh4_nan_results += *r != SH4_QNAN_D;
            *r = SH4_QNAN_D;
        }
        return 0;
    }
    return fp_raise(c, ((x & 2) && !(c->fpscr & FPSCR_DN)) ? FPC_E : host_cause(x));
}

enum { FA_ADD, FA_SUB, FA_MUL, FA_DIV, FA_SQRT };

/* fadd, fsub, fmul, fdiv, fsqrt in single precision */
static inline __attribute__((always_inline)) void fp_arith_s(sh4 *c, int n, int m, int kind)
{
    fpreg a = c->fr[n], b, r;
    float av = a.f, bv, rv;
    b.u = 0;
    if (kind != FA_SQRT)
        b = c->fr[m];
    bv = b.f;
    FP_CLEAR();
    FP_HERE(av);
    switch (kind) {
    case FA_ADD: rv = av + bv; break;
    case FA_SUB: rv = av - bv; break;
    case FA_MUL: rv = av * bv; break;
    case FA_DIV: rv = av / bv; break;
    default: rv = __builtin_sqrtf(av); break;
    }
    FP_HERE(rv);
    u32 x = FP_FLAGS();
    r.f = rv;
    if (__builtin_expect((x & 0x1f) | ((r.u & 0x7f800000u) == 0x7f800000u), 0)) {
        if (fp_special_s(c, x, &r, a.u, b.u, 0))
            return;
    } else {
        c->fpscr = (c->fpscr & fp_keep) | (((x >> 5) & 1) * fp_i_bits);
    }
    c->fr[n] = r;
}

static inline __attribute__((always_inline)) void fp_arith_d(sh4 *c, int n, int m, int kind)
{
    u64 a = get_dr(c, n), b = kind != FA_SQRT ? get_dr(c, m) : 0, r;
    double av, bv, rv;
    memcpy(&av, &a, 8);
    memcpy(&bv, &b, 8);
    FP_CLEAR();
    FP_HERE(av);
    switch (kind) {
    case FA_ADD: rv = av + bv; break;
    case FA_SUB: rv = av - bv; break;
    case FA_MUL: rv = av * bv; break;
    case FA_DIV: rv = av / bv; break;
    default: rv = __builtin_sqrt(av); break;
    }
    FP_HERE(rv);
    u32 x = FP_FLAGS();
    memcpy(&r, &rv, 8);
    if (__builtin_expect((x & 0x1f) | ((r & 0x7ff0000000000000ull) == 0x7ff0000000000000ull), 0)) {
        if (fp_special_d(c, x, &r, a, b))
            return;
    } else {
        c->fpscr = (c->fpscr & fp_keep) | (((x >> 5) & 1) * fp_i_bits);
    }
    set_dr(c, n, r);
}

/* a sum of four products (fipr; one row of ftrv) from operands that held a
 * NaN, an infinity or something that overflowed: sorted out here */
static __attribute__((noinline)) u32 fp_dot_special(const fpreg *v, const fpreg *w, int wstep, fpreg *r, u32 x)
{
    if (nan_s(r->u)) {
        int q = 0, s = 0;
        for (int i = 0; i < 4; i++) {
            q |= nan_s(v[i].u) | nan_s(w[i * wstep].u);
            s |= snan_s(v[i].u) | snan_s(w[i * wstep].u);
        }
        if (!(sh4_diag & DIAG_HOST_NAN)) {
            sh4_nan_results += r->u != SH4_QNAN_S;
            r->u = SH4_QNAN_S;
        }
        return nan_cause(q, s);
    }
    return host_cause(x);
}

#define OBS 0
#include "sh4_exec.inc"
#undef OBS
#define OBS 1
#include "sh4_exec.inc"
#undef OBS

void sh4_step(sh4 *c)
{
    u32 saved = _mm_getcsr();
    set_mxcsr_for(c->fpscr);
    u32 pc = c->pc;
    u32 op = mem_r16(c, pc);
    c->pc = pc + 2;
    c->cycles++;
    c->insns++;
    exec_plain(c, op);
    _mm_setcsr(saved);
}

int sh4_run(sh4 *c)
{
    u32 saved = _mm_getcsr();
    set_mxcsr_for(c->fpscr);
    if (obs_on) {
        /* the observed run: one byte per instruction says whether it ran
         * before and whether a routine begins there (an entry that was not
         * reached by a call or a jump is a fall into the routine) */
        while (c->cycles < c->next_event) {
            u32 pc = c->pc;
            u32 i = ((pc & 0x1fffffffu) - IMAGE_BASE) >> 1;
            if (__builtin_expect(i < disp_slots, 1)) {
                u8 m = obs_map[i];
                if (__builtin_expect((m & (OM_RUN | OM_ENTRY)) != OM_RUN, 0) && obs_step(c, pc, i, m))
                    continue;
            }
            obs_pc = pc;
            u32 op = mem_r16(c, pc);
            c->pc = pc + 2;
            c->cycles++;
            c->insns++;
            exec_obs(c, op);
        }
    } else {
        while (c->cycles < c->next_event) {
            u32 pc = c->pc;
            u32 op = mem_r16(c, pc);
            c->pc = pc + 2;
            c->cycles++;
            c->insns++;
            exec_plain(c, op);
        }
    }
    _mm_setcsr(saved);
    return c->stop ? c->stop : STOP_EVENT;
}
