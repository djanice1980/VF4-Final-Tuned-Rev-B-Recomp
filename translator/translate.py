#!/usr/bin/env python3
"""Translate one of the game's SH-4 programs into C.

usage:
  python3 -I translator/translate.py --program main --image work/main.bin --out work/gen/main
  python3 -I translator/translate.py --program test --image work/test.bin --out work/gen/test

Reads meta/<program>/ (routine entries and names; for main also the FPU
modes the decompilation settled) and the program image, and writes:
  <out>/<prefix>_funcs.h      a declaration per translated routine
  <out>/<prefix>_NN.c         the routines, about 120 to a file
  <out>/<prefix>_dispatch.c   address -> routine table, sorted, with names
  <out>/report.txt            what the translation found (read it)
  <out>/report.json           the same, for tools

Every routine becomes one C function `void <prefix>_<name>(sh4 *restrict c)`
that runs it from its entry until its rts, with SH-4 state in *c.
"""
import argparse
import json
import os
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sh4dec as D  # noqa: E402

BASE = 0x0C020000
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNK = None


class Program:
    def __init__(self, name, image, meta_dir):
        self.name = name
        self.prefix = {"main": "M", "test": "T"}.get(name, name)
        self.data = image
        self.end = BASE + len(image)
        self.routines = {}          # entry -> name
        for row in read_tsv(os.path.join(meta_dir, "routines.tsv")):
            self.routines[int(row["address"], 16)] = row["name"]
        self.discovered = set()     # entries found as call targets, not in the list
        self.index()
        self.fpu_meta = {}          # addr -> {'sz': 1} / {'pr': 1}
        p = os.path.join(meta_dir, "fpu_context.tsv")
        if os.path.exists(p):
            for row in read_tsv(p):
                mt = re.search(r"\((SZ|PR)=([01])\)", row["detail"])
                if not mt or mt.group(1).lower() != row["field"]:
                    sys.exit(f"fpu_context.tsv: cannot read the mode of {row}")
                self.fpu_meta.setdefault(int(row["address"], 16), {})[row["field"]] = int(mt.group(2))
        self.decoded = {}
        self.written = set()        # image bytes stores with known addresses write (analyze_all)
        self.aliased = set()        # calls and jumps to the program through P1/P2 (left to run time)
        self.hints = read_hints(os.path.join(ROOT, "translator", "hints.tsv"), name, self.routines)

    def index(self):
        self.entries = sorted(self.routines)
        self.next_entry = {}
        for i, a in enumerate(self.entries):
            self.next_entry[a] = self.entries[i + 1] if i + 1 < len(self.entries) else self.end

    def discover(self, addrs):
        for a in addrs:
            self.routines[a] = f"sub_{a:08x}"
            self.discovered.add(a)
        self.index()

    def in_image(self, a):
        return BASE <= a < self.end

    def u16(self, a):
        o = a - BASE
        return struct.unpack_from("<H", self.data, o)[0]

    def u32(self, a):
        o = a - BASE
        return struct.unpack_from("<I", self.data, o)[0]

    def insn(self, a):
        i = self.decoded.get(a)
        if i is None:
            i = D.decode(self.u16(a), a)
            self.decoded[a] = i
        return i

    def literal(self, a, size):
        if not self.in_image(a) or not self.in_image(a + max(size, 1) - 1):
            return None
        if size in (2, 4) and any(b in self.written for b in range(a, a + size)):
            return None             # a pool word the program writes: read it from memory
        if size == 4:
            return self.u32(a)
        if size == 2:
            v = self.u16(a)
            return (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
        return a


def read_tsv(path):
    with open(path) as f:
        head = f.readline().rstrip("\n").split("\t")
        return [dict(zip(head, ln.rstrip("\n").split("\t"))) for ln in f if ln.strip()]


# ------------------------------------------------------------------ flow

class Func:
    def __init__(self, prog, entry, name, secondary=False):
        self.prog = prog
        self.entry = entry
        self.name = name
        self.secondary = secondary
        self.home_end = prog.next_entry.get(entry, entry + 2)
        self.insns = {}             # addr -> Insn, primary positions
        self.slots = {}             # branch addr -> slot Insn
        self.succ = {}              # addr -> list of successor addrs (flow inside this function)
        self.calls = {}             # addr -> static call target or None
        self.jumps = {}             # addr -> list of targets / None (unresolved)
        self.tails = set()          # entries jumped/fallen into
        self.outside = set()        # instructions followed past the home extent
        self.illegal = []           # illegal instructions reached
        self.bad_slots = []
        self.has_return = False
        self.preds = None
        self.consts = {}


def canon(a):
    """The address code at a runs from: P1 (0x8...) and P2 (0xa...) are
    windows on the same physical memory as P0, so a jump to 0xac020010 runs
    the code at 0x0c020010 (with the cache bypassed, which nothing here
    models)."""
    if (a >> 29) in (4, 5):
        return a & 0x1FFFFFFF
    return a


def is_entry(prog, a, f):
    return a != f.entry and a in prog.routines


def const_prop(prog, f):
    """Forward constant propagation over f's instructions: register values
    before each instruction (dict reg -> value)."""
    state_in = {f.entry: {}}
    work = [f.entry]
    order = []
    while work:
        a = work.pop()
        st = dict(state_in[a])
        i = f.insns[a]
        order.append(a)
        out = transfer_const(prog, i, st)
        if i.kind in D.DELAYED:
            out = transfer_const(prog, f.slots[a], out)
        if i.kind in (D.BSR, D.JSR, D.BSRF, D.SLEEP, D.TRAPA):
            out = {}
        for s in f.succ.get(a, ()):
            if s not in f.insns:
                continue
            if s not in state_in:
                state_in[s] = out
                work.append(s)
            else:
                old = state_in[s]
                new = {k: v for k, v in old.items() if out.get(k) == v}
                if new != old:
                    state_in[s] = new
                    work.append(s)
    return state_in


def transfer_const(prog, i, st):
    st = dict(st)
    op = i.op
    n = (op >> 8) & 15
    m = (op >> 4) & 15
    top = op >> 12
    if i.pcload is not None:
        reg, a, size = i.pcload
        v = prog.literal(a, size)
        if v is None:
            st.pop(reg, None)
        else:
            st[reg] = v
        return st
    if top == 0xE:
        st[n] = (op & 0xFF) - 0x100 if op & 0x80 else op & 0xFF
        st[n] &= 0xFFFFFFFF
        return st
    if top == 0x6 and (op & 15) == 3:
        if m in st:
            st[n] = st[m]
        else:
            st.pop(n, None)
        return st
    if top == 0x7:
        if n in st:
            imm = (op & 0xFF) - 0x100 if op & 0x80 else op & 0xFF
            st[n] = (st[n] + imm) & 0xFFFFFFFF
        return st
    if top == 0x3 and (op & 15) == 0xC:       # add rm,rn
        if n in st and m in st:
            st[n] = (st[n] + st[m]) & 0xFFFFFFFF
        else:
            st.pop(n, None)
        return st
    if top == 0x4 and (op & 15) in (8, 9) and m in (0, 1, 2):
        k = (2, 8, 16)[m]
        if n in st:
            st[n] = (st[n] << k) & 0xFFFFFFFF if (op & 15) == 8 else st[n] >> k
        return st
    if top == 0x2 and (op & 15) in (9, 0xA, 0xB):   # and / xor / or rm,rn
        if n in st and m in st:
            v = (st[n] & st[m], st[n] ^ st[m], st[n] | st[m])[(op & 15) - 9]
            st[n] = v
        else:
            st.pop(n, None)
        return st
    if (op >> 8) in (0xC9, 0xCA, 0xCB):        # and / xor / or #imm,r0
        if 0 in st:
            g = op & 0xFF
            st[0] = (st[0] & g, st[0] ^ g, st[0] | g)[(op >> 8) - 0xC9]
        return st
    if top == 0x6 and (op & 15) == 9:         # swap.w
        if m in st:
            v = st[m]
            st[n] = ((v << 16) | (v >> 16)) & 0xFFFFFFFF
        else:
            st.pop(n, None)
        return st
    w = i.writes
    if w == "all" or w is None:
        return {}
    for r in w:
        st.pop(r, None)
    return st


def build_flow(prog, f, noreturn, hints):
    """Follow f from its entry; then, while that finds more, resolve calls and
    jumps through registers that hold constants, and switch tables, and
    follow again from scratch (a call found not to return drops its
    continuation). What constants decided is checked again every round: a
    path found later can make a register that was constant at a branch no
    longer so, and the decision must then go."""
    calls, jumps, decided = {}, {}, {}
    from_const = set()          # calls / jumps resolved from a constant register
    for _ in range(24):
        reset_flow(f)
        rebuild(prog, f, noreturn, calls, jumps, decided)
        f.consts = const_prop(prog, f)
        changed = False
        for a in list(decided):
            if a not in f.insns:
                continue            # not reached this round: keep, it may be again
            i = f.insns[a]
            t = t_known(f, a)
            now = None if t is None else (i.target if t == (((i.op >> 8) & 15) == 9) else a + 2)
            if now != decided[a]:
                del decided[a]
                changed = True
        for a, i in f.insns.items():
            if i.kind == D.CBR and a not in decided:
                t = t_known(f, a)
                if t is not None:
                    decided[a] = i.target if t == (((i.op >> 8) & 15) == 9) else a + 2
                    changed = True
        for a, i in list(f.insns.items()):
            if i.kind not in (D.JSR, D.JMP, D.BRAF, D.BSRF):
                continue
            st = f.consts.get(a, {})
            target = None
            if i.reg in st:
                v = st[i.reg]
                raw = (a + 4 + v) & 0xFFFFFFFF if i.kind in (D.BRAF, D.BSRF) else v
                t = canon(raw)
                if prog.in_image(t) and t != raw:
                    # through the P1/P2 alias (the cache routines run uncached): the
                    # code there sees its own PC in that region (PR, mova), so it runs
                    # on the interpreter at its real address: rt_call / rt_jump
                    prog.aliased.add(a)
                elif prog.in_image(t):
                    target = t
                # else outside the program: rt_call / rt_jump at run time
            if target is not None:
                from_const.add(a)
                if i.kind in (D.JSR, D.BSRF):
                    if calls.get(a) != target:
                        calls[a] = target
                        changed = True
                elif jumps.get(a) != [target]:
                    jumps[a] = [target]
                    changed = True
                continue
            if a in from_const:
                from_const.discard(a)
                calls.pop(a, None)
                jumps.pop(a, None)
                changed = True
            if i.kind in (D.BRAF, D.JMP) and a not in jumps:
                tg = hints.get(a)
                if isinstance(tg, tuple):
                    tg = [prog.u32(tg[1] + 4 * n) for n in range(tg[2])]
                tg = tg or switch_table(prog, f, a, i)
                if tg:
                    jumps[a] = tg
                    changed = True
        if not changed:
            return
    sys.exit(f"{f.name}: flow does not settle")


def reset_flow(f):
    f.insns.clear(); f.slots.clear(); f.succ.clear(); f.outside.clear(); f.calls.clear()
    f.jumps.clear(); f.tails.clear(); f.illegal.clear(); f.bad_slots.clear(); f.has_return = False
    f.preds = None


T_WRITERS = re.compile(r"(cmp|tst|clrt|sett|div|addc|subc|negc|addv|subv|rotc?[lr] |shll |shlr |shal |shar |dt |tas|fcmp|ldc.*,sr$)")


def t_known(f, a):
    """T at the conditional branch a when its only way in is the decided
    way out of another conditional branch (and nothing between sets T):
    `bf/s x; slot; bt y` never falls out of the bt."""
    if f.preds is None:
        f.preds = defaultdict(list)
        for x, ss in f.succ.items():
            for y in ss:
                f.preds[y].append(x)
    ps = f.preds.get(a, [])
    if len(ps) != 1:
        return None
    p = ps[0]
    i = f.insns[p]
    if i.kind == D.CBR_S:
        if T_WRITERS.match(f.slots[p].text):
            return None
        fall = p + 4
    elif i.kind == D.CBR:
        fall = p + 2
    else:
        return None
    is_bt = not ((i.op >> 8) & 2)
    if a == i.target and a != fall:
        return 1 if is_bt else 0
    if a == fall and a != i.target:
        return 0 if is_bt else 1
    return None


def rebuild(prog, f, noreturn, calls, jumps, decided):
    work = [f.entry]
    seen = set()
    while work:
        a = work.pop()
        if a in seen:
            continue
        if is_entry(prog, a, f):
            f.tails.add(a)
            continue
        if not prog.in_image(a) or (a & 1):
            f.illegal.append((a, "outside the image"))
            continue
        seen.add(a)
        i = prog.insn(a)
        f.insns[a] = i
        if not (f.entry <= a < f.home_end):
            f.outside.add(a)
        k = i.kind
        if k in D.DELAYED:
            s = D.decode(prog.u16(a + 2), a + 2)
            if s.kind in D.CONTROL or not s.slot_ok:
                f.bad_slots.append(a + 2)
            f.slots[a] = s
        if k == D.NORMAL:
            succ = [a + 2]
        elif k == D.CBR:
            succ = [decided[a]] if a in decided else [a + 2, i.target]
        elif k == D.CBR_S:
            succ = [i.target, a + 4]
        elif k == D.BRA:
            succ = [i.target]
        elif k in (D.BSR, D.JSR, D.BSRF):
            tgt = i.target if k == D.BSR else calls.get(a)
            f.calls[a] = tgt
            succ = [] if (tgt is not None and tgt in noreturn) else [a + 4]
        elif k in (D.BRAF, D.JMP):
            t = jumps.get(a)
            f.jumps[a] = t
            succ = list(t) if t else []
        elif k in (D.RTS, D.RTE):
            f.has_return = True
            succ = []
        elif k in (D.SLEEP, D.TRAPA):
            succ = [a + 2]
        else:
            f.illegal.append((a, f"illegal word 0x{i.op:04x}"))
            succ = []
        f.succ[a] = succ
        work.extend(succ)


def back_paths(f, a, limit, most=64):
    """The ways into a, each a list of the instructions that run before it,
    nearest first (a delayed branch's slot listed before the branch), at most
    `limit` long; forks where a has several ways in."""
    if f.preds is None:
        f.preds = defaultdict(list)
        for x, ss in f.succ.items():
            for y in ss:
                f.preds[y].append(x)
    done = []
    work = [(a, [])]
    while work:
        at, path = work.pop()
        ps = f.preds.get(at, [])
        if len(path) >= limit or not ps or len(done) + len(work) >= most:
            done.append(path)
            continue
        for p in ps:
            i = f.insns[p]
            ext = [(p + 2, f.slots[p])] if i.kind in D.DELAYED else []
            ext.append((p, i))
            work.append((p, path + ext))
    return done


def switch_table(prog, f, ja, i):
    """Targets of a jump through GCC's case table, or into a ladder of bra:

        cmp/hi rMax,rI   (or cmp/hs)       mov ...,r0
        bt default                          and #mask,r0
        mova table,r0                       shll2 r0
        [add rI,rI]                         braf r0
        mov.b|mov.w @(r0,rI),rB             nop
        [extu.b rB,rB]
        braf rB
        nop
      table:  offsets from braf+4, one per case

    Every way into the jump must show its bound (or a constant index); the
    targets are those of all the ways. None when the code has another shape."""
    if i.kind != D.BRAF:
        return None
    out = set()
    for path in back_paths(f, ja, 24):
        t = switch_path(prog, f, ja, i.reg, path)
        if not t:
            return None
        out.update(t)
    return sorted(out) if out else None


def switch_path(prog, f, ja, rb, prev):
    ops = [(a, x.op) for a, x in prev]
    # the ladder: and #mask,r0 ; shll2 r0 ; braf r0
    if len(ops) >= 2 and rb == 0 and ops[0][1] == 0x4008 and (ops[1][1] >> 8) == 0xC9:
        mask = ops[1][1] & 0xFF
        return [ja + 4 + 4 * k for k in range(mask + 1)]
    k = 0
    unsigned = False
    if k < len(ops) and ops[k][1] in (0x600C | (rb << 8) | (rb << 4), 0x600D | (rb << 8) | (rb << 4)):
        unsigned = True        # extu.b / extu.w rB,rB
        k += 1
    if k >= len(ops):
        return None
    la, lop = ops[k]
    if (lop >> 12) != 0 or (lop & 0xF) not in (0xC, 0xD) or ((lop >> 8) & 15) != rb:
        return None
    size = 1 if (lop & 0xF) == 0xC else 2
    ri = (lop >> 4) & 15
    table = f.consts.get(la, {}).get(0)
    if table is None:
        return None
    k += 1
    if size == 2:
        if k < len(ops) and ops[k][1] == (0x300C | (ri << 8) | (ri << 4)):
            k += 1             # add rI,rI
        else:
            return None
    fixed = f.consts.get(la, {}).get(ri)
    if fixed is not None:      # a switch on a constant
        first, count = (fixed // size if size == 2 else fixed), 1
    else:
        first = 0
        count = bound_on(f, prev[k:], ri)
        if count is None or not 0 < count <= 512:
            return None
    out = []
    for n in range(first, first + count):
        e = table + n * size
        if not prog.in_image(e + size - 1):
            return None
        v = prog.data[e - BASE] if size == 1 else prog.u16(e)
        if not unsigned:
            v = v - (0x100 if size == 1 else 0x10000) if v >> (8 * size - 1) else v
        t = ja + 4 + v
        if t & 1 or not prog.in_image(t):
            return None
        out.append(t)
    return out


def bound_on(f, prev, ri):
    """Number of cases the last cmp/hi (or cmp/hs) before the table lets
    through: run forward over the way in, following copies of the index, and
    values known from a cmp/eq #imm,r0 the way in passed."""
    path = list(reversed(prev))
    ids = {r: ("in", r) for r in range(16)}
    known = {}                  # value id -> constant
    fresh = 0
    count = None
    eq = None                   # (id, imm) of the last cmp/eq #imm,r0
    for n, (a, x) in enumerate(path):
        op = x.op
        if (op >> 8) == 0x88:                                   # cmp/eq #imm,r0
            imm = op & 0xFF
            eq = (ids[0], (imm - 0x100 if imm & 0x80 else imm) & 0xFFFFFFFF)
            continue
        if x.kind in (D.CBR, D.CBR_S) and eq is not None:
            nxt = path[n + 1][0] if n + 1 < len(path) else None
            if x.kind == D.CBR_S and nxt == a + 2:
                nxt = path[n + 2][0] if n + 2 < len(path) else None
            taken = nxt == x.target
            on_t = not (op >> 8) & 2                            # bt / bt/s
            if taken == on_t:                                   # T was 1: equal
                known[eq[0]] = eq[1]
            continue
        if (op >> 12) == 3 and (op & 0xF) in (2, 6):
            m = (op >> 4) & 15
            # (a slot's registers are its branch's: no branch writes one)
            cst = f.consts.get(a) if a in f.insns else f.consts.get(a - 2, {})
            bound = cst.get(m, known.get(ids[m]))
            if bound is not None:
                count = (ids[(op >> 8) & 15], bound + 1 if (op & 0xF) == 6 else bound)
            eq = None
            continue
        if (op >> 12) == 6 and (op & 0xF) == 3:                 # mov rm,rn
            ids[(op >> 8) & 15] = ids[(op >> 4) & 15]
            continue
        w = x.writes
        if w == "all" or x.kind in (D.BSR, D.JSR, D.BSRF, D.TRAPA):
            ids = {r: ("w", fresh, r) for r in range(16)}
            fresh += 1
        elif w:
            for r in w:
                ids[r] = ("w", fresh, r)
                fresh += 1
            if 0 in w:
                eq = None
    if count is None or count[0] != ids[ri]:
        return None
    return count[1]


def read_hints(path, program, routines):
    """translator/hints.tsv: jumps the analysis cannot see through, by hand,
    each with its reason. `at` is a routine name plus offset (the same code
    in both programs) or an address; `targets` is ladder*N (N entries of 4
    bytes from the jump's address + 4), @table*N (a table of N 32-bit code
    addresses), or addresses separated by spaces."""
    hints = {}
    byname = {n: a for a, n in routines.items()}
    for row in read_tsv(path):
        if row["program"] not in ("*", program):
            continue
        at = row["at"]
        mt = re.fullmatch(r"(\w+)\+0x([0-9a-f]+)", at)
        if mt:
            if mt.group(1) not in byname:
                continue
            a = byname[mt.group(1)] + int(mt.group(2), 16)
        else:
            a = int(at, 16)
        spec = row["targets"].strip()
        lad = re.fullmatch(r"ladder\*(\d+)", spec)
        tab = re.fullmatch(r"@([0-9a-f]+)\*(\d+)", spec)
        if lad:
            hints[a] = [a + 4 + 4 * k for k in range(int(lad.group(1)))]
        elif tab:
            hints[a] = ("table", int(tab.group(1), 16), int(tab.group(2)))
        else:
            hints[a] = [int(x, 16) for x in spec.split()]
    return hints


# ------------------------------------------------------------------ FPU modes
#
# A mode is (sz, pr). Each field is 0, 1, UNK, or -- while a routine's
# summary is worked out -- "e" (the value it had at the routine's entry) or
# "n" (its opposite; fschg flips SZ). A summary says what a routine leaves
# in each field in terms of what it found there, so a routine that saves and
# restores FPSCR, or never touches it, gives ("e", "e") whoever calls it.

FLIP = {0: 1, 1: 0, "e": "n", "n": "e", UNK: UNK}


def join(a, b):
    return tuple(x if x == y else UNK for x, y in zip(a, b))


def apply_summary(mode, summ):
    """The mode after a call that found `mode`, by the callee's summary."""
    out = []
    for cur, x in zip(mode, summ):
        if x == "e":
            out.append(cur)
        elif x == "n":
            out.append(FLIP[cur])
        else:
            out.append(x)
    return tuple(out)


def mode_flow(prog, f, entry_mode, summaries):
    """(sz, pr) before each instruction, from entry_mode. summaries: routine ->
    exit summary, or missing while not yet known (then a call's continuation
    is not reached yet). Returns (state_in, exit_mode or None, call_modes)."""
    consts = f.consts
    state_in = {f.entry: (entry_mode, None)}
    work = [f.entry]
    exit_mode = None
    call_modes = []

    def leave(m):
        nonlocal exit_mode
        exit_mode = m if exit_mode is None else join(exit_mode, m)

    while work:
        a = work.pop()
        mode, saved = state_in[a]
        i = f.insns[a]
        mode, saved = mode_step(prog, i, mode, saved, consts.get(a, {}))
        if i.kind in D.DELAYED:
            mode, saved = mode_step(prog, f.slots[a], mode, saved, consts.get(a, {}))
        succ = f.succ.get(a, ())
        if i.kind in (D.BSR, D.JSR, D.BSRF):
            tgt = f.calls.get(a)
            if tgt is not None and tgt in prog.routines:
                call_modes.append((tgt, mode))
                summ = summaries.get(tgt)
                if summ is None:
                    succ = ()               # not known to return yet
                else:
                    mode = apply_summary(mode, summ)
            # an indirect call, or one into the middle of a routine: taken to
            # keep the mode (GCC code does); checked at run time in test builds
        if i.kind in (D.RTS, D.RTE):
            leave(mode)
        for s in succ:
            if s not in f.insns:
                if s in prog.routines:      # a tail call, or running into the next routine
                    call_modes.append((s, mode))
                    summ = summaries.get(s)
                    if summ is not None:
                        leave(apply_summary(mode, summ))
                continue
            new = (mode, saved)
            old = state_in.get(s)
            if old is None:
                state_in[s] = new
                work.append(s)
            else:
                merged = (join(old[0], new[0]),
                          old[1] if old[1] == new[1] else UNK)
                if merged != old:
                    state_in[s] = merged
                    work.append(s)
    return state_in, exit_mode, call_modes


def mode_step(prog, i, mode, saved, consts):
    sz, pr = mode
    if i.sz_flip:
        sz = FLIP[sz]
    if i.fpscr_save:
        saved = (sz, pr)
    if i.fpscr_write is not None:
        how, reg = i.fpscr_write
        if how == "pop" and reg == 15 and saved is not None:
            sz, pr = saved
        elif how == "reg" and reg in consts:
            v = consts[reg]
            sz, pr = (v >> 20) & 1, (v >> 19) & 1
        elif how == "pop" and reg in consts and prog.literal(consts[reg], 4) is not None:
            v = prog.literal(consts[reg], 4)        # lds.l @rN+,fpscr from the program's own constant
            sz, pr = (v >> 20) & 1, (v >> 19) & 1
        else:
            sz = pr = UNK
    return (sz, pr), saved


def solve_summaries(prog, funcs):
    """Each routine's exit mode in terms of its entry mode, to a fixed point.
    Starts from 'never returns' and only grows, so it ends."""
    summaries = {}
    for _ in range(2):
        for _ in range(60):
            changed = False
            for a, f in funcs.items():
                _, ex, _ = mode_flow(prog, f, ("e", "e"), summaries)
                if ex is not None:
                    old = summaries.get(a)
                    new = ex if old is None else join(old, ex)
                    if new != old:
                        summaries[a] = new
                        changed = True
            if not changed:
                break
        # a routine still without a summary either never returns (its
        # callers' continuations stay unreached) or may return through a jump
        # the flow could not follow: say nothing about the latter's exit, and
        # settle again
        missing = [a for a, f in funcs.items()
                   if a not in summaries and any(t is None for t in f.jumps.values())]
        if not missing:
            break
        for a in missing:
            summaries[a] = (UNK, UNK)
    return summaries


def solve_modes(prog, funcs):
    """Entry modes by agreement of call sites, iterated to a fixed point;
    routines nothing calls directly start in (0, 0), GCC's mode."""
    summaries = solve_summaries(prog, funcs)
    called = set()
    for f in funcs.values():
        called.update(t for t in f.calls.values() if t is not None)
        called.update(f.tails)
    entry = {a: (0, 0) for a in funcs if a not in called}
    results = {}
    work = list(entry)
    queued = set(work)
    while work:
        a = work.pop()
        queued.discard(a)
        st, _, cm = mode_flow(prog, funcs[a], entry[a], summaries)
        results[a] = st
        for t, md in cm:
            if t not in funcs:
                continue
            new = md if t not in entry else join(entry[t], md)
            if t not in called:
                new = join(new, (0, 0))
            if entry.get(t) != new:
                entry[t] = new
                if t not in queued:
                    queued.add(t)
                    work.append(t)
    for a in funcs:
        if a not in entry:                  # reached only from unreached code
            entry[a] = (0, 0)
            results[a] = mode_flow(prog, funcs[a], (0, 0), summaries)[0]
    return entry, results, summaries


# ------------------------------------------------------------------ emission

def cname(prog, a):
    n = prog.routines.get(a)
    if n is None:
        return f"{prog.prefix}_{a:08x}"
    return f"{prog.prefix}_{n}"


def emit_insn(prog, i, mode):
    sz, pr = mode
    if i.pcload is not None and i.emit is None:
        reg, a, size = i.pcload
        v = prog.literal(a, size)
        if v is None:
            rd = f"rt_r32(c, ATK, 0x{a:08x}u)" if size == 4 else f"(u32)(s32)(s16)rt_r16(c, ATK, 0x{a:08x}u)"
            return [f"c->r[{reg}] = {rd};"]
        return [f"c->r[{reg}] = 0x{v:08x}u;"]
    return i.emit(sz, pr)


def local_starts(prog, f):
    """Block starts f's own flow asks for: its entry, branch targets, the
    instruction after a conditional branch, a call, a sleep or a trapa, and
    routine entries it holds."""
    starts = {f.entry}
    for a, ss in f.succ.items():
        i = f.insns[a]
        if i.kind != D.NORMAL:
            starts.update(s for s in ss if s in f.insns)
            if i.kind in (D.CBR, D.SLEEP, D.TRAPA) and a + 2 in f.insns:
                starts.add(a + 2)
    for a, ss in f.succ.items():
        for s in ss:
            if s in f.insns and s != a + 2:
                starts.add(s)
    addrs = sorted(f.insns)
    for idx, a in enumerate(addrs):
        if idx == 0 or addrs[idx - 1] != a - 2:
            starts.add(a)
        if a in prog.routines:
            starts.add(a)
    return starts


def block_len(f, a, starts):
    """Instructions in the block at a (a delay slot counts as one)."""
    n = 0
    b = a
    while b in f.insns:
        bi = f.insns[b]
        n += 1
        if bi.kind in D.DELAYED:
            n += 1
        if bi.kind != D.NORMAL:
            break
        b += 2
        if b in starts:
            break
    return n


def global_blocks(prog, funcs):
    """Block starts as a property of the address (the same in every routine
    that holds the code) and the length of each block; a length two routines
    disagree on is an error of the translator."""
    starts = set()
    for f in funcs.values():
        starts |= local_starts(prog, f)
    lens = {}
    for f in funcs.values():
        for a in f.insns:
            if a not in starts:
                continue
            n = block_len(f, a, starts)
            if lens.setdefault(a, n) != n:
                sys.exit(f"translate: the block at {a:08x} is {lens[a]} instructions long in one routine and "
                         f"{n} in {cname(prog, f.entry)}")
    return starts, lens


RAM_LO, RAM_HI = 0x0C000000, 0x0E000000
MEM_RE = re.compile(r"@(?:\(([^)]*)\)|(-?r\d+\+?))")
SYNC_RE = re.compile(r"\brt_[rw](?:8|16|32)\(|\bop_(?:macl|macw|tas|pref)\(")


def surely_ram(i, consts):
    """True when every memory operand of i is the stack, a literal of the
    program, or an address constant propagation knows to be in RAM: such an
    access cannot reach a device, so nothing can change under it."""
    for m in MEM_RE.finditer(i.text):
        inner = m.group(1) if m.group(1) is not None else m.group(2)
        if re.fullmatch(r"0x[0-9a-f]+", inner):
            continue                                    # pc-relative: the program's own literal
        if "gbr" in inner or "pc" in inner:
            return False
        regs = [int(x) for x in re.findall(r"\br(\d+)\b", inner)]
        if regs and regs[-1] == 15:
            continue                                    # the stack ((r0,r15) included)
        disp = re.match(r"\s*(-?\d+)\s*,", inner)
        a = int(disp.group(1)) if disp else 0
        for r in regs:
            if r not in consts:
                return False
            a += consts[r]
        if not RAM_LO <= (a & 0x1FFFFFFF) < RAM_HI:
            return False
    return True


def atk(lines, at, k):
    return [ln.replace("ATK", f"RT_ATK(0x{at:08x}u, {k})") for ln in lines]


def emit_func(prog, f, modes, funcs, noreturn, out, gstarts, blens, lits):
    name = cname(prog, f.entry)
    out.append(f"/* {prog.routines.get(f.entry, 'secondary entry')} at 0x{f.entry:08x}"
               f"{' (shared code followed outside its extent)' if f.outside else ''} */")
    out.append(f"void {name}(sh4 *restrict c)")
    out.append("{")
    addrs = sorted(f.insns)
    starts = {a for a in addrs if a in gstarts}
    entries = {a for a in starts if a in prog.routines}
    labels_used = set()

    def label_of(a, skip_entry):
        return f"L_{a:08x}_n" if a in entries and skip_entry else f"L_{a:08x}"

    def go(target, at, fall=False):
        """C for control reaching target from the instruction at at: by bra,
        jmp, braf or a call, the hooks of an entry there run; falling in, a
        conditional branch or a return do not (the interpreter's X(go))."""
        if target in f.insns:
            lab = label_of(target, fall)
            labels_used.add(lab)
            return f"goto {lab};"
        if target in prog.routines:
            nr = " /* noreturn */" if target in noreturn else ""
            if fall:
                return f"RT_FALL(c, {cname(prog, target)}, 0x{target:08x}u); return;{nr}"
            return f"{cname(prog, target)}(c); return;{nr}"
        return f"rt_jump(c, 0x{target:08x}u, 0x{at:08x}u); return; /* not translated */"

    body = []
    if addrs[0] != f.entry:
        lab = label_of(f.entry, False)
        labels_used.add(lab)
        body.append(("code", f"goto {lab};"))
    n = j = 0
    cur = addrs[0]

    def note_literal(ins):
        # a pool word compiled in as a constant: the block reading it goes
        # to the interpreter if the word changes (the runtime, M_lits)
        if ins.pcload is not None and ins.pcload[2] in (2, 4) and prog.literal(ins.pcload[1], ins.pcload[2]) is not None:
            lits.add((ins.pcload[1] | (ins.pcload[2] == 4), cur))

    for idx, a in enumerate(addrs):
        i = f.insns[a]
        mode = modes.get(a, (UNK, UNK))
        if a in starts:
            if a in entries:
                if body:
                    lab = label_of(a, True)
                    labels_used.add(lab)
                    body.append(("code", f"goto {lab};"))
                body.append(("label", f"L_{a:08x}"))
                body.append(("code", f"RT_ENTRY(c, 0x{a:08x}u);"))
                body.append(("label", f"L_{a:08x}_n"))
            else:
                body.append(("label", f"L_{a:08x}"))
            n = blens[a]
            j = 0
            cur = a
            body.append(("code", f"RT_BLOCK(c, 0x{a:08x}u, {n});  /* {a:08x} */"))
        txt = f"/* {a:08x}: {i.text} */"
        k = i.kind
        note_literal(i)
        if k == D.NORMAL:
            lines = atk(emit_insn(prog, i, mode), a, n - j - 1)
            body.append(("code", lines[0] + "  " + txt))
            for ln in lines[1:]:
                body.append(("code", ln))
            j += 1
            nxt = a + 2
            sequential = nxt in f.insns and idx + 1 < len(addrs) and addrs[idx + 1] == nxt
            joined = " ".join(lines)
            if sequential and nxt not in starts and ("rt_set_sr(" in joined or (
                    SYNC_RE.search(joined) and not surely_ram(i, f.consts.get(a, {})))):
                body.append(("code", f"RT_SYNC(c, 0x{nxt:08x}u, {n - j});"))
            if not sequential:
                body.append(("code", go(nxt, a, fall=True)))
            continue
        slot = f.slots.get(a)
        slot_code = []
        if slot is not None:
            if slot.kind in D.CONTROL or not slot.slot_ok:
                slot_code = [f"rt_illegal(c, 0x{slot.op:04x}, 0x{a + 2:08x}u); return; /* slot: {slot.text} */"]
            else:
                note_literal(slot)
                slot_code = atk(emit_insn(prog, slot, mode), a + 2, n - j - 2)
                slot_code[0] += f"  /* {a + 2:08x}: {slot.text} (slot) */"
        if k == D.CBR:
            cond = "c->t" if (i.op >> 8) & 15 == 9 else "!c->t"
            body.append(("code", f"if ({cond}) {{ {go(i.target, a, fall=True)} }}  {txt}"))
            body.append(("code", go(a + 2, a, fall=True)) if (idx + 1 >= len(addrs) or addrs[idx + 1] != a + 2)
                        else ("code", ""))
        elif k == D.CBR_S:
            cond = "c->t" if (i.op >> 8) & 15 == 0xD else "!c->t"
            body.append(("code", f"{{ int take = {cond};  {txt}"))
            for ln in slot_code:
                body.append(("code", "    " + ln))
            body.append(("code", f"    if (take) {{ {go(i.target, a, fall=True)} }} }}"))
            body.append(("code", go(a + 4, a, fall=True)))
        elif k == D.BRA:
            body.append(("code", f"{{  {txt}"))
            for ln in slot_code:
                body.append(("code", "    " + ln))
            body.append(("code", "    " + go(i.target, a) + " }"))
        elif k in (D.BSR, D.JSR, D.BSRF):
            ret = a + 4
            body.append(("code", "{  " + txt))
            if k == D.JSR:
                body.append(("code", f"    u32 t = c->r[{i.reg}];"))
            elif k == D.BSRF:
                body.append(("code", f"    u32 t = 0x{a + 4:08x}u + c->r[{i.reg}];"))
            body.append(("code", f"    c->pr = 0x{ret:08x}u;"))
            for ln in slot_code:
                body.append(("code", "    " + ln))
            tgt = f.calls.get(a)
            # a target known from a constant register is checked: the code
            # that set the register may have been changed (the runtime's
            # changed code, docs/design.md)
            checked = tgt is not None and k != D.BSR
            if tgt is not None and tgt in prog.routines:
                if checked:
                    body.append(("code", f"    if (RT_UNLIKELY(t != 0x{tgt:08x}u)) rt_call(c, t, 0x{a:08x}u);"))
                    body.append(("code", f"    else {cname(prog, tgt)}(c);"))
                else:
                    body.append(("code", f"    {cname(prog, tgt)}(c);"))
            elif tgt is not None and not checked:
                body.append(("code", f"    rt_call(c, 0x{tgt:08x}u, 0x{a:08x}u); /* not a routine entry */"))
            else:
                body.append(("code", f"    rt_call(c, t, 0x{a:08x}u);"))
            if tgt is not None and tgt in noreturn:
                if checked:
                    body.append(("code", f"    if (RT_UNLIKELY(t != 0x{tgt:08x}u) && c->pc == 0x{ret:08x}u) {{ "
                                         f"rt_continue(c); return; }}"))
                body.append(("code", f"    rt_bad_return(c, 0x{ret:08x}u, 0x{a:08x}u); return; /* noreturn */ }}"))
            else:
                body.append(("code", f"    if (RT_UNLIKELY(c->pc != 0x{ret:08x}u)) {{ rt_bad_return(c, 0x{ret:08x}u, "
                                     f"0x{a:08x}u); return; }} }}"))
                if ret not in f.insns or idx + 1 >= len(addrs) or addrs[idx + 1] != ret:
                    body.append(("code", go(ret, a, fall=True)))
        elif k in (D.BRAF, D.JMP):
            body.append(("code", "{  " + txt))
            if k == D.JMP:
                body.append(("code", f"    u32 t = c->r[{i.reg}];"))
            else:
                body.append(("code", f"    u32 t = 0x{a + 4:08x}u + c->r[{i.reg}];"))
            for ln in slot_code:
                body.append(("code", "    " + ln))
            tg = f.jumps.get(a)
            if tg and len(tg) == 1:
                body.append(("code", f"    if (RT_UNLIKELY(t != 0x{tg[0]:08x}u)) {{ rt_jump(c, t, 0x{a:08x}u); return; }}"))
                body.append(("code", f"    {go(tg[0], a)} }}"))
            elif tg:
                body.append(("code", "    switch (t) {"))
                for t in sorted(set(tg)):
                    body.append(("code", f"    case 0x{t:08x}u: {go(t, a)}"))
                body.append(("code", f"    default: rt_jump(c, t, 0x{a:08x}u); return;"))
                body.append(("code", "    } }"))
            else:
                body.append(("code", f"    rt_jump(c, t, 0x{a:08x}u); return; }}"))
        elif k == D.RTS:
            body.append(("code", f"{{ u32 t = c->pr;  {txt}"))
            for ln in slot_code:
                body.append(("code", "    " + ln))
            body.append(("code", "    c->pc = t; return; }"))
        elif k == D.RTE:
            body.append(("code", f"{{ u32 t = c->spc, s = c->ssr;  {txt}"))
            body.append(("code", "    rt_set_sr(c, s);"))
            for ln in slot_code:
                body.append(("code", "    " + ln))
            body.append(("code", "    c->pc = t; RT_RTE(c); return; }"))
        elif k in (D.SLEEP, D.TRAPA):
            body.append(("code", emit_insn(prog, i, mode)[0] + "  " + txt))
            if a + 2 not in f.insns or idx + 1 >= len(addrs) or addrs[idx + 1] != a + 2:
                body.append(("code", go(a + 2, a, fall=True)))
        else:
            body.append(("code", f"rt_illegal(c, 0x{i.op:04x}, 0x{a:08x}u); return;  {txt}"))
        j += 2 if k in D.DELAYED else 1
    # where the interpreter hands the routine back: any block of its own
    resume = []
    for a in sorted(starts):
        lab = label_of(a, True)
        labels_used.add(lab)
        resume.append(f"    case 0x{a:08x}u: goto {lab};")
    for kind, v in body:
        if kind == "label":
            if v in labels_used:
                out.append(f"{v}:;")
        elif v:
            out.append("    " + v)
    out.append("    return;")
    out.append("rt_slow:")
    out.append("    if (rt_slow(c))")
    out.append("        return;")
    out.append("    switch (c->pc) {")
    out.extend(resume)
    out.append("    default: rt_continue(c); return;")
    out.append("    }")
    out.append("}")
    out.append("")


# ------------------------------------------------------------------ driver

def analyze(prog):
    """Flow, noreturn routines and FPU modes of every routine."""
    noreturn = set()
    for it in range(40):
        funcs = {a: Func(prog, a, prog.routines[a]) for a in prog.entries}
        for f in funcs.values():
            build_flow(prog, f, noreturn, prog.hints)
        # a call to an address no listed routine starts at starts one (the
        # decomp's lists leave out some library routines of the test program)
        new = {t for f in funcs.values() for t in f.calls.values()
               if t is not None and t not in prog.routines and prog.in_image(t) and not t & 1}
        if new:
            prog.discover(new)
            continue
        new_nr = set(noreturn)
        for a, f in funcs.items():
            if a in new_nr:
                continue
            if f.has_return or any(t is None for t in f.jumps.values()):
                continue
            if any(t not in new_nr for t in f.tails):
                continue
            new_nr.add(a)
        if new_nr == noreturn:
            break
        noreturn = new_nr

    entry_modes, mode_in, summaries = solve_modes(prog, funcs)
    return funcs, noreturn, entry_modes, mode_in, summaries


def store_target(i, st):
    """(address, bytes) a store instruction writes when constant propagation
    knows the address (st: register values before it), else None."""
    op = i.op
    n, m, top, lo = (op >> 8) & 15, (op >> 4) & 15, op >> 12, op & 15
    size = {0: 1, 1: 2, 2: 4}
    if top == 0x2 and lo in (0, 1, 2) and n in st:                 # mov.x rm,@rn
        return st[n], size[lo]
    if top == 0x2 and lo in (4, 5, 6) and n in st:                 # mov.x rm,@-rn
        return (st[n] - size[lo - 4]) & 0xFFFFFFFF, size[lo - 4]
    if top == 0x1 and n in st:                                     # mov.l rm,@(d,rn)
        return (st[n] + 4 * lo) & 0xFFFFFFFF, 4
    if (op >> 8) in (0x80, 0x81) and m in st:                      # mov.b/w r0,@(d,rn)
        w = 1 if (op >> 8) == 0x80 else 2
        return (st[m] + w * lo) & 0xFFFFFFFF, w
    if top == 0x0 and lo in (4, 5, 6) and n in st and 0 in st:     # mov.x rm,@(r0,rn)
        return (st[n] + st[0]) & 0xFFFFFFFF, size[lo - 4]
    if top == 0x0 and lo == 3 and m == 0xC and n in st:            # movca.l r0,@rn
        return st[n], 4
    if top == 0x4 and lo in (2, 3) and n in st:                    # sts.l / stc.l ...,@-rn
        return (st[n] - 4) & 0xFFFFFFFF, 4
    if top == 0x4 and (op & 0xFF) == 0x1B and n in st:             # tas.b @rn
        return st[n], 1
    if top == 0xF and lo in (0xA, 0xB, 0x7):                       # fmov stores (8 bytes covers both sizes)
        if lo == 0xA and n in st:
            return st[n], 8
        if lo == 0xB and n in st:
            return (st[n] - 8) & 0xFFFFFFFF, 8
        if lo == 0x7 and n in st and 0 in st:
            return (st[n] + st[0]) & 0xFFFFFFFF, 8
    return None


def written_words(prog, funcs):
    """Bytes of the image that stores with known addresses write."""
    out = set()
    for f in funcs.values():
        for a, i in f.insns.items():
            for ins in (i, f.slots.get(a)):
                if ins is None or ins.kind != D.NORMAL:
                    continue
                t = store_target(ins, f.consts.get(a, {}))
                if t is None:
                    continue
                ta, size = t
                ta = canon(ta)
                if prog.in_image(ta):
                    out.update(range(ta, ta + size))
    return out


def analyze_all(prog):
    """analyze(), again while it finds literal pool words the program writes
    (they must be read from memory, and that can change what is constant)."""
    while True:
        res = analyze(prog)
        w = written_words(prog, res[0])
        if w <= prog.written:
            return res
        prog.written |= w


def translate(prog, out_dir):
    funcs, noreturn, entry_modes, mode_in, summaries = analyze_all(prog)
    report_written = sorted({a & ~3 for a in prog.written})

    # mode per instruction (and per slot, which runs in its branch's mode)
    report = {"program": prog.name, "routines": len(funcs), "noreturn": sorted(cname(prog, a) for a in noreturn)}
    agree = disagree = mine_unknown = 0
    disagreements = []
    unknown_by = defaultdict(int)
    final_modes = {}
    for a, f in funcs.items():
        st = mode_in.get(a, {})
        fm = {}
        for ia, i in f.insns.items():
            m = st.get(ia, ((UNK, UNK), None))[0]
            fm[ia] = m
            for ins, at in ((i, ia), (f.slots.get(ia), ia + 2)):
                if ins is None or ins.mode is None:
                    continue
                field = 0 if ins.mode == "sz" else 1
                mine = m[field]
                meta = prog.fpu_meta.get(at, {}).get(ins.mode)
                if mine is UNK:
                    mine_unknown += 1
                    unknown_by[cname(prog, a)] += 1
                elif meta is not None:
                    if mine == meta:
                        agree += 1
                    else:
                        disagree += 1
                        disagreements.append(f"{at:08x} {ins.text} {ins.mode}: translator {mine}, decomp {meta} "
                                             f"(in {cname(prog, a)})")
        final_modes[a] = fm
    report["fpu_mode"] = {"agree_with_decomp": agree, "disagree": disagree, "unknown_runtime_test": mine_unknown,
                          "disagreements": disagreements[:50],
                          "unknown_by_routine": sorted(unknown_by.items(), key=lambda x: -x[1])[:60]}

    # statistics
    stats = defaultdict(int)
    lists = defaultdict(list)
    for a, f in funcs.items():
        stats["instructions"] += len(f.insns)
        stats["outside_extent"] += len(f.outside)
        for ca, t in f.calls.items():
            if t is None:
                stats["calls_dynamic"] += 1
            elif t not in prog.routines:
                lists["calls_to_non_entries"].append(f"{ca:08x} -> {t:08x} in {cname(prog, a)}")
            else:
                stats["calls_static"] += 1
        for ja, t in f.jumps.items():
            if t is None:
                i = f.insns[ja]
                lists["unresolved_jumps"].append(f"{ja:08x} {i.text} in {cname(prog, a)}")
            else:
                stats["jumps_resolved"] += 1
        for ia, why in f.illegal:
            lists["illegal_reached"].append(f"{ia:08x} {why} in {cname(prog, a)}")
        for sa in f.bad_slots:
            lists["bad_slots"].append(f"{sa:08x} in {cname(prog, a)}")
        if f.outside:
            lists["shared_code"].append(f"{cname(prog, a)}: {len(f.outside)} instructions past its extent")
    lists["discovered_entries"] = [f"{a:08x}" for a in sorted(prog.discovered)]
    lists["written_words"] = [f"{a:08x}" for a in report_written]
    lists["aliased_calls_and_jumps"] = [f"{a:08x}" for a in sorted(prog.aliased)]
    lists["hinted_jumps"] = [f"{a:08x} in {cname(prog, f.entry)}" for f in funcs.values()
                             for a in f.jumps if a in prog.hints]
    report["stats"] = dict(stats)
    report.update({k: v for k, v in lists.items()})

    os.makedirs(out_dir, exist_ok=True)
    pfx = prog.prefix
    hdr = [f"/* Generated by translator/translate.py from the {prog.name} program: do not edit,",
           " * do not commit, do not share (it is made from your copy of the game). */",
           f"#ifndef VF4_{pfx}_FUNCS_H", f"#define VF4_{pfx}_FUNCS_H", '#include "sh4rt.h"']
    for a in prog.entries:
        hdr.append(f"void {cname(prog, a)}(sh4 *restrict c);")
    hdr.append("#endif")
    write(os.path.join(out_dir, f"{pfx}_funcs.h"), "\n".join(hdr) + "\n")

    gstarts, blens = global_blocks(prog, funcs)
    report["blocks"] = len(blens)
    lits = set()
    per_file = 120
    files = []
    for k in range(0, len(prog.entries), per_file):
        out = [f"/* Generated by translator/translate.py ({prog.name}, part {k // per_file}): do not edit. */",
               f'#include "{pfx}_funcs.h"', ""]
        for a in prog.entries[k:k + per_file]:
            emit_func(prog, funcs[a], final_modes[a], funcs, noreturn, out, gstarts, blens, lits)
        name = f"{pfx}_{k // per_file:02d}.c"
        write(os.path.join(out_dir, name), "\n".join(out) + "\n")
        files.append(name)

    disp = [f"/* Generated: routine table of the {prog.name} program. */", f'#include "{pfx}_funcs.h"',
            "#include \"rt_dispatch.h\"", "",
            f"const rt_routine {pfx}_routines[] = {{"]
    for a in prog.entries:
        disp.append(f'    {{ 0x{a:08x}u, {cname(prog, a)}, "{prog.routines[a]}" }},')
    disp.append("};")
    disp.append(f"const unsigned {pfx}_routine_count = {len(prog.entries)};")
    disp.append("")
    disp.append("/* every block start and its length in instructions (the interpreter's side")
    disp.append(" * of the block protocol: where it may hand control back) */")
    disp.append(f"const rt_block {pfx}_blocks[] = {{")
    for a in sorted(blens):
        disp.append(f"    {{ 0x{a:08x}u, {blens[a]} }},")
    disp.append("};")
    disp.append(f"const unsigned {pfx}_block_count = {len(blens)};")
    disp.append("")
    disp.append("/* every pool word compiled in as a constant (bit 0: a 4-byte word, else 2)")
    disp.append(" * and the block that reads it, sorted: the runtime's changed code */")
    disp.append(f"const rt_lit {pfx}_lits[] = {{")
    for w, b in sorted(lits):
        disp.append(f"    {{ 0x{w:08x}u, 0x{b:08x}u }},")
    disp.append("};")
    disp.append(f"const unsigned {pfx}_lit_count = {len(lits)};")
    report["literal_readers"] = len(lits)
    write(os.path.join(out_dir, f"{pfx}_dispatch.c"), "\n".join(disp) + "\n")
    files.append(f"{pfx}_dispatch.c")
    report["files"] = files

    write(os.path.join(out_dir, "report.json"), json.dumps(report, indent=1) + "\n")
    lines = [f"{prog.name}: {len(funcs)} routines, {stats['instructions']} instructions",
             f"  calls: {stats['calls_static']} static, {stats['calls_dynamic']} through a register (looked up at run time), "
             f"{len(lists['calls_to_non_entries'])} to addresses that are not entries",
             f"  jumps: {stats['jumps_resolved']} resolved, {len(lists['unresolved_jumps'])} unresolved",
             f"  noreturn routines: {len(noreturn)}",
             f"  instructions followed past their routine's extent: {stats['outside_extent']}",
             f"  illegal instructions reached: {len(lists['illegal_reached'])}; bad delay slots: "
             f"{len(lists['bad_slots'])}",
             f"  FPU mode: {agree} agree with the decomp, {disagree} disagree, {mine_unknown} tested at run time",
             f"  routines found as call targets, not in the decomp's list: {len(lists['discovered_entries'])}; "
             f"jumps resolved by translator/hints.tsv: {len(lists['hinted_jumps'])}",
             f"  blocks: {len(blens)}; literal pool words the program writes (read from memory): "
             f"{len(report_written)}"]
    for k in ("unresolved_jumps", "calls_to_non_entries", "illegal_reached", "bad_slots", "discovered_entries",
              "hinted_jumps", "written_words", "aliased_calls_and_jumps", "shared_code"):
        if lists[k]:
            lines.append(f"{k} ({len(lists[k])}):")
            lines += ["  " + x for x in lists[k][:400]]
    if disagreements:
        lines.append("FPU mode disagreements:")
        lines += ["  " + x for x in disagreements[:200]]
    write(os.path.join(out_dir, "report.txt"), "\n".join(lines) + "\n")
    print("\n".join(lines[:10]))
    return report


def write(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--program", required=True, choices=["main", "test"])
    ap.add_argument("--image", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    with open(a.image, "rb") as fh:
        image = fh.read()
    prog = Program(a.program, image, os.path.join(ROOT, "meta", a.program))
    translate(prog, a.out)


if __name__ == "__main__":
    main()
