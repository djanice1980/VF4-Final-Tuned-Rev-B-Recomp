/* The player's settings as rows, for the launcher's Settings and the game's
 * menu alike (docs/design.md section 22): each one's name, what it does,
 * its value as a ui_row, and its change. */
#ifndef VF4FT_OPTIONS_H
#define VF4FT_OPTIONS_H

#include <stddef.h>

#include "settings.h"
#include "ui.h"

enum {
    OPT_CREDITS, OPT_CARDS, OPT_FULLSCREEN, OPT_WINDOW, OPT_PICTURE, OPT_FILTER, OPT_RENDERER, OPT_SCALE,
    OPT_SOUND, OPT_VOLUME, OPT_NETWORK, OPT_REGION, OPT_CARD1, OPT_CARD2, OPT_DISPLAY, OPT_GPU, OPT_LATENCY,
    OPT_PACING,
    OPT_COUNT
};

typedef struct option_info {
    const char *label;
    const char *about;      /* what it does, for the description panel */
    int at_start;           /* the game takes it only when it starts */
} option_info;

extern const option_info options[OPT_COUNT];

/* the row of option o in s: buf (n bytes) holds its value's text */
void option_row(const settings *s, int o, ui_row *row, char *buf, size_t n);

/* Left (d -1) or Right (+1) on option o */
void option_change(settings *s, int o, int d);

/* the card profiles Player 1 card and Player 2 card go through (None, then
 * these, less the other player's); the names stay the caller's */
void options_card_names(const char *const *names, int n);

/* the room on the screen the game's window is on (or will be), for Window
 * size: its Auto, and the sizes that fit (settings.h; 0 x 0: not known,
 * every size the smallest window allows is offered) */
void options_screen(const window_room *room);

/* the GPUs GPU goes through (Auto, then these; devices.h gpus_list's
 * names). The names stay the caller's */
void options_gpu_names(const char *const *names, int n);

/* GPU and Display: the whole name of the choice (a row cuts a long one
 * short), for the description panel's note: "NAME." (with "not found" or
 * "not connected" when it is not there), or NULL for Auto and the other
 * options. buf holds it (n bytes) */
const char *option_note(const settings *s, int o, char *buf, size_t n);

/* the displays Display goes through (Auto, then these; devices.h
 * displays_list's names). The names stay the caller's */
void options_display_names(const char *const *names, int n);

#endif
