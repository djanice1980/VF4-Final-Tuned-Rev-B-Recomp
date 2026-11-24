/* The game's own menu, over the picture (docs/design.md sections 21, 22):
 * Insert or Esc, a gamepad's Guide button, or View + Menu held for a second
 * open it, and the game pauses while it is open. Its tabs (Game, Cards,
 * Picture, Sound, Gamepad, Keyboard) are switched with Q and E or the shoulder buttons, or
 * clicked; the mouse chooses the row under it (menu_mouse). The Cards tab
 * opens the card editor in the tabs' place (docs/design.md section 42). The
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

#include "cards.h"
#include "settings.h"

#define MENU_MIN_W WINDOW_MIN_UNITS_W  /* the window's smallest size: the menu fits (settings.h) */
#define MENU_MIN_H WINDOW_MIN_UNITS_H

#define MENU_KOFI_URL "https://ko-fi.com/P5P81EV1M0"
#define MENU_KOFI_SHOWN "ko-fi.com/P5P81EV1M0"      /* as the menu shows it, to type in elsewhere */

/* what the player asks of it. MENU_ESCAPE: Esc, Insert or Guide (it closes
 * the menu, or the card editor or its note first); MENU_ERASE: Backspace
 * (back to the game; the editor's Undo; a letter less in the note);
 * MENU_UNDO: a pad's Y (the editor's Undo); MENU_CLEAR: a pad's X or Delete
 * (a letter less in the note). On the Gamepad and Keyboard tabs (section
 * 72) MENU_OK sets the chosen cell, MENU_CLEAR empties it, and MENU_UNDO
 * and MENU_ERASE put its default back */
enum {
    MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_OK, MENU_BACK, MENU_PREV, MENU_NEXT, MENU_ESCAPE, MENU_ERASE,
    MENU_UNDO, MENU_CLEAR
};

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
    /* the card editor's (main.c, docs/design.md section 42): a card as the
     * menu shows it, with a Saved edit that is not in its file yet (1; 0 no
     * file; -1 the file is not a card; *held: a game is using it), and an
     * edit Saved (the fields that differ between from and to; fresh: a card
     * never used or not made yet, which becomes a ready card) */
    int (*card_peek)(const char *name, Uint8 *card, int *held);
    void (*card_edit)(const char *name, const card_fields *from, const card_fields *to, int fresh);
    float refresh;          /* the window's screen's refresh rate in Hz (0 not known; section 44) */
    int refresh_own;        /* that rate is the game's own count of its presents, not the system's report:
                             * it is said as a guess (section 61) */
    const char *gpu_now;    /* the GPU the GPU renderer draws on now ("": none, Software); kept up to date */
    int deck;               /* settings_steam_deck() */
    int session;            /* a two-PC game (session.h): no test menu, restart or card changes */
    /* the host of a two-PC game: its join code (docs/design.md section 50), for the Game tab's Join
     * code row: whether another PC can join and with what code in buf (MENU_CODE_*, as session.h's
     * session_code), and a new code. NULL: no such row */
    int (*join_code)(char *buf, size_t n);
    void (*join_code_new)(void);
    /* a two-PC game: what latency optimization is doing now, for its row's note (buf, n bytes; ""
     * nothing to say). NULL: no note */
    void (*latency_now)(char *buf, size_t n);
    /* Frame pacing's note in a two-PC game (the host's is used): text into buf, or NULL */
    void (*pacing_now)(char *buf, size_t n);
} menu_run;
enum { MENU_CODE_NONE, MENU_CODE_SHOWN, MENU_CODE_GONE };

/* opens it on s (the settings in use, changed in place) */
void menu_open(settings *s, const menu_run *run);
/* opens it as a notice instead: MENU_NOTICE_KOFI, the first start's, with
 * Support on Ko-fi and Close; MENU_NOTICE_REFRESH, the screen's refresh
 * rate (docs/design.md section 44), with OK */
enum { MENU_NOTICE_KOFI = 1, MENU_NOTICE_REFRESH };
void menu_open_notice(settings *s, const menu_run *run, int kind);
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

/* 1 while a cell of the Gamepad or Keyboard tab waits for its button or
 * key: the window then gives every key, button and axis event to
 * menu_capture first, and moves nothing by a held D-pad or stick. It
 * returns 0 when the event is not the menu's (the window goes on with it),
 * 1 when it was (nothing else is to be done with it), 2 when a control
 * changed with it (as MENU_LIVE) */
int menu_capturing(void);
int menu_capture(const SDL_Event *e);

/* 1 while the card editor's ring name note is up: the window then sends
 * the typed text (menu_text), and of the keys only Enter, Esc, Backspace,
 * Left and Right (as MENU_OK, _ESCAPE, _ERASE, _LEFT, _RIGHT) */
int menu_typing(void);
void menu_text(const char *utf8);

/* the menu over the whole of r's target, pw x ph pixels; backdrop is the
 * picture to show behind it (or NULL). r must be ui_init's renderer. */
void menu_draw(SDL_Renderer *r, int pw, int ph, SDL_Texture *backdrop);

/* the operator bar's height in pixels over a picture ph pixels high */
int menu_bar_height(int ph);

/* the operator bar, pw x ph pixels at r's top left */
void menu_draw_bar(SDL_Renderer *r, int pw, int ph);

/* the two-PC game's note (session.h: waiting for the other player, why it
 * ended), a strip of menu_bar_height like the operator bar */
void menu_draw_note(SDL_Renderer *r, int pw, int ph, const char *text);

#endif
