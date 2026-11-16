/* The Wayland compositor going away (docs/design.md sections 53 and 54).
 * Shared by the launcher (runtime/launcher) and the game (runtime/host/main.c).
 *
 * A window's frame can wait forever once its compositor has gone: SDL's
 * OpenGL present waits for a frame callback that never comes (section 53),
 * and a Vulkan swapchain's acquire waits for a buffer the compositor never
 * gives back (section 54). The thread waiting cannot get out, so a thread of
 * this file's (the watch) sleeps on the connection's socket. Once the
 * compositor has closed it, a stretch the main thread marked (compositor_busy)
 * still going COMPOSITOR_STUCK_MS later ends the process there: the caller's
 * stuck function says why and does what must be done first, on the watch's
 * thread, and the process ends with the code it returns (_exit: nothing else
 * of the process's runs). The main thread, should it come out of that stretch
 * after all, waits there for the end. Wayland only: elsewhere these do nothing,
 * and compositor_gone says 0. */
#ifndef VF4FT_COMPOSITOR_H
#define VF4FT_COMPOSITOR_H

#define COMPOSITOR_STUCK_MS 3000

typedef void (*compositor_say_fn)(const char *fmt, ...);
typedef int (*compositor_stuck_fn)(void);

/* the watch starts: once the window is there (SDL's Wayland connection).
 * say logs (from either thread); stuck is called on the watch's thread */
void compositor_watch_start(compositor_say_fn say, compositor_stuck_fn stuck);

/* the watch ends (the program closes as usual) */
void compositor_watch_end(void);

/* main thread: going into (1) or out of (0) a stretch that may never finish
 * once the compositor has gone (a frame being drawn and presented, the window
 * being closed). Not nested */
void compositor_busy(int on);

/* the compositor has gone: libwayland says the connection has failed (*err:
 * its error number) or the compositor has closed it (*err: 0). 0: it is there
 * (or this is not Wayland). err may be NULL */
int compositor_gone(int *err);

#endif
