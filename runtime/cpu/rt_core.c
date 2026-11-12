/* What translated code calls when it leaves its own static flow: calls and
 * jumps whose target is only known at run time, and the ways a routine can
 * go wrong. Everything that depends on the machine around the CPU (events,
 * exceptions, code that is not translated) is the host's: see rt_host_* in
 * rt_dispatch.h. */
#include "rt_dispatch.h"

const rt_routine *rt_table;
unsigned rt_table_count;

const rt_routine *rt_find(const rt_routine *table, unsigned count, u32 addr)
{
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < count && table[lo].addr == addr)
        return &table[lo];
    return NULL;
}

/* P1 (0x8...) and P2 (0xa...) are windows on the same memory as P0 */
static u32 canon(u32 a)
{
    u32 region = a >> 29;
    return (region == 4 || region == 5) ? (a & 0x1fffffffu) : a;
}

/* A target through P1 or P2 (the cache routines run themselves uncached)
 * is the host's too: the code there sees its own PC in that region (in PR,
 * in mova), which the translation, made for P0, does not reproduce. */
void rt_call(sh4 *c, u32 target, u32 from)
{
    const rt_routine *r = canon(target) == target ? rt_find(rt_table, rt_table_count, target) : NULL;
    if (RT_UNLIKELY(r == NULL)) {
        rt_host_outside(c, target, from, 1);
        return;
    }
    r->fn(c);
}

void rt_jump(sh4 *c, u32 target, u32 from)
{
    /* a jump to the start of a routine is a tail call: the routine returns
     * to whatever called this one, through c->pr */
    const rt_routine *r = canon(target) == target ? rt_find(rt_table, rt_table_count, target) : NULL;
    if (RT_UNLIKELY(r == NULL)) {
        rt_host_outside(c, target, from, 0);
        return;
    }
    r->fn(c);
}

void rt_bad_return(sh4 *c, u32 expected, u32 from)
{
    if (c->stop)            /* already failing: unwinding to the host */
        return;
    rt_host_fault(c, RT_FAULT_BAD_RETURN, from, expected);
}

void rt_illegal(sh4 *c, u32 op, u32 at)
{
    rt_host_fault(c, RT_FAULT_ILLEGAL, at, op);
}

/* FPSCR changed outside translated code (the interpreter ran): the host
 * rounding mode translated code keeps in step with it */
void rt_fpscr_loaded(sh4 *c)
{
    rt_set_mxcsr_for(c->fpscr);
}

/* for harnesses that link this code next to another description of the
 * state (VF4debug's): the two must agree */
const unsigned rt_sh4_size = sizeof(sh4);
