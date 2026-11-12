/* The game's waits, turned into jumps of virtual time.
 *
 * These are hooks, not replacements: the routine still runs as the game's
 * own bytes; the hook only moves virtual time to the next scheduled event
 * (an interrupt source, the end of a block read, the sound driver's turn),
 * because nothing can change before it. Without them the game would spin
 * through millions of instructions per frame doing nothing, as it does on
 * the real machine.
 */
#include "hle.h"
#include "machine.h"

static routine *r_render_done_wait, *r_dimm_fclose;

/* The 3D library calls this (empty) routine in every turn of its wait
 * loops: while a frame's lists are sent, while a render finishes
 * (frame_lists_send_and_rearm and others). */
static void hook_busy_wait(sh4 *c, routine *r)
{
    (void)r;
    machine_idle_skip(c);
}

/* render_done_wait (the game's wait at the end of a frame) spins on
 * stopwatch_usec until the render-end interrupt sets a flag or 16 ms pass;
 * only that caller's calls skip time. */
static void hook_stopwatch(sh4 *c, routine *r)
{
    (void)r;
    if (r_render_done_wait && routine_containing(c->pr) == r_render_done_wait)
        machine_idle_skip(c);
}

/* dimm_fclose spins on dimm_fbusy until the file's block reads are over
 * (they end with an interrupt). */
static void hook_fbusy(sh4 *c, routine *r)
{
    (void)r;
    if (r_dimm_fclose && routine_containing(c->pr) == r_dimm_fclose)
        machine_idle_skip(c);
}

void hle_waits_install(sh4 *c)
{
    (void)c;
    r_render_done_wait = routine_by_name("render_done_wait");
    r_dimm_fclose = routine_by_name("dimm_fclose");
    disp_hook("busy_wait_hook_noop", hook_busy_wait,
              "frame wait: the 3D library's wait loops call it; virtual time jumps to the next event");
    disp_hook("stopwatch_usec", hook_stopwatch,
              "frame wait: when called from render_done_wait, virtual time jumps to the next event");
    disp_hook("dimm_fbusy", hook_fbusy,
              "file wait: when called from dimm_fclose, virtual time jumps to the next event");
}
