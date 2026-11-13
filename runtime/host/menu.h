/* The game's own menu, over the picture (docs/design.md section 21): Insert
 * or Esc, a gamepad's Guide button, or View + Menu held for a second open
 * it, and the game pauses while it is open. The player's settings that can
 * change while the game runs change at once; the others are kept for the
 * next start. The cabinet's Test switch and a restart are here too, so a
 * player with only a gamepad (the Steam Deck) can reach the operator's test
 * menu: in it the operator bar turns A and X into Test and Service.
 *
 * This file draws and keeps the menu's state; main.c pauses the machine,
 * reads the controls, and puts the drawing over the picture. */
#ifndef VF4FT_MENU_H
#define VF4FT_MENU_H

#include <SDL3/SDL.h>

#include "settings.h"

#define MENU_W 320          /* the panel, in its own pixels (SDL's debug font: 8 x 8) */
#define MENU_H 216
#define MENU_BAR_H 12       /* the operator bar: the panel's top 12 rows */

/* what the player asks of it */
enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_OK, MENU_BACK };

/* what the window then does */
enum {
    MENU_STAY,              /* nothing (the drawing may have changed) */
    MENU_RESUME,            /* close it: the game goes on */
    MENU_LIVE,              /* a setting that applies at once changed (picture, scaling, fullscreen, volume) */
    MENU_TEST,              /* close it and press the Test switch (the test menu), then the operator bar */
    MENU_OPERATOR,          /* close it and show the operator bar (in the test menu already) */
    MENU_RESTART,           /* close it: the game starts again from power on */
    MENU_QUIT,
};

/* how this run started: settings that change only at the next start are
 * marked when they differ from it */
typedef struct menu_run {
    int renderer, scale, cards;
    int sound;              /* the sound chip runs: the volume applies */
    int test_program;       /* the test program runs now */
} menu_run;

/* opens it on s (the settings in use, changed in place) */
void menu_open(settings *s, const menu_run *run);

/* a command; returns MENU_* */
int menu_command(int cmd);

/* 1 when s was changed since menu_open */
int menu_changed(void);

/* the panel, MENU_W x MENU_H, on r */
void menu_draw(SDL_Renderer *r);

/* the operator bar, MENU_W x MENU_BAR_H at the top of r */
void menu_draw_bar(SDL_Renderer *r);

#endif
