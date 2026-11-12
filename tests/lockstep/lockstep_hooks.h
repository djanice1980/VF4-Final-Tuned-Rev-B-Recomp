/* Forced into the translated code of the lockstep build (gcc -include):
 * every routine entry is reported to the harness, for its coverage count. */
void lockstep_entered(unsigned addr);
#define RT_ENTRY(c, addr) lockstep_entered(addr)
