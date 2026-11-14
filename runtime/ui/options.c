/* The player's settings as rows: see options.h. */
#include "options.h"

const option_info options[OPT_COUNT] = {
    [OPT_CREDITS] = { "Credits",
                      "Free play: Start begins a game. Arcade coins: a credit for each coin (the Coin key or "
                      "button), as on the cabinet.", 0 },
    [OPT_CARDS] = { "Card units",
                    "The cabinet's two VF.NET card readers: a card keeps its player's ring name, rank, "
                    "record and items. Player 1 card and Player 2 card choose whose card is in each.", 1 },
    [OPT_CARD1] = { "Player 1 card",
                    "The card in player 1's unit: one in the game's user/cards folder (the launcher's "
                    "Settings, Cards tab makes and edits them), or a new one the game makes at the player's "
                    "first game. Choosing player 2's card swaps the two cards. They change as the menu "
                    "closes; a card in use in a game leaves once the game has written it.", 0 },
    [OPT_CARD2] = { "Player 2 card",
                    "The card in player 2's unit: one in the game's user/cards folder (the launcher's "
                    "Settings, Cards tab makes and edits them), or a new one the game makes at the player's "
                    "first game. Choosing player 1's card swaps the two cards. They change as the menu "
                    "closes; a card in use in a game leaves once the game has written it.", 0 },
    [OPT_FULLSCREEN] = { "Fullscreen",
                         "Auto is fullscreen on the Steam Deck and in Steam's Big Picture, and a window "
                         "elsewhere. F11 or Alt+Enter switches while the game runs.", 0 },
    [OPT_WINDOW] = { "Window size",
                     "The game's window, at the picture's shape. Auto is the largest size that leaves a "
                     "tenth of the screen free; only the sizes that fit the screen are offered. Fullscreen "
                     "fills the screen whatever this is, and the window has this size outside it. Dragging "
                     "the window's edges still resizes it.", 0 },
    [OPT_PICTURE] = { "Picture",
                      "4:3 is the cabinet's screen, with bars at the sides of a wide one. 16:10 (the Steam "
                      "Deck), 16:9 and 21:9 are wide: the cabinet's screen in the middle, nothing stretched, "
                      "and the camera's view goes on at the sides (the writing and the 2D screens stay 4:3). "
                      "Stretch fills the screen with the 4:3 picture.", 0 },
    [OPT_FILTER] = { "Scaling",
                     "How the picture is scaled to the screen: Sharp keeps its pixels square, Smooth blends "
                     "them.", 0 },
    [OPT_RENDERER] = { "Renderer",
                       "GPU draws the game with Vulkan at the internal resolution below. Software draws it on "
                       "the processor at the cabinet's 640 x 480 (wider with a wide picture).", 1 },
    [OPT_SCALE] = { "Internal resolution",
                    "The size the GPU renderer draws the game at, before the picture is scaled to the window "
                    "or the screen: 1 to 4 times the cabinet's 640 x 480, or a wide picture's (854 x 480 for "
                    "16:9). Higher is sharper and needs a faster GPU. Software does not use it.", 1 },
    [OPT_SOUND] = { "Sound",
                    "The sound processor and its effects. Off saves processor time, and the game is "
                    "silent.", 1 },
    [OPT_VOLUME] = { "Volume", "The game's volume.", 0 },
    [OPT_NETWORK] = { "Network",
                      "The game's own network setting (its test menu's Game Assignments). Off, it skips "
                      "its network check as it starts. There is no network service yet: On only waits at "
                      "the start and reports a time-out.", 1 },
};

static int wrap(int v, int d, int n)
{
    return ((v + d) % n + n) % n;
}

static const char *const *card_names;
static int card_n;

void options_card_names(const char *const *names, int n)
{
    card_names = names;
    card_n = n;
}

/* 1: the card profile's file is in user/cards (the list the menu was given) */
static int card_is_file(const char *name)
{
    for (int i = 0; i < card_n; i++)
        if (!SDL_strcasecmp(card_names[i], name))
            return 1;
    return 0;
}

static int by_name(const void *a, const void *b)
{
    return SDL_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static window_room screen = { 0, 0, 1.0f };

void options_screen(const window_room *room)
{
    screen = *room;
}

/* Window size: Auto, then the listed heights that fit the screen. A height
 * from a hand edit goes to the next listed one each way */
static void window_change(settings *s, int d)
{
    int list[WINDOW_SIZES + 1], n = 0, at = -1;
    list[n++] = WINDOW_AUTO;
    for (int i = 0; i < WINDOW_SIZES; i++)
        if (settings_window_fits(s->picture, settings_window_heights[i], &screen) ||
            settings_window_heights[i] == s->window)
            list[n++] = settings_window_heights[i];
    for (int i = 0; i < n; i++)
        if (list[i] == s->window)
            at = i;
    if (at >= 0) {
        s->window = list[wrap(at, d, n)];
        return;
    }
    int next = WINDOW_AUTO;
    for (int i = 1; i < n; i++) {
        if (d > 0 && list[i] > s->window) {
            next = list[i];
            break;
        }
        if (d < 0 && list[i] < s->window)
            next = list[i];
    }
    s->window = next;
}

#define CARD_CHOICES 72

/* Player 1 card or Player 2 card: None, then by name every card in
 * user/cards, the players' cards the game has not made yet, and the
 * player's own new card (p1, p2) while no file has that name. Choosing the
 * other player's card swaps the two: that player gets this one's */
static void card_change(settings *s, int side, int d)
{
    const char *names[CARD_CHOICES], *list[CARD_CHOICES + 1];
    const char *more[3] = { s->card[side], s->card[!side], settings_card_default[side] };
    int n = 0;
    for (int i = 0; i < card_n && n < CARD_CHOICES - 3; i++)
        names[n++] = card_names[i];
    for (int k = 0; k < 3; k++) {
        int listed = !more[k][0];
        for (int i = 0; i < n && !listed; i++)
            listed = !SDL_strcasecmp(names[i], more[k]);
        if (!listed)
            names[n++] = more[k];
    }
    SDL_qsort(names, (size_t)n, sizeof names[0], by_name);
    int at = 0;
    list[0] = "";
    for (int i = 0; i < n; i++) {
        list[i + 1] = names[i];
        if (s->card[side][0] && !SDL_strcasecmp(names[i], s->card[side]))
            at = i + 1;
    }
    char next[CARD_NAME_MAX], old[CARD_NAME_MAX];   /* list points into s */
    SDL_strlcpy(next, list[wrap(at, d, n + 1)], sizeof next);
    SDL_strlcpy(old, s->card[side], sizeof old);
    if (next[0] && !SDL_strcasecmp(next, s->card[!side]))
        SDL_strlcpy(s->card[!side], old, sizeof s->card[!side]);
    SDL_strlcpy(s->card[side], next, sizeof s->card[side]);
}

void option_row(const settings *s, int o, ui_row *row, char *buf, size_t n)
{
    static const char *const fs[] = { "Auto", "Off", "On" };
    SDL_zerop(row);
    row->label = options[o].label;
    row->kind = UI_ROW_CHOICE;
    row->value = buf;
    buf[0] = 0;
    switch (o) {
    case OPT_CREDITS: SDL_strlcpy(buf, s->coins == COINS_ARCADE ? "Arcade coins" : "Free play", n); break;
    case OPT_CARDS: row->kind = UI_ROW_TOGGLE; row->on = s->cards; break;
    case OPT_NETWORK: row->kind = UI_ROW_TOGGLE; row->on = s->network; break;
    case OPT_CARD1:
    case OPT_CARD2: {
        const char *name = s->card[o - OPT_CARD1];
        if (!name[0])
            SDL_strlcpy(buf, "None", n);
        else
            SDL_snprintf(buf, n, card_is_file(name) ? "%s" : "%s (new)", name);
        break;
    }
    case OPT_FULLSCREEN: SDL_strlcpy(buf, fs[s->fullscreen + 1], n); break;
    case OPT_WINDOW: {
        int w, h;
        settings_window_size(s, &screen, &w, &h);
        if (s->window != WINDOW_AUTO)
            SDL_snprintf(buf, n, "%d x %d", w, h);
        else if (screen.w > 0 && screen.h > 0)
            SDL_snprintf(buf, n, "Auto (%d x %d)", w, h);
        else
            SDL_strlcpy(buf, "Auto", n);
        break;
    }
    case OPT_PICTURE: {
        static const char *const pic[PICTURES] = { "4:3", "16:10 wide", "16:9 wide", "21:9 ultrawide", "Stretch" };
        SDL_strlcpy(buf, pic[s->picture], n);
        break;
    }
    case OPT_FILTER: SDL_strlcpy(buf, s->filter == FILTER_SHARP ? "Sharp" : "Smooth", n); break;
    case OPT_RENDERER: SDL_strlcpy(buf, s->renderer == RENDERER_GPU ? "GPU (Vulkan)" : "Software", n); break;
    case OPT_SCALE:
        SDL_snprintf(buf, n, "%d x %d", settings_picture_width(s->picture) * s->scale, 480 * s->scale);
        row->off = s->renderer != RENDERER_GPU;
        break;
    case OPT_SOUND: row->kind = UI_ROW_TOGGLE; row->on = s->sound; break;
    case OPT_VOLUME:
        row->kind = UI_ROW_SLIDER;
        row->level = s->volume;
        row->off = !s->sound;
        break;
    }
}

void option_change(settings *s, int o, int d)
{
    switch (o) {
    case OPT_CREDITS: s->coins = wrap(s->coins, d, 2); break;
    case OPT_CARDS: s->cards = !s->cards; break;
    case OPT_NETWORK: s->network = !s->network; break;
    case OPT_CARD1:
    case OPT_CARD2: card_change(s, o - OPT_CARD1, d); break;
    case OPT_FULLSCREEN: s->fullscreen = wrap(s->fullscreen + 1, d, 3) - 1; break;
    case OPT_WINDOW: window_change(s, d); break;
    case OPT_PICTURE: s->picture = wrap(s->picture, d, PICTURES); break;
    case OPT_FILTER: s->filter = wrap(s->filter, d, 2); break;
    case OPT_RENDERER: s->renderer = wrap(s->renderer, d, 2); break;
    case OPT_SCALE: s->scale = wrap(s->scale - 1, d, 4) + 1; break;
    case OPT_SOUND: s->sound = !s->sound; break;
    case OPT_VOLUME:
        s->volume += 10 * d;
        if (s->volume < 0) s->volume = 0;
        if (s->volume > 100) s->volume = 100;
        break;
    }
}
