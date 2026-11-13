/* The game's own menu, over the picture (docs/design.md sections 21, 22):
 * Insert or Esc, a gamepad's Guide button, or View + Menu held for a second
 * open it, and the game pauses while it is open. Its tabs (Game, Picture,
 * Sound, Controls) are switched with Q and E or the shoulder buttons. The
 * player's settings that can change while the game runs change at once;
 * the others are kept for the next start. The cabinet's Test switch and a
 * restart are here too, so a player with only a gamepad (the Steam Deck)
 * can reach the operator's test menu: in it the operator bar turns A and X
 * into Test and Service. A release's game also has Check for updates
 * (updates.h).
 *
 * This file draws and keeps the menu's state (with runtime/ui); main.c
 * pauses the machine, reads the controls, and puts the drawing over the
 * picture. */
#ifndef VF4FT_MENU_H
#define VF4FT_MENU_H

#include <SDL3/SDL.h>

#include "settings.h"

#define MENU_MIN_W 640      /* the window's smallest size: the menu fits */
#define MENU_MIN_H 400

/* what the player asks of it */
enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_OK, MENU_BACK, MENU_PREV, MENU_NEXT };

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
    settings start;         /* the settings this run started with */
    int sound;              /* the sound chip runs: the volume applies */
    int test_program;       /* the test program runs now */
} menu_run;

/* opens it on s (the settings in use, changed in place) */
void menu_open(settings *s, const menu_run *run);

/* a command; returns MENU_* */
int menu_command(int cmd);

/* 1 when s was changed since menu_open */
int menu_changed(void);

/* the menu over the whole of r's target, pw x ph pixels; backdrop is the
 * picture to show behind it (or NULL). r must be ui_init's renderer. */
void menu_draw(SDL_Renderer *r, int pw, int ph, SDL_Texture *backdrop);

/* the operator bar's height in pixels over a picture ph pixels high */
int menu_bar_height(int ph);

/* the operator bar, pw x ph pixels at r's top left */
void menu_draw_bar(SDL_Renderer *r, int pw, int ph);

#endif
