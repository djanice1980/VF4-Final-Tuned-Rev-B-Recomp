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
                    "The card profile in player 1's unit (a file in the game's user/cards folder; the "
                    "launcher's Profiles page makes and edits them). A change swaps the card when the menu "
                    "closes; a card in use in a game leaves once the game has written it.", 0 },
    [OPT_CARD2] = { "Player 2 card",
                    "The card profile in player 2's unit (a file in the game's user/cards folder; the "
                    "launcher's Profiles page makes and edits them). A change swaps the card when the menu "
                    "closes; a card in use in a game leaves once the game has written it.", 0 },
    [OPT_FULLSCREEN] = { "Fullscreen",
                         "Auto is fullscreen on the Steam Deck and in Steam's Big Picture, and a window "
                         "elsewhere. F11 or Alt+Enter switches while the game runs.", 0 },
    [OPT_PICTURE] = { "Picture",
                      "4:3 is the cabinet's screen, with bars at the sides of a wide one. Stretch fills the "
                      "screen.", 0 },
    [OPT_FILTER] = { "Scaling",
                     "How the picture is scaled to the screen: Sharp keeps its pixels square, Smooth blends "
                     "them.", 0 },
    [OPT_RENDERER] = { "Renderer",
                       "GPU draws the game with Vulkan at the resolution below. Software draws it on the "
                       "processor at the cabinet's 640 x 480.", 1 },
    [OPT_SCALE] = { "Resolution",
                    "The GPU renderer's size: 1 to 4 times the cabinet's 640 x 480. Software does not use "
                    "it.", 1 },
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

/* Player 1 card or Player 2 card: None, the profiles (not the other
 * player's), and the one chosen now when it is not a file yet */
static void card_change(settings *s, int side, int d)
{
    const char *list[66];
    int n = 0, at = 0;
    list[n++] = "";
    for (int i = 0; i < card_n && n < 65; i++)
        if (!s->card[!side][0] || SDL_strcasecmp(card_names[i], s->card[!side]))
            list[n++] = card_names[i];
    if (s->card[side][0]) {
        at = -1;
        for (int i = 1; i < n; i++)
            if (!SDL_strcasecmp(list[i], s->card[side]))
                at = i;
        if (at < 0)
            list[at = n++] = s->card[side];
    }
    char next[CARD_NAME_MAX];
    SDL_strlcpy(next, list[wrap(at, d, n)], sizeof next);   /* list may point into s */
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
    case OPT_CARD2:
        SDL_strlcpy(buf, s->card[o - OPT_CARD1][0] ? s->card[o - OPT_CARD1] : "None", n);
        row->off = !s->cards;
        break;
    case OPT_FULLSCREEN: SDL_strlcpy(buf, fs[s->fullscreen + 1], n); break;
    case OPT_PICTURE: SDL_strlcpy(buf, s->picture == PICTURE_STRETCH ? "Stretch" : "4:3", n); break;
    case OPT_FILTER: SDL_strlcpy(buf, s->filter == FILTER_SHARP ? "Sharp" : "Smooth", n); break;
    case OPT_RENDERER: SDL_strlcpy(buf, s->renderer == RENDERER_GPU ? "GPU (Vulkan)" : "Software", n); break;
    case OPT_SCALE:
        SDL_snprintf(buf, n, "%d x %d", 640 * s->scale, 480 * s->scale);
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
    case OPT_PICTURE: s->picture = wrap(s->picture, d, 2); break;
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
