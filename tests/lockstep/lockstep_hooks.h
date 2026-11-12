/* Forced into the translated code of the lockstep build (gcc -include):
 * every routine entry is reported to the harness, for its coverage count;
 * the harness has no hooks to run (VF4debug's dispatcher is not consulted),
 * so falling into a routine is a plain call, and an rte has no bookkeeping. */
void lockstep_entered(unsigned addr);
#define RT_ENTRY(c, addr) lockstep_entered(addr)
#define RT_FALL(c, fn, a) fn(c)
#define RT_RTE(c) ((void)0)
