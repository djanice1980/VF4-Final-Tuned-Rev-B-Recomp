/* The controls pages, for the launcher's Settings and the game's menu alike
 * (docs/design.md sections 69 and 72): the Gamepad page and the Keyboard
 * page, each a table with a cell for player 1 and one for player 2
 * (ui_bind_table). A cell takes the next key or button; the Gamepad page's
 * first row gives each player a controller. */
#ifndef VF4FT_CONTROLS_H
#define VF4FT_CONTROLS_H

#include <SDL3/SDL.h>

#include "settings.h"

typedef struct ctl_page {
    int pad;            /* the Gamepad page (1) or the Keyboard page (0) */
    int row, col, top;  /* the row and the player's cell chosen; the first row shown */
    int capturing;      /* the cell waits for its key or button */
} ctl_page;

/* the page to show: its first row and player 1's cell chosen */
void ctl_open(ctl_page *p, int pad);
/* how many rows: the actions, and on the Gamepad page the Controller row before them */
int ctl_count(const ctl_page *p);
/* the row chosen moves by d (it wraps), or by the wheel (it does not) */
void ctl_move(ctl_page *p, int d, int wrap);
/* player 1's cell (0) or player 2's (1) */
void ctl_side(ctl_page *p, int col);

/* Enter or A on the chosen cell: its key or button is the next one pressed
 * (ctl_event takes it); on the Controller row, the next gamepad connected
 * now that the other player was not given, and past the last, Auto */
void ctl_set(settings *s, ctl_page *p);
/* Delete or X: the cell emptied (the Controller row: Auto) */
void ctl_clear(settings *s, ctl_page *p);
/* Backspace or Y: the cell's default back (the Controller row: Auto) */
void ctl_default(settings *s, ctl_page *p);

/* An event while a cell waits (p->capturing): 0 when it is not the page's;
 * 1 when the page took it and nothing changed (the wait ended with Esc, or
 * with a pad's B on the Keyboard page); 2 when the cell changed: it has its
 * new key or button, or Delete emptied it, or Backspace gave it its default
 * back (on the Keyboard page a pad's X and Y do those too). So Esc, Delete
 * and Backspace cannot be a control's key. Every key and button event
 * should come here first while it waits. */
int ctl_event(settings *s, ctl_page *p, const SDL_Event *e);
/* what the bar under the page may offer: 0 the page's own (Set, Clear,
 * Default, the players); 1 a cell waits: leaving it, Clear and Default
 * only; 2 a cell waits and none of those can be pressed (the Gamepad page
 * worked with a gamepad: every button of it would be the cell's) */
int ctl_waiting(const ctl_page *p);

/* the table, and under it a line that says what the chosen row is for */
void ctl_draw(const settings *s, ctl_page *p, float x, float y, float w, float h, float row_h);
/* the mouse at a logical point after ctl_draw: 0 when it is not on a row;
 * else the cell under it is chosen (the row alone left of the cells), and
 * with press it is set as Enter does (2 then, 1 when only the choice moved) */
int ctl_point(settings *s, ctl_page *p, float x, float y, int press);

#endif
