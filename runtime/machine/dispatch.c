/* The per-routine dispatch table and the calls the interpreter makes into
 * it: routine entries (host replacements, hooks, the observer), targets
 * outside the image, exceptions. */
#include "dispatch.h"
#include "mem.h"
#include "observe.h"
#include "tables.h"
#include "machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

routine *routines;
int routine_count;
u8 *disp_flags;
u32 *disp_index;
u32 disp_slots;
int disp_exc_depth;

/* recomp (test mode): the program whose tables are in use (tables.h) */
int disp_program;
const gen_routine *prog_routines;
int prog_routine_count;
const gen_global *prog_globals;
int prog_global_count;

/* ---- names ---- */

typedef struct name_slot {
    const char *name;
    u32 value;
} name_slot;
static name_slot *fn_names, *gl_names;
static u32 fn_names_n, gl_names_n;

static u32 hash_str(const char *s)
{
    u32 h = 2166136261u;
    while (*s)
        h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static void names_put(name_slot *t, u32 n, const char *name, u32 value)
{
    u32 i = hash_str(name) & (n - 1);
    while (t[i].name) {
        if (!strcmp(t[i].name, name))
            return;                     /* the first one wins */
        i = (i + 1) & (n - 1);
    }
    t[i].name = name;
    t[i].value = value;
}

static int names_get(name_slot *t, u32 n, const char *name, u32 *value)
{
    u32 i = hash_str(name) & (n - 1);
    while (t[i].name) {
        if (!strcmp(t[i].name, name)) {
            *value = t[i].value;
            return 1;
        }
        i = (i + 1) & (n - 1);
    }
    return 0;
}

int disp_init(void)
{
    /* recomp (test mode): the tables of disp_program; called again when the
     * BIOS stand-in starts the other program, so whatever the last call
     * made goes first (hooks and replacements are installed again after) */
    prog_routines = disp_program == PROGRAM_TEST ? gen_test_routines : gen_routines;
    prog_routine_count = disp_program == PROGRAM_TEST ? gen_test_routine_count : gen_routine_count;
    prog_globals = disp_program == PROGRAM_TEST ? gen_test_globals : gen_globals;
    prog_global_count = disp_program == PROGRAM_TEST ? gen_test_global_count : gen_global_count;
    free(routines);
    free(disp_flags);
    free(disp_index);
    free(fn_names);
    free(gl_names);
    disp_exc_depth = 0;
    const gen_routine *gen_routines = prog_routines;    /* recomp: the names below are the program's */
    const gen_global *gen_globals = prog_globals;
    int gen_routine_count = prog_routine_count, gen_global_count = prog_global_count;
    routine_count = gen_routine_count;
    routines = calloc((size_t)routine_count, sizeof *routines);
    u32 end = 0;
    for (int i = 0; i < routine_count; i++) {
        routines[i].addr = gen_routines[i].addr;
        routines[i].size = gen_routines[i].size;
        routines[i].name = gen_routines[i].name;
        routines[i].file = gen_routines[i].file;
        routines[i].proto = gen_routines[i].proto;
        routines[i].first_mode = -1;
        if (routines[i].addr + routines[i].size > end)
            end = routines[i].addr + routines[i].size;
    }
    /* the table covers the whole image, so that calls into data are caught too */
    disp_slots = 0x340000 / 2;
    (void)end;
    disp_flags = calloc(disp_slots, 1);
    disp_index = calloc(disp_slots, sizeof *disp_index);
    if (!routines || !disp_flags || !disp_index)
        return -1;
    fn_names_n = 16384;
    gl_names_n = 32768;
    while (fn_names_n < (u32)routine_count * 2)
        fn_names_n *= 2;
    while (gl_names_n < (u32)gen_global_count * 2)
        gl_names_n *= 2;
    fn_names = calloc(fn_names_n, sizeof *fn_names);
    gl_names = calloc(gl_names_n, sizeof *gl_names);
    for (int i = 0; i < routine_count; i++) {
        u32 s = disp_slot(routines[i].addr);
        if (s < disp_slots) {
            disp_index[s] = (u32)i + 1;
            disp_flags[s] = DF_FIRST;
        }
        names_put(fn_names, fn_names_n, routines[i].name, (u32)i);
    }
    for (int i = 0; i < gen_global_count; i++)
        names_put(gl_names, gl_names_n, gen_globals[i].name, gen_globals[i].addr);
    return 0;
}

routine *routine_by_name(const char *name)
{
    u32 i;
    if (names_get(fn_names, fn_names_n, name, &i))
        return &routines[i];
    return NULL;
}

routine *routine_at(u32 addr)
{
    u32 s = disp_slot(addr);
    if (s < disp_slots && disp_index[s])
        return &routines[disp_index[s] - 1];
    return NULL;
}

routine *routine_containing(u32 addr)
{
    addr = (addr & 0x1fffffffu);
    if (routine_count == 0 || addr < routines[0].addr || addr >= IMAGE_BASE + disp_slots * 2)
        return NULL;
    int lo = 0, hi = routine_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (routines[mid].addr <= addr)
            lo = mid;
        else
            hi = mid - 1;
    }
    /* past the last routine's end (rounded up to the 32-byte alignment the
     * compiler gives functions) the address is data */
    if (lo == routine_count - 1 && addr >= ((routines[lo].addr + routines[lo].size + 0x1f) & ~0x1fu))
        return NULL;
    return &routines[lo];
}

uint32_t global_addr(const char *name)
{
    u32 a;
    if (names_get(gl_names, gl_names_n, name, &a))
        return a;
    return 0;
}

static char text_ring[8][96];
static int text_ring_at;

const char *global_text(uint32_t addr)
{
    const gen_global *gen_globals = prog_globals;       /* recomp (test mode): the program's */
    int gen_global_count = prog_global_count;
    addr &= 0x1fffffffu;
    int lo = 0, hi = gen_global_count - 1;
    if (hi < 0 || addr < gen_globals[0].addr)
        return NULL;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (gen_globals[mid].addr <= addr)
            lo = mid;
        else
            hi = mid - 1;
    }
    u32 d = addr - gen_globals[lo].addr;
    if (d > 0x10000)
        return NULL;
    char *b = text_ring[text_ring_at++ & 7];
    if (d)
        snprintf(b, sizeof text_ring[0], "%s+0x%x", gen_globals[lo].name, d);
    else
        snprintf(b, sizeof text_ring[0], "%s", gen_globals[lo].name);
    return b;
}

const char *addr_text(u32 addr)
{
    char *b = text_ring[text_ring_at++ & 7];
    routine *r = routine_containing(addr);
    if (r) {
        u32 d = (addr & 0x1fffffffu) - r->addr;
        if (d)
            snprintf(b, sizeof text_ring[0], "%s+0x%x", r->name, d);
        else
            snprintf(b, sizeof text_ring[0], "%s", r->name);
    } else {
        snprintf(b, sizeof text_ring[0], "%08x", addr);
    }
    return b;
}

void disp_set_flag(routine *r, u8 flag, int on)
{
    u32 s = disp_slot(r->addr);
    if (s >= disp_slots)
        return;
    if (on)
        disp_flags[s] |= flag;
    else
        disp_flags[s] &= (u8)~flag;
}

void disp_set_flag_all(u8 flag, int on)
{
    for (int i = 0; i < routine_count; i++)
        disp_set_flag(&routines[i], flag, on);
}

int disp_replace(const char *name, host_fn fn, const char *why)
{
    routine *r = routine_by_name(name);
    if (!r) {
        fprintf(stderr, "dispatch: no routine named %s (replacement not installed)\n", name);
        return -1;
    }
    r->kind = RK_HOST;
    r->host = fn;
    r->why = why;
    disp_set_flag(r, DF_HOST, 1);
    return 0;
}

int disp_hook(const char *name, host_fn fn, const char *why)
{
    routine *r = routine_by_name(name);
    if (!r) {
        fprintf(stderr, "dispatch: no routine named %s (hook not installed)\n", name);
        return -1;
    }
    r->hook = fn;
    if (!r->why)
        r->why = why;
    disp_set_flag(r, DF_HOST, 1);
    return 0;
}

u32 arg_stack(sh4 *c, int i)
{
    return g_r32(c->r[15] + 4u * (u32)i);
}

/* ---- from the interpreter ---- */

void disp_enter(sh4 *c, u32 target, int kind)
{
    u32 s = disp_slot(target);
    u32 idx = disp_index[s];
    if (!idx) {
        disp_flags[s] = 0;
        return;
    }
    routine *r = &routines[idx - 1];
    u8 f = disp_flags[s];
    if ((f & DF_LOG) && !obs_call(c, r, kind)) {
        /* a jump back to the routine's own first instruction: a loop, not
         * an entry. (Hooks still see the ones that come by a jump, as they
         * do without the observer.) */
        if (kind == DISP_FALL)
            return;
    } else if (obs_sandbox) {
        r->hcalls++;                    /* run by the harness, in a sandbox: not an entry of the game's */
    } else {
        r->calls++;
    }
    if ((f & DF_FIRST) && !obs_sandbox) {
        r->seen = 1;
        r->first_frame = game_vblanks();
        r->first_mode = MODE_PACK(game_mode_main(), game_mode_sub());
        r->first_insn = c->insns;
        disp_flags[s] = f &= (u8)~DF_FIRST;
        machine_first_seen(c, r);
    }
    if (kind == DISP_EXCEPTION)
        r->exc = 1;
    if (f & DF_TRACE)
        obs_trace_call(c, r, kind);
    if (f & DF_BREAK)
        machine_break(c, r);
    if (f & DF_HOST) {
        if (r->hook)
            r->hook(c, r);
        if (r->kind == RK_HOST && !c->stop) {
            u32 saved = _mm_getcsr();
            _mm_setcsr(0x1f80);
            r->host_calls++;
            u32 ret_to = c->pr;
            r->host(c, r);
            _mm_setcsr(saved);
            if (c->pc == target) {      /* the replacement did not redirect */
                obs_host_returned(c, r);
                c->pc = ret_to;
                if (disp_slot(ret_to) >= disp_slots)
                    disp_outside(c, ret_to, DISP_RETURN);
            }
        }
    }
}

void disp_outside(sh4 *c, u32 target, int kind)
{
    if (target == SH4_RETURN_MAGIC) {
        c->stop = STOP_RETURN;
        c->next_event = 0;
        return;
    }
    if (machine_outside(c, target, kind))
        return;
    if (mem_ptr(target))
        return;                         /* code in RAM outside the image: let it run */
    c->stop = STOP_BADPC;
    c->stop_arg = target;
    c->next_event = 0;
}

void disp_enter_exception(sh4 *c, u32 pc)
{
    disp_exc_depth++;
    obs_exception_level(disp_exc_depth);
    u32 s = disp_slot(pc);
    if (s < disp_slots) {
        if (disp_flags[s])
            disp_enter(c, pc, DISP_EXCEPTION);
    } else {
        disp_outside(c, pc, DISP_EXCEPTION);
    }
}

void disp_leave_exception(sh4 *c)
{
    (void)c;
    if (disp_exc_depth > 0)
        disp_exc_depth--;
    obs_exception_level(disp_exc_depth);
}

void disp_trapa(sh4 *c, u32 imm)
{
    machine_trapa(c, imm);
}

u32 guest_call(sh4 *c, u32 addr, u32 a0, u32 a1, u32 a2, u32 a3)
{
    u32 saved_pr = c->pr, saved_pc = c->pc;
    u64 saved_next = c->next_event;
    int saved_stop = c->stop;
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[7] = a3;
    c->pr = SH4_RETURN_MAGIC;
    c->stop = 0;
    c->pc = addr;
    u32 s = disp_slot(addr);
    if (s < disp_slots && disp_flags[s])
        disp_enter(c, addr, DISP_CALL);
    while (!c->stop) {
        c->next_event = c->cycles + 100000000ull;
        sh4_run(c);
    }
    u32 r0 = c->r[0];
    if (c->stop == STOP_RETURN) {
        c->stop = saved_stop;
        c->pc = saved_pc;
    }
    c->pr = saved_pr;
    c->next_event = saved_next < c->cycles ? 0 : saved_next;
    return r0;
}
