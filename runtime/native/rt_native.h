/* The translated game on VF4debug's machine (runtime/machine): see
 * rt_native.c and docs/design.md, "Interrupts and timing". */
#ifndef VF4RT_NATIVE_H
#define VF4RT_NATIVE_H

#include "sh4.h"

/* builds the block table of the main program and starts watching stores into
 * its code; image is the file the translation was made from; 0 on success */
int rt_native_init(const char *image);

/* Runs the machine the way VF4debug's main loop does (the controller's
 * script, then the machine until each stop), with the game's code
 * translated: returns the last stop code once the script is over or a stop
 * ends the run. */
int rt_native_run(sh4 *c);

/* counts for the closing lines */
typedef struct rt_native_stats {
    unsigned long long slow_blocks;     /* rt_slow calls (an event inside a block, or a device brought it forward) */
    unsigned long long interp_insns;    /* instructions the interpreter ran in their place */
    unsigned long long exceptions;      /* interrupts and trapa taken */
    unsigned long long outside_calls;   /* calls and jumps to code that is not a translated routine's start */
    unsigned long long code_page_stores;    /* stores into pages that hold translated code */
    unsigned long long changed_blocks;  /* blocks the game changed, run on the interpreter */
} rt_native_stats;
extern rt_native_stats rt_stats;

#endif
