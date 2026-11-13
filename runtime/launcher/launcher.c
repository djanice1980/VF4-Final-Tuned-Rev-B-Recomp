/* vf4ft-launcher: the builder's window (milestone 5, docs/design.md section 8).
 *
 * Choose set -> Build -> Play, and Update after a new release; Add to Steam.
 * It holds no game code: the work is done by the builder (tools/make_game.py)
 * and the Steam tool (tools/steam_shortcut.py), run with Python, whose output
 * it shows. SDL3 only: its folder dialog, its process API and its built-in
 * 8x8 text, so it needs nothing the game does not.
 *
 * Folders, beside the launcher:
 *   builder/   this repository's tools/, translator/, runtime/, meta/ (a release's
 *              layout; a development build finds the repository above itself)
 *   python/    Windows: the embeddable Python the release carries (pythonw.exe)
 *   game/      the install the builder makes (game/build.json once built)
 *   launcher-set.txt   the set folder last chosen
 * On Linux, Python is the system's python3.
 *
 * Steam Deck: the Windows release, added to Steam as a non-Steam game with
 * Proton, runs in Game Mode; the D-pad and A work the buttons. With no set
 * chosen it looks for one in the usual places (find_set), Linux folders
 * through Wine's Z: drive. Play starts the game in the same Proton prefix and
 * the launcher hides until it ends.
 *
 * Settings (M6): the game's settings file, game/user/settings.txt
 * (runtime/host/settings.h): free play or coins, fullscreen, picture, sound,
 * the card units, and the controls (a row takes the next key or button).
 * The game reads it when it starts.
 *
 * For testing: --game DIR (another install folder), --set DIR, --do ACTION
 * (build, update, play or steam: pressed at the start) and --exit (quit when
 * that action's process ends, with its result as the exit code); --shot
 * PAGE FILE draws one page (main, settings, pad, key1, key2) to a BMP file
 * and quits (with SDL_VIDEO_DRIVER=offscreen no window shows).
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"

#define LW 480
#define LH 320
#define COLS (LW / 8)
#define LOG_LINES 22

static SDL_Window *win;
static SDL_Renderer *ren;
static char base[896], root[1024], game[1024], python[1024], set_path[1024], set_file[1024];
static char lines[LOG_LINES][COLS + 1];
static int nlines;
static char partial[512];
static size_t npartial;
static char status[COLS + 1];
static SDL_Process *proc;
static SDL_Process *game_proc;  /* the game, while it runs (the launcher hides meanwhile) */
static SDL_IOStream *proc_out;
static const char *proc_what;
static int target_linux;       /* Linux only: a native build instead of the Windows exe */
static int built, built_linux;
static char built_ver[64];
static FILE *log_file;         /* launcher.log beside the launcher: Windows has no console for it */
static SDL_AtomicInt picked;   /* the folder dialog's answer waits here for the main thread */
static char picked_path[1024];

static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    SDL_Log("%s", b);
    if (log_file) {
        fprintf(log_file, "[%7.1f] %s\n", SDL_GetTicks() / 1000.0, b);
        fflush(log_file);
    }
    if (nlines == LOG_LINES) {
        memmove(lines[0], lines[1], sizeof lines[0] * (LOG_LINES - 1));
        nlines--;
    }
    /* the 8x8 font has ASCII only; long lines show their end, where the news is */
    size_t n = strlen(b);
    const char *s = n > COLS ? b + n - COLS : b;
    char *d = lines[nlines++];
    size_t i = 0;
    for (; s[i] && i < COLS; i++)
        d[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '?';
    d[i] = 0;
}

static int exists(const char *p)
{
    return SDL_GetPathInfo(p, NULL);
}

static void find_paths(void)
{
    const char *b = SDL_GetBasePath();
    SDL_strlcpy(base, b ? b : "./", sizeof base);
    char probe[1100];
    snprintf(probe, sizeof probe, "%sbuilder/tools/make_game.py", base);
    if (exists(probe)) {
        snprintf(root, sizeof root, "%sbuilder/", base);
    } else {
        /* a development build (build/<dir>/): the repository is above it */
        char up[896];
        SDL_strlcpy(up, base, sizeof up);
        root[0] = 0;
        for (int i = 0; i < 4 && !root[0]; i++) {
            size_t n = strlen(up);
            if (n > 1 && (up[n - 1] == '/' || up[n - 1] == '\\'))
                up[--n] = 0;
            char *sl = strrchr(up, '/');
            char *bs = strrchr(up, '\\');
            if (bs > sl)
                sl = bs;
            if (!sl)
                break;
            sl[1] = 0;
            snprintf(probe, sizeof probe, "%stools/make_game.py", up);
            if (exists(probe))
                SDL_strlcpy(root, up, sizeof root);
        }
    }
    snprintf(game, sizeof game, "%sgame", base);
    snprintf(set_file, sizeof set_file, "%slauncher-set.txt", base);
#ifdef _WIN32
    snprintf(python, sizeof python, "%spython\\pythonw.exe", base);
    if (!exists(python))
        SDL_strlcpy(python, "pythonw.exe", sizeof python);    /* one on PATH, for development */
#else
    SDL_strlcpy(python, "python3", sizeof python);
#endif
}

static void load_state(void)
{
    size_t n = 0;
    char *s = SDL_LoadFile(set_file, &n);
    set_path[0] = 0;
    if (s) {
        SDL_strlcpy(set_path, s, sizeof set_path);
        set_path[strcspn(set_path, "\r\n")] = 0;
        SDL_free(s);
    }
    char p[1100];
    snprintf(p, sizeof p, "%s/build.json", game);
    s = SDL_LoadFile(p, &n);
    built = s != NULL;
    built_linux = 0;
    built_ver[0] = 0;
    if (s) {
        built_linux = strstr(s, "\"target\": \"linux\"") != NULL;
        const char *v = strstr(s, "\"builder\": \"");
        if (v) {
            v += 12;
            size_t k = strcspn(v, "\"");
            snprintf(built_ver, sizeof built_ver, "%.*s", (int)(k < 60 ? k : 60), v);
        }
        SDL_free(s);
    }
}

/* ---- finding the set ---- */

static int is_set(const char *dir)
{
    char a[1200], b[1200], c[1200];
    snprintf(a, sizeof a, "%s/gds-0036f.chd", dir);
    snprintf(b, sizeof b, "%s/vf4tuned/gds-0036f.chd", dir);
    snprintf(c, sizeof c, "%s/vf4tuned.zip", dir);
    return (exists(a) || exists(b)) && exists(c);
}

static int found_set(const char *dir)
{
    if (!is_set(dir))
        return 0;
    SDL_strlcpy(set_path, dir, sizeof set_path);
    SDL_SaveFile(set_file, set_path, strlen(set_path));
    say("found the set: %s", set_path);
    return 1;
}

/* each folder in `dir` + `tail`; `tail` may itself have one "*" level */
typedef struct scan { const char *tail; int found; } scan;

static SDL_EnumerationResult SDLCALL scan_one(void *ud, const char *dir, const char *name)
{
    scan *sc = ud;
    char p[1100];
    size_t dl = strlen(dir);
    int sep = dl && (dir[dl - 1] == '/' || dir[dl - 1] == '\\');
    snprintf(p, sizeof p, "%.*s/%s", (int)(sep ? dl - 1 : dl), dir, name);
    char full[1200];
    snprintf(full, sizeof full, "%s/%s", p, sc->tail);
    if (strstr(sc->tail, "*/")) {
        char head[256];
        const char *star = strstr(sc->tail, "*/");
        snprintf(head, sizeof head, "%.*s", (int)(star - sc->tail), sc->tail);
        char d2[1500];
        snprintf(d2, sizeof d2, "%s/%s", p, head);
        scan sub = {star + 2, 0};
        if (exists(d2))
            SDL_EnumerateDirectory(d2, scan_one, &sub);
        sc->found = sub.found;
    } else if (found_set(full)) {
        sc->found = 1;
    }
    return sc->found ? SDL_ENUM_SUCCESS : SDL_ENUM_CONTINUE;
}

static int find_under(const char *parent, const char *tail)
{
    if (!exists(parent))
        return 0;
    scan sc = {tail, 0};
    SDL_EnumerateDirectory(parent, scan_one, &sc);
    return sc.found;
}

static void find_set(void)
{
    /* the folders where a set usually is: EmuDeck's ROM folders (on the Deck's
     * own storage or an SD card), a home's Games, roms and Downloads, and
     * beside the launcher */
    static const char *const home_tails[] = {
        "Emulation/roms/arcade", "Emulation/roms/naomi2", "Emulation/roms/naomi", "Emulation/roms/mame",
        "Emulation/roms/mame/roms", "Games/naomi2", "Games", "roms", "ROMs", "roms/naomi2", "Downloads",
        "Downloads/*/", "Games/*/", "Desktop", NULL};
    char b[1100];
    snprintf(b, sizeof b, "%sset", base);
    if (found_set(base) || found_set(b))
        return;
#ifdef _WIN32
    const char *roots[] = {"Z:/home", NULL};          /* under Wine/Proton: the Linux side */
    static const char *const media[] = {"Z:/run/media", NULL};
#else
    const char *roots[] = {"/home", NULL};
    static const char *const media[] = {"/run/media", NULL};
#endif
    for (int r = 0; roots[r]; r++)
        for (int i = 0; home_tails[i]; i++)
            if (find_under(roots[r], home_tails[i]))
                return;
    static const char *const media_tails[] = {"Emulation/roms/arcade", "Emulation/roms/naomi2", "roms",
                                              "*/Emulation/roms/arcade", "*/Emulation/roms/naomi2", "*/roms", NULL};
    for (int r = 0; media[r]; r++)
        for (int i = 0; media_tails[i]; i++)
            if (find_under(media[r], media_tails[i]))
                return;
#ifdef _WIN32
    const char *up = SDL_getenv("USERPROFILE");
    if (up) {
        static const char *const win_tails[] = {"Downloads", "Documents", "Desktop", "Games", NULL};
        for (int i = 0; win_tails[i]; i++) {
            snprintf(b, sizeof b, "%s\\%s", up, win_tails[i]);
            if (found_set(b))
                return;
            if (find_under(b, "."))           /* and one folder down */
                return;
        }
    }
#endif
    say("no set found by itself: press Choose set");
}

static void program_path(char *out, size_t n)
{
#ifdef _WIN32
    snprintf(out, n, "%s\\vf4ft.exe", game);
#else
    snprintf(out, n, "%s/%s", game, built_linux ? "vf4ft" : "vf4ft.exe");
#endif
}

static int start(const char *what, const char **args, int capture)
{
    if (proc) {
        say("busy: %s is still running", proc_what);
        return -1;
    }
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    if (capture) {
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    } else {
        SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    }
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (!p) {
        say("cannot start %s: %s", args[0], SDL_GetError());
#ifndef _WIN32
        if (!strcmp(args[0], "python3"))
            say("install Python 3.12 or later (Arch: sudo pacman -S python)");
#endif
        return -1;
    }
    if (!capture) {
        game_proc = p;                /* watched until it ends; the launcher hides meanwhile */
        SDL_HideWindow(win);
        return 0;
    }
    proc = p;
    proc_out = SDL_GetPointerProperty(SDL_GetProcessProperties(p), SDL_PROP_PROCESS_STDOUT_POINTER, NULL);
    proc_what = what;
    npartial = 0;
    snprintf(status, sizeof status, "%s...", what);
    return 0;
}

static void pump_process(void)
{
    if (!proc)
        return;
    char b[1024];
    for (;;) {
        size_t n = proc_out ? SDL_ReadIO(proc_out, b, sizeof b) : 0;
        if (n == 0)
            break;
        for (size_t i = 0; i < n; i++) {
            if (b[i] == '\n' || npartial == sizeof partial - 1) {
                partial[npartial] = 0;
                if (npartial && partial[npartial - 1] == '\r')
                    partial[npartial - 1] = 0;
                say("%s", partial);
                npartial = 0;
            } else {
                partial[npartial++] = b[i];
            }
        }
    }
    int code = 0;
    if (SDL_WaitProcess(proc, false, &code)) {
        if (npartial) {
            partial[npartial] = 0;
            say("%s", partial);
            npartial = 0;
        }
        snprintf(status, sizeof status, "%s %s", proc_what, code == 0 ? "done" : "FAILED (see above)");
        SDL_DestroyProcess(proc);
        proc = NULL;
        proc_out = NULL;
        load_state();
    }
}

static void watch_game(void)
{
    if (!game_proc)
        return;
    int code = 0;
    if (SDL_WaitProcess(game_proc, false, &code)) {
        SDL_DestroyProcess(game_proc);
        game_proc = NULL;
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);
        say("the game ended");
    }
}

static void SDLCALL on_folder(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0]) {
        SDL_strlcpy(picked_path, files[0], sizeof picked_path);
        SDL_SetAtomicInt(&picked, 1);
    }
}

/* ---- the settings pages ---- */

enum { PAGE_MAIN, PAGE_SETTINGS, PAGE_PAD, PAGE_KEY1, PAGE_KEY2 };
static int page = PAGE_MAIN;
static settings st;
static char settings_path[1100];
static int st_row;                      /* the row with the focus on a settings page */
static int capturing;                   /* a controls row waits for its key or button */

enum { R_COINS, R_RENDERER, R_SCALE, R_FULLSCREEN, R_PICTURE, R_FILTER, R_SOUND, R_CARDS, R_PAD, R_KEY1, R_KEY2, R_BACK,
       R_COUNT };

static void settings_open(void)
{
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/user", game);
    snprintf(settings_path, sizeof settings_path, "%s/user/settings.txt", game);
    settings_defaults(&st);
    settings_load(&st, settings_path, NULL);
}

static void settings_store(void)
{
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/user", game);
    if (!exists(game))
        SDL_CreateDirectory(game);
    if (!exists(dir))
        SDL_CreateDirectory(dir);
    if (settings_save(&st, settings_path))
        say("cannot write %s: %s", settings_path, SDL_GetError());
    else
        say("settings saved: %s", settings_path);
}

static int rows_on_page(void)
{
    return page == PAGE_SETTINGS ? R_COUNT : ACT_COUNT + 1;    /* the actions, then Back */
}

static binding *page_binding(int act)
{
    return page == PAGE_PAD ? &st.pad[act] : &st.key[page == PAGE_KEY2][act];
}

static void setting_change(int row, int dir)
{
    switch (row) {
    case R_COINS: st.coins = !st.coins; break;
    case R_FULLSCREEN: st.fullscreen = (st.fullscreen + 1 + (dir > 0 ? 1 : 2)) % 3 - 1; break;
    case R_RENDERER: st.renderer = !st.renderer; break;
    case R_SCALE: st.scale = (st.scale - 1 + (dir > 0 ? 1 : 3)) % 4 + 1; break;
    case R_PICTURE: st.picture = !st.picture; break;
    case R_FILTER: st.filter = !st.filter; break;
    case R_SOUND: st.sound = !st.sound; break;
    case R_CARDS: st.cards = !st.cards; break;
    default: break;
    }
}

/* A or Enter on a settings row */
static void settings_back(void);

static void settings_press(int row)
{
    if (page == PAGE_SETTINGS) {
        if (row == R_PAD) page = PAGE_PAD, st_row = 0;
        else if (row == R_KEY1) page = PAGE_KEY1, st_row = 0;
        else if (row == R_KEY2) page = PAGE_KEY2, st_row = 0;
        else if (row == R_BACK) {
            settings_store();
            page = PAGE_MAIN;
        } else setting_change(row, 1);
        return;
    }
    if (row == ACT_COUNT) {         /* Back, to the settings page */
        settings_back();
        return;
    }
    capturing = 1;
}

static void settings_back(void)
{
    if (page == PAGE_SETTINGS) {
        settings_store();
        page = PAGE_MAIN;
    } else {
        st_row = page == PAGE_PAD ? R_PAD : page == PAGE_KEY1 ? R_KEY1 : R_KEY2;
        page = PAGE_SETTINGS;
    }
}

/* what a controls row takes: replaces the row's binding */
static void capture(const char *name)
{
    binding *b = page_binding(st_row);
    memset(b, 0, sizeof *b);
    settings_binding_add(b, name);
    capturing = 0;
}

static void settings_event(const SDL_Event *e)
{
    if (capturing) {
        if (page == PAGE_PAD) {
            if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                capture(SDL_GetGamepadStringForButton((SDL_GamepadButton)e->gbutton.button));
            else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e->gaxis.value > 16000 &&
                     (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                capture(SDL_GetGamepadStringForAxis((SDL_GamepadAxis)e->gaxis.axis));
            else if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE)
                capturing = 0;
        } else if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
            if (e->key.key == SDLK_ESCAPE)
                capturing = 0;
            else
                capture(SDL_GetScancodeName(e->key.scancode));
        } else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
            capturing = 0;          /* B on a pad: a keyboard row cannot take a pad button */
        }
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return;
    int k = e->type == SDL_EVENT_KEY_DOWN ? (int)e->key.key : -1;
    int g = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.button : -1;
    int n = rows_on_page();
    if (k == SDLK_DOWN || k == SDLK_TAB || g == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
        st_row = (st_row + 1) % n;
    else if (k == SDLK_UP || g == SDL_GAMEPAD_BUTTON_DPAD_UP)
        st_row = (st_row + n - 1) % n;
    else if (page == PAGE_SETTINGS && (k == SDLK_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT))
        setting_change(st_row, -1);
    else if (page == PAGE_SETTINGS && (k == SDLK_RIGHT || g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        setting_change(st_row, 1);
    else if (k == SDLK_RETURN || k == SDLK_SPACE || g == SDL_GAMEPAD_BUTTON_SOUTH)
        settings_press(st_row);
    else if (page != PAGE_SETTINGS && st_row < ACT_COUNT && (k == SDLK_DELETE || k == SDLK_BACKSPACE ||
                                                             g == SDL_GAMEPAD_BUTTON_WEST))
        memset(page_binding(st_row), 0, sizeof(binding));
    else if (page != PAGE_SETTINGS && st_row < ACT_COUNT && g == SDL_GAMEPAD_BUTTON_NORTH) {
        /* Y: put the defaults back on this row */
        settings d;
        settings_defaults(&d);
        *page_binding(st_row) = page == PAGE_PAD ? d.pad[st_row] : d.key[page == PAGE_KEY2][st_row];
    } else if (k == SDLK_ESCAPE || g == SDL_GAMEPAD_BUTTON_EAST)
        settings_back();
}

/* ---- the buttons ---- */

enum { B_SET, B_BUILD, B_UPDATE, B_PLAY, B_SETTINGS, B_STEAM, B_TARGET, B_COUNT };
static const char *const names[B_COUNT] = {"Choose set", "Build", "Update", "Play", "Settings", "Add to Steam",
                                           "Target"};
static SDL_FRect rects[B_COUNT];
static int focus = B_SET;

static int enabled(int b)
{
    switch (b) {
    case B_BUILD: return set_path[0] && root[0] && !proc;
    case B_UPDATE: return built && root[0] && !proc;
    case B_PLAY: return built && !game_proc;
    case B_STEAM: return built && root[0] && !proc && !built_linux;
#ifdef _WIN32
    case B_TARGET: return 0;
#else
    case B_TARGET: return !proc;
#endif
    default: return !proc;
    }
}

static void press(int b)
{
    if (!enabled(b))
        return;
    char script[1100], prog[1100];
    const char *target = target_linux ? "linux" : "windows";
    program_path(prog, sizeof prog);
    switch (b) {
    case B_SET:
        SDL_ShowOpenFolderDialog(on_folder, NULL, win, set_path[0] ? set_path : NULL, false);
        break;
    case B_BUILD: {
        snprintf(script, sizeof script, "%stools/make_game.py", root);
        const char *args[] = {python, "-I", script, "--set", set_path, "--out", game, "--target", target, NULL};
        nlines = 0;
        start("Build", args, 1);
        break;
    }
    case B_UPDATE: {
        snprintf(script, sizeof script, "%stools/make_game.py", root);
        const char *args[] = {python, "-I", script, "--update", game, NULL};
        nlines = 0;
        start("Update", args, 1);
        break;
    }
    case B_PLAY: {
#ifndef _WIN32
        if (!built_linux) {
            /* the Windows exe on Linux plays through Steam (Proton, Steam Input) */
            snprintf(script, sizeof script, "%stools/steam_shortcut.py", root);
            const char *args[] = {python, "-I", script, "play", prog, NULL};
            start("Play through Steam", args, 1);
            break;
        }
#endif
        const char *args[] = {prog, NULL};
        if (start("Play", args, 0) == 0)
            say("started %s", prog);
        break;
    }
    case B_STEAM: {
        snprintf(script, sizeof script, "%stools/steam_shortcut.py", root);
        const char *args[] = {python, "-I", script, "add", prog, NULL};
        say("Steam must be closed (Steam menu, Exit) for this");
        start("Add to Steam", args, 1);
        break;
    }
    case B_TARGET:
        target_linux = !target_linux;
        break;
    case B_SETTINGS:
        settings_open();
        page = PAGE_SETTINGS;
        st_row = 0;
        break;
    }
}

static void text(float x, float y, const char *s, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderDebugText(ren, x, y, s);
}

static void draw_settings(void)
{
    char b[200], v[160];
    const char *title = page == PAGE_SETTINGS ? "Settings" : page == PAGE_PAD ? "Controls: gamepads"
                        : page == PAGE_KEY1 ? "Controls: keyboard, player 1" : "Controls: keyboard, player 2";
    text(8, 6, title, 255, 200, 80);
    int n = rows_on_page();
    for (int r = 0; r < n; r++) {
        float y = 26.0f + 16.0f * (float)r;
        const char *label = "";
        v[0] = 0;
        if (page == PAGE_SETTINGS) {
            static const char *const fs[] = {"Auto (on for Steam Deck)", "Off", "On"};
            switch (r) {
            case R_COINS: label = "Credits"; SDL_strlcpy(v, st.coins == COINS_ARCADE ? "Arcade coins" : "Free play", sizeof v); break;
            case R_RENDERER: label = "Renderer"; SDL_strlcpy(v, st.renderer == RENDERER_GPU ? "GPU (Vulkan)" : "Software (CPU)", sizeof v); break;
            case R_SCALE:
                label = "Resolution";
                snprintf(v, sizeof v, "%d x %d%s", 640 * st.scale, 480 * st.scale, st.renderer == RENDERER_GPU ? "" : " (GPU only)");
                break;
            case R_FULLSCREEN: label = "Fullscreen"; SDL_strlcpy(v, fs[st.fullscreen + 1], sizeof v); break;
            case R_PICTURE: label = "Picture"; SDL_strlcpy(v, st.picture == PICTURE_STRETCH ? "Stretch" : "4:3", sizeof v); break;
            case R_FILTER: label = "Scaling"; SDL_strlcpy(v, st.filter == FILTER_SHARP ? "Sharp" : "Smooth", sizeof v); break;
            case R_SOUND: label = "Sound"; SDL_strlcpy(v, st.sound ? "On" : "Off", sizeof v); break;
            case R_CARDS: label = "Card units"; SDL_strlcpy(v, st.cards ? "On (VF.NET cards in user/cards)" : "Off", sizeof v); break;
            case R_PAD: label = "Controls: gamepads >"; break;
            case R_KEY1: label = "Controls: keyboard, player 1 >"; break;
            case R_KEY2: label = "Controls: keyboard, player 2 >"; break;
            case R_BACK: label = "Save and go back"; break;
            }
        } else if (r < ACT_COUNT) {
            label = settings_action_text[r];
            if (capturing && r == st_row)
                SDL_strlcpy(v, page == PAGE_PAD ? "press a button... (Esc: cancel)" : "press a key... (Esc: cancel)", sizeof v);
            else
                settings_binding_text(page_binding(r), v, sizeof v);
        } else {
            label = "Back";
        }
        if (r == st_row) {
            SDL_SetRenderDrawColor(ren, 50, 70, 110, 255);
            SDL_RenderFillRect(ren, &(SDL_FRect){4, y - 3, LW - 8, 14});
        }
        text(12, y, label, 255, 255, 255);
        if (v[0])       /* the controls' column clears "Service (operator)", 18 characters */
            text(page == PAGE_SETTINGS ? 140 : 168, y, v, 255, 220, 140);
    }
    if (page == PAGE_SETTINGS)
        snprintf(b, sizeof b, "Up/Down: choose  Left/Right/A: change  B: save, back");
    else
        snprintf(b, sizeof b, "A: set  X: clear  Y: default  B: back");
    text(8, LH - 24, b, 170, 200, 170);
    snprintf(b, sizeof b, "Read by the game when it starts. Stick: always moves.");
    if (page == PAGE_SETTINGS || page == PAGE_PAD)
        text(8, LH - 12, page == PAGE_PAD ? b : "Saved in game/user/settings.txt", 120, 140, 120);
}

static void draw(void)
{
    if (page != PAGE_MAIN) {
        SDL_SetRenderDrawColor(ren, 16, 18, 24, 255);
        SDL_RenderClear(ren);
        draw_settings();
        SDL_RenderPresent(ren);
        return;
    }
    SDL_SetRenderDrawColor(ren, 16, 18, 24, 255);
    SDL_RenderClear(ren);
    text(8, 6, "Virtua Fighter 4 Final Tuned: builder", 255, 200, 80);
    char b[1200];
    snprintf(b, sizeof b, "Set:  %s", set_path[0] ? set_path : "(none chosen: your MAME vf4tuned set)");
    text(8, 22, strlen(b) > COLS - 1 ? b + strlen(b) - (COLS - 1) : b, 220, 220, 220);
    if (built)
        snprintf(b, sizeof b, "Game: built by %s, %s", built_ver, built_linux ? "native Linux" : "Windows exe");
    else
        snprintf(b, sizeof b, "Game: not built yet");
    text(8, 34, b, 220, 220, 220);
    if (!root[0])
        text(8, 46, "builder files not found beside the launcher", 255, 90, 90);

    float x = 8, y = 60;
    for (int i = 0; i < B_COUNT; i++) {
        char label[40];
        if (i == B_TARGET)
            snprintf(label, sizeof label, "Target: %s", target_linux ? "Linux" : "Windows");
        else
            SDL_strlcpy(label, names[i], sizeof label);
#ifdef _WIN32
        if (i == B_TARGET)
            continue;
#endif
        float w = 8.0f * (float)strlen(label) + 12;
        if (x + w > LW - 8) {
            x = 8;
            y += 22;
        }
        rects[i] = (SDL_FRect){x, y, w, 16};
        int on = enabled(i);
        SDL_SetRenderDrawColor(ren, on ? 50 : 30, on ? 70 : 32, on ? 110 : 40, 255);
        SDL_RenderFillRect(ren, &rects[i]);
        if (i == focus) {
            SDL_SetRenderDrawColor(ren, 255, 200, 80, 255);
            SDL_RenderRect(ren, &rects[i]);
        }
        text(x + 6, y + 4, label, on ? 255 : 110, on ? 255 : 110, on ? 255 : 110);
        x += w + 6;
    }
    float ly = y + 26;
    SDL_SetRenderDrawColor(ren, 8, 9, 12, 255);
    SDL_RenderFillRect(ren, &(SDL_FRect){4, ly - 2, LW - 8, LH - ly - 14});
    for (int i = 0; i < nlines && ly + 10 * i < LH - 22; i++)
        text(8, ly + 10 * i, lines[i], 170, 200, 170);
    text(8, LH - 11, status, 255, 200, 80);
    SDL_RenderPresent(ren);
}

int main(int argc, char **argv)
{
    const char *opt_game = NULL, *opt_set = NULL, *opt_do = NULL, *shot_page = NULL, *shot_file = NULL;
    int opt_exit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) opt_game = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) opt_set = argv[++i];
        else if (!strcmp(argv[i], "--do") && i + 1 < argc) opt_do = argv[++i];
        else if (!strcmp(argv[i], "--exit")) opt_exit = 1;
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) shot_page = argv[++i], shot_file = argv[++i];
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer("VF4 Final Tuned builder", LW * 2, LH * 2, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        SDL_Log("window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, LW, LH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(ren, 1);
    find_paths();
    {
        char lp[1100];
        snprintf(lp, sizeof lp, "%slauncher.log", base);
        log_file = fopen(lp, "w");
    }
    if (opt_game)
        SDL_strlcpy(game, opt_game, sizeof game);
    load_state();
    if (opt_set)
        SDL_strlcpy(set_path, opt_set, sizeof set_path);
    say("builder files: %s", root[0] ? root : "(not found)");
    say("install folder: %s", game);
    if (!set_path[0])
        find_set();
    say(built ? "Play, or Update after a new release." : set_path[0] ? "Press Build." : "Choose your MAME set's folder, then Build.");
    SDL_strlcpy(status, "ready", sizeof status);
    if (opt_do) {
        static const char *const acts[] = {"set", "build", "update", "play", "steam"};
        int found = 0;
        for (int i = 0; i < 5; i++)
            if (!strcmp(opt_do, acts[i])) {
                found = 1;
                say("--do %s (%s)", opt_do, enabled(i) ? "pressing" : "not available now");
                press(i);
            }
        if (!found)
            say("--do %s: not an action (build, update, play, steam)", opt_do);
    }
    if (shot_page) {
        static const char *const pages[] = {"main", "settings", "pad", "key1", "key2"};
        for (int i = 0; i < 5; i++)
            if (!strcmp(shot_page, pages[i]))
                page = i;
        if (page != PAGE_MAIN)
            settings_open();
        draw();
        SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
        int ok = sf && SDL_SaveBMP(sf, shot_file);
        SDL_DestroySurface(sf);
        SDL_Quit();
        return ok ? 0 : 1;
    }
    int running = 1, result = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                running = 0;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                SDL_OpenGamepad(e.gdevice.which);
            else if (page != PAGE_MAIN) {
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && !capturing) {
                    SDL_ConvertEventToRenderCoordinates(ren, &e);
                    int r = (int)((e.button.y - 23.0f) / 16.0f);
                    if (e.button.y >= 23.0f && r < rows_on_page()) {
                        st_row = r;
                        settings_press(r);
                    }
                } else {
                    settings_event(&e);
                }
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                SDL_ConvertEventToRenderCoordinates(ren, &e);
                SDL_FPoint pt = {e.button.x, e.button.y};
                for (int i = 0; i < B_COUNT; i++)
                    if (SDL_PointInRectFloat(&pt, &rects[i])) {
                        focus = i;
                        press(i);
                    }
            } else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                int k = e.type == SDL_EVENT_KEY_DOWN ? (int)e.key.key : -1;
                int g = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e.gbutton.button : -1;
                int n = B_COUNT;
                if (k == SDLK_RIGHT || k == SDLK_TAB || g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
                    focus = (focus + 1) % n;
                else if (k == SDLK_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
                    focus = (focus + n - 1) % n;
                else if (k == SDLK_RETURN || k == SDLK_SPACE || g == SDL_GAMEPAD_BUTTON_SOUTH)
                    press(focus);
                else if (k == SDLK_ESCAPE)
                    running = 0;
#ifdef _WIN32
                if (focus == B_TARGET)
                    focus = (k == SDLK_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? B_STEAM : B_SET;
#endif
            }
        }
        if (SDL_GetAtomicInt(&picked)) {
            SDL_SetAtomicInt(&picked, 0);
            SDL_strlcpy(set_path, picked_path, sizeof set_path);
            SDL_SaveFile(set_file, set_path, strlen(set_path));
            say("set: %s", set_path);
        }
        int was = proc != NULL;
        pump_process();
        watch_game();
        if (game_proc) {
            SDL_Delay(100);           /* hidden while the game runs: nothing to draw */
            continue;
        }
        if (was && !proc) {
            result = strstr(status, "FAILED") ? 1 : 0;
            if (opt_exit)
                running = 0;
        } else if (opt_exit && opt_do && !proc && !was) {
            running = 0;          /* the action started nothing to wait for */
        }
        draw();
    }
    if (proc)
        SDL_KillProcess(proc, false);
    SDL_Quit();
    return result;
}
