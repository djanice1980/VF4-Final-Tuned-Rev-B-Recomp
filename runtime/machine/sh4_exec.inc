/* The interpreter's core, compiled twice by sh4.c: once plain (OBS 0) and
 * once with the observer's hooks (OBS 1): every store offered to the write
 * journal, reads outside RAM noted, conditional branches recorded, routine
 * entries that are not calls caught. Included, never compiled by itself. */
#if OBS
#define X(n) n##_obs
#define MR8(c, a) obs_r8(c, a)
#define MR16(c, a) obs_r16(c, a)
#define MR32(c, a) obs_r32(c, a)
#define MW8(c, a, v) obs_w8(c, a, v)
#define MW16(c, a, v) obs_w16(c, a, v)
#define MW32(c, a, v) obs_w32(c, a, v)
#define OBS_BR(taken) obs_branch(taken)
#define OBS_XF() (obs_top->flags |= OF_XF)
#else
#define X(n) n##_plain
#define MR8(c, a) mem_r8(c, a)
#define MR16(c, a) mem_r16(c, a)
#define MR32(c, a) mem_r32(c, a)
#define MW8(c, a, v) mem_w8(c, a, v)
#define MW16(c, a, v) mem_w16(c, a, v)
#define MW32(c, a, v) mem_w32(c, a, v)
#define OBS_BR(taken) ((void)0)
#define OBS_XF() ((void)0)
#endif

static void X(exec)(sh4 *c, u32 op);

/* runs the instruction after a branch, then goes to the target */
static inline void X(slot)(sh4 *c)
{
    u32 pc = c->pc;
    u32 op = mem_r16(c, pc);
#if OBS
    {
        u32 i = ((pc & 0x1fffffffu) - IMAGE_BASE) >> 1;
        if (i < disp_slots)
            obs_map[i] |= OM_RUN;
    }
#endif
    c->pc = pc + 2;
    c->in_slot = 1;
    c->cycles++;
    c->insns++;
    X(exec)(c, op);
    c->in_slot = 0;
}

static inline void X(go)(sh4 *c, u32 target, int kind)
{
    c->pc = target;
    u32 i = ((target & 0x1fffffffu) - IMAGE_BASE) >> 1;
    if (__builtin_expect(i < disp_slots, 1)) {
        if (__builtin_expect(disp_flags[i], 0))
            disp_enter(c, target, kind);
#if OBS
        else {
            /* not an entry: a jump or call into the body of another routine? */
            u32 f = ((obs_pc & 0x1fffffffu) - IMAGE_BASE) >> 1;
            if (__builtin_expect(f >= disp_slots || obs_slot_routine[f] != obs_slot_routine[i], 0))
                obs_into(c, target, kind);
        }
#endif
    } else {
#if OBS
        if (kind == DISP_CALL)
            obs_outside_call(c, target);
#endif
        disp_outside(c, target, kind);
    }
}

/* the 64-bit register pair a paired fmov names: bit 0 of the field picks
 * the other bank (XD), the rest the even register */
static inline fpreg *X(pair)(sh4 *c, int field)
{
#if OBS
    if (field & 1)
        OBS_XF();
#endif
    return (field & 1) ? &c->xf[field & 14] : &c->fr[field & 14];
}

static __attribute__((noinline)) void X(fpu)(sh4 *c, u32 op)
{
    int n = N, m = M;
    u32 pr = c->fpscr & FPSCR_PR;
    u32 sz = c->fpscr & FPSCR_SZ;

    switch (op & 15) {
    case 0x0:
        if (pr) fp_arith_d(c, n, m, FA_ADD);
        else fp_arith_s(c, n, m, FA_ADD);
        return;
    case 0x1:
        if (pr) fp_arith_d(c, n, m, FA_SUB);
        else fp_arith_s(c, n, m, FA_SUB);
        return;
    case 0x2:
        if (pr) fp_arith_d(c, n, m, FA_MUL);
        else fp_arith_s(c, n, m, FA_MUL);
        return;
    case 0x3:
        if (pr) fp_arith_d(c, n, m, FA_DIV);
        else fp_arith_s(c, n, m, FA_DIV);
        return;
    case 0x4:     /* fcmp/eq: a NaN makes T 0; a signaling one is an invalid operation */
    case 0x5: {   /* fcmp/gt: a NaN makes T 0 and is an invalid operation, quiet or not */
        int gt = (op & 15) == 5, q, s;
        if (pr) {
            u64 a = get_dr(c, n), b = get_dr(c, m);
            q = nan_d(a) | nan_d(b);
            s = snan_d(a) | snan_d(b);
        } else {
            q = nan_s(c->fr[n].u) | nan_s(c->fr[m].u);
            s = snan_s(c->fr[n].u) | snan_s(c->fr[m].u);
        }
        if (__builtin_expect(q, 0)) {
            if (fp_raise(c, (gt || s) ? FPC_V : 0))
                return;
            c->t = 0;
            return;
        }
        c->fpscr &= fp_keep;
        if (pr) c->t = gt ? get_d(c, n) > get_d(c, m) : get_d(c, n) == get_d(c, m);
        else c->t = gt ? c->fr[n].f > c->fr[m].f : c->fr[n].f == c->fr[m].f;
        return;
    }
    case 0x6: {   /* fmov @(R0,Rm),FRn */
        u32 a = c->r[0] + c->r[m];
        if (sz) {
            fpreg *d = X(pair)(c, n);
            d[0].u = MR32(c, a);
            d[1].u = MR32(c, a + 4);
        } else {
            c->fr[n].u = MR32(c, a);
        }
        return;
    }
    case 0x7: {   /* fmov FRm,@(R0,Rn) */
        u32 a = c->r[0] + c->r[n];
        if (sz) {
            fpreg *s = X(pair)(c, m);
            MW32(c, a, s[0].u);
            MW32(c, a + 4, s[1].u);
        } else {
            MW32(c, a, c->fr[m].u);
        }
        return;
    }
    case 0x8: {   /* fmov @Rm,FRn */
        u32 a = c->r[m];
        if (sz) {
            fpreg *d = X(pair)(c, n);
            d[0].u = MR32(c, a);
            d[1].u = MR32(c, a + 4);
        } else {
            c->fr[n].u = MR32(c, a);
        }
        return;
    }
    case 0x9: {   /* fmov @Rm+,FRn */
        u32 a = c->r[m];
        if (sz) {
            fpreg *d = X(pair)(c, n);
            d[0].u = MR32(c, a);
            d[1].u = MR32(c, a + 4);
            c->r[m] = a + 8;
        } else {
            c->fr[n].u = MR32(c, a);
            c->r[m] = a + 4;
        }
        return;
    }
    case 0xa: {   /* fmov FRm,@Rn */
        u32 a = c->r[n];
        if (sz) {
            fpreg *s = X(pair)(c, m);
            MW32(c, a, s[0].u);
            MW32(c, a + 4, s[1].u);
        } else {
            MW32(c, a, c->fr[m].u);
        }
        return;
    }
    case 0xb: {   /* fmov FRm,@-Rn */
        if (sz) {
            u32 a = c->r[n] - 8;
            fpreg *s = X(pair)(c, m);
            MW32(c, a, s[0].u);
            MW32(c, a + 4, s[1].u);
            c->r[n] = a;
        } else {
            u32 a = c->r[n] - 4;
            MW32(c, a, c->fr[m].u);
            c->r[n] = a;
        }
        return;
    }
    case 0xc:     /* fmov FRm,FRn */
        if (sz) {
            fpreg *s = X(pair)(c, m), *d = X(pair)(c, n);
            u32 a = s[0].u, b = s[1].u;
            d[0].u = a;
            d[1].u = b;
        } else {
            c->fr[n].u = c->fr[m].u;
        }
        return;
    case 0xe: {   /* fmac FR0,FRm,FRn: the product is not rounded before the sum */
        if (pr) { illegal(c, op); return; }
        fpreg f0 = c->fr[0], fm = c->fr[m], fn = c->fr[n], r;
        float v0 = f0.f, rv;
        FP_CLEAR();
        FP_HERE(v0);
        rv = fmaf(v0, fm.f, fn.f);
        FP_HERE(rv);
        u32 x = FP_FLAGS();
        r.f = rv;
        if (__builtin_expect((x & 0x1f) | ((r.u & 0x7f800000u) == 0x7f800000u), 0)) {
            if (fp_special_s(c, x, &r, f0.u, fm.u, fn.u))
                return;
        } else {
            c->fpscr = (c->fpscr & fp_keep) | (((x >> 5) & 1) * fp_i_bits);
        }
        c->fr[n] = r;
        return;
    }
    case 0xd:
        break;
    default:
        illegal(c, op);
        return;
    }

    switch (m) {   /* 1111nnnn mmmm 1101 */
    case 0x0: c->fr[n].u = c->fpul; return;                       /* fsts */
    case 0x1: c->fpul = c->fr[n].u; return;                       /* flds */
    case 0x2: {                                                   /* float: inexact in single precision only */
        s32 iv = (s32)c->fpul;
        if (pr) {
            set_d(c, n, (double)iv);
            c->fpscr &= fp_keep;
        } else {
            float rv;
            FP_CLEAR();
            __asm__ volatile("" : "+r"(iv));
            rv = (float)iv;
            FP_HERE(rv);
            u32 x = FP_FLAGS();
            c->fpscr = (c->fpscr & fp_keep) | (((x >> 5) & 1) * fp_i_bits);
            if (__builtin_expect(c->fpscr & FPSCR_ENABLE & (FPC_I >> 5), 0) && (x & 0x20) && fp_raise(c, FPC_I))
                return;
            c->fr[n].f = rv;
        }
        return;
    }
    case 0x3: {                                                   /* ftrc: out of range is the only exception */
        int bad;
        s32 v = pr ? float_to_int(get_d(c, n), &bad) : float_to_int((double)c->fr[n].f, &bad);
        if (__builtin_expect(bad, 0)) {
            if (fp_raise(c, FPC_V))
                return;
        } else {
            c->fpscr &= fp_keep;
        }
        c->fpul = (u32)v;
        return;
    }
    case 0x4:                                                     /* fneg */
        c->fr[pr ? (n & 14) : n].u ^= 0x80000000u;
        return;
    case 0x5:                                                     /* fabs */
        c->fr[pr ? (n & 14) : n].u &= 0x7fffffffu;
        return;
    case 0x6:                                                     /* fsqrt */
        if (pr) fp_arith_d(c, n, 0, FA_SQRT);
        else fp_arith_s(c, n, 0, FA_SQRT);
        return;
    case 0x7: {                                                   /* fsrra */
        if (pr) { illegal(c, op); return; }
        fpreg a = c->fr[n], r;
        u32 mag = a.u & 0x7fffffffu;
        if (__builtin_expect(mag - 0x00800000u >= 0x7f000000u || (a.u >> 31), 0)) {
            /* not a positive normalized number: the cases of the manual's
             * operation text, one by one */
            u32 cause;
            if (mag > 0x7f800000u) {                              /* a NaN */
                cause = snan_s(a.u) ? FPC_V : 0;
                r.u = SH4_QNAN_S;
            } else if (mag == 0 || (mag < 0x00800000u && (c->fpscr & FPSCR_DN))) {
                cause = FPC_Z;                                    /* a zero: division by zero, infinity of its sign */
                r.u = (a.u & 0x80000000u) | 0x7f800000u;
            } else if (a.u >> 31) {                               /* negative, minus infinity too: invalid */
                cause = FPC_V;
                r.u = SH4_QNAN_S;
            } else if (mag == 0x7f800000u) {                      /* plus infinity: zero, nothing raised */
                cause = 0;
                r.u = 0;
            } else {                                              /* a denormalized number with DN clear */
                cause = FPC_E;
                r.u = a.u;
            }
            if (fp_raise(c, cause))
                return;
            if ((cause & FPC_E) || (r.u == SH4_QNAN_S && (sh4_diag & DIAG_HOST_NAN)))
                r.f = (float)(1.0 / sqrt((double)a.f));     /* (diagnosis only: as it was) */
            else if (r.u == SH4_QNAN_S)
                sh4_nan_results++;
            c->fr[n] = r;
            return;
        }
        r.f = (float)(1.0 / sqrt((double)a.f));
        /* the operation text sets "inexact" for every normalized operand */
        c->fpscr = (c->fpscr & fp_keep) | fp_i_bits;
        if (__builtin_expect(c->fpscr & FPSCR_ENABLE & (FPC_I >> 5), 0) && fp_raise(c, FPC_I))
            return;
        c->fr[n] = r;
        return;
    }
    case 0x8:                                                     /* fldi0 */
        if (pr) { illegal(c, op); return; }
        c->fr[n].u = 0;
        return;
    case 0x9:                                                     /* fldi1 */
        if (pr) { illegal(c, op); return; }
        c->fr[n].u = 0x3f800000u;
        return;
    case 0xa: {                                                   /* fcnvsd FPUL,DRn */
        fpreg v;
        v.u = c->fpul;
        if (__builtin_expect(nan_s(v.u), 0)) {
            if (fp_raise(c, snan_s(v.u) ? FPC_V : 0))
                return;
            if (sh4_diag & DIAG_HOST_NAN) {
                set_d(c, n, (double)v.f);
            } else {
                sh4_nan_results++;
                set_dr(c, n, SH4_QNAN_D);
            }
            return;
        }
        if (__builtin_expect(den_s(v.u) && !(c->fpscr & FPSCR_DN), 0) && fp_raise(c, FPC_E))
            return;
        c->fpscr &= fp_keep;
        set_d(c, n, (double)v.f);
        return;
    }
    case 0xb: {                                                   /* fcnvds DRn,FPUL */
        fpreg v;
        u64 du = get_dr(c, n);
        double dv = get_d(c, n);
        float rv;
        if (__builtin_expect(nan_d(du), 0)) {
            if (fp_raise(c, snan_d(du) ? FPC_V : 0))
                return;
            if (sh4_diag & DIAG_HOST_NAN) {
                v.f = (float)dv;
            } else {
                sh4_nan_results++;
                v.u = SH4_QNAN_S;
            }
            c->fpul = v.u;
            return;
        }
        if (__builtin_expect(den_d(du) && !(c->fpscr & FPSCR_DN), 0) && fp_raise(c, FPC_E))
            return;
        FP_CLEAR();
        FP_HERE(dv);
        rv = (float)dv;
        FP_HERE(rv);
        u32 x = FP_FLAGS();
        if (__builtin_expect(x & 0x1c, 0)) {
            if (fp_raise(c, host_cause(x)))
                return;
        } else {
            c->fpscr = (c->fpscr & fp_keep) | (((x >> 5) & 1) * fp_i_bits);
        }
        v.f = rv;
        c->fpul = v.u;
        return;
    }
    case 0xe: {                                                   /* fipr FVm,FVn */
        if (pr) { illegal(c, op); return; }
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
        if (__builtin_expect((x & 0x1d) | ((r.u & 0x7f800000u) == 0x7f800000u), 0)) {
            if (fp_raise(c, fp_dot_special(&c->fr[vn], &c->fr[vm], 1, &r, x)))
                return;
        } else {
            c->fpscr = (c->fpscr & fp_keep) | (((x >> 5) & 1) * fp_i_bits);
        }
        c->fr[vn + 3] = r;
        return;
    }
    case 0xf:
        if ((n & 3) == 1) {                                       /* ftrv XMTRX,FVn */
            if (pr) { illegal(c, op); return; }
            OBS_XF();
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
            if (__builtin_expect((x & 0x1d) | special, 0)) {
                u32 cause = 0;
                for (int i = 0; i < 4; i++)
                    cause |= fp_dot_special(v, &c->xf[i], 4, &r[i], x);
                if (fp_raise(c, cause))
                    return;
            } else {
                c->fpscr = (c->fpscr & fp_keep) | (((x >> 5) & 1) * fp_i_bits);
            }
            for (int i = 0; i < 4; i++)
                c->fr[vn + i] = r[i];
            return;
        }
        if ((n & 1) == 0) {                                       /* fsca FPUL,DRn */
            if (pr) { illegal(c, op); return; }
            /* the operation text sets "inexact" whatever the angle */
            c->fpscr = (c->fpscr & fp_keep) | fp_i_bits;
            if (__builtin_expect(c->fpscr & FPSCR_ENABLE & (FPC_I >> 5), 0) && fp_raise(c, FPC_I))
                return;
            /* recomp: fsca.h's table (runtime/cpu), not the host's sin and cos, so every PC
             * gets the same bits (docs/design.md section 45); the values are the same */
            c->fr[n].u = rt_fsca_sin(c->fpul & 0xffff);
            c->fr[n + 1].u = rt_fsca_cos(c->fpul & 0xffff);
            return;
        }
        if (n == 3) {                                             /* fschg */
            c->fpscr ^= FPSCR_SZ;
            return;
        }
        if (n == 11) {                                            /* frchg */
            OBS_XF();
            sh4_set_fpscr(c, c->fpscr ^ FPSCR_FR);
            return;
        }
        break;
    default:
        break;
    }
    illegal(c, op);
}

static void X(store_queue_flush)(sh4 *c, u32 addr)
{
    int q = (addr >> 5) & 1;
    u32 qacr = io_read(c, q ? 0xff00003cu : 0xff000038u, 4);
    u32 dst = (addr & 0x03ffffe0u) | ((qacr & 0x1c) << 24);
    u8 *p = mem_page[dst >> 16];
    if (p) {
#if OBS
        obs_store_block(c, dst, c->sq[q]);
#endif
        mem_code_note(dst, 32, c->sq[q]);   /* recomp */
        memcpy(p + (dst & 0xffff), c->sq[q], 32);
    }
    else
        io_sq_write(c, dst, c->sq[q]);
}

static void X(exec)(sh4 *c, u32 op)
{
    switch (op >> 12) {
    case 0x0:
        switch (op & 15) {
        case 0x2:
            if (op & 0x80) { c->r[N] = c->r_bank[(op >> 4) & 7]; return; }   /* stc Rm_BANK,Rn */
            switch (M) {
            case 0: c->r[N] = sh4_get_sr(c); return;
            case 1: c->r[N] = c->gbr; return;
            case 2: c->r[N] = c->vbr; return;
            case 3: c->r[N] = c->ssr; return;
            case 4: c->r[N] = c->spc; return;
            }
            break;
        case 0x3:
            switch (M) {
            case 0: {                                             /* bsrf */
                u32 t = c->pc + 2 + c->r[N];
                c->pr = c->pc + 2;
                X(slot)(c);
                X(go)(c, t, DISP_CALL);
                return;
            }
            case 2: {                                             /* braf */
                u32 t = c->pc + 2 + c->r[N];
#if OBS
                obs_computed(c, t);
#endif
                X(slot)(c);
                X(go)(c, t, DISP_JUMP);
                return;
            }
            case 8:                                               /* pref */
                if ((c->r[N] & 0xfc000000u) == 0xe0000000u)
                    X(store_queue_flush)(c, c->r[N]);
                return;
            case 9: case 10: case 11: return;                     /* ocbi, ocbp, ocbwb */
            case 12: MW32(c, c->r[N], c->r[0]); return;        /* movca.l */
            }
            break;
        case 0x4: MW8(c, c->r[0] + RN, RM); return;
        case 0x5: MW16(c, c->r[0] + RN, RM); return;
        case 0x6: MW32(c, c->r[0] + RN, RM); return;
        case 0x7: c->macl = RN * RM; return;                      /* mul.l */
        case 0x8:
            switch (op) {
            case 0x0008: c->t = 0; return;
            case 0x0018: c->t = 1; return;
            case 0x0028: c->mach = c->macl = 0; return;
            case 0x0038: return;                                  /* ldtlb: no MMU */
            case 0x0048: c->sr &= ~SR_S; return;
            case 0x0058: c->sr |= SR_S; return;
            }
            break;
        case 0x9:
            switch (op & 0xf0ff) {
            case 0x0009: if (op == 0x0009) return; break;         /* nop */
            case 0x0019: if (op == 0x0019) { c->sr &= ~(SR_Q | SR_M); c->t = 0; return; } break;
            case 0x0029: c->r[N] = c->t; return;                  /* movt */
            }
            break;
        case 0xa:
            switch (M) {
            case 0: c->r[N] = c->mach; return;
            case 1: c->r[N] = c->macl; return;
            case 2: c->r[N] = c->pr; return;
            case 3: c->r[N] = c->sgr; return;
            case 5: c->r[N] = c->fpul; return;
            case 6: c->r[N] = c->fpscr; return;
            case 15: c->r[N] = c->dbr; return;
            }
            break;
        case 0xb:
            switch (op) {
            case 0x000b: {                                        /* rts */
                u32 t = c->pr;
                X(slot)(c);
                c->pc = t;
#if OBS
                obs_on_return(c);
#endif
                if (__builtin_expect((((t & 0x1fffffffu) - IMAGE_BASE) >> 1) >= disp_slots, 0))
                    disp_outside(c, t, DISP_RETURN);
                return;
            }
            case 0x001b:                                          /* sleep */
                c->stop = STOP_SLEEP;
                c->next_event = 0;
                return;
            case 0x002b: {                                        /* rte */
                u32 t = c->spc, s = c->ssr;
                /* the slot instruction runs with the restored SR on the chip;
                 * the register bank is what code can see of that */
                sh4_set_sr(c, s);
                X(slot)(c);
                c->pc = t;
                disp_leave_exception(c);
                return;
            }
            }
            break;
        case 0xc: c->r[N] = (u32)(s32)(s8)MR8(c, c->r[0] + RM); return;
        case 0xd: c->r[N] = (u32)(s32)(s16)MR16(c, c->r[0] + RM); return;
        case 0xe: c->r[N] = MR32(c, c->r[0] + RM); return;
        case 0xf: {                                               /* mac.l @Rm+,@Rn+ */
            s64 a = (s32)MR32(c, c->r[N]);
            c->r[N] += 4;
            s64 b = (s32)MR32(c, c->r[M]);
            c->r[M] += 4;
            s64 mac = (s64)(((u64)c->mach << 32) | c->macl);
            s64 prod = a * b;
            if (c->sr & SR_S) {
                /* saturate the sum to 48 bits */
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
            return;
        }
        }
        break;

    case 0x1: MW32(c, RN + ((op & 15) << 2), RM); return;

    case 0x2:
        switch (op & 15) {
        case 0x0: MW8(c, RN, RM); return;
        case 0x1: MW16(c, RN, RM); return;
        case 0x2: MW32(c, RN, RM); return;
        case 0x4: { u32 v = RM; RN -= 1; MW8(c, RN, v); return; }
        case 0x5: { u32 v = RM; RN -= 2; MW16(c, RN, v); return; }
        case 0x6: { u32 v = RM; RN -= 4; MW32(c, RN, v); return; }
        case 0x7: {                                               /* div0s */
            u32 q = RN >> 31, m = RM >> 31;
            c->sr = (c->sr & ~(SR_Q | SR_M)) | (q ? SR_Q : 0) | (m ? SR_M : 0);
            c->t = q ^ m;
            return;
        }
        case 0x8: c->t = (RN & RM) == 0; return;
        case 0x9: RN &= RM; return;
        case 0xa: RN ^= RM; return;
        case 0xb: RN |= RM; return;
        case 0xc: {                                               /* cmp/str */
            u32 x = RN ^ RM;
            c->t = !(x & 0xff000000u) || !(x & 0x00ff0000u) || !(x & 0x0000ff00u) || !(x & 0x000000ffu);
            return;
        }
        case 0xd: RN = (RN >> 16) | (RM << 16); return;           /* xtrct */
        case 0xe: c->macl = (u32)(u16)RN * (u32)(u16)RM; return;  /* mulu.w */
        case 0xf: c->macl = (u32)((s32)(s16)RN * (s32)(s16)RM); return;
        }
        break;

    case 0x3:
        switch (op & 15) {
        case 0x0: c->t = RN == RM; return;
        case 0x2: c->t = RN >= RM; return;
        case 0x3: c->t = (s32)RN >= (s32)RM; return;
        case 0x4: {                                               /* div1 */
            u32 old_q = (c->sr & SR_Q) != 0, mm = (c->sr & SR_M) != 0;
            u32 q = RN >> 31;
            u32 d = RM, tmp0, tmp1;
            RN = (RN << 1) | c->t;
            tmp0 = RN;
            if (old_q == mm) {
                RN -= d;
                tmp1 = RN > tmp0;
            } else {
                RN += d;
                tmp1 = RN < tmp0;
            }
            /* Q after the step: the carry/borrow against the bit shifted out,
             * with the sense the manual's four cases give */
            if (!old_q) {
                if (!mm) q = q ? !tmp1 : tmp1;
                else     q = q ? tmp1 : !tmp1;
            } else {
                if (!mm) q = q ? !tmp1 : tmp1;
                else     q = q ? tmp1 : !tmp1;
            }
            c->sr = (c->sr & ~SR_Q) | (q ? SR_Q : 0);
            c->t = q == mm;
            return;
        }
        case 0x5: {                                               /* dmulu.l */
            u64 p = (u64)RN * (u64)RM;
            c->mach = (u32)(p >> 32);
            c->macl = (u32)p;
            return;
        }
        case 0x6: c->t = RN > RM; return;
        case 0x7: c->t = (s32)RN > (s32)RM; return;
        case 0x8: RN -= RM; return;
        case 0xa: {                                               /* subc */
            u32 a = RN, b = RM;
            u32 r = a - b - c->t;
            c->t = (a < b) || (a - b < c->t);
            RN = r;
            return;
        }
        case 0xb: {                                               /* subv */
            s32 a = (s32)RN, b = (s32)RM, r;
            c->t = __builtin_sub_overflow(a, b, &r);
            RN = (u32)r;
            return;
        }
        case 0xc: RN += RM; return;
        case 0xd: {                                               /* dmuls.l */
            s64 p = (s64)(s32)RN * (s64)(s32)RM;
            c->mach = (u32)((u64)p >> 32);
            c->macl = (u32)p;
            return;
        }
        case 0xe: {                                               /* addc */
            u32 a = RN, b = RM;
            u32 r = a + b;
            u32 r2 = r + c->t;
            c->t = (r < a) || (r2 < r);
            RN = r2;
            return;
        }
        case 0xf: {                                               /* addv */
            s32 a = (s32)RN, b = (s32)RM, r;
            c->t = __builtin_add_overflow(a, b, &r);
            RN = (u32)r;
            return;
        }
        }
        break;

    case 0x4:
        switch (op & 15) {
        case 0x0:
            switch (M) {
            case 0: c->t = RN >> 31; RN <<= 1; return;            /* shll */
            case 1: RN -= 1; c->t = RN == 0; return;              /* dt */
            case 2: c->t = RN >> 31; RN <<= 1; return;            /* shal */
            }
            break;
        case 0x1:
            switch (M) {
            case 0: c->t = RN & 1; RN >>= 1; return;              /* shlr */
            case 1: c->t = (s32)RN >= 0; return;                  /* cmp/pz */
            case 2: c->t = RN & 1; RN = (u32)((s32)RN >> 1); return;   /* shar */
            }
            break;
        case 0x2: {
            u32 v;
            switch (M) {
            case 0: v = c->mach; break;
            case 1: v = c->macl; break;
            case 2: v = c->pr; break;
            case 3: v = c->sgr; break;
            case 5: v = c->fpul; break;
            case 6: v = c->fpscr; break;
            case 15: v = c->dbr; break;
            default: illegal(c, op); return;
            }
            RN -= 4;
            MW32(c, RN, v);
            return;
        }
        case 0x3: {
            u32 v;
            if (op & 0x80) {
                v = c->r_bank[(op >> 4) & 7];
            } else {
                switch (M) {
                case 0: v = sh4_get_sr(c); break;
                case 1: v = c->gbr; break;
                case 2: v = c->vbr; break;
                case 3: v = c->ssr; break;
                case 4: v = c->spc; break;
                default: illegal(c, op); return;
                }
            }
            RN -= 4;
            MW32(c, RN, v);
            return;
        }
        case 0x4:
            switch (M) {
            case 0: c->t = RN >> 31; RN = (RN << 1) | c->t; return;          /* rotl */
            case 2: { u32 t = RN >> 31; RN = (RN << 1) | c->t; c->t = t; return; }   /* rotcl */
            }
            break;
        case 0x5:
            switch (M) {
            case 0: c->t = RN & 1; RN = (RN >> 1) | (c->t << 31); return;    /* rotr */
            case 1: c->t = (s32)RN > 0; return;                               /* cmp/pl */
            case 2: { u32 t = RN & 1; RN = (RN >> 1) | (c->t << 31); c->t = t; return; }   /* rotcr */
            }
            break;
        case 0x6: {
            u32 v = MR32(c, RN);
            RN += 4;
            switch (M) {
            case 0: c->mach = v; return;
            case 1: c->macl = v; return;
            case 2: c->pr = v; return;
            case 5: c->fpul = v; return;
            case 6: sh4_set_fpscr(c, v); return;
            case 15: c->dbr = v; return;
            }
            RN -= 4;
            break;
        }
        case 0x7: {
            u32 v = MR32(c, RN);
            RN += 4;
            if (op & 0x80) { c->r_bank[(op >> 4) & 7] = v; return; }
            switch (M) {
            case 0:
                if (c->in_slot) { RN -= 4; illegal(c, op); return; }
                sh4_set_sr(c, v);
                return;
            case 1: c->gbr = v; return;
            case 2: c->vbr = v; return;
            case 3: c->ssr = v; return;
            case 4: c->spc = v; return;
            }
            RN -= 4;
            break;
        }
        case 0x8:
            switch (M) {
            case 0: RN <<= 2; return;
            case 1: RN <<= 8; return;
            case 2: RN <<= 16; return;
            }
            break;
        case 0x9:
            switch (M) {
            case 0: RN >>= 2; return;
            case 1: RN >>= 8; return;
            case 2: RN >>= 16; return;
            }
            break;
        case 0xa:
            switch (M) {
            case 0: c->mach = RN; return;
            case 1: c->macl = RN; return;
            case 2: c->pr = RN; return;
            case 5: c->fpul = RN; return;
            case 6: sh4_set_fpscr(c, RN); return;
            case 15: c->dbr = RN; return;
            }
            break;
        case 0xb:
            switch (M) {
            case 0: {                                             /* jsr */
                u32 t = RN;
                c->pr = c->pc + 2;
                X(slot)(c);
                X(go)(c, t, DISP_CALL);
                return;
            }
            case 1: {                                             /* tas.b */
                u32 v = MR8(c, RN);
                c->t = v == 0;
                MW8(c, RN, v | 0x80);
                return;
            }
            case 2: {                                             /* jmp */
                u32 t = RN;
#if OBS
                obs_computed(c, t);
#endif
                X(slot)(c);
                X(go)(c, t, DISP_JUMP);
                return;
            }
            }
            break;
        case 0xc: {                                               /* shad */
            s32 s = (s32)RM;
            if (s >= 0) RN <<= (s & 31);
            else if ((s & 31) == 0) RN = (u32)((s32)RN >> 31);
            else RN = (u32)((s32)RN >> ((~s & 31) + 1));
            return;
        }
        case 0xd: {                                               /* shld */
            s32 s = (s32)RM;
            if (s >= 0) RN <<= (s & 31);
            else if ((s & 31) == 0) RN = 0;
            else RN >>= ((~s & 31) + 1);
            return;
        }
        case 0xe:
            if (op & 0x80) { c->r_bank[(op >> 4) & 7] = RN; return; }
            switch (M) {
            case 0:
                if (c->in_slot) { illegal(c, op); return; }
                sh4_set_sr(c, RN);
                return;
            case 1: c->gbr = RN; return;
            case 2: c->vbr = RN; return;
            case 3: c->ssr = RN; return;
            case 4: c->spc = RN; return;
            }
            break;
        case 0xf: {                                               /* mac.w @Rm+,@Rn+ */
            s32 a = (s16)MR16(c, c->r[N]);
            c->r[N] += 2;
            s32 b = (s16)MR16(c, c->r[M]);
            c->r[M] += 2;
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
            return;
        }
        }
        break;

    case 0x5: c->r[N] = MR32(c, RM + ((op & 15) << 2)); return;

    case 0x6:
        switch (op & 15) {
        case 0x0: c->r[N] = (u32)(s32)(s8)MR8(c, RM); return;
        case 0x1: c->r[N] = (u32)(s32)(s16)MR16(c, RM); return;
        case 0x2: c->r[N] = MR32(c, RM); return;
        case 0x3: c->r[N] = RM; return;
        case 0x4: { u32 v = (u32)(s32)(s8)MR8(c, RM); if (N != M) RM += 1; c->r[N] = v; return; }
        case 0x5: { u32 v = (u32)(s32)(s16)MR16(c, RM); if (N != M) RM += 2; c->r[N] = v; return; }
        case 0x6: { u32 v = MR32(c, RM); if (N != M) RM += 4; c->r[N] = v; return; }
        case 0x7: c->r[N] = ~RM; return;
        case 0x8: { u32 v = RM; c->r[N] = (v & 0xffff0000u) | ((v & 0xff) << 8) | ((v >> 8) & 0xff); return; }
        case 0x9: { u32 v = RM; c->r[N] = (v << 16) | (v >> 16); return; }
        case 0xa: {                                               /* negc */
            u32 v = RM;
            u32 t = 0 - v;
            c->r[N] = t - c->t;
            c->t = (0 < t) || (t < c->r[N]);
            return;
        }
        case 0xb: c->r[N] = 0 - RM; return;
        case 0xc: c->r[N] = RM & 0xff; return;
        case 0xd: c->r[N] = RM & 0xffff; return;
        case 0xe: c->r[N] = (u32)(s32)(s8)RM; return;
        case 0xf: c->r[N] = (u32)(s32)(s16)RM; return;
        }
        break;

    case 0x7: RN += (u32)IMM8S; return;

    case 0x8:
        switch (N) {
        case 0x0: MW8(c, RM + (op & 15), c->r[0]); return;
        case 0x1: MW16(c, RM + ((op & 15) << 1), c->r[0]); return;
        case 0x4: c->r[0] = (u32)(s32)(s8)MR8(c, RM + (op & 15)); return;
        case 0x5: c->r[0] = (u32)(s32)(s16)MR16(c, RM + ((op & 15) << 1)); return;
        case 0x8: c->t = c->r[0] == (u32)IMM8S; return;
        case 0x9:                                                 /* bt */
            OBS_BR(c->t != 0);
            if (c->t) c->pc = c->pc + 2 + (u32)(IMM8S * 2);
            return;
        case 0xb:                                                 /* bf */
            OBS_BR(c->t == 0);
            if (!c->t) c->pc = c->pc + 2 + (u32)(IMM8S * 2);
            return;
        case 0xd: {                                               /* bt/s */
            u32 t = c->pc + 2 + (u32)(IMM8S * 2);
            int take = c->t != 0;
            OBS_BR(take);
            X(slot)(c);
            if (take) c->pc = t;
            return;
        }
        case 0xf: {                                               /* bf/s */
            u32 t = c->pc + 2 + (u32)(IMM8S * 2);
            int take = c->t == 0;
            OBS_BR(take);
            X(slot)(c);
            if (take) c->pc = t;
            return;
        }
        }
        break;

    case 0x9:
        c->r[N] = (u32)(s32)(s16)MR16(c, c->pc + 2 + (IMM8U << 1));
        return;

    case 0xa: {                                                   /* bra */
        s32 d = (s32)(op << 20) >> 19;
        u32 t = c->pc + 2 + (u32)d;
        X(slot)(c);
        X(go)(c, t, DISP_JUMP);
        return;
    }
    case 0xb: {                                                   /* bsr */
        s32 d = (s32)(op << 20) >> 19;
        u32 t = c->pc + 2 + (u32)d;
        c->pr = c->pc + 2;
        X(slot)(c);
        X(go)(c, t, DISP_CALL);
        return;
    }

    case 0xc:
        switch (N) {
        case 0x0: MW8(c, c->gbr + IMM8U, c->r[0]); return;
        case 0x1: MW16(c, c->gbr + (IMM8U << 1), c->r[0]); return;
        case 0x2: MW32(c, c->gbr + (IMM8U << 2), c->r[0]); return;
        case 0x3:                                                 /* trapa */
            disp_trapa(c, IMM8U);
            return;
        case 0x4: c->r[0] = (u32)(s32)(s8)MR8(c, c->gbr + IMM8U); return;
        case 0x5: c->r[0] = (u32)(s32)(s16)MR16(c, c->gbr + (IMM8U << 1)); return;
        case 0x6: c->r[0] = MR32(c, c->gbr + (IMM8U << 2)); return;
        case 0x7: c->r[0] = ((c->pc + 2) & ~3u) + (IMM8U << 2); return;   /* mova */
        case 0x8: c->t = (c->r[0] & IMM8U) == 0; return;
        case 0x9: c->r[0] &= IMM8U; return;
        case 0xa: c->r[0] ^= IMM8U; return;
        case 0xb: c->r[0] |= IMM8U; return;
        case 0xc: c->t = (MR8(c, c->gbr + c->r[0]) & IMM8U) == 0; return;
        case 0xd: { u32 a = c->gbr + c->r[0]; MW8(c, a, MR8(c, a) & IMM8U); return; }
        case 0xe: { u32 a = c->gbr + c->r[0]; MW8(c, a, MR8(c, a) ^ IMM8U); return; }
        case 0xf: { u32 a = c->gbr + c->r[0]; MW8(c, a, MR8(c, a) | IMM8U); return; }
        }
        break;

    case 0xd:
        c->r[N] = MR32(c, ((c->pc + 2) & ~3u) + (IMM8U << 2));
        return;

    case 0xe: c->r[N] = (u32)IMM8S; return;

    case 0xf:
        X(fpu)(c, op);
        return;
    }
    illegal(c, op);
}


#undef X
#undef MR8
#undef MR16
#undef MR32
#undef MW8
#undef MW16
#undef MW32
#undef OBS_BR
#undef OBS_XF
