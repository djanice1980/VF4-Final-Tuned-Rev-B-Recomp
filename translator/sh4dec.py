"""SH-4 instruction decoding and the C each instruction becomes.

decode(op, addr) mirrors the case structure of VF4debug's interpreter
(src/sh4_exec.inc) case for case, so the two accept exactly the same words;
the C emitted for each instruction does what the interpreter's case does,
using the helpers of runtime/cpu/sh4rt.h for anything longer than a line.

FPU mode: an instruction whose meaning depends on FPSCR.SZ or FPSCR.PR is
emitted for a mode given at emit time: 0 or 1 when the translator knows it,
None when it does not (the C then tests FPSCR at run time).
"""

# flow kinds
NORMAL, CBR, CBR_S, BRA, BSR, BRAF, BSRF, JMP, JSR, RTS, RTE, SLEEP, TRAPA, ILLEGAL = range(14)
DELAYED = {CBR_S, BRA, BSR, BRAF, BSRF, JMP, JSR, RTS, RTE}
CONTROL = DELAYED | {CBR, SLEEP, TRAPA, ILLEGAL}


def h32(v):
    return f"0x{v & 0xffffffff:08x}u"


def R(n):
    return f"c->r[{n}]"


def FR(n):
    return f"c->fr[{n}]"


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def s12(v):
    return v - 0x1000 if v & 0x800 else v


SZ_RT = "((c->fpscr & FPSCR_SZ) != 0)"
PR_RT = "((c->fpscr & FPSCR_PR) != 0)"


class Insn:
    __slots__ = ("addr", "op", "text", "kind", "target", "reg", "mode", "emit", "writes", "pcload",
                 "slot_ok", "fpscr_write", "fpscr_save", "sz_flip")

    def __init__(self, addr, op):
        self.addr = addr
        self.op = op
        self.text = f".word 0x{op:04x}"
        self.kind = ILLEGAL
        self.target = None      # pc-relative branch target
        self.reg = None         # register of braf/bsrf/jmp/jsr
        self.mode = None        # 'sz' or 'pr': the FPSCR field the meaning depends on
        self.emit = None        # f(sz, pr) -> list of C statements
        self.writes = None      # GPRs written: set of ints, or 'all'
        self.pcload = None      # (reg, address, size) for mov.w/mov.l @(disp,PC)
        self.slot_ok = True     # may sit in a delay slot
        self.fpscr_write = None # 'reg' n, 'pop' n, for FPSCR loads
        self.fpscr_save = False # sts.l fpscr,@-r15
        self.sz_flip = False    # fschg


def _simple(i, text, code, writes=()):
    i.kind = NORMAL
    i.text = text
    lines = code if isinstance(code, list) else [code]
    i.emit = lambda sz, pr, _l=lines: list(_l)
    i.writes = set(writes)


def _fpu_mode(i, mode, text, fn, writes=()):
    """fn(v) -> list of C lines, v the mode as 0/1 or a C expression."""
    i.kind = NORMAL
    i.text = text
    i.mode = mode
    i.writes = set(writes)

    def emit(sz, pr, _fn=fn, _mode=mode):
        v = sz if _mode == "sz" else pr
        if v is None:
            return [f"if ({SZ_RT if _mode == 'sz' else PR_RT}) {{"] + ["    " + x for x in _fn(1)] + \
                   ["} else {"] + ["    " + x for x in _fn(0)] + ["}"]
        return _fn(v)
    i.emit = emit


def decode(op, addr):
    i = Insn(addr, op)
    n = (op >> 8) & 15
    m = (op >> 4) & 15
    imm8 = op & 0xff
    at = h32(addr)
    top = op >> 12
    low = op & 15

    def ill():
        i.kind = ILLEGAL
        i.text = f".word 0x{op:04x}"
        i.emit = lambda sz, pr: [f"rt_illegal(c, 0x{op:04x}, {at}); return;"]
        i.writes = "all"
        return i

    if top == 0x0:
        if low == 0x2:
            if op & 0x80:
                b = (op >> 4) & 7
                _simple(i, f"stc r{b}_bank,r{n}", f"{R(n)} = c->r_bank[{b}];", [n])
                return i
            src = {0: ("sr", "rt_get_sr(c)"), 1: ("gbr", "c->gbr"), 2: ("vbr", "c->vbr"),
                   3: ("ssr", "c->ssr"), 4: ("spc", "c->spc")}.get(m)
            if src is None:
                return ill()
            _simple(i, f"stc {src[0]},r{n}", f"{R(n)} = {src[1]};", [n])
            return i
        if low == 0x3:
            if m == 0:
                i.kind, i.text, i.reg, i.writes = BSRF, f"bsrf r{n}", n, set()
                i.slot_ok = False
                return i
            if m == 2:
                i.kind, i.text, i.reg, i.writes = BRAF, f"braf r{n}", n, set()
                i.slot_ok = False
                return i
            if m == 8:
                _simple(i, f"pref @r{n}", f"op_pref(c, {n});")
                return i
            if m in (9, 10, 11):
                _simple(i, f"{('ocbi', 'ocbp', 'ocbwb')[m - 9]} @r{n}", "/* no cache */")
                return i
            if m == 12:
                _simple(i, f"movca.l r0,@r{n}", f"mem_w32(c, {R(n)}, {R(0)});")
                return i
            return ill()
        if low in (0x4, 0x5, 0x6):
            w = {4: "8", 5: "16", 6: "32"}[low]
            sfx = {4: "b", 5: "w", 6: "l"}[low]
            _simple(i, f"mov.{sfx} r{m},@(r0,r{n})", f"mem_w{w}(c, {R(0)} + {R(n)}, {R(m)});")
            return i
        if low == 0x7:
            _simple(i, f"mul.l r{m},r{n}", f"c->macl = {R(n)} * {R(m)};")
            return i
        if low == 0x8:
            t = {0x0008: ("clrt", "c->t = 0;"), 0x0018: ("sett", "c->t = 1;"),
                 0x0028: ("clrmac", "c->mach = c->macl = 0;"), 0x0038: ("ldtlb", "/* no MMU */"),
                 0x0048: ("clrs", "c->sr &= ~SR_S;"), 0x0058: ("sets", "c->sr |= SR_S;")}.get(op)
            if t is None:
                return ill()
            _simple(i, t[0], t[1])
            return i
        if low == 0x9:
            if op == 0x0009:
                _simple(i, "nop", "/* nop */")
                return i
            if op == 0x0019:
                _simple(i, "div0u", "c->sr &= ~(SR_Q | SR_M); c->t = 0;")
                return i
            if (op & 0xf0ff) == 0x0029:
                _simple(i, f"movt r{n}", f"{R(n)} = c->t;", [n])
                return i
            return ill()
        if low == 0xa:
            src = {0: ("mach", "c->mach"), 1: ("macl", "c->macl"), 2: ("pr", "c->pr"), 3: ("sgr", "c->sgr"),
                   5: ("fpul", "c->fpul"), 6: ("fpscr", "c->fpscr"), 15: ("dbr", "c->dbr")}.get(m)
            if src is None:
                return ill()
            _simple(i, f"sts {src[0]},r{n}", f"{R(n)} = {src[1]};", [n])
            return i
        if low == 0xb:
            if op == 0x000b:
                i.kind, i.text, i.writes, i.slot_ok = RTS, "rts", set(), False
                return i
            if op == 0x001b:
                i.kind, i.text, i.writes = SLEEP, "sleep", "all"
                i.emit = lambda sz, pr: [f"rt_sleep(c, {at});"]
                i.slot_ok = False
                return i
            if op == 0x002b:
                i.kind, i.text, i.writes, i.slot_ok = RTE, "rte", "all", False
                return i
            return ill()
        if low in (0xc, 0xd, 0xe):
            if low == 0xc:
                _simple(i, f"mov.b @(r0,r{m}),r{n}", f"{R(n)} = (u32)(s32)(s8)mem_r8(c, {R(0)} + {R(m)});", [n])
            elif low == 0xd:
                _simple(i, f"mov.w @(r0,r{m}),r{n}", f"{R(n)} = (u32)(s32)(s16)mem_r16(c, {R(0)} + {R(m)});", [n])
            else:
                _simple(i, f"mov.l @(r0,r{m}),r{n}", f"{R(n)} = mem_r32(c, {R(0)} + {R(m)});", [n])
            return i
        if low == 0xf:
            _simple(i, f"mac.l @r{m}+,@r{n}+", f"op_macl(c, {n}, {m});", [n, m])
            return i
        return ill()

    if top == 0x1:
        d = (op & 15) << 2
        _simple(i, f"mov.l r{m},@({d},r{n})", f"mem_w32(c, {R(n)} + {d}u, {R(m)});")
        return i

    if top == 0x2:
        if low in (0, 1, 2):
            w, sfx = {0: ("8", "b"), 1: ("16", "w"), 2: ("32", "l")}[low]
            _simple(i, f"mov.{sfx} r{m},@r{n}", f"mem_w{w}(c, {R(n)}, {R(m)});")
            return i
        if low in (4, 5, 6):
            w, sfx, k = {4: ("8", "b", 1), 5: ("16", "w", 2), 6: ("32", "l", 4)}[low]
            _simple(i, f"mov.{sfx} r{m},@-r{n}",
                    f"{{ u32 v = {R(m)}; {R(n)} -= {k}; mem_w{w}(c, {R(n)}, v); }}", [n])
            return i
        t = {7: (f"div0s r{m},r{n}", f"op_div0s(c, {n}, {m});", []),
             8: (f"tst r{m},r{n}", f"c->t = ({R(n)} & {R(m)}) == 0;", []),
             9: (f"and r{m},r{n}", f"{R(n)} &= {R(m)};", [n]),
             0xa: (f"xor r{m},r{n}", f"{R(n)} ^= {R(m)};", [n]),
             0xb: (f"or r{m},r{n}", f"{R(n)} |= {R(m)};", [n]),
             0xc: (f"cmp/str r{m},r{n}",
                   f"{{ u32 x = {R(n)} ^ {R(m)}; c->t = !(x & 0xff000000u) || !(x & 0x00ff0000u) || "
                   f"!(x & 0x0000ff00u) || !(x & 0x000000ffu); }}", []),
             0xd: (f"xtrct r{m},r{n}", f"{R(n)} = ({R(n)} >> 16) | ({R(m)} << 16);", [n]),
             0xe: (f"mulu.w r{m},r{n}", f"c->macl = (u32)(u16){R(n)} * (u32)(u16){R(m)};", []),
             0xf: (f"muls.w r{m},r{n}", f"c->macl = (u32)((s32)(s16){R(n)} * (s32)(s16){R(m)});", [])}.get(low)
        if t is None:
            return ill()
        _simple(i, t[0], t[1], t[2])
        return i

    if top == 0x3:
        t = {0x0: (f"cmp/eq r{m},r{n}", f"c->t = {R(n)} == {R(m)};", []),
             0x2: (f"cmp/hs r{m},r{n}", f"c->t = {R(n)} >= {R(m)};", []),
             0x3: (f"cmp/ge r{m},r{n}", f"c->t = (s32){R(n)} >= (s32){R(m)};", []),
             0x4: (f"div1 r{m},r{n}", f"op_div1(c, {n}, {m});", [n]),
             0x5: (f"dmulu.l r{m},r{n}",
                   f"{{ u64 p = (u64){R(n)} * (u64){R(m)}; c->mach = (u32)(p >> 32); c->macl = (u32)p; }}", []),
             0x6: (f"cmp/hi r{m},r{n}", f"c->t = {R(n)} > {R(m)};", []),
             0x7: (f"cmp/gt r{m},r{n}", f"c->t = (s32){R(n)} > (s32){R(m)};", []),
             0x8: (f"sub r{m},r{n}", f"{R(n)} -= {R(m)};", [n]),
             0xa: (f"subc r{m},r{n}", f"op_subc(c, {n}, {m});", [n]),
             0xb: (f"subv r{m},r{n}", f"op_subv(c, {n}, {m});", [n]),
             0xc: (f"add r{m},r{n}", f"{R(n)} += {R(m)};", [n]),
             0xd: (f"dmuls.l r{m},r{n}",
                   f"{{ s64 p = (s64)(s32){R(n)} * (s64)(s32){R(m)}; c->mach = (u32)((u64)p >> 32); "
                   f"c->macl = (u32)p; }}", []),
             0xe: (f"addc r{m},r{n}", f"op_addc(c, {n}, {m});", [n]),
             0xf: (f"addv r{m},r{n}", f"op_addv(c, {n}, {m});", [n])}.get(low)
        if t is None:
            return ill()
        _simple(i, t[0], t[1], t[2])
        return i

    if top == 0x4:
        if low == 0x0:
            t = {0: (f"shll r{n}", f"c->t = {R(n)} >> 31; {R(n)} <<= 1;"),
                 1: (f"dt r{n}", f"{R(n)} -= 1; c->t = {R(n)} == 0;"),
                 2: (f"shal r{n}", f"c->t = {R(n)} >> 31; {R(n)} <<= 1;")}.get(m)
            if t is None:
                return ill()
            _simple(i, t[0], t[1], [n])
            return i
        if low == 0x1:
            t = {0: (f"shlr r{n}", f"c->t = {R(n)} & 1; {R(n)} >>= 1;", [n]),
                 1: (f"cmp/pz r{n}", f"c->t = (s32){R(n)} >= 0;", []),
                 2: (f"shar r{n}", f"c->t = {R(n)} & 1; {R(n)} = (u32)((s32){R(n)} >> 1);", [n])}.get(m)
            if t is None:
                return ill()
            _simple(i, t[0], t[1], t[2])
            return i
        if low == 0x2:
            src = {0: ("mach", "c->mach"), 1: ("macl", "c->macl"), 2: ("pr", "c->pr"), 3: ("sgr", "c->sgr"),
                   5: ("fpul", "c->fpul"), 6: ("fpscr", "c->fpscr"), 15: ("dbr", "c->dbr")}.get(m)
            if src is None:
                return ill()
            _simple(i, f"sts.l {src[0]},@-r{n}",
                    f"{{ u32 v = {src[1]}; {R(n)} -= 4; mem_w32(c, {R(n)}, v); }}", [n])
            i.fpscr_save = (m == 6 and n == 15)
            return i
        if low == 0x3:
            if op & 0x80:
                b = (op >> 4) & 7
                _simple(i, f"stc.l r{b}_bank,@-r{n}",
                        f"{{ u32 v = c->r_bank[{b}]; {R(n)} -= 4; mem_w32(c, {R(n)}, v); }}", [n])
                return i
            src = {0: ("sr", "rt_get_sr(c)"), 1: ("gbr", "c->gbr"), 2: ("vbr", "c->vbr"),
                   3: ("ssr", "c->ssr"), 4: ("spc", "c->spc")}.get(m)
            if src is None:
                return ill()
            _simple(i, f"stc.l {src[0]},@-r{n}",
                    f"{{ u32 v = {src[1]}; {R(n)} -= 4; mem_w32(c, {R(n)}, v); }}", [n])
            return i
        if low == 0x4:
            t = {0: (f"rotl r{n}", f"c->t = {R(n)} >> 31; {R(n)} = ({R(n)} << 1) | c->t;"),
                 2: (f"rotcl r{n}", f"{{ u32 t = {R(n)} >> 31; {R(n)} = ({R(n)} << 1) | c->t; c->t = t; }}")}.get(m)
            if t is None:
                return ill()
            _simple(i, t[0], t[1], [n])
            return i
        if low == 0x5:
            t = {0: (f"rotr r{n}", f"c->t = {R(n)} & 1; {R(n)} = ({R(n)} >> 1) | (c->t << 31);", [n]),
                 1: (f"cmp/pl r{n}", f"c->t = (s32){R(n)} > 0;", []),
                 2: (f"rotcr r{n}", f"{{ u32 t = {R(n)} & 1; {R(n)} = ({R(n)} >> 1) | (c->t << 31); c->t = t; }}",
                     [n])}.get(m)
            if t is None:
                return ill()
            _simple(i, t[0], t[1], t[2])
            return i
        if low == 0x6:
            dst = {0: ("mach", "c->mach = v;"), 1: ("macl", "c->macl = v;"), 2: ("pr", "c->pr = v;"),
                   5: ("fpul", "c->fpul = v;"), 6: ("fpscr", "rt_set_fpscr(c, v);"),
                   15: ("dbr", "c->dbr = v;")}.get(m)
            if dst is None:
                return ill()
            _simple(i, f"lds.l @r{n}+,{dst[0]}",
                    f"{{ u32 v = mem_r32(c, {R(n)}); {R(n)} += 4; {dst[1]} }}", [n])
            if m == 6:
                i.fpscr_write = ("pop", n)
            return i
        if low == 0x7:
            if op & 0x80:
                b = (op >> 4) & 7
                _simple(i, f"ldc.l @r{n}+,r{b}_bank",
                        f"{{ u32 v = mem_r32(c, {R(n)}); {R(n)} += 4; c->r_bank[{b}] = v; }}", [n])
                return i
            dst = {0: ("sr", "rt_set_sr(c, v);"), 1: ("gbr", "c->gbr = v;"), 2: ("vbr", "c->vbr = v;"),
                   3: ("ssr", "c->ssr = v;"), 4: ("spc", "c->spc = v;")}.get(m)
            if dst is None:
                return ill()
            _simple(i, f"ldc.l @r{n}+,{dst[0]}",
                    f"{{ u32 v = mem_r32(c, {R(n)}); {R(n)} += 4; {dst[1]} }}", "all" if m == 0 else [n])
            if m == 0:
                i.writes = "all"
                i.slot_ok = False
            return i
        if low == 0x8:
            k = {0: 2, 1: 8, 2: 16}.get(m)
            if k is None:
                return ill()
            _simple(i, f"shll{k} r{n}", f"{R(n)} <<= {k};", [n])
            return i
        if low == 0x9:
            k = {0: 2, 1: 8, 2: 16}.get(m)
            if k is None:
                return ill()
            _simple(i, f"shlr{k} r{n}", f"{R(n)} >>= {k};", [n])
            return i
        if low == 0xa:
            dst = {0: ("mach", f"c->mach = {R(n)};"), 1: ("macl", f"c->macl = {R(n)};"),
                   2: ("pr", f"c->pr = {R(n)};"), 5: ("fpul", f"c->fpul = {R(n)};"),
                   6: ("fpscr", f"rt_set_fpscr(c, {R(n)});"), 15: ("dbr", f"c->dbr = {R(n)};")}.get(m)
            if dst is None:
                return ill()
            _simple(i, f"lds r{n},{dst[0]}", dst[1])
            if m == 6:
                i.fpscr_write = ("reg", n)
            return i
        if low == 0xb:
            if m == 0:
                i.kind, i.text, i.reg, i.writes, i.slot_ok = JSR, f"jsr @r{n}", n, set(), False
                return i
            if m == 1:
                _simple(i, f"tas.b @r{n}", f"op_tas(c, {n});")
                return i
            if m == 2:
                i.kind, i.text, i.reg, i.writes, i.slot_ok = JMP, f"jmp @r{n}", n, set(), False
                return i
            return ill()
        if low == 0xc:
            _simple(i, f"shad r{m},r{n}", f"op_shad(c, {n}, {m});", [n])
            return i
        if low == 0xd:
            _simple(i, f"shld r{m},r{n}", f"op_shld(c, {n}, {m});", [n])
            return i
        if low == 0xe:
            if op & 0x80:
                b = (op >> 4) & 7
                _simple(i, f"ldc r{n},r{b}_bank", f"c->r_bank[{b}] = {R(n)};")
                return i
            dst = {0: ("sr", f"rt_set_sr(c, {R(n)});"), 1: ("gbr", f"c->gbr = {R(n)};"),
                   2: ("vbr", f"c->vbr = {R(n)};"), 3: ("ssr", f"c->ssr = {R(n)};"),
                   4: ("spc", f"c->spc = {R(n)};")}.get(m)
            if dst is None:
                return ill()
            _simple(i, f"ldc r{n},{dst[0]}", dst[1])
            if m == 0:
                i.writes = "all"
                i.slot_ok = False
            return i
        if low == 0xf:
            _simple(i, f"mac.w @r{m}+,@r{n}+", f"op_macw(c, {n}, {m});", [n, m])
            return i
        return ill()

    if top == 0x5:
        d = (op & 15) << 2
        _simple(i, f"mov.l @({d},r{m}),r{n}", f"{R(n)} = mem_r32(c, {R(m)} + {d}u);", [n])
        return i

    if top == 0x6:
        t = {0x0: (f"mov.b @r{m},r{n}", f"{R(n)} = (u32)(s32)(s8)mem_r8(c, {R(m)});", [n]),
             0x1: (f"mov.w @r{m},r{n}", f"{R(n)} = (u32)(s32)(s16)mem_r16(c, {R(m)});", [n]),
             0x2: (f"mov.l @r{m},r{n}", f"{R(n)} = mem_r32(c, {R(m)});", [n]),
             0x3: (f"mov r{m},r{n}", f"{R(n)} = {R(m)};", [n]),
             0x7: (f"not r{m},r{n}", f"{R(n)} = ~{R(m)};", [n]),
             0x8: (f"swap.b r{m},r{n}",
                   f"{{ u32 v = {R(m)}; {R(n)} = (v & 0xffff0000u) | ((v & 0xff) << 8) | ((v >> 8) & 0xff); }}",
                   [n]),
             0x9: (f"swap.w r{m},r{n}", f"{{ u32 v = {R(m)}; {R(n)} = (v << 16) | (v >> 16); }}", [n]),
             0xa: (f"negc r{m},r{n}", f"op_negc(c, {n}, {m});", [n]),
             0xb: (f"neg r{m},r{n}", f"{R(n)} = 0 - {R(m)};", [n]),
             0xc: (f"extu.b r{m},r{n}", f"{R(n)} = {R(m)} & 0xff;", [n]),
             0xd: (f"extu.w r{m},r{n}", f"{R(n)} = {R(m)} & 0xffff;", [n]),
             0xe: (f"exts.b r{m},r{n}", f"{R(n)} = (u32)(s32)(s8){R(m)};", [n]),
             0xf: (f"exts.w r{m},r{n}", f"{R(n)} = (u32)(s32)(s16){R(m)};", [n])}.get(low)
        if t is not None:
            _simple(i, t[0], t[1], t[2])
            return i
        if low in (4, 5, 6):
            w, sfx, k, ext = {4: ("8", "b", 1, "(u32)(s32)(s8)"), 5: ("16", "w", 2, "(u32)(s32)(s16)"),
                              6: ("32", "l", 4, "")}[low]
            inc = f" {R(m)} += {k};" if n != m else ""
            _simple(i, f"mov.{sfx} @r{m}+,r{n}", f"{{ u32 v = {ext}mem_r{w}(c, {R(m)});{inc} {R(n)} = v; }}",
                    [n, m])
            return i
        return ill()

    if top == 0x7:
        _simple(i, f"add #{s8(imm8)},r{n}", f"{R(n)} += {h32(s8(imm8))};", [n])
        return i

    if top == 0x8:
        d4 = op & 15
        if n == 0x0:
            _simple(i, f"mov.b r0,@({d4},r{m})", f"mem_w8(c, {R(m)} + {d4}u, {R(0)});")
            return i
        if n == 0x1:
            _simple(i, f"mov.w r0,@({d4 * 2},r{m})", f"mem_w16(c, {R(m)} + {d4 * 2}u, {R(0)});")
            return i
        if n == 0x4:
            _simple(i, f"mov.b @({d4},r{m}),r0", f"{R(0)} = (u32)(s32)(s8)mem_r8(c, {R(m)} + {d4}u);", [0])
            return i
        if n == 0x5:
            _simple(i, f"mov.w @({d4 * 2},r{m}),r0",
                    f"{R(0)} = (u32)(s32)(s16)mem_r16(c, {R(m)} + {d4 * 2}u);", [0])
            return i
        if n == 0x8:
            _simple(i, f"cmp/eq #{s8(imm8)},r0", f"c->t = {R(0)} == {h32(s8(imm8))};")
            return i
        tgt = (addr + 4 + s8(imm8) * 2) & 0xffffffff
        if n in (0x9, 0xb):
            i.kind, i.target, i.writes, i.slot_ok = CBR, tgt, set(), False
            i.text = f"{'bt' if n == 9 else 'bf'} 0x{tgt:08x}"
            return i
        if n in (0xd, 0xf):
            i.kind, i.target, i.writes, i.slot_ok = CBR_S, tgt, set(), False
            i.text = f"{'bt/s' if n == 0xd else 'bf/s'} 0x{tgt:08x}"
            return i
        return ill()

    if top == 0x9:
        a = (addr + 4 + (imm8 << 1)) & 0xffffffff
        i.kind, i.text, i.writes = NORMAL, f"mov.w @(0x{a:08x}),r{n}", {n}
        i.pcload = (n, a, 2)
        i.emit = None   # filled by the translator with the literal's value
        return i

    if top in (0xa, 0xb):
        tgt = (addr + 4 + s12(op & 0xfff) * 2) & 0xffffffff
        i.kind = BRA if top == 0xa else BSR
        i.text = f"{'bra' if top == 0xa else 'bsr'} 0x{tgt:08x}"
        i.target, i.writes, i.slot_ok = tgt, set(), False
        return i

    if top == 0xc:
        g = imm8
        t = {0x0: (f"mov.b r0,@({g},gbr)", f"mem_w8(c, c->gbr + {g}u, {R(0)});", []),
             0x1: (f"mov.w r0,@({g * 2},gbr)", f"mem_w16(c, c->gbr + {g * 2}u, {R(0)});", []),
             0x2: (f"mov.l r0,@({g * 4},gbr)", f"mem_w32(c, c->gbr + {g * 4}u, {R(0)});", []),
             0x4: (f"mov.b @({g},gbr),r0", f"{R(0)} = (u32)(s32)(s8)mem_r8(c, c->gbr + {g}u);", [0]),
             0x5: (f"mov.w @({g * 2},gbr),r0", f"{R(0)} = (u32)(s32)(s16)mem_r16(c, c->gbr + {g * 2}u);", [0]),
             0x6: (f"mov.l @({g * 4},gbr),r0", f"{R(0)} = mem_r32(c, c->gbr + {g * 4}u);", [0]),
             0x8: (f"tst #{g},r0", f"c->t = ({R(0)} & {g}u) == 0;", []),
             0x9: (f"and #{g},r0", f"{R(0)} &= {g}u;", [0]),
             0xa: (f"xor #{g},r0", f"{R(0)} ^= {g}u;", [0]),
             0xb: (f"or #{g},r0", f"{R(0)} |= {g}u;", [0]),
             0xc: (f"tst.b #{g},@(r0,gbr)", f"c->t = (mem_r8(c, c->gbr + {R(0)}) & {g}u) == 0;", []),
             0xd: (f"and.b #{g},@(r0,gbr)",
                   f"{{ u32 a = c->gbr + {R(0)}; mem_w8(c, a, mem_r8(c, a) & {g}u); }}", []),
             0xe: (f"xor.b #{g},@(r0,gbr)",
                   f"{{ u32 a = c->gbr + {R(0)}; mem_w8(c, a, mem_r8(c, a) ^ {g}u); }}", []),
             0xf: (f"or.b #{g},@(r0,gbr)",
                   f"{{ u32 a = c->gbr + {R(0)}; mem_w8(c, a, mem_r8(c, a) | {g}u); }}", [])}.get(n)
        if t is not None:
            _simple(i, t[0], t[1], t[2])
            return i
        if n == 0x3:
            i.kind, i.text, i.writes = TRAPA, f"trapa #{g}", "all"
            i.emit = lambda sz, pr: [f"rt_trapa(c, {g}u, {at});"]
            i.slot_ok = False
            return i
        if n == 0x7:
            a = ((addr + 4) & ~3) + (g << 2)
            _simple(i, f"mova @(0x{a:08x}),r0", f"{R(0)} = {h32(a)};", [0])
            i.pcload = (0, a, 0)      # size 0: the address itself
            return i
        return ill()

    if top == 0xd:
        a = (((addr + 4) & ~3) + (imm8 << 2)) & 0xffffffff
        i.kind, i.text, i.writes = NORMAL, f"mov.l @(0x{a:08x}),r{n}", {n}
        i.pcload = (n, a, 4)
        i.emit = None
        return i

    if top == 0xe:
        _simple(i, f"mov #{s8(imm8)},r{n}", f"{R(n)} = {h32(s8(imm8))};", [n])
        return i

    return _fpu(i, op, n, m, at, ill)


def _fmov_load(n, addr_expr, post_inc_reg=None):
    def fn(sz):
        if sz:
            out = [f"{{ u32 a = {addr_expr}; fpreg *d = FPAIR(c, {n}); d[0].u = mem_r32(c, a); "
                   f"d[1].u = mem_r32(c, a + 4);" + (f" {R(post_inc_reg)} = a + 8;" if post_inc_reg is not None
                                                     else "") + " }"]
        else:
            out = [f"{{ u32 a = {addr_expr}; {FR(n)}.u = mem_r32(c, a);" +
                   (f" {R(post_inc_reg)} = a + 4;" if post_inc_reg is not None else "") + " }"]
        return out
    return fn


def _fmov_store(m, addr_expr):
    def fn(sz):
        if sz:
            return [f"{{ u32 a = {addr_expr}; fpreg *s = FPAIR(c, {m}); mem_w32(c, a, s[0].u); "
                    f"mem_w32(c, a + 4, s[1].u); }}"]
        return [f"mem_w32(c, {addr_expr}, {FR(m)}.u);"]
    return fn


def _fpu(i, op, n, m, at, ill):
    low = op & 15
    arith = {0: ("fadd", "FA_ADD"), 1: ("fsub", "FA_SUB"), 2: ("fmul", "FA_MUL"), 3: ("fdiv", "FA_DIV")}
    if low in arith:
        name, kind = arith[low]
        _fpu_mode(i, "pr", f"{name} fr{m},fr{n}",
                  lambda pr: [f"op_farith(c, {n}, {m}, {kind}, {pr}, {at});"])
        return i
    if low in (4, 5):
        name = "fcmp/eq" if low == 4 else "fcmp/gt"
        _fpu_mode(i, "pr", f"{name} fr{m},fr{n}",
                  lambda pr: [f"op_fcmp(c, {n}, {m}, {low - 4}, {pr}, {at});"])
        return i
    if low == 6:
        _fpu_mode(i, "sz", f"fmov @(r0,r{m}),fr{n}", _fmov_load(n, f"{R(0)} + {R(m)}"))
        return i
    if low == 7:
        _fpu_mode(i, "sz", f"fmov fr{m},@(r0,r{n})", _fmov_store(m, f"{R(0)} + {R(n)}"))
        return i
    if low == 8:
        _fpu_mode(i, "sz", f"fmov @r{m},fr{n}", _fmov_load(n, R(m)))
        return i
    if low == 9:
        _fpu_mode(i, "sz", f"fmov @r{m}+,fr{n}", _fmov_load(n, R(m), post_inc_reg=m), [m])
        return i
    if low == 0xa:
        _fpu_mode(i, "sz", f"fmov fr{m},@r{n}", _fmov_store(m, R(n)))
        return i
    if low == 0xb:
        def fn(sz):
            if sz:
                return [f"{{ u32 a = {R(n)} - 8; fpreg *s = FPAIR(c, {m}); mem_w32(c, a, s[0].u); "
                        f"mem_w32(c, a + 4, s[1].u); {R(n)} = a; }}"]
            return [f"{{ u32 a = {R(n)} - 4; mem_w32(c, a, {FR(m)}.u); {R(n)} = a; }}"]
        _fpu_mode(i, "sz", f"fmov fr{m},@-r{n}", fn, [n])
        return i
    if low == 0xc:
        def fn(sz):
            if sz:
                return [f"{{ fpreg *s = FPAIR(c, {m}), *d = FPAIR(c, {n}); u32 a = s[0].u, b = s[1].u; "
                        f"d[0].u = a; d[1].u = b; }}"]
            return [f"{FR(n)}.u = {FR(m)}.u;"]
        _fpu_mode(i, "sz", f"fmov fr{m},fr{n}", fn)
        return i
    if low == 0xe:
        _fpu_mode(i, "pr", f"fmac fr0,fr{m},fr{n}",
                  lambda pr: [f"rt_illegal(c, 0x{op:04x}, {at}); return;"] if pr else
                  [f"op_fmac(c, {n}, {m}, {at});"])
        return i
    if low != 0xd:
        return ill()

    if m == 0:
        _simple(i, f"fsts fpul,fr{n}", f"{FR(n)}.u = c->fpul;")
        return i
    if m == 1:
        _simple(i, f"flds fr{n},fpul", f"c->fpul = {FR(n)}.u;")
        return i
    if m == 2:
        _fpu_mode(i, "pr", f"float fpul,fr{n}", lambda pr: [f"op_float(c, {n}, {pr}, {at});"])
        return i
    if m == 3:
        _fpu_mode(i, "pr", f"ftrc fr{n},fpul", lambda pr: [f"op_ftrc(c, {n}, {pr}, {at});"])
        return i
    if m in (4, 5):
        name, expr = ("fneg", "^= 0x80000000u") if m == 4 else ("fabs", "&= 0x7fffffffu")
        _fpu_mode(i, "pr", f"{name} fr{n}", lambda pr: [f"{FR((n & 14) if pr else n)}.u {expr};"])
        return i
    if m == 6:
        _fpu_mode(i, "pr", f"fsqrt fr{n}", lambda pr: [f"op_farith(c, {n}, 0, FA_SQRT, {pr}, {at});"])
        return i

    def pr_illegal(text, code):
        _fpu_mode(i, "pr", text,
                  lambda pr: [f"rt_illegal(c, 0x{op:04x}, {at}); return;"] if pr else [code])
        return i

    if m == 7:
        return pr_illegal(f"fsrra fr{n}", f"op_fsrra(c, {n}, {at});")
    if m == 8:
        return pr_illegal(f"fldi0 fr{n}", f"{FR(n)}.u = 0;")
    if m == 9:
        return pr_illegal(f"fldi1 fr{n}", f"{FR(n)}.u = 0x3f800000u;")
    if m == 0xa:
        _simple(i, f"fcnvsd fpul,dr{n}", f"rt_op_fcnvsd(c, {n}, {at});")
        return i
    if m == 0xb:
        _simple(i, f"fcnvds dr{n},fpul", f"rt_op_fcnvds(c, {n}, {at});")
        return i
    if m == 0xe:
        return pr_illegal(f"fipr fv{(n & 3) << 2},fv{n & 12}", f"op_fipr(c, {n}, {at});")
    if m == 0xf:
        if (n & 3) == 1:
            return pr_illegal(f"ftrv xmtrx,fv{n & 12}", f"op_ftrv(c, {n}, {at});")
        if (n & 1) == 0:
            return pr_illegal(f"fsca fpul,dr{n}", f"op_fsca(c, {n}, {at});")
        if n == 3:
            _simple(i, "fschg", "c->fpscr ^= FPSCR_SZ;")
            i.sz_flip = True
            return i
        if n == 11:
            _simple(i, "frchg", "rt_set_fpscr(c, c->fpscr ^ FPSCR_FR);")
            return i
    return ill()
