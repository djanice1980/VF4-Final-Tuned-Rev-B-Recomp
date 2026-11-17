/* The machine's pace from the screen's refresh (docs/design.md section 58).
 *
 * The machine draws 60 frames a second and the window presents on the
 * screen's refresh, taking the newest frame each time. Paced by a timer of
 * its own, the machine drifts against the screen: where a frame comes too
 * close to the moment a present takes one, frames are shown twice and
 * others never. So the machine follows the screen when it can: the window's
 * thread says when each present took its frame (vsync_take), a filter keeps
 * the refresh's period and phase from those, and the machine's thread starts
 * each frame so that it is handed over halfway between two of those moments
 * (vsync_next): as far from both as it can be, however long this PC takes
 * over a frame. Where the screen refreshes k times to a frame (120 Hz: 2),
 * only the first present of each k takes a new frame.
 *
 * It follows only a steady refresh within 0.25% of 60 Hz or a multiple of
 * it (59.94 and 120 are; 144 and 75 are not). Otherwise, and while nothing
 * is presented (the window hidden or minimized), the caller's own timer
 * paces the machine as before. Pacing never changes the run. */
#ifndef VF4FT_VSYNC_H
#define VF4FT_VSYNC_H

#include <SDL3/SDL.h>

/* nothing is known any more (the start; the window on another screen) */
void vsync_reset(void);

/* window thread: a present takes the newest frame now (ns, SDL_GetTicksNS).
 * 0 when this one is to show the frame before again: the machine follows the
 * screen, the screen refreshes several times to a frame, and this is not
 * the first of them */
int vsync_take(Uint64 ns);

/* machine thread, once a frame when it is paced. due: when this frame was
 * due (ns); frame_ns: the machine's own period; lead_ns: how long after its
 * start a frame is handed over (the middle of what it has been lately);
 * fresh: the pace starts again from now (after a pause, or far behind).
 * Returns when the next frame is due, following the screen; 0 when the
 * screen cannot be followed (the caller's timer goes on from due, and
 * vsync_take takes every frame) */
Uint64 vsync_next(Uint64 now, Uint64 due, Uint64 frame_ns, Uint64 lead_ns, int fresh);

/* machine thread: this frame is not paced by the screen; why, for the log
 * (a string that stays: "a two-PC game") */
void vsync_free(const char *why);

/* what paces the machine, for the log: "the screen's refresh (59.94 Hz)",
 * or "the game's own timer (...)" with why */
void vsync_describe(char *buf, size_t n);

#endif
