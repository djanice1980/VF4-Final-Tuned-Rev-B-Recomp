/* The player's settings as rows, for the launcher's Settings and the game's
 * menu alike (docs/design.md section 22): each one's name, what it does,
 * its value as a ui_row, and its change. */
#ifndef VF4FT_OPTIONS_H
#define VF4FT_OPTIONS_H

#include <stddef.h>

#include "settings.h"
#include "ui.h"

enum {
    OPT_CREDITS, OPT_CARDS, OPT_FULLSCREEN, OPT_PICTURE, OPT_FILTER, OPT_RENDERER, OPT_SCALE, OPT_SOUND,
    OPT_VOLUME, OPT_NETWORK, OPT_COUNT
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

#endif
