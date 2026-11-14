/* The game's own menu, over the picture (docs/design.md sections 21, 22):
 * Insert or Esc, a gamepad's Guide button, or View + Menu held for a second
 * open it, and the game pauses while it is open. Its tabs (Game, Picture,
 * Sound, Controls) are switched with Q and E or the shoulder buttons, or
 * clicked; the mouse chooses the row under it (menu_mouse). The
 * player's settings that can change while the game runs change at once;
 * the others are kept for the next start. The cabinet's Test switch and a
 * restart are here too, so a player with only a gamepad (the Steam Deck)
 * can reach the operator's test menu: in it the operator bar turns A and X
 * into Test and Service. A release's game also has Check for updates
 * (updates.h) and Open the launcher. Support on Ko-fi opens the author's Ko-fi page, and the
 * first start shows a notice about it, once (docs/design.md section 26).
 *
 * This file draws and keeps the menu's state (with runtime/ui); main.c
 * pauses the machine, reads the controls, and puts the drawing over the
 * picture. */
#ifndef VF4FT_MENU_H
#define VF4FT_MENU_H

#include <SDL3/SDL.h>

#include "settings.h"

#define MENU_MIN_W WINDOW_MIN_UNITS_W  /* the window's smallest size: the menu fits (settings.h) */
#define MENU_MIN_H WINDOW_MIN_UNITS_H

#define MENU_KOFI_URL "https://ko-fi.com/P5P81EV1M0"
#define MENU_KOFI_SHOWN "ko-fi.com/P5P81EV1M0"      /* as the menu shows it, to type in elsewhere */

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
    MENU_KOFI,              /* open MENU_KOFI_URL in the browser, then menu_kofi() */
    MENU_KOFI_CLOSE,        /* the same, and close it (the notice's Support on Ko-fi) */
    MENU_LAUNCHER,          /* quit, and the launcher opens (updates.h: gu_open_launcher) */
};

/* how this run started: settings that change only at the next start are
 * marked when they differ from it */
typedef struct menu_run {
    settings start;         /* the settings this run started with */
    int sound;              /* the sound chip runs: the volume applies */
    int no_device;          /* sound was on, but no audio device opened at the start */
    int test_program;       /* the test program runs now */
    const char *user_dir;   /* the player's folder (ends in a separator): the Cards tab reads cards/ there */
} menu_run;

/* opens it on s (the settings in use, changed in place) */
void menu_open(settings *s, const menu_run *run);
/* opens it as the first start's notice instead: Ko-fi, with Support on
 * Ko-fi and Close */
void menu_open_notice(settings *s, const menu_run *run);
/* what opening the Ko-fi page did (1 opened, 0 not), for the menu to say */
void menu_kofi(int opened);

/* a command; returns MENU_* */
int menu_command(int cmd);

/* the mouse (docs/design.md s31), at px, py in pixels of menu_draw's
 * target: MENU_MOUSE_MOVE chooses the row under it; MENU_MOUSE_LEFT clicks
 * (a tab, a row as MENU_OK on it, the notice's buttons, Ko-fi's badge);
 * MENU_MOUSE_RIGHT is Left on the row under it, else Back; MENU_MOUSE_WHEEL
 * moves the choice d rows (no wrapping). Returns MENU_*, or -1 when the
 * drawing did not change (before the menu is drawn too). */
enum { MENU_MOUSE_MOVE, MENU_MOUSE_LEFT, MENU_MOUSE_RIGHT, MENU_MOUSE_WHEEL };
int menu_mouse(int what, float px, float py, int d);

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
