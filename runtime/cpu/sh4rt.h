/* What translated code is compiled against: guest memory, the runtime's
 * hooks, and the semantics of the instructions that are more than a line.
 *
 * Every instruction does exactly what VF4debug's interpreter does
 * (src/sh4_exec.inc and src/sh4.c, same maintainer, MIT): the FPU special
 * cases below are that code, renamed, without its diagnosis switches. The
 * interpreter was written from the SH-4 software manual; nothing here comes
 * from an emulator. tests/ checks the two against each other.
 *
 * mem_page keeps VF4debug's name and layout on purpose, so translated code
 * links into either the runtime or the lockstep harness.
 */
#ifndef VF4RT_SH4RT_H
#define VF4RT_SH4RT_H

#include "sh4.h"
#include "fsca.h"
#include <math.h>
#include <string.h>
#include <xmmintrin.h>

#define RT_INLINE static inline __attribute__((always_inline))
#define RT_LIKELY(x) __builtin_expect(!!(x), 1)
#define RT_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---- guest memory ---------------------------------------------------- */

/* Every access names its instruction: RT_ATK(address, k), where k is how
 * many instructions of the block the clock has already been moved past it
 * (a block adds its whole count at its start). The fast path ignores it;
 * the slow path (the device models) uses it to show them the clock and PC
 * VF4debug's interpreter would: see rt_io_read in the runtime. */
#define RT_ATK(at, k) ((((u64)(k)) << 32) | (u64)(at))

/* host address of each 64 KiB page of the 4 GiB space, or NULL where the
 * device models answer; stores look in mem_wpage, which is also NULL where a
 * page holds translated code, so a store there goes to the runtime, which
 * sees whether it changes the code (docs/design.md, "Code the game
 * changes") */
extern u8 *mem_page[65536];
extern u8 *mem_wpage[65536];
u32 rt_io_read(sh4 *c, u64 atk, u32 addr, int size);
void rt_io_write(sh4 *c, u64 atk, u32 addr, u32 val, int size);
void rt_sq_write(sh4 *c, u64 atk, u32 dst, const u32 *words);

RT_INLINE u32 rt_r32(sh4 *c, u64 atk, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (RT_LIKELY(p != NULL)) { u32 v; memcpy(&v, p + (a & 0xffff), 4); return v; }
    return rt_io_read(c, atk, a, 4);
}
RT_INLINE u32 rt_r16(sh4 *c, u64 atk, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (RT_LIKELY(p != NULL)) { u16 v; memcpy(&v, p + (a & 0xffff), 2); return v; }
    return rt_io_read(c, atk, a, 2);
}
RT_INLINE u32 rt_r8(sh4 *c, u64 atk, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (RT_LIKELY(p != NULL)) return p[a & 0xffff];
    return rt_io_read(c, atk, a, 1);
}
RT_INLINE void rt_w32(sh4 *c, u64 atk, u32 a, u32 v)
{
    u8 *p = mem_wpage[a >> 16];
    if (RT_LIKELY(p != NULL)) memcpy(p + (a & 0xffff), &v, 4);
    else rt_io_write(c, atk, a, v, 4);
}
RT_INLINE void rt_w16(sh4 *c, u64 atk, u32 a, u32 v)
{
    u8 *p = mem_wpage[a >> 16];
    u16 h = (u16)v;
    if (RT_LIKELY(p != NULL)) memcpy(p + (a & 0xffff), &h, 2);
    else rt_io_write(c, atk, a, v, 2);
}
RT_INLINE void rt_w8(sh4 *c, u64 atk, u32 a, u32 v)
{
    u8 *p = mem_wpage[a >> 16];
    if (RT_LIKELY(p != NULL)) p[a & 0xffff] = (u8)v;
    else rt_io_write(c, atk, a, v, 1);
}

/* ---- the runtime's side (runtime/cpu/sh4rt.c, or a test harness) ------ */

typedef void (*rt_fn)(sh4 *c);

/* a call or jump whose target is only known at run time; a target that is
 * not the start of a translated routine is the host's (rt_host_outside) */
void rt_call(sh4 *c, u32 target, u32 from);
void rt_jump(sh4 *c, u32 target, u32 from);
/* a callee came back somewhere other than the instruction after the call */
void rt_bad_return(sh4 *c, u32 expected, u32 from);
void rt_trapa(sh4 *c, u32 imm, u32 at);
void rt_sleep(sh4 *c, u32 at);
void rt_illegal(sh4 *c, u32 op, u32 at);
/* an FPU exception whose enable bit is set, or an FPU error */
void rt_fpu_trap(sh4 *c, u32 at);

/* The block protocol (docs/design.md, "Interrupts and timing"). Each block
 * start checks whether the next event falls inside the block; each
 * instruction in the middle of a block that can bring the next event
 * forward checks after itself. Either way, rt_slow takes over with c->pc
 * and the clock exact for the instruction boundary: it serves the events
 * and runs the rest of the block on the interpreter. It returns 1 when that
 * ended the routine (a return), else 0 with c->pc at a block start the
 * routine's closing switch goes on from. rt_continue runs whatever is at
 * c->pc (another routine, code the routine does not hold) until it returns. */
int rt_slow(sh4 *c);
void rt_continue(sh4 *c);
/* A block whose code the game has changed since the translation (a byte of
 * it, or a pool word it reads as a constant) runs on the interpreter: its
 * byte here is set (one per halfword of the image, by the block's start). */
#define RT_CODE_SLOTS 0xff0000u     /* 0x0c020000 to the end of the 32 MiB of RAM */
extern u8 rt_code_changed[RT_CODE_SLOTS];
#ifndef RT_BLOCK
#define RT_BLOCK(c, at, n) do { \
        if (RT_UNLIKELY((c)->cycles + (n) > (c)->next_event || rt_code_changed[RT_SLOT(at)])) { \
            (c)->pc = (at); goto rt_slow; } \
        (c)->cycles += (n); (c)->insns += (n); } while (0)
#endif
#ifndef RT_SYNC
#define RT_SYNC(c, next, k) do { \
        if (RT_UNLIKELY((c)->next_event < (c)->cycles)) { \
            (c)->pc = (next); (c)->cycles -= (k); (c)->insns -= (k); goto rt_slow; } } while (0)
#endif

/* Routine entries: VF4debug's dispatch byte for each halfword of the image
 * (hooks, host replacements, breakpoints, first entries); an entry reached
 * by a call or a jump with a non-zero byte goes to rt_entry, which returns 1
 * when a host replacement ran instead of the routine (c->pc is then where it
 * returned to). Falling into a routine, or returning to one, is no entry
 * (as in the interpreter): RT_FALL. */
extern u8 *disp_flags;
int rt_entry(sh4 *c, u32 addr);
extern int rt_skip_entry;
#define RT_SLOT(a) ((((a) & 0x1fffffffu) - 0x0c020000u) >> 1)
#ifndef RT_ENTRY
#define RT_ENTRY(c, a) do { if (RT_UNLIKELY(disp_flags[RT_SLOT(a)]) && rt_entry((c), (a))) return; } while (0)
#endif
#ifndef RT_FALL
#define RT_FALL(c, fn, a) do { if (RT_UNLIKELY(disp_flags[RT_SLOT(a)])) rt_skip_entry = 1; fn(c); } while (0)
#endif
/* after an rte: the exception's bookkeeping */
void rt_rte(sh4 *c);
#ifndef RT_RTE
#define RT_RTE(c) rt_rte(c)
#endif

/* ---- state helpers ------------------------------------------------------- */

RT_INLINE u32 rt_get_sr(const sh4 *c) { return (c->sr & ~SR_T) | (c->t & 1); }

RT_INLINE void rt_swap_bank(sh4 *c)
{
    for (int i = 0; i < 8; i++) {
        u32 t = c->r[i];
        c->r[i] = c->r_bank[i];
        c->r_bank[i] = t;
    }
}

RT_INLINE void rt_set_sr(sh4 *c, u32 v)
{
    u32 old = c->sr;
    v &= SR_VALID;
    int old_b = (old & SR_MD) && (old & SR_RB);
    int new_b = (v & SR_MD) && (v & SR_RB);
    if (old_b != new_b)
        rt_swap_bank(c);
    c->sr = v & ~SR_T;
    c->t = v & 1;
    c->next_event = 0;      /* the interrupt mask may have opened: recheck */
}

/* the host's MXCSR for the guest's rounding mode and DN bit, with every
 * exception masked: loaded before each arithmetic instruction of the FPU,
 * so the flags read after it are its own */
extern u32 rt_fp_mx;

RT_INLINE void rt_set_mxcsr_for(u32 fpscr)
{
    u32 m = 0x1f80;
    if ((fpscr & FPSCR_RM) == 1)
        m |= 0x6000;                     /* toward zero */
    if (fpscr & FPSCR_DN)
        m |= 0x8040;                     /* FTZ | DAZ */
    rt_fp_mx = m;
    _mm_setcsr(m);
}

RT_INLINE void rt_set_fpscr(sh4 *c, u32 v)
{
    v &= FPSCR_VALID;
    if ((v ^ c->fpscr) & FPSCR_FR) {
        fpreg t[16];
        memcpy(t, c->fr, sizeof t);
        memcpy(c->fr, c->xf, sizeof t);
        memcpy(c->xf, t, sizeof t);
    }
    c->fpscr = v;
    rt_set_mxcsr_for(v);
}

/* ---- integer instructions with more than a line -------------------------- */

RT_INLINE void op_div0s(sh4 *c, int n, int m)
{
    u32 q = c->r[n] >> 31, mm = c->r[m] >> 31;
    c->sr = (c->sr & ~(SR_Q | SR_M)) | (q ? SR_Q : 0) | (mm ? SR_M : 0);
    c->t = q ^ mm;
}

RT_INLINE void op_div1(sh4 *c, int n, int m)
{
    u32 old_q = (c->sr & SR_Q) != 0, mm = (c->sr & SR_M) != 0;
    u32 q = c->r[n] >> 31;
    u32 d = c->r[m], tmp0, tmp1;
    c->r[n] = (c->r[n] << 1) | c->t;
    tmp0 = c->r[n];
    if (old_q == mm) {
        c->r[n] -= d;
        tmp1 = c->r[n] > tmp0;
    } else {
        c->r[n] += d;
        tmp1 = c->r[n] < tmp0;
    }
    if (!mm) q = q ? !tmp1 : tmp1;
    else     q = q ? tmp1 : !tmp1;
    c->sr = (c->sr & ~SR_Q) | (q ? SR_Q : 0);
    c->t = q == mm;
}

RT_INLINE void op_subc(sh4 *c, int n, int m)
{
    u32 a = c->r[n], b = c->r[m];
    u32 r = a - b - c->t;
    c->t = (a < b) || (a - b < c->t);
    c->r[n] = r;
}

RT_INLINE void op_addc(sh4 *c, int n, int m)
{
    u32 a = c->r[n], b = c->r[m];
    u32 r = a + b;
    u32 r2 = r + c->t;
    c->t = (r < a) || (r2 < r);
    c->r[n] = r2;
}

RT_INLINE void op_subv(sh4 *c, int n, int m)
{
    s32 a = (s32)c->r[n], b = (s32)c->r[m], r;
    c->t = __builtin_sub_overflow(a, b, &r);
    c->r[n] = (u32)r;
}

RT_INLINE void op_addv(sh4 *c, int n, int m)
{
    s32 a = (s32)c->r[n], b = (s32)c->r[m], r;
    c->t = __builtin_add_overflow(a, b, &r);
    c->r[n] = (u32)r;
}

RT_INLINE void op_negc(sh4 *c, int n, int m)
{
    u32 v = c->r[m];
    u32 t = 0 - v;
    c->r[n] = t - c->t;
    c->t = (0 < t) || (t < c->r[n]);
}

RT_INLINE void op_shad(sh4 *c, int n, int m)
{
    s32 s = (s32)c->r[m];
    if (s >= 0) c->r[n] <<= (s & 31);
    else if ((s & 31) == 0) c->r[n] = (u32)((s32)c->r[n] >> 31);
    else c->r[n] = (u32)((s32)c->r[n] >> ((~s & 31) + 1));
}

RT_INLINE void op_shld(sh4 *c, int n, int m)
{
    s32 s = (s32)c->r[m];
    if (s >= 0) c->r[n] <<= (s & 31);
    else if ((s & 31) == 0) c->r[n] = 0;
    else c->r[n] >>= ((~s & 31) + 1);
}

RT_INLINE void op_macl(sh4 *c, u64 atk, int n, int m)
{
    s64 a = (s32)rt_r32(c, atk, c->r[n]);
    c->r[n] += 4;
    s64 b = (s32)rt_r32(c, atk, c->r[m]);
    c->r[m] += 4;
    s64 mac = (s64)(((u64)c->mach << 32) | c->macl);
    s64 prod = a * b;
    if (c->sr & SR_S) {
        const s64 hi = 0x00007fffffffffffLL, lo = -0x0000800000000000LL;
        s64 total;
        if (__builtin_add_overflow(mac, prod, &total))
            total = prod < 0 ? lo : hi;
        if (total > hi) total = hi;
        if (total < lo) total = lo;
        mac = total;
    } else {
        mac = (s64)((u64)mac + (u64)prod);
    }
    c->mach = (u32)((u64)mac >> 32);
    c->macl = (u32)mac;
}

RT_INLINE void op_macw(sh4 *c, u64 atk, int n, int m)
{
    s32 a = (s16)rt_r16(c, atk, c->r[n]);
    c->r[n] += 2;
    s32 b = (s16)rt_r16(c, atk, c->r[m]);
    c->r[m] += 2;
    s64 prod = (s64)a * b;
    if (c->sr & SR_S) {
        s64 total = (s64)(s32)c->macl + prod;
        if (total > 0x7fffffffLL) { total = 0x7fffffffLL; c->mach |= 1; }
        if (total < -0x80000000LL) { total = -0x80000000LL; c->mach |= 1; }
        c->macl = (u32)total;
    } else {
        u64 mac = (((u64)c->mach << 32) | c->macl) + (u64)prod;
        c->mach = (u32)(mac >> 32);
        c->macl = (u32)mac;
    }
}

RT_INLINE void op_tas(sh4 *c, u64 atk, int n)
{
    u32 v = rt_r8(c, atk, c->r[n]);
    c->t = v == 0;
    rt_w8(c, atk, c->r[n], v | 0x80);
}

/* pref to the store queue area sends a queue's 32 bytes */
RT_INLINE void op_pref(sh4 *c, u64 atk, int n)
{
    u32 addr = c->r[n];
    if ((addr & 0xfc000000u) != 0xe0000000u)
        return;
    int q = (addr >> 5) & 1;
    u32 qacr = rt_io_read(c, atk, q ? 0xff00003cu : 0xff000038u, 4);
    u32 dst = (addr & 0x03ffffe0u) | ((qacr & 0x1c) << 24);
    u8 *p = mem_wpage[dst >> 16];
    if (p)
        memcpy(p + (dst & 0xffff), c->sq[q], 32);
    else
        rt_sq_write(c, atk, dst, c->sq[q]);
}

/* ---- the FPU --------------------------------------------------------------- */

#define FP_KEEP (~FPSCR_CAUSE)
#define FP_I_BITS (FPC_I | (FPC_I >> 10))
#define FP_CLEAR() _mm_setcsr(rt_fp_mx)
#define FP_FLAGS() _mm_getcsr()
/* keeps the compiler from moving an operation across the MXCSR load before
 * it or the read after it */
#define FP_HERE(v) __asm__ volatile("" : "+x"(v))

RT_INLINE u64 rt_get_dr(sh4 *c, int n) { return ((u64)c->fr[n & 14].u << 32) | c->fr[(n & 14) + 1].u; }
RT_INLINE void rt_set_dr(sh4 *c, int n, u64 v) { c->fr[n & 14].u = (u32)(v >> 32); c->fr[(n & 14) + 1].u = (u32)v; }
RT_INLINE double rt_get_d(sh4 *c, int n) { u64 v = rt_get_dr(c, n); double d; memcpy(&d, &v, 8); return d; }
RT_INLINE void rt_set_d(sh4 *c, int n, double d) { u64 v; memcpy(&v, &d, 8); rt_set_dr(c, n, v); }

RT_INLINE int nan_s(u32 u) { return (u & 0x7fffffffu) > 0x7f800000u; }
/* the chip's signaling NaN has the top bit of the fraction SET */
RT_INLINE int snan_s(u32 u) { return (u & 0x7fffffffu) >= 0x7fc00000u; }
RT_INLINE int den_s(u32 u) { return !(u & 0x7f800000u) && (u & 0x007fffffu); }
RT_INLINE int nan_d(u64 u) { return (u & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
RT_INLINE int snan_d(u64 u) { return (u & 0x7fffffffffffffffull) >= 0x7ff8000000000000ull; }
RT_INLINE int den_d(u64 u) { return !(u & 0x7ff0000000000000ull) && (u & 0x000fffffffffffffull); }

RT_INLINE u32 host_cause(u32 x)
{
    return ((x & 4) << 13) | ((x & 8) << 11) | ((x & 0x10) << 9) | ((x & 0x20) << 7);
}

/* sets the cause field and the flags; 1 when it traps (the destination must
 * stay as it was) */
RT_INLINE int fp_raise(sh4 *c, u32 cause, u32 at)
{
    c->fpscr = (c->fpscr & ~FPSCR_CAUSE) | cause | ((cause >> 10) & FPSCR_FLAG);
    if ((cause & FPC_E) || (c->fpscr & (cause >> 5) & FPSCR_ENABLE)) {
        rt_fpu_trap(c, at);
        return 1;
    }
    return 0;
}

RT_INLINE u32 nan_cause(int q, int s) { return (s || !q) ? FPC_V : 0; }

/* out-of-line: the uncommon endings (runtime/cpu/sh4rt_fpu.c) */
int rt_fp_special_s(sh4 *c, u32 x, fpreg *r, u32 a, u32 b, u32 d, u32 at);
int rt_fp_special_d(sh4 *c, u32 x, u64 *r, u64 a, u64 b, u32 at);
u32 rt_fp_dot_special(const fpreg *v, const fpreg *w, int wstep, fpreg *r, u32 x);

enum { FA_ADD, FA_SUB, FA_MUL, FA_DIV, FA_SQRT };

RT_INLINE void fp_arith_s(sh4 *c, int n, int m, int kind, u32 at)
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
    if (RT_UNLIKELY((x & 0x1f) | ((r.u & 0x7f800000u) == 0x7f800000u))) {
        if (rt_fp_special_s(c, x, &r, a.u, b.u, 0, at))
            return;
    } else {
        c->fpscr = (c->fpscr & FP_KEEP) | (((x >> 5) & 1) * FP_I_BITS);
    }
    c->fr[n] = r;
}

RT_INLINE void fp_arith_d(sh4 *c, int n, int m, int kind, u32 at)
{
    u64 a = rt_get_dr(c, n), b = kind != FA_SQRT ? rt_get_dr(c, m) : 0, r;
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
    if (RT_UNLIKELY((x & 0x1f) | ((r & 0x7ff0000000000000ull) == 0x7ff0000000000000ull))) {
        if (rt_fp_special_d(c, x, &r, a, b, at))
            return;
    } else {
        c->fpscr = (c->fpscr & FP_KEEP) | (((x >> 5) & 1) * FP_I_BITS);
    }
    rt_set_dr(c, n, r);
}

/* fadd, fsub, fmul, fdiv, fsqrt: pr is a constant where the translator knows
 * the mode, else the run-time test */
RT_INLINE void op_farith(sh4 *c, int n, int m, int kind, int pr, u32 at)
{
    if (pr) fp_arith_d(c, n, m, kind, at);
    else fp_arith_s(c, n, m, kind, at);
}

RT_INLINE void op_fcmp(sh4 *c, int n, int m, int gt, int pr, u32 at)
{
    int q, s;
    if (pr) {
        u64 a = rt_get_dr(c, n), b = rt_get_dr(c, m);
        q = nan_d(a) | nan_d(b);
        s = snan_d(a) | snan_d(b);
    } else {
        q = nan_s(c->fr[n].u) | nan_s(c->fr[m].u);
        s = snan_s(c->fr[n].u) | snan_s(c->fr[m].u);
    }
    if (RT_UNLIKELY(q)) {
        if (fp_raise(c, (gt || s) ? FPC_V : 0, at))
            return;
        c->t = 0;
        return;
    }
    c->fpscr &= FP_KEEP;
    if (pr) c->t = gt ? rt_get_d(c, n) > rt_get_d(c, m) : rt_get_d(c, n) == rt_get_d(c, m);
    else c->t = gt ? c->fr[n].f > c->fr[m].f : c->fr[n].f == c->fr[m].f;
}

RT_INLINE void op_fmac(sh4 *c, int n, int m, u32 at)
{
    fpreg f0 = c->fr[0], fm = c->fr[m], fn = c->fr[n], r;
    float v0 = f0.f, rv;
    FP_CLEAR();
    FP_HERE(v0);
    rv = fmaf(v0, fm.f, fn.f);
    FP_HERE(rv);
    u32 x = FP_FLAGS();
    r.f = rv;
    if (RT_UNLIKELY((x & 0x1f) | ((r.u & 0x7f800000u) == 0x7f800000u))) {
        if (rt_fp_special_s(c, x, &r, f0.u, fm.u, fn.u, at))
            return;
    } else {
        c->fpscr = (c->fpscr & FP_KEEP) | (((x >> 5) & 1) * FP_I_BITS);
    }
    c->fr[n] = r;
}

RT_INLINE void op_float(sh4 *c, int n, int pr, u32 at)
{
    s32 iv = (s32)c->fpul;
    if (pr) {
        rt_set_d(c, n, (double)iv);
        c->fpscr &= FP_KEEP;
    } else {
        float rv;
        FP_CLEAR();
        __asm__ volatile("" : "+r"(iv));
        rv = (float)iv;
        FP_HERE(rv);
        u32 x = FP_FLAGS();
        c->fpscr = (c->fpscr & FP_KEEP) | (((x >> 5) & 1) * FP_I_BITS);
        if (RT_UNLIKELY(c->fpscr & FPSCR_ENABLE & (FPC_I >> 5)) && (x & 0x20) && fp_raise(c, FPC_I, at))
            return;
        c->fr[n].f = rv;
    }
}

/* ftrc: out of range is an invalid operation */
RT_INLINE s32 rt_float_to_int(double v, int *bad)
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

RT_INLINE void op_ftrc(sh4 *c, int n, int pr, u32 at)
{
    int bad;
    s32 v = pr ? rt_float_to_int(rt_get_d(c, n), &bad) : rt_float_to_int((double)c->fr[n].f, &bad);
    if (RT_UNLIKELY(bad)) {
        if (fp_raise(c, FPC_V, at))
            return;
    } else {
        c->fpscr &= FP_KEEP;
    }
    c->fpul = (u32)v;
}

void rt_op_fsrra_special(sh4 *c, int n, u32 at);

RT_INLINE void op_fsrra(sh4 *c, int n, u32 at)
{
    fpreg a = c->fr[n], r;
    u32 mag = a.u & 0x7fffffffu;
    if (RT_UNLIKELY(mag - 0x00800000u >= 0x7f000000u || (a.u >> 31))) {
        rt_op_fsrra_special(c, n, at);
        return;
    }
    r.f = (float)(1.0 / sqrt((double)a.f));
    c->fpscr = (c->fpscr & FP_KEEP) | FP_I_BITS;
    if (RT_UNLIKELY(c->fpscr & FPSCR_ENABLE & (FPC_I >> 5)) && fp_raise(c, FPC_I, at))
        return;
    c->fr[n] = r;
}

void rt_op_fcnvsd(sh4 *c, int n, u32 at);
void rt_op_fcnvds(sh4 *c, int n, u32 at);

RT_INLINE void op_fipr(sh4 *c, int n, u32 at)
{
    int vn = n & 12, vm = (n & 3) << 2;
    fpreg r;
    float s = c->fr[vn].f;
    FP_CLEAR();
    FP_HERE(s);
    s = s * c->fr[vm].f;
    s += c->fr[vn + 1].f * c->fr[vm + 1].f;
    s += c->fr[vn + 2].f * c->fr[vm + 2].f;
    s += c->fr[vn + 3].f * c->fr[vm + 3].f;
    FP_HERE(s);
    u32 x = FP_FLAGS();
    r.f = s;
    if (RT_UNLIKELY((x & 0x1d) | ((r.u & 0x7f800000u) == 0x7f800000u))) {
        if (fp_raise(c, rt_fp_dot_special(&c->fr[vn], &c->fr[vm], 1, &r, x), at))
            return;
    } else {
        c->fpscr = (c->fpscr & FP_KEEP) | (((x >> 5) & 1) * FP_I_BITS);
    }
    c->fr[vn + 3] = r;
}

RT_INLINE void op_ftrv(sh4 *c, int n, u32 at)
{
    int vn = n & 12;
    fpreg v[4], r[4];
    u32 special = 0;
    float v0 = c->fr[vn].f, v1 = c->fr[vn + 1].f, v2 = c->fr[vn + 2].f, v3 = c->fr[vn + 3].f;
    v[0] = c->fr[vn], v[1] = c->fr[vn + 1], v[2] = c->fr[vn + 2], v[3] = c->fr[vn + 3];
    FP_CLEAR();
    FP_HERE(v0);
    for (int i = 0; i < 4; i++) {
        float s = c->xf[i].f * v0;
        s += c->xf[4 + i].f * v1;
        s += c->xf[8 + i].f * v2;
        s += c->xf[12 + i].f * v3;
        r[i].f = s;
        special |= (r[i].u & 0x7f800000u) == 0x7f800000u;
    }
    __asm__ volatile("" : : "m"(r));
    u32 x = FP_FLAGS();
    if (RT_UNLIKELY((x & 0x1d) | special)) {
        u32 cause = 0;
        for (int i = 0; i < 4; i++)
            cause |= rt_fp_dot_special(v, &c->xf[i], 4, &r[i], x);
        if (fp_raise(c, cause, at))
            return;
    } else {
        c->fpscr = (c->fpscr & FP_KEEP) | (((x >> 5) & 1) * FP_I_BITS);
    }
    for (int i = 0; i < 4; i++)
        c->fr[vn + i] = r[i];
}

/* the sine and cosine from fsca.h's table: the same bits on every PC */
RT_INLINE void op_fsca(sh4 *c, int n, u32 at)
{
    u32 k = c->fpul & 0xffff;
    c->fpscr = (c->fpscr & FP_KEEP) | FP_I_BITS;
    if (RT_UNLIKELY(c->fpscr & FPSCR_ENABLE & (FPC_I >> 5)) && fp_raise(c, FPC_I, at))
        return;
    c->fr[n].u = rt_fsca_sin(k);
    c->fr[n + 1].u = rt_fsca_cos(k);
}

/* the register pair a paired fmov names: bit 0 picks the other bank (XD) */
#define FPAIR(c, field) (((field) & 1) ? &(c)->xf[(field) & 14] : &(c)->fr[(field) & 14])

#endif
