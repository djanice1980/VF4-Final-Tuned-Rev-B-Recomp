/* The observer.
 *
 * With the observer on, the interpreter runs its second copy of the core
 * (src/sh4_exec.inc with OBS 1) and the observer sees everything: every
 * entry into a routine (call, jump, fall-through, exception) and every
 * return, on a shadow stack of its own; every store; every conditional
 * branch; every read outside RAM. From that it keeps
 *
 *   - exact counts: calls per routine and per (origin, target) edge;
 *   - which instructions of the image ran, and which way each conditional
 *     branch went;
 *   - per routine, the distinct paths (sets of branch outcomes) and return
 *     values with their counts;
 *   - a call log (JSON lines): for the calls chosen by the sampling rule,
 *     the registers by the ABI, the bytes behind pointer arguments at entry
 *     and at return, the routines called directly, the memory written by
 *     the routine and by its callees with the bytes before and after.
 *
 * The log's "hit" / "ret" / "noret" / "mode" / "note" lines keep the keys
 * of the decompilation's tools/flycast_observe.py; everything new is in
 * new keys. README.md, "The call log", lists them.
 */
#ifndef VF4_OBSERVE_H
#define VF4_OBSERVE_H

#include "sh4.h"
#include "mem.h"
#include "dispatch.h"
#include <stdio.h>

typedef struct obs_rule {
    int hits;        /* the first calls of a routine, kept whatever they show */
    int max_hits;    /* calls kept per routine in all */
    int ret;         /* unused (every kept call has its return); kept for the start line */
    int peek;        /* bytes read behind a pointer whose pointee size is not known */
    int peek_max;    /* most bytes read behind one pointer */
    int writes;      /* stores a kept call may make (with its callees) before its write list is cut */
    int quiet;       /* unused */
} obs_rule;

extern obs_rule obs_rules;
extern FILE *obs_log;

/* ---- what the interpreter's observed core uses ---- */

/* obs_map bits, one byte per halfword of the image */
#define OM_RUN   0x01    /* the instruction ran */
#define OM_ENTRY 0x02    /* a routine begins here */
#define OM_BT    0x04    /* a conditional branch here was taken */
#define OM_BN    0x08    /* a conditional branch here was not taken */

/* obs_frame flags */
#define OF_XF    0x01    /* the routine touched the back bank (xf0-xf15) itself */
#define OF_XFC   0x02    /* a routine it called directly did */

/* what every frame keeps of its own doing, captured or not: its first own
 * stores and the first routines it called, so that a call that only shows
 * itself worth keeping at its return still has them */
#define LW_N 6
#define LC_N 8
typedef struct obs_lstore {
    u32 addr, old, val;
    u32 size;
} obs_lstore;

struct obs_capture;
typedef struct obs_frame {
    u64 sig;             /* the set of the routine's branch outcomes in this call, as 64 hashed bits */
    u64 ic;              /* instruction count at entry */
    u32 flags;
    u32 ridx;            /* routine index; OBS_BARRIER for an exception boundary */
    u32 pr, sp;          /* return address and r15 at entry */
    u32 from;            /* the instruction that entered it (0: an exception) */
    u32 hw0;             /* count of reads outside RAM at entry */
    u32 n;               /* which call of the routine this is (1: the first) */
    u32 a[4], f[4];      /* r4-r7 and fr4-fr7 at entry */
    struct obs_capture *cap;   /* set when the call is being recorded in full */
    u16 mode;
    u8 kind, lvl;
    u32 spp;             /* r15 at entry as a physical address: what lies below it is the call's own stack */
    u32 own_n;           /* its own stores outside that stack, so far (exact) */
    u32 pl0;             /* count of reads of planted values at entry */
    u32 lc_more;
    u16 lc_n, clx;       /* clx: this call's place in its caller's list of callees with arguments */
    obs_lstore lw[LW_N];
    struct { u16 ridx, n; } lc[LC_N];
} obs_frame;

extern int obs_on;               /* the observed core runs */
extern int obs_jr_on;            /* stores are being journaled right now */
extern int obs_rd_on;            /* the running routine's reads of globals are being noted right now */
extern u8 *obs_map;
extern u16 *obs_slot_routine;    /* routine index + 1 of every halfword of the image (0: none) */
extern u32 obs_pc;               /* the instruction being executed (the branch, while its delay slot runs) */
extern obs_frame *obs_top;

int obs_step(sh4 *c, u32 pc, u32 slot, u8 m);     /* first run of an instruction, or a routine's entry */
void obs_store(sh4 *c, u32 addr, u32 n, u32 val); /* before a store of 1, 2 or 4 bytes to RAM */
void obs_store_block(sh4 *c, u32 addr, const u32 *words);   /* before a store-queue block of 32 bytes goes to RAM */
void obs_read(u32 addr, u32 val, u32 n);          /* a read of RAM while obs_rd_on */
void obs_computed(sh4 *c, u32 target);            /* a computed jump (jmp, braf) */
void obs_io_read(sh4 *c, u32 addr, u32 val, int size);
void obs_io_write(sh4 *c, u32 addr, u32 val, int size);
void obs_into(sh4 *c, u32 target, int kind);      /* a jump or call into the body of another routine */
void obs_outside_call(sh4 *c, u32 target);        /* a call out of the image (a BIOS service) */
void obs_on_return(sh4 *c);

static inline void obs_branch(int taken)
{
    u32 i = ((obs_pc & 0x1fffffffu) - IMAGE_BASE) >> 1;
    if (__builtin_expect(i < disp_slots, 1))
        obs_map[i] |= taken ? OM_BT : OM_BN;
    obs_top->sig |= 1ull << (((i << 1 | (u32)(taken != 0)) * 0x9e3779b1u) >> 26);
}

static inline u32 obs_r32(sh4 *c, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1)) {
        u32 v = *(u32 *)(p + (a & 0xffff));
        if (__builtin_expect(obs_rd_on, 0))
            obs_read(a, v, 4);
        return v;
    }
    u32 v = io_read(c, a, 4);
    obs_io_read(c, a, v, 4);
    return v;
}
static inline u32 obs_r16(sh4 *c, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1)) {
        u32 v = *(u16 *)(p + (a & 0xffff));
        if (__builtin_expect(obs_rd_on, 0))
            obs_read(a, v, 2);
        return v;
    }
    u32 v = io_read(c, a, 2);
    obs_io_read(c, a, v, 2);
    return v;
}
static inline u32 obs_r8(sh4 *c, u32 a)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1)) {
        u32 v = p[a & 0xffff];
        if (__builtin_expect(obs_rd_on, 0))
            obs_read(a, v, 1);
        return v;
    }
    u32 v = io_read(c, a, 1);
    obs_io_read(c, a, v, 1);
    return v;
}
/* A store to RAM. What does not go into the running routine's own stack
 * frame is counted for it, and its first few are kept in its frame; a mark
 * on a planted value goes; and while a call is being recorded the store is
 * journaled. */
#define OBS_STORE(c, a, v, n, oldexpr) do { \
        obs_frame *t_ = obs_top; \
        if (t_->spp - 1 - ((a) & 0x1fffffffu) >= 0x10000u) { \
            u32 k_ = t_->own_n++; \
            if (k_ < LW_N) { \
                t_->lw[k_].addr = (a); \
                t_->lw[k_].old = (oldexpr); \
                t_->lw[k_].val = (v); \
                t_->lw[k_].size = (n); \
            } \
            if (__builtin_expect(obs_jr_on, 0)) \
                obs_store(c, a, n, v); \
        } \
        if (__builtin_expect(mem_plant_page[((a) >> 16) & 0x1fff], 0)) \
            mem_unplant(a, n); \
    } while (0)

static inline void obs_w32(sh4 *c, u32 a, u32 v)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1)) {
        u32 *q = (u32 *)(p + (a & 0xffff));
        OBS_STORE(c, a, v, 4, *q);
        { u32 nv = (u32)v; mem_code_note(a, 4, &nv); }   /* recomp */
        *q = v;
    } else {
        obs_io_write(c, a, v, 4);
        io_write(c, a, v, 4);
    }
}
static inline void obs_w16(sh4 *c, u32 a, u32 v)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1)) {
        u16 *q = (u16 *)(p + (a & 0xffff));
        OBS_STORE(c, a, v & 0xffff, 2, *q);
        { u16 nv = (u16)v; mem_code_note(a, 2, &nv); }   /* recomp */
        *q = (u16)v;
    } else {
        obs_io_write(c, a, v, 2);
        io_write(c, a, v, 2);
    }
}
static inline void obs_w8(sh4 *c, u32 a, u32 v)
{
    u8 *p = mem_page[a >> 16];
    if (__builtin_expect(p != NULL, 1)) {
        u8 *q = p + (a & 0xffff);
        OBS_STORE(c, a, v & 0xff, 1, *q);
        { u8 nv = (u8)v; mem_code_note(a, 1, &nv); }   /* recomp */
        *q = (u8)v;
    } else {
        obs_io_write(c, a, v, 1);
        io_write(c, a, v, 1);
    }
}

/* ---- for the rest of the harness ---- */

/* turns the observer on (counts, coverage); obs_open also starts the call log */
int obs_enable(void);
int obs_open(const char *path, const char *run_name);
void obs_close(void);
/* a routine is entered; 0 when it is only a jump back to the routine's own
 * first instruction from inside it (a loop, not a call) */
int obs_call(sh4 *c, routine *r, int kind);
void obs_host_returned(sh4 *c, routine *r);
void obs_mode_check(sh4 *c);      /* once a frame: emits "mode" lines */
void obs_note(const char *what);
/* the controller changed the machine: a poke ("poke" line), or a jump of
 * the whole state (a snapshot loaded: the shadow stack is dropped) */
void obs_poke(u32 addr, u32 value, int size, const char *text);
void obs_machine_jumped(const char *why);
void obs_trace_call(sh4 *c, routine *r, int kind);
void obs_exception_level(int depth);
/* once a frame, after the controller: a "frame" line when the switches or one of the watched globals changed */
void obs_frame_line(sh4 *c);
/* a global for the frame line; 0 when the name is unknown */
int obs_frame_global(const char *name, int size);
/* a range of memory to keep with every full record of the routines a pattern names (the controller's "snap") */
int obs_snap_add(const char *name, u32 addr, int deref, u32 len, const char *pattern);
/* a sandbox (the controller's "sandbox" and "call"): what runs between begin and end is counted and
 * recorded apart from the game's own run; top: the routine a "call" is about to enter (or NULL) */
void obs_sandbox_begin(u32 id);
void obs_sandbox_end(void);
void obs_harness_call(routine *r);
extern u32 obs_sandbox;          /* the sandbox's number while one is open, else 0 */

/* a main and a sub mode (each -1 or more) in one number */
#define MODE_PACK(m, s) ((((m) + 1) << 8) | (((s) + 1) & 255))
#define MODE_MAIN(x) (((x) >> 8) - 1)
#define MODE_SUB(x) (((x) & 255) - 1)

/* game state the log lines carry */
int game_mode_main(void);
int game_mode_sub(void);
u32 game_frames(void);
u32 game_vblanks(void);
const char *game_mode_name(int main_mode, int sub_mode, char *buf, size_t n);

/* reports */
void obs_write_profile(const char *path);
void obs_write_coverage(const char *path);
void obs_write_edges(const char *path);
int obs_seen_count(void);

#endif
