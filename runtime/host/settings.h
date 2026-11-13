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
    ACT_START, ACT_COIN,
    ACT_TEST, ACT_SERVICE,
    ACT_COUNT
};

#define BIND_MAX 4          /* inputs one action can have */
#define BIND_NAME 24        /* an SDL key or gamepad button name */

typedef struct binding {
    char name[BIND_MAX][BIND_NAME];     /* SDL_GetScancodeName / SDL_GetGamepadStringForButton (or a
                                         * trigger: "lefttrigger", "righttrigger"); "" unused */
} binding;

enum { COINS_FREE_PLAY, COINS_ARCADE };
enum { FULLSCREEN_AUTO = -1, FULLSCREEN_OFF = 0, FULLSCREEN_ON = 1 };
enum { PICTURE_4_3, PICTURE_STRETCH };
enum { FILTER_SMOOTH, FILTER_SHARP };
enum { RENDERER_GPU, RENDERER_SOFTWARE };

typedef struct settings {
    int coins;              /* COINS_*: free play is the default */
    int fullscreen;         /* FULLSCREEN_*: auto is on for the Steam Deck and Big Picture */
    int picture;            /* PICTURE_* */
    int filter;             /* FILTER_* */
    int renderer;           /* RENDERER_*: the GPU (Vulkan) by default, the software picture when it cannot */
    int scale;              /* the GPU renderer's resolution: 1..4 times 640 x 480 */
    int sound;              /* 1 on */
    int cards;              /* 1: the cabinet has its two card units; the cards are user/cards/p1.card, p2.card */
    binding key[2][ACT_COUNT];      /* keyboard, player 1 and player 2 */
    binding pad[ACT_COUNT];         /* every gamepad (the first is player 1, the second player 2) */
} settings;

extern const char *const settings_action_name[ACT_COUNT];   /* "up", "guard", "throw", ... */
extern const char *const settings_action_text[ACT_COUNT];   /* "Up", "Guard", "Throw (P+G)", ... */

void settings_defaults(settings *s);

/* reads path over the defaults; a missing file is no error. Messages (bad
 * lines) go to say, which may be NULL. 0, or -1 when the file is there and
 * cannot be read */
int settings_load(settings *s, const char *path, void (*say)(const char *msg));

/* writes the whole file (through path.tmp and a rename); 0 or -1 */
int settings_save(const settings *s, const char *path);

/* a binding as text: "Up, W" (empty: "-") */
void settings_binding_text(const binding *b, char *out, size_t n);

/* adds name to b (at the end; the oldest goes when it is full); a name it
 * has already is left where it is */
void settings_binding_add(binding *b, const char *name);

#endif
