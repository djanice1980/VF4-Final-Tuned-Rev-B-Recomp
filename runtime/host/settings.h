/* The player's settings (M6, docs/design.md section 10): user/settings.txt
 * beside the game, a text file of "name = value" lines that the launcher's
 * Settings page writes and the game reads when it starts. Shared by the
 * game (runtime/host/main.c) and the launcher (runtime/launcher). Unknown
 * names and bad values are skipped with a message, so an old file or a
 * hand edit never stops the game; the defaults fill in what is missing. */
#ifndef VF4FT_SETTINGS_H
#define VF4FT_SETTINGS_H

#include <stddef.h>

#define SETTINGS_FILE "user/settings.txt"

/* what a control does in the game; the game's own three buttons, the usual
 * pairs on one button, start and coin, and the cabinet's Test and Service
 * switches (the operator's: Test starts the game's test mode, where Service
 * moves and Test chooses) */
enum {
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
    ACT_GUARD, ACT_PUNCH, ACT_KICK,
    ACT_THROW,          /* P+G */
    ACT_PK,             /* P+K */
    ACT_PKG,            /* P+K+G */
    ACT_KG,             /* K+G (the PlayStation 2 version's R2) */
    ACT_START, ACT_COIN,
    ACT_TEST, ACT_SERVICE,
    ACT_COUNT
};

#define BIND_MAX 4          /* inputs one action can have */
#define CARD_NAME_MAX 32    /* a card profile's name, with its 0 (user/cards/NAME.card) */
#define BIND_NAME 24        /* an SDL key or gamepad button name */
#define GPU_NAME_MAX 128    /* a GPU's name (Vulkan's), with its 0: a longer one is cut */
#define DISPLAY_NAME_MAX 128    /* a display's name (SDL's), with its 0 */

typedef struct binding {
    char name[BIND_MAX][BIND_NAME];     /* SDL_GetScancodeName / SDL_GetGamepadStringForButton (or a
                                         * trigger: "lefttrigger", "righttrigger"); "" unused */
} binding;

enum { COINS_FREE_PLAY, COINS_ARCADE };
enum { FULLSCREEN_AUTO = -1, FULLSCREEN_OFF = 0, FULLSCREEN_ON = 1 };
enum { PICTURE_4_3, PICTURE_16_10, PICTURE_16_9, PICTURE_21_9, PICTURE_STRETCH, PICTURES };
/* the board's region: the country number the BIOS gives the game (its board
 * settings, docs/design.md section 41) */
enum { REGION_JAPAN, REGION_USA, REGION_EXPORT, REGION_KOREA, REGION_AUSTRALIA, REGIONS };
enum { WINDOW_AUTO = 0, WINDOW_MIN = 240, WINDOW_MAX = 4320 };
enum { FILTER_SMOOTH, FILTER_SHARP };
enum { RENDERER_GPU, RENDERER_SOFTWARE };
/* latency optimization in a two-PC game (rollback, docs/design.md section
 * 57): session.h's SESSION_ROLLBACK_*, in that order */
enum { LATENCY_AUTO, LATENCY_ON, LATENCY_OFF, LATENCIES };
/* Frame pacing (docs/design.md sections 58 and 59): how the game's 60 frames
 * a second meet the screen's refresh. Game clock: the game keeps its own
 * clock and each frame is put on a refresh; Screen refresh: the game follows
 * a screen at 60 Hz or a multiple of it; Off: the newest frame at each
 * refresh, as before 0.5.12 */
enum { PACING_GAME, PACING_SCREEN, PACING_OFF, PACINGS };

typedef struct settings {
    int coins;              /* COINS_*: free play is the default */
    int fullscreen;         /* FULLSCREEN_*: auto is on for the Steam Deck and Big Picture */
    int window;             /* the game's window: WINDOW_AUTO, or its height in pixels (the width is the
                             * picture's shape: settings_window_size) */
    int picture;            /* PICTURE_* */
    int filter;             /* FILTER_* */
    int renderer;           /* RENDERER_*: the GPU (Vulkan) by default, the software picture when it cannot */
    char gpu[GPU_NAME_MAX]; /* the GPU the GPU renderer draws on, by its name (devices.h); "" Auto: SDL's
                             * choice. One not there at the start: Auto for that run, kept for the next */
    char display[DISPLAY_NAME_MAX]; /* the display the game opens on, by its name (devices.h); "" Auto:
                                     * where the system puts it. One not connected: Auto, kept as for gpu */
    int scale;              /* the GPU renderer's resolution: 1..4 times 640 x 480 */
    int sound;              /* 1 on */
    int volume;             /* 0..100 (%), the game's menu changes it while it plays */
    int cards;              /* 1: the cabinet has its two card units */
    char card[2][CARD_NAME_MAX];    /* the card profile in each player's unit: user/cards/NAME.card
                                     * (one not there yet: a card never used, kept under that name
                                     * once the game has written it); "" none (the unit stays empty) */
    int network;            /* 1: the game's own NETWORK setting on; 0 off (the default: no network check at start) */
    int region;             /* REGION_*: the board's region, Japan by default (the game reads it as it starts) */
    int latency;            /* LATENCY_*: Auto by default; this PC's own choice in a two-PC game */
    int pacing;             /* PACING_*: Game clock by default */
    binding key[2][ACT_COUNT];      /* keyboard, player 1 and player 2 */
    binding pad[ACT_COUNT];         /* every gamepad (the first is player 1, the second player 2) */
} settings;

extern const char *const settings_action_name[ACT_COUNT];   /* "up", "guard", "throw", ... */
extern const char *const settings_action_text[ACT_COUNT];   /* "Up", "Guard", "Throw (P+G)", ... */

void settings_defaults(settings *s);

/* The Steam Deck (docs/design.md section 44): STEAM_DECK_NONE, or a Deck:
 * the LCD model (its firmware's product name Jupiter), the OLED (Galileo),
 * or one whose model is not known (Steam's SteamDeck=1 alone). The product
 * name is /sys/class/dmi/id/product_name, read under Proton through Wine's
 * Z: drive; it is read once. */
enum { STEAM_DECK_NONE, STEAM_DECK_UNKNOWN_MODEL, STEAM_DECK_LCD, STEAM_DECK_OLED };
int settings_steam_deck(void);
extern const char *const settings_steam_deck_name[4];  /* "", "Steam Deck", "Steam Deck LCD", "Steam Deck OLED" */
/* for tests: the product name from this file instead (before the first
 * settings_steam_deck) */
void settings_steam_deck_test(const char *product_file);

/* 1 when the window is to be fullscreen: Fullscreen on, or auto on a Steam
 * Deck or in Steam's Big Picture (SteamTenfoot=1). The game's window and the
 * launcher's both follow it */
int settings_fullscreen_wanted(const settings *s);

/* 1 when a screen refreshing hz times a second shows the game's 60
 * pictures a second unevenly: hz known and not within 1 Hz of a multiple of
 * 60 (90 on a Steam Deck OLED, 144, 75, 50; not 59.94, 120 or 240) */
int settings_refresh_uneven(float hz);

/* The screen's refresh rate for its notice (docs/design.md section 61): the
 * system's report (sys; 0: none) held against the rate the game's own
 * presents came at (seen; 0: none that counts. The caller gives one only
 * once it is settled, with the window in front). Returns the rate to go by
 * (0: not known). *own: it is the presents' rate, a guess, and not the
 * report.
 *
 * The report can be wrong (under Proton 60 was reported on screens at 120
 * and at 50 Hz), and so can the presents' rate: an overlay shows every
 * other present, a covered window is shown a third as often on Wayland, and
 * by a timer of Xwayland's under Wine (61 to 62.5, or 76 to 83.5 times a
 * second). So the presents' rate is taken only where none of those
 * explains it:
 *   within 1% of the report: the report's figure;
 *   in one of the timer's two ranges: the report stands;
 *   a tenth or more above the report (to 400 Hz): the presents' rate;
 *   below an uneven report, and at 60 Hz or a multiple of it: the presents'
 *     rate (the screen was set to it since; no notice is due);
 *   a tenth or more below the report, 48 Hz or more, and not about a half,
 *     a third or a quarter of it: the presents' rate;
 *   anything else: the report. */
float settings_refresh_choose(float sys, float seen, int *own);

/* the defaults on this machine: settings_defaults, and on a Steam Deck the
 * 16:10 picture, its screen's shape (the player's settings.txt says
 * otherwise once written) */
void settings_defaults_here(settings *s);

/* reads path over the defaults; a missing file is no error. Messages (bad
 * lines) go to say, which may be NULL. 0, or -1 when the file is there and
 * cannot be read */
int settings_load(settings *s, const char *path, void (*say)(const char *msg));

/* writes the whole file (through path.tmp and a rename); 0 or -1 */
int settings_save(const settings *s, const char *path);

/* a GPU's or a display's name as settings.txt keeps it (gpu, display): src
 * with '#' (settings.txt's comments) and control characters as spaces,
 * spaces at the ends dropped, cut to n - 1 bytes at a whole character. The
 * lists of GPUs and displays (devices.h) hold their names so, so that a
 * saved name and a listed one compare equal */
void settings_name_clean(char *dst, size_t n, const char *src);

/* name can be a card profile's: a file name on every system and a value in
 * settings.txt (1 to 31 characters, no / \ : * ? " < > | # or control
 * characters, no space or dot at either end, not "none") */
int settings_card_name_ok(const char *name);

/* each player's own new card, offered in the game menu's card list while no
 * card has that name: "p1" and "p2" (the game makes it at its player's
 * first game). No player has a card by default */
extern const char *const settings_card_default[2];
extern const char *const settings_region_text[REGIONS];      /* "Japan", "USA", "Export", "Korea", "Australia" */
extern const char *const settings_latency_name[LATENCIES];   /* "auto", "on", "off" (settings.txt's) */
extern const char *const settings_pacing_name[PACINGS];       /* "game", "screen", "off" (settings.txt's) */

/* the Picture setting's name in settings.txt ("4:3", "16:10", "16:9",
 * "21:9", "stretch"), and the width the game's screen is drawn at for it,
 * 480 lines tall: 640 for 4:3 and stretch; 768, 854 and 1146 for the wide
 * screens (21:9's is a 3440 x 1440 screen's shape) */
const char *settings_picture_name(int picture);
int settings_picture_width(int picture);

/* The Window size setting (docs/design.md sections 40 and 43): the window's
 * height in pixels, its width the picture's screen shape (16:9 at 1080 is
 * 1920 x 1080; stretch is 4:3; 21:9 is the ultrawide screens' own widths at
 * their heights). The listed heights, smallest first: the GPU renderer's
 * four (480 to 1920, a pixel for each of its pixels) and the heights of the
 * usual 4:3, 16:10, 16:9 and 21:9 screens, from 1024 x 576 to 5120 x 2160 */
#define WINDOW_SIZES 16
extern const int settings_window_heights[WINDOW_SIZES];
/* the game's window is never smaller than this in window units, so its
 * menu fits (menu.h): in pixels, times the screen's pixel density (1280 x
 * 800 on a Wayland screen at 200%) */
#define WINDOW_MIN_UNITS_W 640
#define WINDOW_MIN_UNITS_H 400
/* the room for the window's picture: the screen's usable area less the
 * window's frame, in pixels (0 x 0: not known), and the pixel density */
typedef struct window_room {
    int w, h;
    float density;
} window_room;
/* the width for a height at the picture's shape (even) */
int settings_window_width(int picture, int height);
/* 1: a height's window fits the room and is no smaller than the smallest
 * window */
int settings_window_fits(int picture, int height, const window_room *room);
/* the window's size in pixels for s in that room: the setting's height,
 * or Auto's (the largest listed one that fits in nine tenths of the room;
 * 720 when the room is not known), made smaller at the same shape when it
 * is larger than the room, and larger when it is smaller than the
 * smallest window */
void settings_window_size(const settings *s, const window_room *room, int *w, int *h);

/* a binding as text: "Up, W" (empty: "-") */
void settings_binding_text(const binding *b, char *out, size_t n);

/* adds name to b (at the end; the oldest goes when it is full); a name it
 * has already is left where it is */
void settings_binding_add(binding *b, const char *name);

#endif
