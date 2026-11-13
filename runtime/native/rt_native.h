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

/* test mode: keeps the machine's state as it is now, before the game's
 * first instruction (the BIOS stand-in starts the other program from it:
 * rt_native.c); 0 on success, else the Test switch stops the game as before */
int rt_native_power_on(void);

/* the program running now: PROGRAM_MAIN or PROGRAM_TEST (tables.h) */
int rt_native_program(void);
extern unsigned rt_native_switches;     /* how many times the BIOS stand-in started the other program */

/* the game window's Restart (its menu, runtime/host/menu.c): from the frame
 * hook (machine_host_frame) only, the machine starts the game again from its
 * power-on state, the way the test program's EXIT does, keeping what the
 * cabinet keeps. Does not return; nothing happens when no power-on state was
 * kept or the run is not on */
void rt_native_restart(void);

/* the names of the decompilation the machine looks up (tables.c): 0 when all
 * are there, else says which are gone */
int rt_native_names_check(void);

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
