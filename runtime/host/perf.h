/* The frame rate display (docs/design.md section 88): how many pictures a second the window
 * shows, the time from one to the next, what the game's machine and the ray tracing took for a
 * frame, and a graph of the last pictures' times. A small panel at the window's bottom left, drawn
 * with the ui kit by the window's thread (main.c), which also tells it of each new picture. */
#ifndef VF4FT_PERF_H
#define VF4FT_PERF_H

#include <SDL3/SDL.h>

#define PERF_SPAN 120       /* the pictures the graph holds */

/* a new picture was put on the screen at ns (SDL_GetTicksNS). A gap of a second or more is a
 * pause (the menu, a loading): it is not counted, and the count starts anew after it */
void perf_picture(Uint64 ns);

/* forgets what it holds (the display was switched on) */
void perf_reset(void);

/* the panel's size in pixels in a window ph pixels high; graph 1: with the graph under the numbers */
void perf_size(int graph, int ph, int *w, int *h);

/* draws the panel at the ui kit's renderer's top left, w x h pixels as perf_size gave them.
 * game_ms: what the machine took for its last frame; rays_ms: what the ray tracing took for its
 * last (below 0: it is off). The numbers are written anew four times a second, the graph each time */
void perf_draw(int graph, int w, int h, float game_ms, float rays_ms);

#endif
