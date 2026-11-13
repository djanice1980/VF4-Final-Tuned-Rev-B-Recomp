/* The player's settings as rows: see options.h. */
#include "options.h"

const option_info options[OPT_COUNT] = {
    [OPT_CREDITS] = { "Credits",
                      "Free play: Start begins a game. Arcade coins: a credit for each coin (the Coin key or "
                      "button), as on the cabinet.", 0 },
    [OPT_CARDS] = { "Card units",
                    "The cabinet's two VF.NET card readers. Each player's card is a file in the game's "
                    "user/cards folder (p1.card, p2.card).", 1 },
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
};

static int wrap(int v, int d, int n)
{
    return ((v + d) % n + n) % n;
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
