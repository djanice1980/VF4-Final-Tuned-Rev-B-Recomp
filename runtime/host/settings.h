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

typedef struct binding {
    char name[BIND_MAX][BIND_NAME];     /* SDL_GetScancodeName / SDL_GetGamepadStringForButton (or a
                                         * trigger: "lefttrigger", "righttrigger"); "" unused */
} binding;

enum { COINS_FREE_PLAY, COINS_ARCADE };
enum { FULLSCREEN_AUTO = -1, FULLSCREEN_OFF = 0, FULLSCREEN_ON = 1 };
enum { PICTURE_4_3, PICTURE_16_10, PICTURE_16_9, PICTURE_21_9, PICTURE_STRETCH, PICTURES };
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
    int volume;             /* 0..100 (%), the game's menu changes it while it plays */
    int cards;              /* 1: the cabinet has its two card units */
    char card[2][CARD_NAME_MAX];    /* the card profile in each player's unit: user/cards/NAME.card
                                     * (one not there yet: a card never used, kept under that name
                                     * once the game has written it); "" none (the unit stays empty) */
    int network;            /* 1: the game's own NETWORK setting on; 0 off (the default: no network check at start) */
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

/* name can be a card profile's: a file name on every system and a value in
 * settings.txt (1 to 31 characters, no / \ : * ? " < > | # or control
 * characters, no space or dot at either end, not "none") */
int settings_card_name_ok(const char *name);

/* each player's own card by default: "p1" and "p2" (the cards' files before
 * profiles; the game makes one at its player's first game) */
extern const char *const settings_card_default[2];

/* the Picture setting's name in settings.txt ("4:3", "16:10", "16:9",
 * "21:9", "stretch"), and the width the game's screen is drawn at for it,
 * 480 lines tall: 640 for 4:3 and stretch; 768, 854 and 1146 for the wide
 * screens (21:9's is a 3440 x 1440 screen's shape) */
const char *settings_picture_name(int picture);
int settings_picture_width(int picture);

/* a binding as text: "Up, W" (empty: "-") */
void settings_binding_text(const binding *b, char *out, size_t n);

/* adds name to b (at the end; the oldest goes when it is full); a name it
 * has already is left where it is */
void settings_binding_add(binding *b, const char *name);

#endif
