/* The seam between the translated game and VF4debug's machine
 * (runtime/machine): docs/design.md, "Interrupts and timing".
 *
 * Translated code runs block by block on the C stack and adds each block's
 * instruction count at its start. Everything that is about time or about
 * code the translator did not cover comes here:
 *
 *   rt_slow      an event falls inside a block, or a device brought it
 *                forward: serve the events at the exact instruction, run the
 *                rest of the block on VF4debug's interpreter, hand back at
 *                the next block start;
 *   rt_event     what VF4debug's machine_run does between two runs of the
 *                interpreter (events, the controller's stops, interrupts),
 *                an interrupt's handler run as a nested call that must come
 *                back to where it was taken;
 *   rt_drive     run whatever is at c->pc until it returns: translated
 *                routines as C, anything else (the BIOS stand-in's vector
 *                code, code outside the image) on the interpreter;
 *   rt_io_*      the device models' slow path, with the clock and PC put
 *                back to the instruction's own (RT_ATK);
 *   rt_code_store a store into a page that holds translated code: a block
 *                whose bytes (or a pool word it compiled in) it changes runs
 *                on the interpreter from then on (rt_code_changed).
 *
 * So the machine sees exactly what it sees under VF4debug's interpreter:
 * the same instruction counts at every device access and every event, the
 * same interrupt points. The run is VF4debug's run, state for state.
 *
 * Test mode (M6): the game's Test switch makes it call the BIOS's test menu
 * (service 0x44). The board's BIOS then starts the game's test program,
 * test.bin, at the same address as the game, and its EXIT calls the BIOS
 * again, which restarts the game. Here (rt_program) the machine goes back to
 * the state it had before the game's first instruction (taken by
 * rt_native_power_on), keeps what the cabinet keeps with the power off
 * (the battery-backed RAM, the settings EEPROM, the cards in the units),
 * loads the other program as the Naomi header's table says, and switches
 * every table to that program: the routines and globals the machine looks
 * up (dispatch.c), the translated code (M_ or T_), the hooks.
 */
#include "sh4.h"            /* VF4debug's (runtime/machine): first, see runtime/cpu/sh4.h */
#include "mem.h"
#include "hw.h"
#include "dispatch.h"
#include "machine.h"
#include "control.h"
#include "tables.h"
#include "rt_dispatch.h"
#include "rt_native.h"
#include "hle.h"
#include "observe.h"
#include "view.h"
#include "tables.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void events_fire(sh4 *c);  /* machine.c (recomp: made visible) */

rt_native_stats rt_stats;

int rt_skip_entry;

static u16 *blk;            /* per halfword of the image: the block's length at a block start, else 0 */
static u32 blk_slots;

/* the program running now (test mode): its translation's tables */
static const rt_block *blocks;
static unsigned block_count;
static const rt_lit *lits;
static unsigned lit_count;

/* ---- code the game changes (docs/design.md, "Code the game changes") ---- */

u8 rt_code_changed[RT_CODE_SLOTS];
static u16 *code_back;      /* per halfword: 0, or 1 + how far into the first block holding it it is */
static u8 *code_flags;      /* per halfword: CODE_LIT, CODE_START */
enum { CODE_LIT = 1,        /* part of a pool word compiled in as a constant */
       CODE_START = 2 };    /* a block starts here (a delay slot can also start a block of its own) */
static u8 *code_host;       /* the host address of 0x0c020000 */
static u32 code_slots;      /* halfwords from 0x0c020000 the two tables cover */
static u8 *code_orig;       /* the game's code as its run began (rt_native_init), for a snapshot's load */
static u32 code_orig_len;
static u32 rt_store_at;     /* the translated store being made (rt_io_write), else 0 */
static sh4 *rt_cpu;

static void block_changed(u32 s, u32 why)
{
    if (rt_code_changed[s])
        return;
    u32 n = blk[s];
    u32 at = 0x0c020000u + 2 * s;
    if (rt_store_at && rt_cpu && rt_store_at >= at && rt_store_at + 2 < at + 2 * n)
        /* the store is in the block it changes, and instructions of the
         * block after it would run as they were: not handled */
        rt_host_fault(rt_cpu, 104, rt_store_at, why);
    rt_code_changed[s] = 1;
    blk[s] = 0;
    rt_stats.changed_blocks++;
    if (rt_stats.changed_blocks <= 16)
        fprintf(stderr, "vf4ft: the game changed %08x: the block at %08x runs on the interpreter from now on\n",
                why, at);
    else if (rt_stats.changed_blocks == 17)
        fprintf(stderr, "vf4ft: (more changed blocks: see the count at the end)\n");
}

/* a byte of the image at 0x0c020000 + off is about to change */
static void code_byte_changed(u32 off)
{
    u32 s = off >> 1, addr = 0x0c020000u + off;
    if (code_back[s])
        block_changed(s - (code_back[s] - 1u), addr);
    if (code_flags[s] & CODE_START)
        block_changed(s, addr);
    if (code_flags[s] & CODE_LIT) {
        /* the blocks that read a pool word holding the byte */
        unsigned lo = 0, hi = lit_count;
        u32 from = (addr - 3u) & ~1u;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if ((lits[mid].word & ~1u) < from)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (unsigned k = lo; k < lit_count && (lits[k].word & ~1u) <= addr; k++) {
            u32 base = lits[k].word & ~1u, size = lits[k].word & 1u ? 4 : 2;
            if (addr < base + size)
                block_changed((lits[k].block - 0x0c020000u) >> 1, addr);
        }
    }
}

/* the halfwords [h, h + n) of the host's memory hold nothing translated */
static inline int code_untouched(const u8 *h, u32 n)
{
    uintptr_t off = (uintptr_t)(h - code_host);
    if (off >= 2u * (uintptr_t)code_slots)
        return 1;
    u32 s = (u32)(off >> 1), e = (u32)((off + n - 1) >> 1);
    if (e >= code_slots)
        e = code_slots - 1;
    for (; s <= e; s++)
        if (code_back[s] | code_flags[s])
            return 0;
    return 1;
}

/* mem_code_store (runtime/machine/mem.h): every store into a page that holds
 * translated code, before it is made */
static void rt_code_store(u32 addr, u32 n, const void *bytes)
{
    rt_stats.code_page_stores++;
    const u8 *b = bytes;
    for (u32 i = 0; i < n; i++) {
        u8 *h = mem_ptr(addr + i);
        if (!h || *h == b[i])
            continue;
        uintptr_t off = (uintptr_t)(h - code_host);
        if (off < 2u * (uintptr_t)code_slots)
            code_byte_changed((u32)off);
    }
}

/* the bytes the translation was made from (the image file) against memory:
 * whatever already differs (made before the run, or by a snapshot) */
static int code_compare(const char *image)
{
    /* no file: the program came out of the DIMM image just before this
     * (runtime/host/game_machine.c), so memory holds those very bytes */
    if (!image)
        return 0;
    FILE *f = fopen(image, "rb");
    if (!f) {
        fprintf(stderr, "vf4ft: cannot read %s\n", image);
        return 1;
    }
    u32 len = 2 * code_slots;
    u8 *orig = malloc(len);
    if (!orig || fread(orig, 1, len, f) != len) {
        fprintf(stderr, "vf4ft: %s is shorter than the translated code\n", image);
        fclose(f);
        free(orig);
        return 1;
    }
    fclose(f);
    for (u32 off = 0; off < len; off++)
        if (orig[off] != code_host[off])
            code_byte_changed(off);
    free(orig);
    return 0;
}

static int code_watch_init(const char *image)
{
    u32 hi = 0;
    for (unsigned i = 0; i < block_count; i++)
        if ((blocks[i].addr + 2 * blocks[i].n) > hi)
            hi = blocks[i].addr + 2 * blocks[i].n;
    for (unsigned i = 0; i < lit_count; i++)
        if ((lits[i].word & ~1u) + 4 > hi)
            hi = (lits[i].word & ~1u) + 4;
    code_slots = (hi - 0x0c020000u + 1) / 2;
    if (code_slots > RT_CODE_SLOTS || code_slots > blk_slots) {
        fprintf(stderr, "vf4ft: the translated code reaches %08x, past the image\n", hi);
        return 1;
    }
    free(code_back);
    free(code_flags);
    memset(rt_code_changed, 0, sizeof rt_code_changed);
    code_back = calloc(code_slots, sizeof *code_back);
    code_flags = calloc(code_slots, 1);
    code_host = mem_ptr(0x0c020000u);
    if (!code_back || !code_flags || !code_host) {
        fprintf(stderr, "vf4ft: out of memory for the code tables\n");
        return 1;
    }
    /* Blocks split the code (translator/translate.py, global_blocks), but for
     * a delay slot that is also a branch target: it ends one block and starts
     * the next (CODE_START). Anything else in two blocks is not this
     * translator's. */
    for (unsigned i = 0; i < block_count; i++) {
        u32 s = (blocks[i].addr - 0x0c020000u) >> 1;
        code_flags[s] |= CODE_START;
        for (u32 k = 0; k < blocks[i].n; k++) {
            if (!code_back[s + k])
                code_back[s + k] = (u16)(k + 1);
            else if (k != 0) {
                fprintf(stderr, "vf4ft: %08x is inside two blocks: the translation is not of this runtime\n",
                        blocks[i].addr + 2 * k);
                return 1;
            }
        }
    }
    for (unsigned i = 0; i < lit_count; i++) {
        u32 s = ((lits[i].word & ~1u) - 0x0c020000u) >> 1;
        code_flags[s] |= CODE_LIT;
        if (lits[i].word & 1u)
            code_flags[s + 1] |= CODE_LIT;
    }
    /* every page (at every address it has) that holds translated code */
    for (u32 p = 0; p < 65536; p++) {
        u8 *h = mem_page_ram[p];
        mem_code_page[p] = h && h + 0x10000 > code_host && h < code_host + 2 * code_slots;
    }
    mem_wpage_sync();
    mem_code_store = rt_code_store;
    return code_compare(image);
}

static jmp_buf rt_end;
static int rt_end_code;
static int rt_resumed;      /* rt_native_load: the run goes on from the snapshot's place (rt_native_run) */

/* the tables of a program's translation, and the watch on its code; image:
 * the file it was translated from, or NULL when memory holds its bytes */
static int program_tables(int program, const char *image)
{
    rt_table = program == PROGRAM_TEST ? T_routines : M_routines;
    rt_table_count = program == PROGRAM_TEST ? T_routine_count : M_routine_count;
    blocks = program == PROGRAM_TEST ? T_blocks : M_blocks;
    block_count = program == PROGRAM_TEST ? T_block_count : M_block_count;
    lits = program == PROGRAM_TEST ? T_lits : M_lits;
    lit_count = program == PROGRAM_TEST ? T_lit_count : M_lit_count;
    blk_slots = disp_slots;
    free(blk);
    blk = calloc(blk_slots, sizeof *blk);
    if (!blk) {
        fprintf(stderr, "vf4ft: out of memory for the block table\n");
        return 1;
    }
    for (unsigned i = 0; i < block_count; i++) {
        u32 s = disp_slot(blocks[i].addr);
        if (s >= blk_slots || blocks[i].n == 0 || blocks[i].n > 0xffff) {
            fprintf(stderr, "vf4ft: block %08x (%u instructions) does not fit the image: the translation is "
                    "not of this program\n", blocks[i].addr, blocks[i].n);
            return 1;
        }
        blk[s] = (u16)blocks[i].n;
    }
    return code_watch_init(image);
}

int rt_native_init(const char *image)
{
    if (program_tables(PROGRAM_MAIN, image))
        return 1;
    /* memory holds the program's own bytes until the run starts */
    free(code_orig);
    code_orig_len = 2 * code_slots;
    code_orig = malloc(code_orig_len);
    if (!code_orig) {
        fprintf(stderr, "vf4ft: out of memory for the code tables\n");
        return 1;
    }
    memcpy(code_orig, code_host, code_orig_len);
    return 0;
}

/* the block starting at pc, run as C: only at the program's own (P0)
 * addresses; through P1/P2 the code runs on the interpreter (rt_call) */
static inline u32 blk_len(u32 pc)
{
    u32 s = pc - 0x0c020000u;
    return (s >> 1) < blk_slots && !(s & 1) ? blk[s >> 1] : 0;
}

static u32 canon(u32 a)
{
    u32 region = a >> 29;
    return (region == 4 || region == 5) ? (a & 0x1fffffffu) : a;
}

/* ---- ends ------------------------------------------------------------------ */

static void __attribute__((noreturn)) rt_finish(int code)
{
    rt_end_code = code;
    longjmp(rt_end, 1);
}

void rt_host_fault(sh4 *c, int what, u32 at, u32 detail)
{
    if (what == 104) {
        machine_stop(c, STOP_FATAL, "translated code: a store at %08x (%s) changed %08x, in the block it runs in",
                     at, addr_text(at), detail);
        rt_finish(STOP_FATAL);
    }
    static const char *const why[] = { "?", "a call or jump to an address no routine starts at",
                                       "a routine came back somewhere other than after its call",
                                       "an illegal instruction" };
    const char *w = what >= 1 && what <= 3 ? why[what] : what == 100 ? "an FPU exception that traps"
                  : what == 101 ? "sleep" : what == 102 ? "an interrupt's handler came back somewhere else"
                  : what == 103 ? "the boot routine returned" : "?";
    machine_stop(c, STOP_FATAL, "translated code: %s, at %08x (%s), %08x", w, at, addr_text(at), detail);
    rt_finish(STOP_FATAL);
}

void rt_fpu_trap(sh4 *c, u32 at)
{
    /* the game runs with no FPU exception enabled; one that traps would
     * need the C of the instruction stopped half way */
    rt_host_fault(c, 100, at, c->fpscr);
}

void rt_sleep(sh4 *c, u32 at)
{
    rt_host_fault(c, 101, at, 0);
}

/* ---- the controller ----------------------------------------------------------- */

/* the machine stopped (c->stop): what VF4debug's main loop does with
 * machine_run's return, then its next machine_run's start */
static void rt_stopped(sh4 *c)
{
    int code = c->stop;
    if (code == STOP_SLEEP)
        rt_host_fault(c, 101, c->pc, 0);
    if (!control_stopped(c, code) || machine_interrupted)
        rt_finish(code);
    if (!control_next(c))
        rt_finish(code);
    c->stop = 0;
}

/* ---- the interpreter's part ----------------------------------------------------- */

enum { K_PLAIN, K_CALL, K_RETURN, K_TRAPA, K_SLEEP };

static int step_kind(u32 op)
{
    if ((op & 0xf0ff) == 0x400b || (op & 0xf000) == 0xb000 || (op & 0xf0ff) == 0x0003)
        return K_CALL;                  /* jsr, bsr, bsrf */
    if (op == 0x000b || op == 0x002b)
        return K_RETURN;                /* rts, rte */
    if ((op & 0xff00) == 0xc300)
        return K_TRAPA;
    if (op == 0x001b)
        return K_SLEEP;
    return K_PLAIN;
}

static void rt_drive(sh4 *c);
static void rt_event(sh4 *c, u32 at);

/* one instruction on the interpreter, exactly as VF4debug's run loop takes
 * it (the caller has checked for events); a call or an exception it starts
 * runs to its return. Returns 1 when the instruction was a return. */
static int interp_step(sh4 *c)
{
    u32 pc = c->pc;
    if (!mem_ptr(pc)) {
        machine_stop(c, STOP_BADPC, "the processor went to %08x, where there is no memory", pc);
        c->stop_arg = pc;
        return 0;
    }
    u32 op = g_r16(pc);
    int kind = step_kind(op);
    if (kind == K_SLEEP)
        rt_host_fault(c, 101, pc, 0);
    sh4_step(c);
    rt_fpscr_loaded(c);
    rt_stats.interp_insns++;
    if (kind == K_CALL && c->pc != c->pr && !c->stop) {
        rt_drive(c);                    /* the callee, to its return */
    } else if (kind == K_TRAPA) {
        rt_stats.exceptions++;
        rt_drive(c);                    /* the handler, to its rte */
        if (c->pc != pc + 2)
            rt_host_fault(c, 102, pc, c->pc);
    }
    return kind == K_RETURN;
}

/* Runs from c->pc until the code there returns (an rts or rte at this
 * level), leaving c->pc where it returned to. A translated routine's start
 * runs as C; its hooks already ran where the interpreter branched to it
 * (X(go)), or it was fallen or returned into: no entry either way. */
static void rt_drive(sh4 *c)
{
    for (;;) {
        u32 pc = c->pc;
        const rt_routine *r = blk_len(pc) ? rt_find(rt_table, rt_table_count, canon(pc)) : NULL;
        if (r) {
            if (disp_flags[disp_slot(pc)])
                rt_skip_entry = 1;
            r->fn(c);
            rt_skip_entry = 0;
            return;
        }
        if (c->cycles >= c->next_event) {
            rt_event(c, pc);
            continue;
        }
        if (interp_step(c))
            return;
    }
}

void rt_continue(sh4 *c)
{
    rt_stats.outside_calls++;
    rt_drive(c);
}

int rt_slow(sh4 *c)
{
    rt_stats.slow_blocks++;
    for (;;) {
        if (c->cycles >= c->next_event) {
            rt_event(c, c->pc);
            continue;
        }
        u32 n = blk_len(c->pc);
        if (n && c->cycles + n <= c->next_event)
            return 0;                   /* the block fits: back to C */
        if (interp_step(c))
            return 1;
    }
}

/* ---- events, stops, interrupts ------------------------------------------------ */

/* VF4debug's machine_run between two runs of its interpreter, at the
 * instruction boundary c->pc = at with the clock exact */
static void rt_event(sh4 *c, u32 at)
{
    c->pc = at;
    for (;;) {
        if (c->stop)
            rt_stopped(c);
        events_fire(c);
        if (c->stop)
            continue;
        u32 code;
        int level = hw_irq_pending(c, &code);
        int taken = level && !(c->sr & SR_BL) && (u32)level > ((c->sr & SR_IMASK) >> 4);
        if (taken) {
            hw.intevt = code;
            sh4_exception(c, 0x600);
        }
        if (cfg.max_insns && c->insns >= cfg.max_insns) {
            machine_stop(c, STOP_LIMIT, "instruction limit (%llu)", (unsigned long long)cfg.max_insns);
            continue;
        }
        u64 next = event_next();
        if (cfg.max_insns && cfg.max_insns - c->insns < next - c->cycles)
            next = c->cycles + (cfg.max_insns - c->insns);
        c->next_event = next;
        if (taken) {
            rt_stats.exceptions++;
            rt_drive(c);                /* the handler, to its rte */
            if (c->pc != at)
                rt_host_fault(c, 102, at, c->pc);
        }
        if (c->cycles < c->next_event)
            return;
    }
}

void rt_trapa(sh4 *c, u32 imm, u32 at)
{
    /* trapa ends its block, so the clock is exact here */
    c->pc = at + 2;
    disp_trapa(c, imm);
    rt_stats.exceptions++;
    rt_drive(c);
    if (c->pc != at + 2)
        rt_host_fault(c, 102, at, c->pc);
}

void rt_rte(sh4 *c)
{
    disp_leave_exception(c);
}

/* ---- entries and code that is not translated -------------------------------------- */

int rt_entry(sh4 *c, u32 addr)
{
    if (rt_skip_entry) {
        rt_skip_entry = 0;
        return 0;
    }
    c->pc = addr;
    disp_enter(c, addr, DISP_CALL);
    return c->pc != addr;
}

void rt_host_outside(sh4 *c, u32 target, u32 from, int is_call)
{
    (void)from;
    rt_stats.outside_calls++;
    c->pc = target;
    /* what the interpreter's branch does on arrival (X(go)) */
    u32 s = disp_slot(target);
    if (s >= disp_slots) {
        /* the BIOS stand-in's services and firmware notes answer at once */
        disp_outside(c, target, is_call ? DISP_CALL : DISP_JUMP);
        if (c->pc != target)
            return;
    } else if (disp_flags[s]) {
        disp_enter(c, target, is_call ? DISP_CALL : DISP_JUMP);
        if (c->pc != target)
            return;                     /* a host replacement ran */
    }
    rt_drive(c);
}

/* ---- the device models' slow path ------------------------------------------------- */

u32 rt_io_read(sh4 *c, u64 atk, u32 addr, int size)
{
    u32 k = (u32)(atk >> 32);
    c->pc = (u32)atk + 2;
    c->cycles -= k;
    c->insns -= k;
    u32 v = io_read(c, addr, size);
    c->cycles += k;
    c->insns += k;
    return v;
}

void rt_io_write(sh4 *c, u64 atk, u32 addr, u32 val, int size)
{
    mem_track(addr);        /* the page armed for rollback (mem_track.h) */
    /* most stores into code pages are not into code: the interrupt
     * handler's stack, for one, is inside the program's .text */
    u8 *p = mem_page[addr >> 16];
    if (p && mem_code_page[addr >> 16] && code_untouched(p + (addr & 0xffff), (u32)size)) {
        rt_stats.code_page_stores++;
        p += addr & 0xffff;
        if (size == 4)
            memcpy(p, &val, 4);
        else if (size == 2)
            { u16 h = (u16)val; memcpy(p, &h, 2); }
        else
            *p = (u8)val;
        return;
    }
    u32 k = (u32)(atk >> 32);
    c->pc = (u32)atk + 2;
    c->cycles -= k;
    c->insns -= k;
    rt_store_at = (u32)atk;
    mem_code_write(c, addr, val, size);     /* io_write, or a store into a code page */
    rt_store_at = 0;
    c->cycles += k;
    c->insns += k;
}

void rt_sq_write(sh4 *c, u64 atk, u32 dst, const u32 *words)
{
    mem_track(dst);         /* rollback (mem_track.h) */
    u32 k = (u32)(atk >> 32);
    c->pc = (u32)atk + 2;
    c->cycles -= k;
    c->insns -= k;
    u8 *p = mem_page[dst >> 16];
    if (p) {                                /* a code page (mem_wpage), or one armed for rollback */
        rt_store_at = (u32)atk;
        mem_code_note(dst, 32, words);
        rt_store_at = 0;
        memcpy(p + (dst & 0xffff), words, 32);
    } else {
        io_sq_write(c, dst, words);
    }
    c->cycles += k;
    c->insns += k;
}

/* ---- test mode: the other program --------------------------------------------------- */

static u8 *power_on;            /* the machine before the game's first instruction (snapshot_save_mem) */
static size_t power_on_size;
static jmp_buf rt_restart;
static int rt_switch_to;
static int rt_program_now;
static int rt_restart_set;      /* rt_restart is rt_native_run's, which is running */
unsigned rt_native_switches;
static void rt_program(sh4 *c, int program);

int rt_native_power_on(void)
{
    free(power_on);
    power_on = NULL;
    if (snapshot_save_mem(&power_on, &power_on_size)) {
        power_on = NULL;
        fprintf(stderr, "vf4ft: cannot keep the machine's power-on state: test mode will stop the game\n");
        return 1;
    }
    hle_program = rt_program;
    return 0;
}

int rt_native_program(void)
{
    return rt_program_now;
}

void rt_native_restart(void)
{
    if (!power_on || !rt_restart_set)
        return;
    rt_switch_to = PROGRAM_MAIN;
    longjmp(rt_restart, 1);
}

/* service 0x44 (hle_boot.c), deep in whatever called it: back to
 * rt_native_run's frame, which switches with nothing of the old program on
 * the stack */
static void rt_program(sh4 *c, int program)
{
    (void)c;
    rt_switch_to = program;
    longjmp(rt_restart, 1);
}

/* the program's load table in the Naomi header of the DIMM image (main at
 * 0x360, the test program at 0x3c0): up to 8 rows of (offset in the image,
 * address, bytes), ended by 0xffffffff; 0 when it was loaded */
static int program_load(int program)
{
    const u8 *img = hw_cart_image();
    u32 size = hw_cart_image_size();
    if (!img || size < 0x400)
        return -1;
    u32 at = program == PROGRAM_TEST ? 0x3c0 : 0x360, loaded = 0;
    for (int row = 0; row < 8; row++, at += 12) {
        u32 off, dst, n;
        memcpy(&off, img + at, 4);
        if (off == 0xffffffffu)
            break;
        memcpy(&dst, img + at + 4, 4);
        memcpy(&n, img + at + 8, 4);
        u8 *h = mem_ptr(dst);
        if (!h || off > size || n > size - off || !mem_ptr(dst + n - 1) || mem_ptr(dst + n - 1) != h + n - 1)
            return -1;
        mem_track_host(h, n);
        memcpy(h, img + off, n);
        loaded += n;
    }
    return loaded ? 0 : -1;
}

static void program_switch(sh4 *c, int program)
{
    static const char *const names[] = { "the game (main.bin)", "the test program (test.bin)" };
    if (!power_on || c != &cpu) {
        machine_stop(c, STOP_FATAL, "the game asked the BIOS for its test menu (service 0x44), and the machine's "
                     "power-on state was not kept");
        rt_finish(STOP_FATAL);
    }
    if (obs_on || obs_log) {
        machine_stop(c, STOP_FATAL, "service 0x44: the other program cannot start with the observer on (its "
                     "records are main.bin's)");
        rt_finish(STOP_FATAL);
    }
    /* what the cabinet keeps with the power off */
    static u8 backup[0x10000], eeprom[0x80], card[2][128];
    int has_card[2];
    g_read(0xa0200000u, backup, sizeof backup);
    memcpy(eeprom, hw_maple_eeprom(), sizeof eeprom);
    for (int n = 0; n < 2; n++)
        has_card[n] = !hw_card_get(n, card[n]);
    u32 frame = machine_frame;
    u64 insns = c->insns;
    if (snapshot_load_mem(power_on, power_on_size)) {
        machine_stop(c, STOP_FATAL, "service 0x44: %s", snapshot_error);
        rt_finish(STOP_FATAL);
    }
    g_write(0xa0200000u, backup, sizeof backup);
    memcpy(hw_maple_eeprom(), eeprom, sizeof eeprom);
    for (int n = 0; n < 2; n++)
        if (has_card[n])
            hw_card_put(n, card[n]);
    /* the count of vertical blanks and instructions goes on (the host's and
     * the controller's frame numbers); the clock starts again with the rest */
    machine_frame = frame;
    c->insns = insns;
    /* the power-on state has the game in memory; the test program goes over it */
    if (program == PROGRAM_TEST && program_load(PROGRAM_TEST)) {
        machine_stop(c, STOP_FATAL, "service 0x44: the DIMM image has no test program to load");
        rt_finish(STOP_FATAL);
    }
    disp_program = program;
    if (disp_init() || hle_init(c) || program_tables(program, NULL)) {
        machine_stop(c, STOP_FATAL, "service 0x44: the tables of %s cannot be made", names[program]);
        rt_finish(STOP_FATAL);
    }
    obs_program_changed();
    view_program_changed();
    rt_skip_entry = 0;
    rt_program_now = program;
    rt_native_switches++;
    c->stop = 0;
    if (!cfg.quiet)
        fprintf(stderr, "vf4ft: vertical blank %u: the BIOS starts %s\n", machine_frame, names[program]);
}

/* ---- the run ---------------------------------------------------------------------- */

/* A snapshot of a run of this build (snapshot_save_mem, taken in a frame
 * hook): memory, the processor and the models as they were, part way into
 * the game's code. What this file keeps that no snapshot holds is made to
 * fit what was loaded:
 *   - the blocks the game had changed: memory against the program's own
 *     bytes, kept as the run began (rt_native_init);
 *   - the host's rounding mode: rt_native_run loads it from FPSCR;
 *   - the routines the run was inside of have no C frames here:
 *     rt_native_run goes on from c->pc instead of the game's start.
 * Before the run, the machine is loaded and the run starts from it. From
 * the frame hook while the run goes on, the machine is loaded there and
 * the run starts again from rt_native_run's frame, as a restart does (the
 * call does not return). Only the game (main.bin) is loaded, never into the
 * test program's run. -1 when it cannot be: the machine is as it was and
 * snapshot_error says why. */
#define RT_LOADED (-1)              /* rt_switch_to: a snapshot was loaded */

static int code_refit(void)
{
    if (program_tables(PROGRAM_MAIN, NULL) || code_orig_len != 2 * code_slots)
        return -1;
    for (u32 off = 0; off < code_orig_len; off++)
        if (code_orig[off] != code_host[off])
            code_byte_changed(off);
    rt_skip_entry = 0;
    rt_resumed = 1;
    return 0;
}

int rt_native_load(const u8 *data, size_t size)
{
    if (rt_program_now != PROGRAM_MAIN || !code_orig || (rt_cpu && !rt_restart_set)) {
        snprintf(snapshot_error, sizeof snapshot_error, "the machine is not running the game");
        return -1;
    }
    if (snapshot_load_mem(data, size))
        return -1;
    if (rt_restart_set) {
        rt_switch_to = RT_LOADED;
        longjmp(rt_restart, 1);
    }
    if (code_refit()) {
        snprintf(snapshot_error, sizeof snapshot_error, "out of memory for the code tables");
        return -1;
    }
    return 0;
}

/* Rollback (runtime/machine/rollback.c, docs/design.md section 55): the
 * frame hook has put memory and the models back to an earlier frame's end
 * of this same run. The run starts again from there as after a snapshot's
 * load, without code_refit: every byte of the code that differs from the
 * program's own was marked when it was stored (rt_code_store) or loaded
 * (code_refit), and marks are never taken back, so they still cover the
 * code as it was then. */
#define RT_REWOUND (-2)             /* rt_switch_to: rt_native_rewind */

int rt_native_can_rewind(void)
{
    return rt_program_now == PROGRAM_MAIN && rt_restart_set;
}

void rt_native_rewind(void)
{
    if (!rt_native_can_rewind())
        return;
    rt_switch_to = RT_REWOUND;
    longjmp(rt_restart, 1);
}

int rt_native_run(sh4 *c)
{
    rt_cpu = c;
    if (setjmp(rt_end)) {
        rt_restart_set = 0;
        return rt_end_code;
    }
    if (!control_next(c))
        return STOP_NONE;
    c->stop = 0;
    if (setjmp(rt_restart)) {
        if (rt_switch_to == RT_LOADED) {    /* rt_native_load from the frame hook */
            if (code_refit()) {
                machine_stop(c, STOP_FATAL, "a snapshot was loaded, and the code tables cannot be made");
                rt_finish(STOP_FATAL);
            }
            c->stop = 0;
        } else if (rt_switch_to == RT_REWOUND) {
            rt_skip_entry = 0;
            rt_resumed = 1;
            c->stop = 0;
        } else {
            program_switch(c, rt_switch_to);    /* test mode: the other program, from its start (or a restart) */
            rt_resumed = 0;
        }
    }
    rt_restart_set = 1;
    rt_fpscr_loaded(c);
    rt_event(c, c->pc);
    if (rt_resumed)
        /* resumed (rt_native_load): rt_drive runs to a return at its level,
         * and each one goes on into the code after the call, which runs on
         * the interpreter (a routine's C cannot start part way) while what it
         * calls runs as C again. The game's main loop never returns: this
         * ends by a stop, as the run from the start does */
        for (;;)
            rt_drive(c);
    rt_drive(c);
    rt_host_fault(c, 103, c->pc, 0);
    return STOP_FATAL;
}

/* moved here from runtime/headless/main.c (VF4debug's src/main.c), shared with the game */
/* The names of the decompilation this program relies on. They are looked
 * up in the tables generated at build time; when the decompilation renames
 * one, the build still succeeds, so say so here instead of misbehaving. */
int rt_native_names_check(void)
{
    static const char *const globals[] = {
        "main_mode_no", "sub_mode_no", "game_frame_count", "vblank_count", "mode_data", "bios_signature",
        "boot_country", "bios_rtc_seconds", "board_serial_1", "board_serial_2", "dimm_board_id_word",
        "dimm_reply_word", "dimm_comms_mode", "mie_ident_expected"
    };
    static const char *const fns[] = {
        "_start", "exception_entry", "irq_entry_via_firmware", "busy_wait_hook_noop", "stopwatch_usec",
        "render_done_wait", "dimm_fbusy", "dimm_fclose"
    };
    int missing = 0;
    for (unsigned i = 0; i < sizeof globals / sizeof globals[0]; i++)
        if (!global_addr(globals[i])) {
            fprintf(stderr, "vf4ft: the decompilation no longer has a global named %s\n", globals[i]);
            missing++;
        }
    for (unsigned i = 0; i < sizeof fns / sizeof fns[0]; i++)
        if (!routine_by_name(fns[i])) {
            fprintf(stderr, "vf4ft: the decompilation no longer has a routine named %s\n", fns[i]);
            missing++;
        }
    if (missing)
        fprintf(stderr, "vf4ft: %d names this program uses are gone (renamed in the decompilation?). Find the "
                "new names and change them in runtime/machine/ (grep for the old ones), then rebuild.\n", missing);
    return missing;
}
