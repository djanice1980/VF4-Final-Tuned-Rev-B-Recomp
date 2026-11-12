/* The view: a picture of what the game sends to the graphics hardware,
 * drawn by the harness so that a person can watch a run. The game's state
 * does not depend on it; the one thing it writes into the machine is the
 * picture of a pass the game renders into a texture, in texture memory,
 * which the game never reads. See view.c. */
#ifndef VF4_VIEW_H
#define VF4_VIEW_H

#include "sh4.h"
#include <stdio.h>

extern int view_on;

/* the command line: 0 when the option is not the view's, 1 when it was
 * taken, 2 when it was taken with its value, -1 when the value is bad */
int view_option(const char *option, const char *value);
int view_init(void);
void view_close(void);
void view_stats(FILE *f);
/* the script's "picture FILE": the next frame the game renders goes there */
void view_picture(const char *path);

/* called from hw_gfx.c, only while view_on is set */
void view_list_segment(sh4 *c, u32 addr, u32 len);       /* a display list segment the ELAN is told to run */
void view_reg_write(sh4 *c, int chip, u32 off, u32 v);   /* a write to a rendering chip's core register */
void view_frame(sh4 *c);                                 /* once per vertical blank */
/* a rendering chip's core register as it stands (hw_gfx.c) */
u32 view_core_reg(int chip, u32 off);

#endif
