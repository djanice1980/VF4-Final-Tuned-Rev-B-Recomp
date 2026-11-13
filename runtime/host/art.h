/* Steam's artwork for the launcher's entry, made from the player's own build
 * (docs/design.md section 36): the game runs without a window (--art DIR),
 * the GPU renderer draws chosen vertical blanks at 4 x 640 x 480, and each
 * picture is cropped and scaled to Steam's sizes and saved as PNG files.
 * The run is the same on every machine (a blank cabinet, no input), so the
 * vertical blanks below show the same scenes everywhere. */
#ifndef VF4FT_ART_H
#define VF4FT_ART_H

#include <SDL3/SDL.h>

/* --art-frames LIST ("2400,3000-6000/300"): whole pictures of those
 * vertical blanks instead of the artwork, for choosing scenes. 0, or -1 */
int art_frames_parse(const char *list);

/* the vertical blanks wanted, in order (art_frames_parse's, or the
 * artwork's); n is set to how many */
const unsigned *art_frames(int *n);

/* a picture of vertical blank frame (RGBA, w x h) into dir: that frame's
 * outputs. Returns how many files were written; failures go to say */
int art_take(const char *dir, unsigned frame, const void *rgba, Uint32 w, Uint32 h, void (*say)(const char *msg));

#endif
