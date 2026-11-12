/* ARM7 interpreter for the sound driver: see arm7.h.
 *
 * ARM state (no Thumb: the AICA's core is an ARM7DI), 32-bit modes. What the
 * driver relies on that is the ARM7's own:
 *   - a store of r15 (STR, STM) stores the instruction's address + 12; the
 *     driver's calls are "stmfd sp!,{pc}; mov r0,r0; b target", returning
 *     to the instruction after the branch;
 *   - a word load from an address that is not a multiple of 4 reads the
 *     aligned word rotated right by 8 per byte of misalignment.
 * Coprocessor instructions take the undefined-instruction trap (there is no
 * coprocessor). Instructions are counted, not cycles. */
#include "arm7.h"

#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#define F_N 0x80000000u
#define F_Z 0x40000000u
#define F_C 0x20000000u
#define F_V 0x10000000u
#define F_I 0x80u
#define F_F 0x40u

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1b, M_SYS = 0x1f };

static uint8_t *ram;
static u32 ram_mask;

void arm7_set_ram(uint8_t *r, uint32_t mask)
{
    ram = r;
    ram_mask = mask;
}

/* sound RAM is below 0x00800000; the fast path takes it, the chip the rest */
static inline u32 rd32(u32 a)
{
    if (a < 0x00800000u) {
        u32 v;
        memcpy(&v, ram + (a & ram_mask & ~3u), 4);
        return v;
    }
    return arm7_bus_r32(a & ~3u);
}
static inline u32 rd8(u32 a)
{
    if (a < 0x00800000u)
        return ram[a & ram_mask];
    return arm7_bus_r8(a);
}
static inline void wr32(u32 a, u32 v)
{
    if (a < 0x00800000u) {
        memcpy(ram + (a & ram_mask & ~3u), &v, 4);
        return;
    }
    arm7_bus_w32(a & ~3u, v);
}
static inline void wr8(u32 a, u32 v)
{
    if (a < 0x00800000u) {
        ram[a & ram_mask] = (uint8_t)v;
        return;
    }
    arm7_bus_w8(a, v & 0xff);
}
static inline u32 rd16(u32 a)
{
    if (a < 0x00800000u) {
        uint16_t v;
        memcpy(&v, ram + (a & ram_mask & ~1u), 2);
        return v;
    }
    return arm7_bus_r16(a & ~1u);
}
static inline void wr16(u32 a, u32 v)
{
    if (a < 0x00800000u) {
        uint16_t h = (uint16_t)v;
        memcpy(ram + (a & ram_mask & ~1u), &h, 2);
        return;
    }
    arm7_bus_w16(a & ~1u, v & 0xffff);
}

static inline u32 ror32(u32 v, u32 n)
{
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

/* ---- modes and banked registers ---- */

static int bank_of(u32 mode)
{
    switch (mode) {
    case M_FIQ: return 1;
    case M_IRQ: return 2;
    case M_SVC: return 3;
    case M_ABT: return 4;
    case M_UND: return 5;
    default: return 0;          /* user, system */
    }
}

/* the CPSR's mode field becomes new_mode: the banked registers follow */
static void set_mode(arm7 *s, u32 new_mode)
{
    u32 old = s->cpsr & 0x1f;
    int ob = bank_of(old), nb = bank_of(new_mode);
    if (ob != nb) {
        s->r13_14[ob][0] = s->r[13];
        s->r13_14[ob][1] = s->r[14];
        if (ob)
            s->spsrs[ob] = s->spsr;
        if ((ob == 1) != (nb == 1)) {
            if (ob == 1) {
                memcpy(s->fiq_r8_12, &s->r[8], sizeof s->fiq_r8_12);
                memcpy(&s->r[8], s->usr_r8_12, sizeof s->usr_r8_12);
            } else {
                memcpy(s->usr_r8_12, &s->r[8], sizeof s->usr_r8_12);
                memcpy(&s->r[8], s->fiq_r8_12, sizeof s->fiq_r8_12);
            }
        }
        s->r[13] = s->r13_14[nb][0];
        s->r[14] = s->r13_14[nb][1];
        s->spsr = nb ? s->spsrs[nb] : 0;
    }
    s->cpsr = (s->cpsr & ~0x1fu) | new_mode;
}

static void write_cpsr(arm7 *s, u32 v)
{
    set_mode(s, v & 0x1f);
    s->cpsr = v;
}

/* an exception: to mode at vector, with the return address in r14 */
static void exception(arm7 *s, u32 mode, u32 vector, u32 ret, u32 mask)
{
    u32 old = s->cpsr;
    set_mode(s, mode);
    s->spsr = old;
    s->r[14] = ret;
    s->cpsr |= mask;
    s->r[15] = vector;
}

void arm7_reset(arm7 *s)
{
    memset(s, 0, sizeof *s);
    s->cpsr = M_SVC | F_I | F_F;
    s->r[15] = 0;
}

/* ---- the barrel shifter ---- */

static inline int cond_ok(u32 cpsr, u32 cond)
{
    int n = !!(cpsr & F_N), z = !!(cpsr & F_Z), c = !!(cpsr & F_C), v = !!(cpsr & F_V);
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xa: return n == v;
    case 0xb: return n != v;
    case 0xc: return !z && n == v;
    case 0xd: return z || n != v;
    case 0xe: return 1;
    default: return 0;          /* 0xf: never, on the ARM7 */
    }
}

/* operand 2 of a data-processing instruction (bit 25 clear), with its carry out */
static u32 shifted_reg(arm7 *s, u32 op, u32 pc, int *carry)
{
    u32 rm = op & 15, type = (op >> 5) & 3;
    u32 v;
    int c = !!(s->cpsr & F_C);
    if (op & 0x10) {
        /* shift by register: r15 reads 12 ahead here */
        u32 amt = s->r[(op >> 8) & 15] & 0xff;
        v = rm == 15 ? pc + 12 : s->r[rm];
        if (amt == 0) {
            *carry = c;
            return v;
        }
        switch (type) {
        case 0:
            if (amt < 32) { c = (v >> (32 - amt)) & 1; v <<= amt; }
            else if (amt == 32) { c = v & 1; v = 0; }
            else { c = 0; v = 0; }
            break;
        case 1:
            if (amt < 32) { c = (v >> (amt - 1)) & 1; v >>= amt; }
            else if (amt == 32) { c = v >> 31; v = 0; }
            else { c = 0; v = 0; }
            break;
        case 2:
            if (amt < 32) { c = ((s32)v >> (amt - 1)) & 1; v = (u32)((s32)v >> amt); }
            else { c = v >> 31; v = c ? 0xffffffffu : 0; }
            break;
        default:
            if ((amt & 31) == 0) c = v >> 31;
            else { c = (v >> ((amt & 31) - 1)) & 1; v = ror32(v, amt); }
            break;
        }
        *carry = c;
        return v;
    }
    u32 amt = (op >> 7) & 31;
    v = rm == 15 ? pc + 8 : s->r[rm];
    switch (type) {
    case 0:
        if (amt) { c = (v >> (32 - amt)) & 1; v <<= amt; }
        break;
    case 1:
        if (amt) { c = (v >> (amt - 1)) & 1; v >>= amt; }
        else { c = v >> 31; v = 0; }            /* LSR #32 */
        break;
    case 2:
        if (amt) { c = ((s32)v >> (amt - 1)) & 1; v = (u32)((s32)v >> amt); }
        else { c = v >> 31; v = c ? 0xffffffffu : 0; }      /* ASR #32 */
        break;
    default:
        if (amt) { c = (v >> (amt - 1)) & 1; v = ror32(v, amt); }
        else { u32 cin = !!(s->cpsr & F_C); c = v & 1; v = (v >> 1) | (cin << 31); }   /* RRX */
        break;
    }
    *carry = c;
    return v;
}

static inline void set_nz(arm7 *s, u32 v)
{
    s->cpsr = (s->cpsr & ~(F_N | F_Z)) | (v & F_N) | (v ? 0 : F_Z);
}

static inline void set_nzcv(arm7 *s, u32 v, int c, int ov)
{
    s->cpsr = (s->cpsr & ~(F_N | F_Z | F_C | F_V)) | (v & F_N) | (v ? 0 : F_Z) | (c ? F_C : 0) | (ov ? F_V : 0);
}

static void undefined(arm7 *s, u32 pc, u32 op)
{
    if (s->undefined_seen++ < 8)
        fprintf(stderr, "arm7: undefined instruction %08x at %08x\n", op, pc);
    exception(s, M_UND, 0x04, pc + 4, F_I);
}

/* ---- one instruction ---- */

static void step(arm7 *s)
{
    u32 pc = s->r[15];
    u32 op = rd32(pc);
    s->r[15] = pc + 4;          /* the next instruction; reads of r15 below use pc + 8 */
    s->insns++;
    if (!cond_ok(s->cpsr, op >> 28))
        return;

    switch ((op >> 25) & 7) {
    case 0:
    case 1: {
        if ((op & 0x0fc000f0u) == 0x00000090u) {                 /* MUL, MLA */
            u32 rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            u32 v = s->r[rm] * s->r[rs];
            if (op & (1u << 21))
                v += s->r[rn];
            s->r[rd] = v;
            if (op & (1u << 20))
                set_nz(s, v);
            return;
        }
        if ((op & 0x0f8000f0u) == 0x00800090u) {                 /* UMULL, UMLAL, SMULL, SMLAL */
            u32 hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            u64 v = (op & (1u << 22)) ? (u64)((s64)(s32)s->r[rm] * (s64)(s32)s->r[rs])
                                      : (u64)s->r[rm] * (u64)s->r[rs];
            if (op & (1u << 21))
                v += ((u64)s->r[hi] << 32) | s->r[lo];
            s->r[lo] = (u32)v;
            s->r[hi] = (u32)(v >> 32);
            if (op & (1u << 20))
                s->cpsr = (s->cpsr & ~(F_N | F_Z)) | ((u32)(v >> 32) & F_N) | (v ? 0 : F_Z);
            return;
        }
        if ((op & 0x0fb00ff0u) == 0x01000090u) {                 /* SWP, SWPB */
            u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15, a = s->r[rn];
            if (op & (1u << 22)) {
                u32 t = rd8(a);
                wr8(a, s->r[rm]);
                s->r[rd] = t;
            } else {
                u32 t = ror32(rd32(a), (a & 3) * 8);
                wr32(a, s->r[rm]);
                s->r[rd] = t;
            }
            return;
        }
        if ((op & 0x0e000090u) == 0x00000090u && (op & 0x60)) {  /* LDRH, STRH, LDRSB, LDRSH */
            u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
            u32 off = (op & (1u << 22)) ? (((op >> 4) & 0xf0) | (op & 15)) : s->r[op & 15];
            u32 base = rn == 15 ? pc + 8 : s->r[rn];
            u32 a = (op & (1u << 23)) ? base + off : base - off;
            u32 at = (op & (1u << 24)) ? a : base;
            if (op & (1u << 20)) {
                u32 v;
                switch ((op >> 5) & 3) {
                case 1: v = rd16(at); break;
                case 2: v = (u32)(s32)(int8_t)rd8(at); break;
                default: v = (u32)(s32)(int16_t)rd16(at); break;
                }
                if (!(op & (1u << 24)) || (op & (1u << 21)))
                    s->r[rn] = a;
                s->r[rd] = v;
            } else {
                wr16(at, rd == 15 ? pc + 12 : s->r[rd]);
                if (!(op & (1u << 24)) || (op & (1u << 21)))
                    s->r[rn] = a;
            }
            return;
        }
        if ((op & 0x0fbf0fffu) == 0x010f0000u) {                 /* MRS */
            s->r[(op >> 12) & 15] = (op & (1u << 22)) ? s->spsr : s->cpsr;
            return;
        }
        if ((op & 0x0db0f000u) == 0x0120f000u) {                 /* MSR */
            u32 v = (op & (1u << 25)) ? ror32(op & 0xff, ((op >> 8) & 15) * 2) : s->r[op & 15];
            u32 mask = 0;
            if (op & (1u << 16)) mask |= 0x000000ffu;
            if (op & (1u << 17)) mask |= 0x0000ff00u;
            if (op & (1u << 18)) mask |= 0x00ff0000u;
            if (op & (1u << 19)) mask |= 0xff000000u;
            if (op & (1u << 22)) {
                if (bank_of(s->cpsr & 0x1f))
                    s->spsr = (s->spsr & ~mask) | (v & mask);
            } else {
                if ((s->cpsr & 0x1f) == M_USR)
                    mask &= 0xff000000u;
                write_cpsr(s, (s->cpsr & ~mask) | (v & mask));
            }
            return;
        }
        if ((op & 0x0ffffff0u) == 0x012fff10u) {                 /* BX (no Thumb: the bit is dropped) */
            s->r[15] = s->r[op & 15] & ~3u;
            return;
        }
        if ((op & 0x0e000090u) == 0x00000090u && !(op & (1u << 25))) {
            undefined(s, pc, op);
            return;
        }
        /* data processing */
        u32 opc = (op >> 21) & 15, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        int S = (op >> 20) & 1, sc;
        u32 b;
        if (op & (1u << 25)) {
            u32 rot = ((op >> 8) & 15) * 2;
            b = ror32(op & 0xff, rot);
            sc = rot ? (int)(b >> 31) : !!(s->cpsr & F_C);
        } else {
            b = shifted_reg(s, op, pc, &sc);
        }
        u32 a = rn == 15 ? ((op & 0x02000010u) == 0x00000010u ? pc + 12 : pc + 8) : s->r[rn];
        u32 r = 0, cin = !!(s->cpsr & F_C);
        int c = sc, v = !!(s->cpsr & F_V), arith = 1, wr = 1;
        switch (opc) {
        case 0x0: r = a & b; arith = 0; break;                                   /* AND */
        case 0x1: r = a ^ b; arith = 0; break;                                   /* EOR */
        case 0x2: r = a - b; c = a >= b; v = ((a ^ b) & (a ^ r)) >> 31; break;   /* SUB */
        case 0x3: r = b - a; c = b >= a; v = ((b ^ a) & (b ^ r)) >> 31; break;   /* RSB */
        case 0x4: r = a + b; c = r < a; v = (~(a ^ b) & (a ^ r)) >> 31; break;   /* ADD */
        case 0x5: { u64 t = (u64)a + b + cin; r = (u32)t; c = (int)(t >> 32); v = (~(a ^ b) & (a ^ r)) >> 31; break; }  /* ADC */
        case 0x6: { u64 t = (u64)a - b - (1 - cin); r = (u32)t; c = !(t >> 63); v = ((a ^ b) & (a ^ r)) >> 31; break; } /* SBC */
        case 0x7: { u64 t = (u64)b - a - (1 - cin); r = (u32)t; c = !(t >> 63); v = ((b ^ a) & (b ^ r)) >> 31; break; } /* RSC */
        case 0x8: r = a & b; arith = 0; wr = 0; break;                           /* TST */
        case 0x9: r = a ^ b; arith = 0; wr = 0; break;                           /* TEQ */
        case 0xa: r = a - b; c = a >= b; v = ((a ^ b) & (a ^ r)) >> 31; wr = 0; break;   /* CMP */
        case 0xb: r = a + b; c = r < a; v = (~(a ^ b) & (a ^ r)) >> 31; wr = 0; break;   /* CMN */
        case 0xc: r = a | b; arith = 0; break;                                   /* ORR */
        case 0xd: r = b; arith = 0; break;                                       /* MOV */
        case 0xe: r = a & ~b; arith = 0; break;                                  /* BIC */
        default: r = ~b; arith = 0; break;                                       /* MVN */
        }
        if (wr) {
            s->r[rd] = r;
            if (rd == 15) {
                if (S && bank_of(s->cpsr & 0x1f))
                    write_cpsr(s, s->spsr);         /* the return from an exception */
                s->r[15] &= ~3u;
                return;
            }
        }
        if (S) {
            if (arith)
                set_nzcv(s, r, c, v);
            else
                set_nzcv(s, r, c, !!(s->cpsr & F_V));
        }
        return;
    }
    case 2:
    case 3: {                                                       /* LDR, STR, LDRB, STRB */
        if ((op & (1u << 25)) && (op & 0x10)) {
            undefined(s, pc, op);
            return;
        }
        u32 rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        u32 off;
        if (op & (1u << 25)) {
            int unused;
            off = shifted_reg(s, op & ~0x10u, pc, &unused);
        } else {
            off = op & 0xfff;
        }
        u32 base = rn == 15 ? pc + 8 : s->r[rn];
        u32 a = (op & (1u << 23)) ? base + off : base - off;
        u32 at = (op & (1u << 24)) ? a : base;
        int wb = !(op & (1u << 24)) || (op & (1u << 21));
        if (op & (1u << 20)) {
            u32 v = (op & (1u << 22)) ? rd8(at) : ror32(rd32(at), (at & 3) * 8);
            if (wb)
                s->r[rn] = a;
            s->r[rd] = v;               /* the load wins over the write-back */
            if (rd == 15)
                s->r[15] &= ~3u;
        } else {
            u32 v = rd == 15 ? pc + 12 : s->r[rd];
            if (op & (1u << 22))
                wr8(at, v);
            else
                wr32(at, v);
            if (wb)
                s->r[rn] = a;
        }
        return;
    }
    case 4: {                                                       /* LDM, STM */
        u32 rn = (op >> 16) & 15, list = op & 0xffff;
        int n = __builtin_popcount(list);
        u32 base = s->r[rn], a;
        int up = (op >> 23) & 1, pre = (op >> 24) & 1, S = (op >> 22) & 1, W = (op >> 21) & 1;
        if (up)
            a = pre ? base + 4 : base;
        else
            a = pre ? base - 4u * n : base - 4u * n + 4;
        u32 newbase = up ? base + 4u * n : base - 4u * n;
        if (op & (1u << 20)) {
            /* S without r15: the user bank; with r15: the CPSR comes back from the SPSR */
            int user = S && !(list & 0x8000);
            u32 mode = s->cpsr & 0x1f;
            if (W)
                s->r[rn] = newbase;
            if (user)
                set_mode(s, M_USR);
            for (int i = 0; i < 16; i++)
                if (list & (1u << i)) {
                    s->r[i] = rd32(a);
                    a += 4;
                }
            if (user)
                set_mode(s, mode);
            if (list & 0x8000) {
                if (S && bank_of(s->cpsr & 0x1f))
                    write_cpsr(s, s->spsr);
                s->r[15] &= ~3u;
            }
        } else {
            int user = S;
            u32 mode = s->cpsr & 0x1f;
            if (user)
                set_mode(s, M_USR);
            int first = 1;
            for (int i = 0; i < 16; i++)
                if (list & (1u << i)) {
                    u32 v = i == 15 ? pc + 12 : s->r[i];
                    if (i == (int)rn && W && !first)
                        v = newbase;    /* the ARM7 stores the written-back base unless it is first */
                    wr32(a, v);
                    a += 4;
                    first = 0;
                }
            if (user)
                set_mode(s, mode);
            if (W)
                s->r[rn] = newbase;
        }
        return;
    }
    case 5: {                                                       /* B, BL */
        s32 off = (s32)(op << 8) >> 6;
        if (op & (1u << 24))
            s->r[14] = pc + 4;
        s->r[15] = pc + 8 + (u32)off;
        return;
    }
    case 6:                                                         /* coprocessor transfers: none here */
        undefined(s, pc, op);
        return;
    default:
        if (op & (1u << 24)) {                                      /* SWI */
            exception(s, M_SVC, 0x08, pc + 4, F_I);
            return;
        }
        undefined(s, pc, op);
        return;
    }
}

void arm7_run(arm7 *s, uint32_t n)
{
    while (n--) {
        if (s->fiq && !(s->cpsr & F_F)) {
            /* r14_fiq: the next instruction + 4, so that "subs pc, lr, #4" returns to it */
            exception(s, M_FIQ, 0x1c, s->r[15] + 4, F_I | F_F);
            s->insns++;
            continue;
        }
        step(s);
    }
}
