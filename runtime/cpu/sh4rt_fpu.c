/* The FPU's uncommon endings, out of line (see sh4rt.h). The same code as
 * VF4debug's src/sh4.c and src/sh4_exec.inc (same maintainer, MIT), less its
 * diagnosis switches: every result that is not a number is the chip's own
 * quiet NaN, and each instruction's cause and flag bits are what the SH-4
 * software manual's operation text says. */
#include "sh4rt.h"

u32 rt_fp_mx = 0x1f80;

/* FSCA's quarter-turn sine table (fsca.h; written by tools/make_fsca_table.py) */
const uint32_t rt_fsca_quarter[16385] = {
#include "fsca_table.inc"
};

int rt_fp_special_s(sh4 *c, u32 x, fpreg *r, u32 a, u32 b, u32 d, u32 at)
{
    if (nan_s(r->u)) {
        if (fp_raise(c, nan_cause(nan_s(a) | nan_s(b) | nan_s(d), snan_s(a) | snan_s(b) | snan_s(d)), at))
            return 1;
        r->u = SH4_QNAN_S;
        return 0;
    }
    /* a denormalized operand with DN clear is an FPU error and nothing else */
    return fp_raise(c, ((x & 2) && !(c->fpscr & FPSCR_DN)) ? FPC_E : host_cause(x), at);
}

int rt_fp_special_d(sh4 *c, u32 x, u64 *r, u64 a, u64 b, u32 at)
{
    if (nan_d(*r)) {
        if (fp_raise(c, nan_cause(nan_d(a) | nan_d(b), snan_d(a) | snan_d(b)), at))
            return 1;
        *r = SH4_QNAN_D;
        return 0;
    }
    return fp_raise(c, ((x & 2) && !(c->fpscr & FPSCR_DN)) ? FPC_E : host_cause(x), at);
}

u32 rt_fp_dot_special(const fpreg *v, const fpreg *w, int wstep, fpreg *r, u32 x)
{
    if (nan_s(r->u)) {
        int q = 0, s = 0;
        for (int i = 0; i < 4; i++) {
            q |= nan_s(v[i].u) | nan_s(w[i * wstep].u);
            s |= snan_s(v[i].u) | snan_s(w[i * wstep].u);
        }
        r->u = SH4_QNAN_S;
        return nan_cause(q, s);
    }
    return host_cause(x);
}

/* fsrra of anything but a positive normalized number */
void rt_op_fsrra_special(sh4 *c, int n, u32 at)
{
    fpreg a = c->fr[n], r;
    u32 mag = a.u & 0x7fffffffu;
    u32 cause;
    if (mag > 0x7f800000u) {                              /* a NaN */
        cause = snan_s(a.u) ? FPC_V : 0;
        r.u = SH4_QNAN_S;
    } else if (mag == 0 || (mag < 0x00800000u && (c->fpscr & FPSCR_DN))) {
        cause = FPC_Z;                                    /* a zero: infinity of its sign */
        r.u = (a.u & 0x80000000u) | 0x7f800000u;
    } else if (a.u >> 31) {                               /* negative: invalid */
        cause = FPC_V;
        r.u = SH4_QNAN_S;
    } else if (mag == 0x7f800000u) {                      /* plus infinity: zero */
        cause = 0;
        r.u = 0;
    } else {                                              /* denormalized with DN clear */
        cause = FPC_E;
        r.u = a.u;
    }
    if (fp_raise(c, cause, at))
        return;
    c->fr[n] = r;
}

void rt_op_fcnvsd(sh4 *c, int n, u32 at)
{
    fpreg v;
    v.u = c->fpul;
    if (RT_UNLIKELY(nan_s(v.u))) {
        if (fp_raise(c, snan_s(v.u) ? FPC_V : 0, at))
            return;
        rt_set_dr(c, n, SH4_QNAN_D);
        return;
    }
    if (RT_UNLIKELY(den_s(v.u) && !(c->fpscr & FPSCR_DN)) && fp_raise(c, FPC_E, at))
        return;
    c->fpscr &= FP_KEEP;
    rt_set_d(c, n, (double)v.f);
}

void rt_op_fcnvds(sh4 *c, int n, u32 at)
{
    fpreg v;
    u64 du = rt_get_dr(c, n);
    double dv = rt_get_d(c, n);
    float rv;
    if (RT_UNLIKELY(nan_d(du))) {
        if (fp_raise(c, snan_d(du) ? FPC_V : 0, at))
            return;
        c->fpul = SH4_QNAN_S;
        return;
    }
    if (RT_UNLIKELY(den_d(du) && !(c->fpscr & FPSCR_DN)) && fp_raise(c, FPC_E, at))
        return;
    FP_CLEAR();
    FP_HERE(dv);
    rv = (float)dv;
    FP_HERE(rv);
    u32 x = FP_FLAGS();
    if (RT_UNLIKELY(x & 0x1c)) {
        if (fp_raise(c, host_cause(x), at))
            return;
    } else {
        c->fpscr = (c->fpscr & FP_KEEP) | (((x >> 5) & 1) * FP_I_BITS);
    }
    v.f = rv;
    c->fpul = v.u;
}
