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

/* the two programs (generated) */
extern const rt_routine M_routines[];
extern const unsigned M_routine_count;
extern const rt_routine T_routines[];
extern const unsigned T_routine_count;

enum {
    RT_FAULT_NO_CODE = 1,   /* a call or jump to an address no routine starts at */
    RT_FAULT_BAD_RETURN,    /* a callee came back somewhere else */
    RT_FAULT_ILLEGAL,       /* an illegal instruction, or one in a delay slot */
};

/* The host's: stop running translated code (set c->stop and a c->pc no
 * caller expects, so every routine on the C stack returns at once) and say
 * why. */
void rt_host_fault(sh4 *c, int what, u32 at, u32 detail);

#endif
