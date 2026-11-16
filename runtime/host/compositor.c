/* The Wayland compositor going away: the watch (compositor.h; docs/design.md
 * sections 53 and 54). The launcher's since section 53, shared with the game
 * since section 54. */
#include <SDL3/SDL.h>

#include "compositor.h"

#ifdef _WIN32

void compositor_watch_start(compositor_say_fn say, compositor_stuck_fn stuck)
{
    (void)say;
    (void)stuck;
}

void compositor_watch_end(void)
{
}

void compositor_busy(int on)
{
    (void)on;
}

int compositor_gone(int *err)
{
    if (err)
        *err = 0;
    return 0;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* SDL_GetTicks() as the marked stretch started, | 1 (0: none; TAKEN: the
 * watch is ending the process) */
static SDL_AtomicU32 busy_since;
#define TAKEN 0xfffffffeu   /* even: never a stamp */

static int way_fd = -1;             /* the Wayland connection's socket (-1: not Wayland, or not known) */
static int watch_wake[2] = {-1, -1};    /* a byte written: the watch ends */
static SDL_AtomicInt watch_stop;
static SDL_Thread *watch_thread;
static compositor_stuck_fn stuck_fn;

/* SDL's Wayland connection (NULL: not Wayland) */
static void *way_display(void)
{
    return SDL_GetPointerProperty(SDL_GetGlobalProperties(), SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, NULL);
}

/* libwayland's function NAME (NULL: not there). SDL's Wayland driver has
 * loaded the library already, so this is the same copy; kept open */
static SDL_FunctionPointer way_func(const char *name)
{
    static SDL_SharedObject *so;
    static int looked;
    if (!looked) {
        looked = 1;
        so = SDL_LoadObject("libwayland-client.so.0");
    }
    return so ? SDL_LoadFunction(so, name) : NULL;
}

/* the compositor has closed the connection: its socket has hung up (or has an error) */
static int way_hung_up(void)
{
    struct pollfd p = {.fd = way_fd, .events = 0};
    return way_fd >= 0 && poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR));
}

/* the watch's thread: asleep until the socket hangs up or the program
 * closes. poll with no events asked for wakes for a hang-up or an error
 * only, never for the compositor's messages. It takes no lock of
 * libwayland's: a thread stuck in libwayland may hold one */
static int SDLCALL watch_run(void *unused)
{
    (void)unused;
    struct pollfd p[2] = {{.fd = way_fd, .events = 0}, {.fd = watch_wake[0], .events = POLLIN}};
    for (;;) {
        int n = poll(p, 2, -1);
        if (SDL_GetAtomicInt(&watch_stop) || (n < 0 && errno != EINTR) || (n > 0 && (p[0].revents & POLLNVAL)))
            return 0;
        if (n > 0 && (p[0].revents & (POLLHUP | POLLERR)))
            break;
    }
    /* the compositor has closed it: the program closes as usual, unless a marked stretch never ends */
    while (!SDL_GetAtomicInt(&watch_stop)) {
        /* signed: a stretch started in this millisecond can read 1 ms ahead (the | 1) */
        Uint32 t = SDL_GetAtomicU32(&busy_since);
        if (t && t != TAKEN && (Sint32)((Uint32)SDL_GetTicks() - t) >= COMPOSITOR_STUCK_MS &&
            SDL_CompareAndSwapAtomicU32(&busy_since, t, TAKEN))
            _exit(stuck_fn());     /* the main thread is still in it: it never goes on from here */
        SDL_Delay(100);
    }
    return 0;
}

void compositor_watch_start(compositor_say_fn say, compositor_stuck_fn stuck)
{
    void *d = way_display();
    if (!d || watch_thread)
        return;
    stuck_fn = stuck;
    int (*get_fd)(void *) = (int (*)(void *))way_func("wl_display_get_fd");
    if (!get_fd) {
        say("window: libwayland's wl_display_get_fd not found: a frame that never finishes when the "
            "compositor goes away is not noticed");
        return;
    }
    way_fd = get_fd(d);
    if (pipe(watch_wake) != 0) {
        say("window: the compositor's watch cannot start (%s)", strerror(errno));
        return;
    }
    fcntl(watch_wake[0], F_SETFD, FD_CLOEXEC);     /* not in the processes the program starts */
    fcntl(watch_wake[1], F_SETFD, FD_CLOEXEC);
    watch_thread = SDL_CreateThread(watch_run, "compositor watch", NULL);
    if (!watch_thread)
        say("window: the compositor's watch cannot start (%s)", SDL_GetError());
}

void compositor_watch_end(void)
{
    if (!watch_thread)
        return;
    SDL_SetAtomicInt(&watch_stop, 1);
    ssize_t w = write(watch_wake[1], "x", 1);
    (void)w;
    SDL_WaitThread(watch_thread, NULL);
    watch_thread = NULL;
    close(watch_wake[0]);
    close(watch_wake[1]);
}

void compositor_busy(int on)
{
    Uint32 was = SDL_SetAtomicU32(&busy_since, on ? ((Uint32)SDL_GetTicks() | 1) : 0);
    if (was == TAKEN) {
        /* the watch is ending the process (stuck_fn, then _exit): nothing more here */
        SDL_SetAtomicU32(&busy_since, TAKEN);
        for (;;)
            SDL_Delay(1000);
    }
}

int compositor_gone(int *err)
{
    static int (*get_error)(void *);
    static int looked;
    if (err)
        *err = 0;
    void *d = way_display();
    if (!d)
        return 0;
    if (!looked) {
        looked = 1;
        get_error = (int (*)(void *))way_func("wl_display_get_error");
    }
    int e = get_error ? get_error(d) : 0;
    if (err)
        *err = e;
    return e || way_hung_up();
}

#endif
