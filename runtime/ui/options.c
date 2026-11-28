/* The player's settings as rows: see options.h. */
#include "options.h"

/* a no-break space (U+00A0): the wrap keeps "1920 x 1080" on one line */
#define NB "\xc2\xa0"

/* Ray tracing's text, around the sentence about DirectX 12 that is said only where DirectX 12 is
 * offered (options_directx). No longer than this: the launcher's box holds nine lines over the two
 * of "The Software renderer does not use it." (docs/design.md section 107: with eleven, that note
 * ran out of the box). */
#define RT_ABOUT_1 "Off: the game's own light. Vulkan: the graphics chip traces rays: soft shadows under " \
                   "the fighters, shade from what is near, reflections in glass. "
#define RT_ABOUT_2 "It needs a fast chip with ray tracing; the log says if it could not start. Replaces " \
                   "ambient occlusion."
static const char rt_about_directx[] = RT_ABOUT_1 "DirectX 12: the same through DirectX. " RT_ABOUT_2;
/* Renderer's text where the game can draw through DirectX 12 too (docs/design.md section 103). No longer
 * than the other one: the launcher's panel holds eight lines of it over a note of three. (That a change
 * between Vulkan and DirectX 12 waits for the next start is the menu's own mark and note, when it is made.) */
static const char renderer_about_directx[] =
    "GPU (Vulkan) draws the game with the graphics chip at the internal resolution below. GPU (DirectX 12) is "
    "the same picture through DirectX: try it if Vulkan freezes or fails. Software draws on the processor at "
    "the cabinet's 640 x 480, also when no GPU can draw.";

const option_info options[OPT_COUNT] = {
    [OPT_CREDITS] = { "Credits",
                      "Free play: Start begins a game. Arcade coins: a credit for each coin (the Coin key or "
                      "button), as on the cabinet.", 0 },
    [OPT_CARDS] = { "Card units",
                    "The cabinet's two VF.NET card readers: a card keeps its player's ring name, rank, "
                    "record and items, and holds the player to its character. Player 1 card and Player 2 card "
                    "choose whose card is in each.", 1 },
    [OPT_CARD1] = { "Player 1 card",
                    "None (the default): player 1 picks any character. With a card in, the select screen holds "
                    "the player to the card's character, as a cabinet does. A card is one in the game's "
                    "user/cards folder (the launcher's Settings, Cards tab makes them; Edit changes one), or a "
                    "new one (p1) the game makes at the player's first game. Choosing player 2's card swaps the "
                    "two cards. They change as the menu closes; a card in use in a game leaves once the game has "
                    "written it.", 0 },
    [OPT_CARD2] = { "Player 2 card",
                    "None (the default): player 2 picks any character. With a card in, the select screen holds "
                    "the player to the card's character, as a cabinet does. A card is one in the game's "
                    "user/cards folder (the launcher's Settings, Cards tab makes them; Edit changes one), or a "
                    "new one (p2) the game makes at the player's first game. Choosing player 1's card swaps the "
                    "two cards. They change as the menu closes; a card in use in a game leaves once the game has "
                    "written it.", 0 },
    [OPT_FULLSCREEN] = { "Fullscreen",
                         "Borderless window covers the screen and leaves its resolution and refresh rate "
                         "alone. Exclusive sets the screen to Screen mode while the game is fullscreen. "
                         "Auto is Borderless window on the Steam Deck and in Big Picture, a window "
                         "elsewhere. F11 or Alt+Enter switches in the game. The launcher is never "
                         "Exclusive, so a mode that fails can be set back there.", 0 },
    [OPT_SCREENMODE] = { "Screen mode",
                         "For Fullscreen Exclusive: the resolution and refresh rate the game sets the "
                         "screen to, from the display's own list. Auto is the desktop's. The game shows "
                         "60 frames a second, which a mode at 60 or 120" NB "Hz shows evenly. On Linux "
                         "with Wayland the screen keeps its mode, and the picture is scaled to "
                         "it.", 0 },
    [OPT_WINDOW] = { "Window size",
                     "The game's window, at the picture's shape, in the usual screens' sizes (1366" NB "x" NB "768, "
                     "1920" NB "x" NB "1080, 2560" NB "x" NB "1080 at 21:9 and so on), smallest first. "
                     "Maximized, the default, fills the screen's free room. "
                     "Auto is the largest size that leaves a tenth of the screen free; only the sizes that fit "
                     "the screen are offered. Fullscreen "
                     "fills the screen whatever this is, and the window has this size outside it. Dragging "
                     "the window's edges still resizes it.", 0 },
    [OPT_DISPLAY] = { "Display",
                      "The display the game opens on, in a window or fullscreen. Auto leaves it to the system "
                      "(the main display, as a rule). One not connected is passed over (Auto) until it is back. "
                      "A Linux desktop with Wayland places windows itself: there only fullscreen goes to the "
                      "display chosen.", 0 },
    [OPT_PICTURE] = { "Picture",
                      "4:3 is the cabinet's screen, with bars at the sides of a wide one. 16:10 (the Steam "
                      "Deck's, its default there), 16:9 and 21:9 are wide: the cabinet's screen in the middle, "
                      "nothing stretched, and the camera's view goes on at the sides (the writing and the 2D "
                      "screens stay 4:3). Stretch fills the screen with the 4:3 picture.", 0 },
    [OPT_FILTER] = { "Scaling",
                     "How the picture is scaled to the screen: Sharp keeps its pixels square, Smooth blends "
                     "them.", 0 },
    [OPT_RENDERER] = { "Renderer",
                       "GPU draws the game with Vulkan at the internal resolution below. Software draws it on "
                       "the processor at the cabinet's 640 x 480 (wider with a wide picture). The launcher's "
                       "window follows it too. When no GPU can draw, Software is used. A change applies at "
                       "once.", 0 },
    [OPT_GPU] = { "GPU",
                  "The graphics card the game and the launcher draw on. Auto takes the first in this list that "
                  "works. One chosen that is missing or cannot draw gives way to the first that works; when "
                  "none works, Software is used.", 0 },
    /* eight lines in the launcher's panel, for the note under it (docs/design.md section 107) */
    [OPT_SCALE] = { "Internal resolution",
                    "The size the GPU renderer draws the game at, before it is scaled to the window: 1 to 4 "
                    "times the cabinet's 640 x 480 (854 x 480 at 16:9). Higher is sharper and needs a faster "
                    "GPU. Ray tracing's cost follows it most of all. A change applies at once.", 0 },
    [OPT_SOUND] = { "Sound",
                    "The sound processor and its effects. Off saves processor time, and the game is "
                    "silent.", 1 },
    [OPT_VOLUME] = { "Volume", "The game's volume.", 0 },
    [OPT_NETWORK] = { "Network",
                      "The game's own network setting (its test menu's Game Assignments). Off, it skips "
                      "its network check as it starts. There is no network service yet: On only waits at "
                      "the start and reports a time-out.", 1 },
    [OPT_REGION] = { "Region",
                     "The board's region, as the cabinet's BIOS gives it. Japan shows a warning screen as "
                     "the game starts, win streaks in Japanese, and Shun Di reddening as he drinks. The "
                     "others skip the warning and show win streaks in English, and USA adds a rating "
                     "screen to the attract loop. The game's text is the same in every region.", 1 },
    [OPT_LATENCY] = { "Latency optimization",
                      "For two PCs over a slow connection. On, this PC doesn't wait for the other player's late "
                      "controls: it guesses them, and puts a wrong guess right at once, unseen (rollback). Off, "
                      "the game waits for them. Auto turns it on when the connection needs it. Each PC chooses "
                      "for itself. A Steam Deck may not be fast enough for it: if the game slows down there, "
                      "choose Off on the Deck.", 0 },
    [OPT_PACING] = { "Frame pacing",
                     "How the game's 60 frames a second meet the screen's refresh. Game clock: the game keeps "
                     "its own time; when the screen's is a little off, one frame is passed over or shown again "
                     "now and then. Screen refresh: the game follows a screen at 60 Hz or a multiple of it, "
                     "with no frame passed over, a touch fast or slow. Off: as before 0.5.12. A two-PC game "
                     "uses the host's choice.", 0 },
    [OPT_SKIP] = { "Frame skip",
                   "Off: every picture is drawn, 60 a second. Every other frame: the game keeps its full speed "
                   "and its timing, and every second picture is drawn, 30 a second. Try it if the game runs in "
                   "slow motion on a PC with a slow processor, after Lighting: GPU. In a two-PC game each PC "
                   "chooses for itself.", 0 },
    [OPT_STATS] = { "Frame rate display",
                    "At the window's bottom left while the game runs: the pictures shown a second and the "
                    "time from one to the next, then what the game itself and the ray tracing took for "
                    "a frame and the slowest of the last two seconds. Numbers and graph adds a bar for "
                    "each of the last 120 pictures: the line across is one frame at 60 a second, and "
                    "a slow one is marked.", 0 },
    /* nine lines in the launcher's panel at most, for the Software renderer's note under it (section 107) */
    [OPT_OCCLUSION] = { "Ambient occlusion",
                        "Off: the picture as the cabinet draws it. SSAO: corners, folds and the places where things "
                        "meet (a foot on the floor) are a little darker, which gives the picture more depth. It "
                        "needs a little more of the graphics chip. The scores and bars are not touched. A change "
                        "applies at once.", 0 },
    /* option_about has this text with DirectX 12's sentence where that is offered */
    [OPT_RAYTRACING] = { "Ray tracing", RT_ABOUT_1 RT_ABOUT_2, 0 },
    /* eight lines in the launcher's panel, for the note under it (docs/design.md section 107) */
    [OPT_RAYQUALITY] = { "Ray tracing quality",
                         "High traces every pixel of the internal resolution. Medium: a quarter of them (half "
                         "each way), filled in: much less work, shadows a little softer. Low: that with fewer "
                         "rays. Internal resolution counts as much: each step down is far less to trace.", 0 },
    [OPT_LIGHTING] = { "Lighting",
                       "Which chip works out the lights with the GPU renderer. GPU: the graphics chip, which "
                       "takes that work off the processor. CPU: the processor, as before 0.5.24: choose it "
                       "if the picture looks wrong with GPU. The picture is the same. The software renderer "
                       "always uses the processor.", 0 },
    /* eight lines in the launcher's panel at most: the note under it says what the profile sets */
    [OPT_PROFILE] = { "Profile",
                      "Several of the settings below in one step. Default is the game as installed. Original "
                      "Arcade and Low spec draw at the cabinet's size; the RT ones turn ray tracing on, each "
                      "heavier than the last. Change a setting by hand and this says Custom.", 0 },
};

static int wrap(int v, int d, int n)
{
    return ((v + d) % n + n) % n;
}

static int directx_offered;

void options_directx(int offered)
{
    directx_offered = offered;
}

const char *option_about(int o)
{
    if (o == OPT_RENDERER && directx_offered)
        return renderer_about_directx;
    return o == OPT_RAYTRACING && directx_offered ? rt_about_directx : options[o].about;
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

static const char *const *gpu_names;
static const char *const *display_names;
static const char *const *mode_names;
static int gpu_n, display_n, mode_n;

void options_screen_modes(const char *const *names, int n)
{
    mode_names = names;
    mode_n = n;
}

void options_gpu_names(const char *const *names, int n)
{
    gpu_names = names;
    gpu_n = n;
}

void options_display_names(const char *const *names, int n)
{
    display_names = names;
    display_n = n;
}

static int name_in(const char *const *names, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!SDL_strcasecmp(names[i], name))
            return i;
    return -1;
}

/* GPU or Display: Auto (""), then the names there now; a chosen one that
 * is not there now stays in the list after them while it is chosen */
static void name_change(char *v, size_t size, const char *const *names, int n, int d)
{
    int at = v[0] ? name_in(names, n, v) : -1, count = n + 1 + (v[0] && at < 0);
    int pos = !v[0] ? 0 : at >= 0 ? at + 1 : n + 1;
    pos = wrap(pos, d, count);
    if (pos == 0)
        v[0] = 0;
    else if (pos <= n)
        SDL_strlcpy(v, names[pos - 1], size);
}

/* the window's height in pixels for Window size value v (settings_window_size) */
static int window_height(const settings *s, int v)
{
    settings t = *s;
    int w, h;
    t.window = v;
    settings_window_size(&t, &screen, &w, &h);
    return h;
}

/* Window size (docs/design.md section 51): the listed heights that fit the
 * screen, smallest first, with Auto in the place of the size it gives (in
 * the place of the listed height of that size, which it stands for). Left
 * is always a smaller window and Right a larger one: the ends do not wrap.
 * Maximized is past the largest, at the right end.
 * A value not in the list (a hand edit, another screen's size, a listed
 * height that is Auto's size here) steps from the window it gives to the
 * next smaller or larger one, so no size shows twice. The value after Left
 * (d -1) or Right (+1): s->window itself at that end */
static int window_step(const settings *s, int d)
{
    int list[WINDOW_SIZES + 2], hs[WINDOW_SIZES + 2], n = 0, auto_h = window_height(s, WINDOW_AUTO), auto_in = 0;
    for (int i = 0; i < WINDOW_SIZES; i++) {
        int h = settings_window_heights[i];
        if (!settings_window_fits(s->picture, h, &screen))
            continue;
        if (!auto_in && h >= auto_h) {      /* Auto's place; a listed height of its size gives way to it */
            list[n] = WINDOW_AUTO, hs[n++] = auto_h, auto_in = 1;
            if (h == auto_h)
                continue;
        }
        list[n] = h, hs[n++] = h;
    }
    if (!auto_in)
        list[n] = WINDOW_AUTO, hs[n++] = auto_h;
    list[n] = WINDOW_MAXIMIZED, hs[n++] = 1 << 30;      /* past the largest: the whole of the screen's room */
    int at = -1;
    for (int i = 0; i < n; i++)
        if (list[i] == s->window)
            at = i;
    if (at >= 0)
        return at + d >= 0 && at + d < n ? list[at + d] : s->window;
    int now = window_height(s, s->window);
    if (d > 0) {
        for (int i = 0; i < n; i++)
            if (hs[i] > now)
                return list[i];
    } else {
        for (int i = n - 1; i >= 0; i--)
            if (hs[i] < now)
                return list[i];
    }
    return s->window;
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

/* the profile s's settings are (settings_profile_settle, on a copy) */
static int profile_of(const settings *s)
{
    static settings t;
    t = *s;
    settings_profile_settle(&t);
    return t.profile;
}

void option_row(const settings *s, int o, ui_row *row, char *buf, size_t n)
{
    static const char *const fs[] = { "Auto", "Off", "Borderless window", "Exclusive" };
    SDL_zerop(row);
    row->label = options[o].label;
    row->kind = UI_ROW_CHOICE;
    row->value = buf;
    buf[0] = 0;
    switch (o) {
    case OPT_CREDITS: SDL_strlcpy(buf, s->coins == COINS_ARCADE ? "Arcade coins" : "Free play", n); break;
    case OPT_CARDS: row->kind = UI_ROW_TOGGLE; row->on = s->cards; break;
    case OPT_NETWORK: row->kind = UI_ROW_TOGGLE; row->on = s->network; break;
    case OPT_REGION: SDL_strlcpy(buf, settings_region_text[s->region], n); break;
    case OPT_LATENCY: {
        static const char *const lt[LATENCIES] = { "Auto", "On", "Off" };
        SDL_strlcpy(buf, lt[s->latency], n);
        break;
    }
    case OPT_PACING: {
        static const char *const pt[PACINGS] = { "Game clock", "Screen refresh", "Off" };
        SDL_strlcpy(buf, pt[s->pacing], n);
        break;
    }
    case OPT_LIGHTING: {
        static const char *const lt[LIGHTINGS] = { "CPU", "GPU" };
        SDL_strlcpy(buf, lt[s->lighting], n);
        break;
    }
    case OPT_STATS: {
        static const char *const st[STATS] = { "Off", "Numbers", "Numbers and graph" };
        SDL_strlcpy(buf, st[s->stats], n);
        break;
    }
    case OPT_SKIP: {
        static const char *const sk[SKIPS] = { "Off", "Every other frame" };
        SDL_strlcpy(buf, sk[s->skip], n);
        break;
    }
    case OPT_OCCLUSION: {
        static const char *const oc[OCCLUSIONS] = { "Off", "SSAO" };
        SDL_strlcpy(buf, oc[s->occlusion], n);
        break;
    }
    case OPT_RAYTRACING: {
        static const char *const rt[RAYTRACINGS] = { "Off", "Vulkan", "DirectX 12" };
        /* DirectX 12 where it is not offered: the game draws without (the hosts set it to off on reading) */
        SDL_strlcpy(buf, rt[s->raytracing == RAYTRACING_DIRECTX && !directx_offered ? RAYTRACING_OFF : s->raytracing], n);
        break;
    }
    case OPT_RAYQUALITY: {
        static const char *const rq[RAYQUALITIES] = { "Low", "Medium", "High" };
        SDL_strlcpy(buf, rq[s->rayquality], n);
        break;
    }
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
    case OPT_SCREENMODE: {
        int w, h;
        const char *at = SDL_strchr(s->screenmode, '@');
        if (!s->screenmode[0] || !at || !settings_screenmode_parse(s->screenmode, &w, &h, NULL))
            SDL_strlcpy(buf, "Auto", n);
        else        /* one the display does not list: its nearest is taken, and the row says so */
            SDL_snprintf(buf, n, "%d x %d, %s Hz%s", w, h, at + 1,
                         !mode_n || name_in(mode_names, mode_n, s->screenmode) >= 0 ? "" : " (nearest)");
        row->off = s->fullscreen != FULLSCREEN_EXCLUSIVE;
        break;
    }
    case OPT_WINDOW: {
        int w, h;
        settings_window_size(s, &screen, &w, &h);
        if (s->window == WINDOW_MAXIMIZED)
            SDL_strlcpy(buf, "Maximized", n);
        else if (s->window != WINDOW_AUTO)
            SDL_snprintf(buf, n, "%d x %d", w, h);
        else if (screen.w > 0 && screen.h > 0)
            SDL_snprintf(buf, n, "Auto (%d x %d)", w, h);
        else
            SDL_strlcpy(buf, "Auto", n);
        row->ends = (window_step(s, -1) == s->window ? UI_END_LEFT : 0) |
                    (window_step(s, 1) == s->window ? UI_END_RIGHT : 0);
        break;
    }
    case OPT_PICTURE: {
        static const char *const pic[PICTURES] = { "4:3", "16:10 wide", "16:9 wide", "21:9 ultrawide", "Stretch" };
        SDL_strlcpy(buf, pic[s->picture], n);
        break;
    }
    case OPT_FILTER: SDL_strlcpy(buf, s->filter == FILTER_SHARP ? "Sharp" : "Smooth", n); break;
    case OPT_RENDERER:
        SDL_strlcpy(buf, s->renderer != RENDERER_GPU ? "Software"
                         : directx_offered && s->graphics == GRAPHICS_DIRECTX ? "GPU (DirectX 12)" : "GPU (Vulkan)", n);
        break;
    case OPT_SCALE:
        SDL_snprintf(buf, n, "%d x %d", settings_picture_width(s->picture) * s->scale, 480 * s->scale);
        row->off = s->renderer != RENDERER_GPU;
        break;
    case OPT_GPU:
        if (!s->gpu[0])
            SDL_strlcpy(buf, "Auto", n);    /* which GPU that is: option_note */
        else
            SDL_snprintf(buf, n, name_in(gpu_names, gpu_n, s->gpu) >= 0 ? "%s" : "%s (not found)", s->gpu);
        /* with DirectX 12 the chip is Windows' choice (section 103) */
        row->off = s->renderer != RENDERER_GPU || (directx_offered && s->graphics == GRAPHICS_DIRECTX);
        break;
    case OPT_DISPLAY:
        if (!s->display[0])
            SDL_strlcpy(buf, "Auto", n);
        else
            SDL_snprintf(buf, n, name_in(display_names, display_n, s->display) >= 0 ? "%s" : "%s (not connected)",
                         s->display);
        break;
    case OPT_SOUND: row->kind = UI_ROW_TOGGLE; row->on = s->sound; break;
    case OPT_VOLUME:
        row->kind = UI_ROW_SLIDER;
        row->level = s->volume;
        row->off = !s->sound;
        break;
    case OPT_PROFILE: SDL_strlcpy(buf, settings_profile_text[profile_of(s)], n); break;
    }
}

/* ray tracing as the game will take it: on with the GPU renderer, and not DirectX 12's where that
 * is not offered */
static int tracing(const settings *s)
{
    return s->renderer == RENDERER_GPU && s->raytracing != RAYTRACING_OFF &&
           !(s->raytracing == RAYTRACING_DIRECTX && !directx_offered);
}

const char *option_note(const settings *s, int o, char *buf, size_t n)
{
    /* the size ray tracing traces: the internal resolution, or half of it each way below High */
    int div = s->rayquality == RAYQUALITY_HIGH ? 1 : 2;
    int tw = (settings_picture_width(s->picture) * s->scale + div - 1) / div, th = (480 * s->scale + div - 1) / div;
    if (o == OPT_GPU && s->gpu[0])
        SDL_snprintf(buf, n, name_in(gpu_names, gpu_n, s->gpu) >= 0 ? "%s." : "%s: not found.", s->gpu);
    else if (o == OPT_DISPLAY && s->display[0])
        SDL_snprintf(buf, n, name_in(display_names, display_n, s->display) >= 0 ? "%s." : "%s: not connected.",
                     s->display);
    else if (o == OPT_SCALE && tracing(s))
        SDL_snprintf(buf, n, "Ray tracing is on and traces %d x %d of this.", tw, th);
    else if (o == OPT_RAYQUALITY && tracing(s))
        SDL_snprintf(buf, n, "Traces %d x %d now. A lower Internal resolution traces less.", tw, th);
    else if (o == OPT_PROFILE) {
        if (profile_of(s) == PROFILE_CUSTOM)
            SDL_strlcpy(buf, "Your own settings: no profile's.", n);
        else if (tracing(s))
            SDL_snprintf(buf, n, "%d x %d, ray tracing %s%s.", settings_picture_width(s->picture) * s->scale, 480 * s->scale,
                         s->rayquality == RAYQUALITY_HIGH ? "High" : s->rayquality == RAYQUALITY_MEDIUM ? "Medium" : "Low",
                         s->skip == SKIP_HALF ? ", every other frame" : "");
        else
            SDL_snprintf(buf, n, "%d x %d, no ray tracing%s.", settings_picture_width(s->picture) * s->scale, 480 * s->scale,
                         s->skip == SKIP_HALF ? ", every other frame" : "");
    } else
        return NULL;
    return buf;
}

/* Profile (docs/design.md section 107). The settings a profile may set, as the player had them
 * (own), and as the row's last step left them (left): one set of each for the program.
 *
 * A profile does not own everything: the picture's shape (but Original Arcade's and Steam Deck's),
 * the ray tracing quality (but the RT ones'), and which tracer, Vulkan or DirectX 12. Those are
 * the player's own under every profile that leaves them alone: without that, a step through Steam
 * Deck left its 16:10 on every profile after it, and a step through Low spec turned a player's
 * DirectX 12 tracer into Vulkan. (Never DirectX of a profile's doing: only what the player's
 * settings said, and where this game has it.)
 *
 * The player's own are taken when the row is stepped from settings that are not as its last step
 * left them (the first time, from the file, after a change by hand): all of them when they are no
 * profile's (Custom then brings them back), else the three a hand has changed since. */
typedef struct {
    int have, renderer, lighting, scale, occlusion, raytracing, rayquality, skip, picture;
} profile_set;
static profile_set own, left;
static int own_custom;      /* own were no profile's: the row offers Custom */

static void set_take(profile_set *k, const settings *s)
{
    k->have = 1;
    k->renderer = s->renderer; k->lighting = s->lighting; k->scale = s->scale; k->occlusion = s->occlusion;
    k->raytracing = s->raytracing; k->rayquality = s->rayquality; k->skip = s->skip; k->picture = s->picture;
}

static int set_same(const profile_set *k, const settings *s)
{
    return k->renderer == s->renderer && k->lighting == s->lighting && k->scale == s->scale &&
           k->occlusion == s->occlusion && k->raytracing == s->raytracing && k->rayquality == s->rayquality &&
           k->skip == s->skip && k->picture == s->picture;
}

static void profile_change(settings *s, int d)
{
    settings_profile_settle(s);
    if (!left.have || (s->profile == PROFILE_CUSTOM && !set_same(&left, s))) {
        set_take(&own, s);
        own_custom |= s->profile == PROFILE_CUSTOM;
    } else if (!set_same(&left, s)) {       /* a profile's still, or another's: what a hand has changed of the three */
        if (s->picture != left.picture) own.picture = s->picture;
        if (s->rayquality != left.rayquality) own.rayquality = s->rayquality;
        if (s->raytracing != left.raytracing) own.raytracing = s->raytracing;
    }
    /* the profiles in their order, then Custom when there are settings of the player's own to go back to */
    int p = wrap(s->profile, d, own_custom ? PROFILES : PROFILE_CUSTOM);
    if (p == PROFILE_CUSTOM) {
        s->renderer = own.renderer; s->lighting = own.lighting; s->scale = own.scale; s->occlusion = own.occlusion;
        s->raytracing = own.raytracing; s->rayquality = own.rayquality; s->skip = own.skip; s->picture = own.picture;
        s->profile = PROFILE_CUSTOM;
        settings_profile_settle(s);     /* (the player's own may be a profile's by now) */
    } else {
        /* the player's own first, then what the profile owns over them: ray tracing off, or on
         * with the player's tracer (Vulkan when they had none) */
        s->picture = own.picture;
        s->rayquality = own.rayquality;
        s->raytracing = own.raytracing == RAYTRACING_DIRECTX && !directx_offered ? RAYTRACING_OFF : own.raytracing;
        settings_profile_apply(s, p);
    }
    set_take(&left, s);
}

void option_change(settings *s, int o, int d)
{
    switch (o) {
    case OPT_CREDITS: s->coins = wrap(s->coins, d, 2); break;
    case OPT_CARDS: s->cards = !s->cards; break;
    case OPT_NETWORK: s->network = !s->network; break;
    case OPT_REGION: s->region = wrap(s->region, d, REGIONS); break;
    case OPT_LATENCY: s->latency = wrap(s->latency, d, LATENCIES); break;
    case OPT_PACING: s->pacing = wrap(s->pacing, d, PACINGS); break;
    case OPT_LIGHTING: s->lighting = wrap(s->lighting, d, LIGHTINGS); break;
    case OPT_SKIP: s->skip = wrap(s->skip, d, SKIPS); break;
    case OPT_STATS: s->stats = wrap(s->stats, d, STATS); break;
    case OPT_OCCLUSION: s->occlusion = wrap(s->occlusion, d, OCCLUSIONS); break;
    case OPT_RAYTRACING:
        s->raytracing = wrap(s->raytracing, d, RAYTRACINGS);
        if (s->raytracing == RAYTRACING_DIRECTX && !directx_offered)    /* the Windows game's alone */
            s->raytracing = wrap(s->raytracing, d < 0 ? -1 : 1, RAYTRACINGS);
        break;
    case OPT_RAYQUALITY: s->rayquality = wrap(s->rayquality, d, RAYQUALITIES); break;
    case OPT_CARD1:
    case OPT_CARD2: card_change(s, o - OPT_CARD1, d); break;
    case OPT_FULLSCREEN: s->fullscreen = wrap(s->fullscreen + 1, d, 4) - 1; break;
    case OPT_SCREENMODE: name_change(s->screenmode, sizeof s->screenmode, mode_names, mode_n, d); break;
    case OPT_WINDOW: s->window = window_step(s, d); break;
    case OPT_PICTURE: s->picture = wrap(s->picture, d, PICTURES); break;
    case OPT_FILTER: s->filter = wrap(s->filter, d, 2); break;
    case OPT_RENDERER: {
        /* GPU (Vulkan), then GPU (DirectX 12) where the game can use it (section 103), then Software */
        int kinds = directx_offered ? 3 : 2;
        int at = s->renderer != RENDERER_GPU ? kinds - 1 : directx_offered && s->graphics == GRAPHICS_DIRECTX ? 1 : 0;
        at = wrap(at, d, kinds);
        s->renderer = at == kinds - 1 ? RENDERER_SOFTWARE : RENDERER_GPU;
        if (directx_offered && at != kinds - 1)
            s->graphics = at == 1 ? GRAPHICS_DIRECTX : GRAPHICS_VULKAN;
        break;
    }
    case OPT_SCALE: s->scale = wrap(s->scale - 1, d, 4) + 1; break;
    case OPT_GPU: name_change(s->gpu, sizeof s->gpu, gpu_names, gpu_n, d); break;
    case OPT_DISPLAY: name_change(s->display, sizeof s->display, display_names, display_n, d); break;
    case OPT_SOUND: s->sound = !s->sound; break;
    case OPT_VOLUME:
        s->volume += 10 * d;
        if (s->volume < 0) s->volume = 0;
        if (s->volume > 100) s->volume = 100;
        break;
    case OPT_PROFILE: profile_change(s, d); return;
    }
    settings_profile_settle(s);     /* a setting changed by hand: the Profile row says what the settings are now */
}
