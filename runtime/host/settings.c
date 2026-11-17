/* The player's settings: see settings.h. */
#include "settings.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const settings_action_name[ACT_COUNT] = {
    "up", "down", "left", "right", "guard", "punch", "kick", "throw", "pk", "pkg", "kg", "start", "coin", "test",
    "service",
};
const char *const settings_action_text[ACT_COUNT] = {
    "Up", "Down", "Left", "Right", "Guard", "Punch", "Kick", "Throw (P+G)", "P+K", "P+K+G", "K+G", "Start", "Coin",
    "Test (operator)", "Service (operator)",
};

static void set(binding *b, const char *a, const char *c)
{
    memset(b, 0, sizeof *b);
    if (a)
        settings_binding_add(b, a);
    if (c)
        settings_binding_add(b, c);
}

const char *const settings_card_default[2] = { "p1", "p2" };
const char *const settings_region_text[REGIONS] = { "Japan", "USA", "Export", "Korea", "Australia" };
const char *const settings_latency_name[LATENCIES] = { "auto", "on", "off" };
const char *const settings_pacing_name[PACINGS] = { "game", "screen", "off" };

int settings_card_name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= CARD_NAME_MAX || !SDL_strcasecmp(name, "none"))
        return 0;
    if (name[0] == ' ' || name[0] == '.' || name[n - 1] == ' ' || name[n - 1] == '.')
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || strchr("/\\:*?\"<>|#", c))
            return 0;
    }
    /* Windows' device names, which no file can have */
    static const char *const dev[] = { "con", "prn", "aux", "nul" };
    for (size_t i = 0; i < SDL_arraysize(dev); i++)
        if (!SDL_strcasecmp(name, dev[i]))
            return 0;
    if (n == 4 && (!SDL_strncasecmp(name, "com", 3) || !SDL_strncasecmp(name, "lpt", 3)) && name[3] >= '0' &&
        name[3] <= '9')
        return 0;
    return 1;
}

const char *const settings_steam_deck_name[4] = { "", "Steam Deck", "Steam Deck LCD", "Steam Deck OLED" };
static const char *deck_test_file;

void settings_steam_deck_test(const char *product_file)
{
    deck_test_file = product_file;
}

/* the model from the firmware's product name: Valve's names for the two
 * boards, Jupiter (the LCD Deck) and Galileo (the OLED) */
static int deck_model(const char *path)
{
    size_t n = 0;
    char *text = path ? SDL_LoadFile(path, &n) : NULL;
    int model = STEAM_DECK_NONE;
    if (text) {
        if (!SDL_strncmp(text, "Jupiter", 7))
            model = STEAM_DECK_LCD;
        else if (!SDL_strncmp(text, "Galileo", 7))
            model = STEAM_DECK_OLED;
        SDL_free(text);
    }
    return model;
}

int settings_steam_deck(void)
{
    static int known = -1;
    if (known >= 0)
        return known;
    const char *path = "/sys/class/dmi/id/product_name";
#ifdef _WIN32
    /* under Proton the Linux files are on Wine's Z: drive; on a real Windows
     * PC Z: is whatever it is, so it is only looked at under Wine */
    path = SDL_getenv("WINEPREFIX") || SDL_getenv("STEAM_COMPAT_DATA_PATH") ? "Z:\\sys\\class\\dmi\\id\\product_name"
                                                                            : NULL;
#endif
    if (deck_test_file)
        path = deck_test_file;
    known = deck_model(path);
    const char *env = SDL_getenv("SteamDeck");
    if (known == STEAM_DECK_NONE && env && atoi(env))
        known = STEAM_DECK_UNKNOWN_MODEL;
    return known;
}

int settings_fullscreen_wanted(const settings *s)
{
    const char *tenfoot = SDL_getenv("SteamTenfoot");
    return s->fullscreen == FULLSCREEN_ON ||
           (s->fullscreen == FULLSCREEN_AUTO &&
            (settings_steam_deck() != STEAM_DECK_NONE || (tenfoot && atoi(tenfoot))));
}

int settings_refresh_uneven(float hz)
{
    if (!(hz > 0))
        return 0;
    float k = SDL_roundf(hz / 60.0f);
    if (k < 1)
        k = 1;
    return SDL_fabsf(hz - 60.0f * k) > 1.0f;
}

void settings_defaults_here(settings *s)
{
    settings_defaults(s);
    if (settings_steam_deck() != STEAM_DECK_NONE)
        s->picture = PICTURE_16_10;     /* the Deck's own shape: the 3D fills its screen */
}

void settings_defaults(settings *s)
{
    memset(s, 0, sizeof *s);
    s->coins = COINS_FREE_PLAY;
    s->fullscreen = FULLSCREEN_AUTO;
    s->window = WINDOW_AUTO;
    s->picture = PICTURE_4_3;
    s->filter = FILTER_SMOOTH;
    s->renderer = RENDERER_GPU;
    s->scale = 2;
    s->sound = 1;
    s->volume = 100;
    s->cards = 1;
    /* no card in either unit (s->card[] stays empty): a card is one
     * character's, and the select screen holds a player with a card to it,
     * so without one every player picks freely (docs/design.md section 42) */
    s->network = 0;
    s->region = REGION_JAPAN;
    s->latency = LATENCY_AUTO;
    s->pacing = PACING_GAME;
    /* keyboard, player 1 (M3's keys); player 2 has none until bound */
    binding *k = s->key[0];
    set(&k[ACT_UP], "Up", "W");
    set(&k[ACT_DOWN], "Down", "S");
    set(&k[ACT_LEFT], "Left", "A");
    set(&k[ACT_RIGHT], "Right", "D");
    set(&k[ACT_GUARD], "J", NULL);
    set(&k[ACT_PUNCH], "K", NULL);
    set(&k[ACT_KICK], "L", NULL);
    set(&k[ACT_THROW], "U", NULL);
    set(&k[ACT_PK], "I", NULL);
    set(&k[ACT_PKG], "O", NULL);
    set(&k[ACT_KG], "P", NULL);
    set(&k[ACT_START], "Return", NULL);
    set(&k[ACT_COIN], "5", NULL);
    set(&k[ACT_TEST], "F2", NULL);          /* M3's keys; no gamepad button by default, so a match */
    set(&k[ACT_SERVICE], "F3", NULL);       /* is never left for test mode by accident */
    /* gamepads, by SDL's names for the positions: south/east/west/north are
     * A/B/X/Y on an Xbox pad and the Steam Deck. The left stick always
     * steers as well */
    binding *p = s->pad;
    set(&p[ACT_UP], "dpup", NULL);
    set(&p[ACT_DOWN], "dpdown", NULL);
    set(&p[ACT_LEFT], "dpleft", NULL);
    set(&p[ACT_RIGHT], "dpright", NULL);
    /* the PlayStation 2 versions' defaults, by position (their manuals: square
     * guard, triangle and cross punch, circle kick; VF4 Evolution's TYPE A:
     * L1 P+G, R1 P+K, L2 P+K+G, R2 K+G) */
    set(&p[ACT_GUARD], "x", NULL);
    set(&p[ACT_PUNCH], "y", "a");
    set(&p[ACT_KICK], "b", NULL);
    set(&p[ACT_THROW], "leftshoulder", NULL);
    set(&p[ACT_PK], "rightshoulder", NULL);
    set(&p[ACT_PKG], "lefttrigger", NULL);
    set(&p[ACT_KG], "righttrigger", NULL);
    set(&p[ACT_START], "start", NULL);
    set(&p[ACT_COIN], "back", NULL);
}

void settings_binding_add(binding *b, const char *name)
{
    if (!name || !*name)
        return;
    for (int i = 0; i < BIND_MAX; i++)
        if (!SDL_strcasecmp(b->name[i], name))
            return;
    int i = 0;
    while (i < BIND_MAX && b->name[i][0])
        i++;
    if (i == BIND_MAX) {
        memmove(b->name[0], b->name[1], sizeof b->name[0] * (BIND_MAX - 1));
        i = BIND_MAX - 1;
    }
    SDL_strlcpy(b->name[i], name, BIND_NAME);
}

void settings_binding_text(const binding *b, char *out, size_t n)
{
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < BIND_MAX && at + 1 < n; i++) {
        if (!b->name[i][0])
            continue;
        at += (size_t)SDL_snprintf(out + at, n - at, "%s%s", at ? ", " : "", b->name[i]);
    }
    if (!out[0])
        SDL_strlcpy(out, "-", n);
}

static char *trim(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'))
        p[--n] = 0;
    return p;
}

void settings_name_clean(char *dst, size_t n, const char *src)
{
    size_t at = 0;
    if (!n)
        return;
    while (*src && (*src == ' ' || *src == '#' || (unsigned char)*src < 0x20))
        src++;
    for (; *src && at + 1 < n; src++) {
        unsigned char c = (unsigned char)*src;
        dst[at++] = c == '#' || c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    if (((unsigned char)*src & 0xc0) == 0x80) {
        /* cut in the middle of a character: its first bytes go too */
        while (at && ((unsigned char)dst[at - 1] & 0xc0) == 0x80)
            at--;
        if (at)
            at--;
    }
    while (at && dst[at - 1] == ' ')
        at--;
    dst[at] = 0;
}

static int choice(const char *v, const char *const *names, int count, int *out)
{
    for (int i = 0; i < count; i++)
        if (!SDL_strcasecmp(v, names[i])) {
            *out = i;
            return 1;
        }
    return 0;
}

static const char *const coins_names[] = { "free play", "arcade" };
static const char *const onoff_names[] = { "off", "on" };
static const char *const region_names[REGIONS] = { "japan", "usa", "export", "korea", "australia" };
static const char *const picture_names[PICTURES] = { "4:3", "16:10", "16:9", "21:9", "stretch" };

const char *settings_picture_name(int picture)
{
    return picture_names[picture >= 0 && picture < PICTURES ? picture : 0];
}

/* the cabinet's 480 lines at the picture's shape; even, for the middle */
int settings_picture_width(int picture)
{
    switch (picture) {
    case PICTURE_16_10: return 768;
    case PICTURE_16_9: return 854;
    case PICTURE_21_9: return 1146;
    default: return 640;
    }
}

/* 576 1024 x 576; 600 800 x 600; 768 1024 x 768 and 1366 x 768; 800 1280 x
 * 800 (a Steam Deck); 900 1440 x 900 and 1600 x 900; 1050 1400 x 1050 and
 * 1680 x 1050; 1200 1600 x 1200 and 1920 x 1200; 1536 2048 x 1536; 1600
 * 2560 x 1600 and 3840 x 1600 */
const int settings_window_heights[WINDOW_SIZES] = {
    480, 576, 600, 720, 768, 800, 900, 960, 1050, 1080, 1200, 1440, 1536, 1600, 1920, 2160
};

/* the screen's shape, not the picture's width over 480 (854 is a pixel more
 * than 16:9's): 16:9 at 1080 is 1920. 21:9 is 3440 x 1440's, but the
 * ultrawide screens are not one shape: at their own heights the window is
 * their width (2560 x 1080 and 5120 x 2160 are 64:27, 3840 x 1600 is 12:5) */
int settings_window_width(int picture, int height)
{
    int num = 4, den = 3;
    switch (picture) {
    case PICTURE_16_10: num = 16, den = 10; break;
    case PICTURE_16_9: num = 16, den = 9; break;
    case PICTURE_21_9:
        if (height == 1080)
            return 2560;
        if (height == 1600)
            return 3840;
        if (height == 2160)
            return 5120;
        num = 43, den = 18;
        break;
    }
    int w = (height * num + den / 2) / den;
    return w + (w & 1);
}

/* the smallest window in pixels */
static void window_min(const window_room *room, int *mw, int *mh)
{
    float den = room->density > 0 ? room->density : 1.0f;
    *mw = (int)(WINDOW_MIN_UNITS_W * den + 0.5f);
    *mh = (int)(WINDOW_MIN_UNITS_H * den + 0.5f);
}

int settings_window_fits(int picture, int height, const window_room *room)
{
    int mw, mh, w = settings_window_width(picture, height);
    window_min(room, &mw, &mh);
    if (w < mw || height < mh)
        return 0;
    return room->w <= 0 || room->h <= 0 || (w <= room->w && height <= room->h);
}

/* w x h at the same shape, scaled to fit in (big 0) or cover (big 1) bw x bh */
static void window_scale(int *w, int *h, int bw, int bh, int big)
{
    if (((long long)*w * bh > (long long)*h * bw) != big) {
        *h = (int)((long long)*h * bw / *w);
        *w = bw;
    } else {
        *w = (int)((long long)*w * bh / *h);
        *h = bh;
    }
}

void settings_window_size(const settings *s, const window_room *room, int *w, int *h)
{
    int known = room->w > 0 && room->h > 0, ht = s->window, mw, mh;
    window_min(room, &mw, &mh);
    if (ht == WINDOW_AUTO) {
        window_room most = *room;
        most.w = room->w * 9 / 10;
        most.h = room->h * 9 / 10;
        ht = known ? 0 : 720;
        for (int i = 0; known && i < WINDOW_SIZES; i++)
            if (settings_window_fits(s->picture, settings_window_heights[i], &most))
                ht = settings_window_heights[i];
        for (int i = 0; !ht && i < WINDOW_SIZES; i++) {     /* a small screen: the smallest there is */
            int h1 = settings_window_heights[i];
            if (settings_window_width(s->picture, h1) >= mw && h1 >= mh)
                ht = h1;
        }
        if (!ht)
            ht = settings_window_heights[WINDOW_SIZES - 1];
    }
    *h = ht;
    *w = settings_window_width(s->picture, ht);
    if (known && (*w > room->w || *h > room->h))
        window_scale(w, h, room->w, room->h, 0);    /* the largest at this shape in the room */
    if (*w < mw || *h < mh)
        window_scale(w, h, mw, mh, 1);              /* the smallest at this shape there can be */
}

static const char *const filter_names[] = { "smooth", "sharp" };
static const char *const renderer_names[] = { "gpu", "software" };

static void parse_binding(binding *b, char *v)
{
    memset(b, 0, sizeof *b);
    if (!strcmp(v, "-"))
        return;
    for (char *tok = v; tok;) {
        char *comma = strchr(tok, ',');
        if (comma)
            *comma = 0;
        settings_binding_add(b, trim(tok));
        tok = comma ? comma + 1 : NULL;
    }
}

int settings_load(settings *s, const char *path, void (*say)(const char *msg))
{
    char msg[300];
    if (!SDL_GetPathInfo(path, NULL))
        return 0;
    size_t size = 0;
    char *text = SDL_LoadFile(path, &size);
    if (!text) {
        if (say) {
            SDL_snprintf(msg, sizeof msg, "settings: cannot read %s: %s", path, SDL_GetError());
            say(msg);
        }
        return -1;
    }
    int line_no = 0;
    for (char *line = text; line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        line_no++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *eq = strchr(line, '=');
        char *name = line, *v = NULL;
        if (eq) {
            *eq = 0;
            v = trim(eq + 1);
        }
        name = trim(name);
        if (*name) {
            int ok = 0, x;
            if (!v) {
                ok = 0;
            } else if (!strcmp(name, "coins")) {
                if ((ok = choice(v, coins_names, 2, &x)))
                    s->coins = x;
            } else if (!strcmp(name, "fullscreen")) {
                if (!SDL_strcasecmp(v, "auto"))
                    ok = 1, s->fullscreen = FULLSCREEN_AUTO;
                else if ((ok = choice(v, onoff_names, 2, &x)))
                    s->fullscreen = x;
            } else if (!strcmp(name, "window")) {
                x = atoi(v);
                if (!strcmp(v, "auto"))
                    ok = 1, s->window = WINDOW_AUTO;
                else if ((ok = x >= WINDOW_MIN && x <= WINDOW_MAX))
                    s->window = x;
            } else if (!strcmp(name, "picture")) {
                if ((ok = choice(v, picture_names, PICTURES, &x)))
                    s->picture = x;
            } else if (!strcmp(name, "filter")) {
                if ((ok = choice(v, filter_names, 2, &x)))
                    s->filter = x;
            } else if (!strcmp(name, "renderer")) {
                if ((ok = choice(v, renderer_names, 2, &x)))
                    s->renderer = x;
            } else if (!strcmp(name, "gpu") || !strcmp(name, "display")) {
                char *to = name[0] == 'g' ? s->gpu : s->display;
                size_t size = name[0] == 'g' ? sizeof s->gpu : sizeof s->display;
                ok = 1;
                if (!SDL_strcasecmp(v, "auto"))
                    to[0] = 0;
                else
                    settings_name_clean(to, size, v);
            } else if (!strcmp(name, "scale")) {
                x = atoi(v);
                if ((ok = x >= 1 && x <= 4))
                    s->scale = x;
            } else if (!strcmp(name, "sound")) {
                if ((ok = choice(v, onoff_names, 2, &x)))
                    s->sound = x;
            } else if (!strcmp(name, "volume")) {
                char *end;
                long l = strtol(v, &end, 10);
                if ((ok = end != v && !*end && l >= 0 && l <= 100))
                    s->volume = (int)l;
            } else if (!strcmp(name, "cards")) {
                if ((ok = choice(v, onoff_names, 2, &x)))
                    s->cards = x;
            } else if (!strcmp(name, "card1") || !strcmp(name, "card2")) {
                int side = name[4] - '1';
                if (!SDL_strcasecmp(v, "none"))
                    ok = 1, s->card[side][0] = 0;
                else if ((ok = settings_card_name_ok(v)))
                    SDL_strlcpy(s->card[side], v, sizeof s->card[side]);
            } else if (!strcmp(name, "network")) {
                if ((ok = choice(v, onoff_names, 2, &x)))
                    s->network = x;
            } else if (!strcmp(name, "region")) {
                if ((ok = choice(v, region_names, REGIONS, &x)))
                    s->region = x;
            } else if (!strcmp(name, "latency")) {
                if ((ok = choice(v, settings_latency_name, LATENCIES, &x)))
                    s->latency = x;
            } else if (!strcmp(name, "pacing")) {
                if ((ok = choice(v, settings_pacing_name, PACINGS, &x)))
                    s->pacing = x;
            } else if (!strcmp(name, "pace")) {
                /* 0.5.12's: timer was a choice made (off now); screen was only its default */
                ok = 1;
                if (!SDL_strcasecmp(v, "timer"))
                    s->pacing = PACING_OFF;
            } else {
                /* key1.punch, key2.punch, pad.punch */
                char *dot = strchr(name, '.');
                binding *set_ = NULL;
                if (dot) {
                    *dot = 0;
                    int a;
                    if (choice(dot + 1, settings_action_name, ACT_COUNT, &a)) {
                        if (!strcmp(name, "key1")) set_ = &s->key[0][a];
                        else if (!strcmp(name, "key2")) set_ = &s->key[1][a];
                        else if (!strcmp(name, "pad")) set_ = &s->pad[a];
                    }
                    *dot = '.';
                }
                if (set_) {
                    parse_binding(set_, v);
                    ok = 1;
                }
            }
            if (!ok && say) {
                SDL_snprintf(msg, sizeof msg, "settings: %s line %d skipped (\"%s\": %s)", path, line_no, name,
                             v ? "not a setting, or not one of its values" : "no '='");
                say(msg);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    SDL_free(text);
    return 0;
}

int settings_save(const settings *s, const char *path)
{
    static char out[16384];
    size_t at = 0;
#define PUT(...) (at += (size_t)SDL_snprintf(out + at, at < sizeof out ? sizeof out - at : 0, __VA_ARGS__))
    PUT("# Virtua Fighter 4 Final Tuned (recomp): settings. The launcher's Settings page and the\n"
        "# game's menu write this file; the game reads it when it starts. Lines it does not know\n"
        "# are skipped.\n\n");
    PUT("coins = %s            # free play | arcade (coins; an operator's own coin setting is kept)\n",
        coins_names[s->coins]);
    PUT("fullscreen = %s       # auto (on for the Steam Deck and Big Picture) | on | off\n",
        s->fullscreen == FULLSCREEN_AUTO ? "auto" : onoff_names[s->fullscreen]);
    if (s->window == WINDOW_AUTO)
        PUT("window = auto           # the window's height: auto (the largest of these that leaves a tenth of\n");
    else
        PUT("window = %-4d           # the window's height: auto (the largest of these that leaves a tenth of\n",
            s->window);
    PUT("                         # the screen) | 480 | 576 | 600 | 720 | 768 | 800 | 900 | 960 | 1050 |\n"
        "                         # 1080 | 1200 | 1440 | 1536 | 1600 | 1920 | 2160 (pixels; the width is\n"
        "                         # the picture's shape)\n");
    PUT("picture = %s          # 4:3 | 16:10 | 16:9 | 21:9 (wide: the 3D fills the sides) | stretch\n"
        "                         # (4:3 by default; 16:10, its screen's shape, on a Steam Deck)\n",
        settings_picture_name(s->picture));
    PUT("filter = %s           # smooth | sharp (how the picture is scaled to the window)\n", filter_names[s->filter]);
    PUT("renderer = %s         # gpu (Vulkan) | software (the CPU draws the picture); the launcher's\n"
        "                         # window follows it too, and software is used when no GPU can draw\n",
        renderer_names[s->renderer]);
    PUT("gpu = %s              # the GPU the game and the launcher draw on: auto (the first in the list\n"
        "                         # that works) or a GPU's name as Vulkan gives it (the game's menu and the\n"
        "                         # launcher list them); one not there or that cannot draw: the first that\n"
        "                         # works instead (the setting is kept)\n",
        s->gpu[0] ? s->gpu : "auto");
    PUT("display = %s          # the display the game opens on, windowed or fullscreen: auto (where the\n"
        "                         # system puts it) or a display's name (listed as for gpu); one not connected:\n"
        "                         # auto for that run\n",
        s->display[0] ? s->display : "auto");
    PUT("scale = %d               # the GPU renderer's internal resolution: 1 to 4 times 640 x 480 (the\n"
        "                         # picture's width at a wide one), before it is scaled to the window\n", s->scale);
    PUT("sound = %s\n", onoff_names[s->sound]);
    PUT("volume = %d             # 0 to 100 (%%); the game's menu changes it too\n", s->volume);
    PUT("cards = %s            # the two card units\n", onoff_names[s->cards]);
    PUT("card1 = %s            # player 1's card: a profile in user/cards (NAME.card), or none\n",
        s->card[0][0] ? s->card[0] : "none");
    PUT("card2 = %s            # player 2's\n", s->card[1][0] ? s->card[1] : "none");
    PUT("network = %s          # the game's own NETWORK setting (test menu, Game Assignments); off skips\n"
        "                       # its network check as it starts\n", onoff_names[s->network]);
    PUT("region = %s            # japan | usa | export | korea | australia: the board's region, as the\n"
        "                       # cabinet's BIOS gives it (the warning screen, win streaks, ...)\n",
        region_names[s->region]);
    PUT("latency = %s          # auto | on | off: latency optimization in a two-PC game (rollback): this PC\n"
        "                       # guesses the other player's late controls and puts a wrong guess right\n"
        "                       # instead of waiting; auto turns it on when the connection needs it. It\n"
        "                       # takes processor time: a Steam Deck may not be fast enough (off there)\n",
        settings_latency_name[s->latency]);
    PUT("pacing = %s           # game | screen | off: how the game's 60 frames a second meet the screen's\n"
        "                       # refresh. game: the game keeps its own clock and each frame is put on a\n"
        "                       # refresh (one is passed over or shown again when the two have gone a\n"
        "                       # whole refresh apart); screen: the game follows a screen at 60 Hz or a\n"
        "                       # multiple of it (not in a two-PC game); off: the newest frame at each\n"
        "                       # refresh, as before 0.5.12\n",
        settings_pacing_name[s->pacing]);
    PUT("\n# Controls: SDL's key names, and its gamepad names (a b x y are the positions of an\n"
        "# Xbox pad: south, east, west, north). Several inputs: \"Up, W\". None: \"-\".\n");
    char b[200];
    for (int who = 0; who < 3; who++) {
        PUT("\n");
        for (int a = 0; a < ACT_COUNT; a++) {
            const binding *bd = who < 2 ? &s->key[who][a] : &s->pad[a];
            settings_binding_text(bd, b, sizeof b);
            PUT("%s.%s = %s\n", who == 0 ? "key1" : who == 1 ? "key2" : "pad", settings_action_name[a], b);
        }
    }
#undef PUT
    if (at >= sizeof out)
        return -1;
    char tmp[4200];
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!SDL_SaveFile(tmp, out, at))
        return -1;
    if (!SDL_RenamePath(tmp, path)) {
        SDL_RemovePath(tmp);
        return -1;
    }
    return 0;
}
