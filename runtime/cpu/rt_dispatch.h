/* The table of translated routines: one row per routine of a program,
 * sorted by address (generated as <prefix>_dispatch.c), and what the host
 * (the game runtime, or a test harness) provides to translated code. */
#ifndef VF4RT_DISPATCH_H
#define VF4RT_DISPATCH_H

#include <stddef.h>
#include "sh4rt.h"

typedef struct rt_routine {
    u32 addr;
    rt_fn fn;
    const char *name;
} rt_routine;

/* the routine starting exactly at addr, or NULL */
const rt_routine *rt_find(const rt_routine *table, unsigned count, u32 addr);

/* the program running now: rt_call and rt_jump look targets up here */
extern const rt_routine *rt_table;
extern unsigned rt_table_count;

/* every block start of a program and its length in instructions */
typedef struct rt_block {
    u32 addr;
    u32 n;
} rt_block;

/* a pool word the translation compiled in as a constant, and the block that
 * reads it: word bit 0 is set for a 4-byte word (the address is even) */
typedef struct rt_lit {
    u32 word;
    u32 block;
} rt_lit;

/* the two programs (generated) */
extern const rt_routine M_routines[];
extern const unsigned M_routine_count;
extern const rt_block M_blocks[];
extern const unsigned M_block_count;
extern const rt_lit M_lits[];
extern const unsigned M_lit_count;
extern const rt_routine T_routines[];
extern const unsigned T_routine_count;
extern const rt_block T_blocks[];
extern const unsigned T_block_count;
extern const rt_lit T_lits[];
extern const unsigned T_lit_count;

enum {
    RT_FAULT_NO_CODE = 1,   /* a call or jump to an address no routine starts at */
    RT_FAULT_BAD_RETURN,    /* a callee came back somewhere else */
    RT_FAULT_ILLEGAL,       /* an illegal instruction, or one in a delay slot */
};

/* FPSCR changed outside translated code */
void rt_fpscr_loaded(sh4 *c);

/* The host's: translated code cannot go on (say why and stop the run). */
void rt_host_fault(sh4 *c, int what, u32 at, u32 detail);

/* The host's: a call (is_call) or jump to an address no translated routine
 * starts at: run what is there until it returns, leaving c->pc where it
 * returned to (the runtime: the BIOS stand-in's services and the
 * interpreter; a test harness: a fault). */
void rt_host_outside(sh4 *c, u32 target, u32 from, int is_call);

#endif
