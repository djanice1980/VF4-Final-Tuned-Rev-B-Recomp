/* The player's settings: see settings.h. */
#include "settings.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

const char *const settings_action_name[ACT_COUNT] = {
    "up", "down", "left", "right", "guard", "punch", "kick", "throw", "pk", "pkg", "start", "coin",
};
const char *const settings_action_text[ACT_COUNT] = {
    "Up", "Down", "Left", "Right", "Guard", "Punch", "Kick", "Throw (P+G)", "P+K", "P+K+G", "Start", "Coin",
};

static void set(binding *b, const char *a, const char *c)
{
    memset(b, 0, sizeof *b);
    if (a)
        settings_binding_add(b, a);
    if (c)
        settings_binding_add(b, c);
}

void settings_defaults(settings *s)
{
    memset(s, 0, sizeof *s);
    s->coins = COINS_FREE_PLAY;
    s->fullscreen = FULLSCREEN_AUTO;
    s->picture = PICTURE_4_3;
    s->filter = FILTER_SMOOTH;
    s->sound = 1;
    s->cards = 1;
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
    set(&k[ACT_START], "Return", NULL);
    set(&k[ACT_COIN], "5", NULL);
    /* gamepads, by SDL's names for the positions: south/east/west/north are
     * A/B/X/Y on an Xbox pad and the Steam Deck. The left stick always
     * steers as well */
    binding *p = s->pad;
    set(&p[ACT_UP], "dpup", NULL);
    set(&p[ACT_DOWN], "dpdown", NULL);
    set(&p[ACT_LEFT], "dpleft", NULL);
    set(&p[ACT_RIGHT], "dpright", NULL);
    set(&p[ACT_GUARD], "x", NULL);
    set(&p[ACT_PUNCH], "y", NULL);
    set(&p[ACT_KICK], "b", NULL);
    set(&p[ACT_THROW], "a", NULL);
    set(&p[ACT_PK], "leftshoulder", NULL);
    set(&p[ACT_PKG], "rightshoulder", NULL);
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
static const char *const picture_names[] = { "4:3", "stretch" };
static const char *const filter_names[] = { "smooth", "sharp" };

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
            } else if (!strcmp(name, "picture")) {
                if ((ok = choice(v, picture_names, 2, &x)))
                    s->picture = x;
            } else if (!strcmp(name, "filter")) {
                if ((ok = choice(v, filter_names, 2, &x)))
                    s->filter = x;
            } else if (!strcmp(name, "sound")) {
                if ((ok = choice(v, onoff_names, 2, &x)))
                    s->sound = x;
            } else if (!strcmp(name, "cards")) {
                if ((ok = choice(v, onoff_names, 2, &x)))
                    s->cards = x;
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
    PUT("# Virtua Fighter 4 Final Tuned (recomp): settings. The launcher's Settings page writes\n"
        "# this file; the game reads it when it starts. Lines it does not know are skipped.\n\n");
    PUT("coins = %s            # free play | arcade (coins; an operator's own coin setting is kept)\n",
        coins_names[s->coins]);
    PUT("fullscreen = %s       # auto (on for the Steam Deck and Big Picture) | on | off\n",
        s->fullscreen == FULLSCREEN_AUTO ? "auto" : onoff_names[s->fullscreen]);
    PUT("picture = %s          # 4:3 | stretch\n", picture_names[s->picture]);
    PUT("filter = %s           # smooth | sharp (how the picture is scaled to the window)\n", filter_names[s->filter]);
    PUT("sound = %s\n", onoff_names[s->sound]);
    PUT("cards = %s            # the two card units; the cards are user/cards/p1.card and p2.card\n",
        onoff_names[s->cards]);
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
