/* vf4ft-launcher: the builder's window (milestone 5, docs/design.md
 * sections 8 and 22).
 *
 * Three steps, as its stepper shows: Game files (the player's MAME set),
 * Build, Play; and Update after a new release, Add to Steam, and the game's
 * Settings. It holds no game code: the work is done by the builder
 * (tools/make_game.py --launcher, whose progress lines it shows and whose
 * standard input it closes to cancel) and the Steam tool
 * (tools/steam_shortcut.py), run with Python. It draws with runtime/ui, the
 * kit the game's menu uses, and needs nothing the game does not.
 *
 * Folders, beside the launcher:
 *   builder/   this repository's tools/, translator/, runtime/, meta/ (a release's
 *              layout; a development build finds the repository above itself)
 *   python/    Windows: the embeddable Python the release carries (pythonw.exe)
 *   game/      the install the builder makes (game/build.json once built), unless
 *              the player chose another folder (Change on the Build page)
 *   launcher-set.txt   the set folder last chosen
 *   launcher-game.txt  the install folder chosen (none: game/ beside the launcher)
 *   launcher-direct.txt  "1": Start the game directly (the Play page's switch)
 *   launcher.log       what it showed (Windows has no console for it)
 * On Linux, Python is the system's python3.
 *
 * Steam Deck: the Windows release, added to Steam as a non-Steam game with
 * Proton, runs in Game Mode; the D-pad and A work it. With no set chosen it
 * looks for one in the usual places (find_set), Linux folders through Wine's
 * Z: drive, and offers what it finds (Use this set); it never chooses one by
 * itself. Play starts the game in the same Proton prefix and the launcher
 * hides until it ends.
 *
 * Settings (M6): the game's settings file, game/user/settings.txt
 * (runtime/host/settings.h), in tabs as the game's menu has them: free play
 * or coins, the card units, fullscreen, picture, sound, and the controls (a
 * row takes the next key or button). The game reads it when it starts.
 *
 * For testing: --game DIR (another install folder), --pick-game DIR (as if
 * Change on the Build page picked DIR), --set DIR, --do ACTION
 * (set, build, update, play or steam: pressed at the start) and --exit (quit
 * when that action's process ends, with its result as the exit code; 1 when
 * the action started nothing), --cancel-after S (presses Cancel S seconds
 * after it started); --shot PAGE FILE draws one page (files,
 * build, work, fail, play, settings, picture, pad, key1, key2) to a BMP file and quits,
 * its caps in the style --shot-device names (keys, xbox, ps5, switch, steam...:
 * ui_device_named), --shot-scale K times as big
 * (with SDL_VIDEO_DRIVER=offscreen no window shows).
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>

#include "options.h"
#include "settings.h"
#include "ui.h"

#define LW 960.0f           /* the page, in logical units (ui.h) */
#define LH 600.0f
#define CX 64.0f            /* the content's left edge and width */
#define CW 832.0f
#define BTN_Y 516.0f        /* the buttons' row */
#define BTN_H 48.0f
#define LOG_MAX 200
#define LOG_W 240

static SDL_Window *win;
static SDL_Renderer *ren;
static char base[896], root[1024], game[1024], python[1024], set_path[1024], set_file[1024];

/* a path as shown: under the home folder as ~/... (not on Windows) */
static const char *home_short(const char *path, char *buf, size_t n)
{
#ifndef _WIN32
    const char *home = SDL_getenv("HOME");
    size_t h = home ? strlen(home) : 0;
    if (h > 1 && !strncmp(path, home, h) && (path[h] == '/' || !path[h])) {
        SDL_snprintf(buf, n, "~%s", path + h);
        return buf;
    }
#endif
    SDL_strlcpy(buf, path, n);
    return buf;
}
static char builder_ver[64];        /* this release's builder: an update is there when it is not built_ver */
static char lines[LOG_MAX][LOG_W];
static int nlines;
static char partial[1024];
static size_t npartial;
static FILE *log_file;
static int target_linux;            /* Linux only: a native build instead of the Windows exe */
static int built, built_linux;
static char built_ver[64];
static SDL_AtomicInt picked;        /* the folder dialog's answer waits here for the main thread */
static char picked_path[1024];
static SDL_AtomicInt picked_game;   /* the same for the install folder's dialog */
static char picked_game_path[1024];
static char game_file[1024];        /* launcher-game.txt */
static char found_path[1024];       /* a set find_set found: offered, used once the player says so */
static char saves_from[1024];       /* the install before Change: its user/ goes into the new one at Build */
static char game_note[400];         /* the Build page's line about the install folder */
static int game_note_bad;
static int no_add_to_steam;         /* Steam started the launcher, or Wine/Proton runs it: no Add to Steam */

/* Start the game directly (docs/design.md section 29): with the Play page's
 * switch on, the launcher starts hidden and presses Play itself, and closes
 * when the game ends, so a Steam entry for the launcher plays the game. It
 * shows itself instead when it is needed: no game built, a new builder or
 * an update to put in, the game's "Open the launcher" (exit code 64,
 * runtime/host/updates.h) or a game that ended with an error. --show (the
 * game's Open the launcher, for a game started on its own) never plays. */
#define GAME_EXIT_LAUNCHER 64
static int play_direct;             /* the switch */
static int direct_run;              /* this run started the game directly: the launcher closes with it */
static int direct_close;            /* the game it started directly ended: close */

static void direct_path(char *p, size_t n)
{
    const char *b = SDL_GetBasePath();     /* base, before find_paths has set it */
    snprintf(p, n, "%slauncher-direct.txt", b ? b : "./");
}

static void say(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    SDL_Log("%s", b);
    if (log_file) {
        fprintf(log_file, "[%7.1f] %s\n", SDL_GetTicks() / 1000.0, b);
        fflush(log_file);
    }
    if (nlines == LOG_MAX) {
        memmove(lines[0], lines[1], sizeof lines[0] * (LOG_MAX - 1));
        nlines--;
    }
    SDL_strlcpy(lines[nlines++], b, LOG_W);
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
    snprintf(game_file, sizeof game_file, "%slauncher-game.txt", base);
    char *g = SDL_LoadFile(game_file, NULL);
    if (g) {
        g[strcspn(g, "\r\n")] = 0;
        if (g[0])
            SDL_strlcpy(game, g, sizeof game);
        SDL_free(g);
    }
#ifdef _WIN32
    snprintf(python, sizeof python, "%spython\\pythonw.exe", base);
    if (!exists(python))
        SDL_strlcpy(python, "pythonw.exe", sizeof python);    /* one on PATH, for development */
#else
    SDL_strlcpy(python, "python3", sizeof python);
#endif
    /* the builder's version: VERSION = "..." in tools/vf4build/__init__.py */
    builder_ver[0] = 0;
    snprintf(probe, sizeof probe, "%stools/vf4build/__init__.py", root);
    char *s = root[0] ? SDL_LoadFile(probe, NULL) : NULL;
    const char *v = s ? strstr(s, "VERSION = \"") : NULL;
    if (v) {
        v += 11;
        snprintf(builder_ver, sizeof builder_ver, "%.*s", (int)SDL_min(strcspn(v, "\""), 60), v);
    }
    SDL_free(s);
}

/* what the install folder holds: built, for which target, by which builder */
static void load_install(void)
{
    size_t n = 0;
    char p[1100];
    snprintf(p, sizeof p, "%s/build.json", game);
    char *s = SDL_LoadFile(p, &n);
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

static void load_state(void)
{
    char *s = SDL_LoadFile(set_file, NULL);
    set_path[0] = 0;
    if (s) {
        SDL_strlcpy(set_path, s, sizeof set_path);
        set_path[strcspn(set_path, "\r\n")] = 0;
        SDL_free(s);
    }
    load_install();
}

static int update_ready(void)
{
    return built && builder_ver[0] && strcmp(builder_ver, built_ver) != 0;
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

static int set_ok(void)
{
    return set_path[0] && is_set(set_path);
}

static int found_set(const char *dir)
{
    if (!is_set(dir))
        return 0;
    SDL_strlcpy(found_path, dir, sizeof found_path);
    say("found a set in %s: offered on the Game files page", found_path);
    return 1;
}

/* ---- the install folder ---- */

/* what the builder puts in an install folder (tools/make_game.py's
 * INSTALL_NAMES, which refuses a folder with anything else in it), and what
 * the game and Wine or Proton add beside the game */
static const char *const install_names[] = {
    "build", "data", "user", "build.log", "build.json", "build.json.part", "READ ME.txt", "Font license.txt",
    "vf4ft.exe", "vf4ft", "SDL3.dll", "vf4ft.log", "vkd3d-proton.cache", "vkd3d-proton.cache.write",
    "vf4ft.dxvk-cache", NULL};

typedef struct dir_look { int entries, others; char other[128]; } dir_look;

static SDL_EnumerationResult SDLCALL look_one(void *ud, const char *dir, const char *name)
{
    (void)dir;
    dir_look *l = ud;
    l->entries++;
    for (int i = 0; install_names[i]; i++)
#ifdef _WIN32
        if (!SDL_strcasecmp(name, install_names[i]))
#else
        if (!strcmp(name, install_names[i]))
#endif
            return SDL_ENUM_CONTINUE;
    if (!l->others++)
        SDL_strlcpy(l->other, name, sizeof l->other);
    return SDL_ENUM_CONTINUE;
}

/* 0: not there or empty, 1: a game's install (the builder's own files
 * only), -1: other files too (*other: the first of them) */
static int install_kind(const char *dir, char *other, size_t n)
{
    if (!exists(dir))
        return 0;
    dir_look l;
    SDL_zero(l);
    if (!SDL_EnumerateDirectory(dir, look_one, &l)) {
        SDL_strlcpy(other, SDL_GetError(), n);
        return -1;
    }
    if (l.others) {
        SDL_strlcpy(other, l.other, n);
        return -1;
    }
    return l.entries ? 1 : 0;
}

static int has_files(const char *dir)
{
    dir_look l;
    SDL_zero(l);
    return exists(dir) && SDL_EnumerateDirectory(dir, look_one, &l) && l.entries > 0;
}

/* the folder the player picked with Change: the game goes in it when it is
 * empty or a game's install already; else into a "VF4 Final Tuned" folder
 * in it, so that the builder never writes among other files */
static void choose_game_dir(const char *pick)
{
    char dir[1024], other[160];
    size_t pl = strlen(pick);
    int sep = pl && (pick[pl - 1] == '/' || pick[pl - 1] == '\\');
    if (install_kind(pick, other, sizeof other) >= 0) {
        SDL_snprintf(dir, sizeof dir, "%.*s", (int)(sep ? pl - 1 : pl), pick);
        game_note[0] = 0;
    } else {
#ifdef _WIN32
        SDL_snprintf(dir, sizeof dir, "%.*s\\VF4 Final Tuned", (int)(sep ? pl - 1 : pl), pick);
#else
        SDL_snprintf(dir, sizeof dir, "%.*s/VF4 Final Tuned", (int)(sep ? pl - 1 : pl), pick);
#endif
        if (install_kind(dir, other, sizeof other) < 0) {
            SDL_snprintf(game_note, sizeof game_note,
                         "Not changed: the folder you chose has other files in it, and so has its "
                         "\"VF4 Final Tuned\" folder. Choose an empty folder.");
            game_note_bad = 1;
            say("install folder: not changed: %s has other files in it (%s)", dir, other);
            return;
        }
        SDL_snprintf(game_note, sizeof game_note,
                     "The folder you chose has other files in it, so the game goes in a folder of its own there.");
        game_note_bad = 0;
    }
    if (!strcmp(dir, game))
        return;
    /* the saves of the install it leaves go with the player (copied at Build) */
    char u[1100];
    snprintf(u, sizeof u, "%s/user", game);
    if (!saves_from[0] && has_files(u))
        SDL_strlcpy(saves_from, game, sizeof saves_from);
    if (saves_from[0] && !strcmp(saves_from, dir))
        saves_from[0] = 0;
    SDL_strlcpy(game, dir, sizeof game);
    SDL_SaveFile(game_file, game, strlen(game));
    say("install folder: %s (chosen)", game);
    load_install();
}

/* recursive copy of src's files into dst (made); 0 when all went */
static int copy_tree(const char *src, const char *dst);

typedef struct copy_ctx { const char *dst; int failed; } copy_ctx;

static SDL_EnumerationResult SDLCALL copy_one(void *ud, const char *dir, const char *name)
{
    copy_ctx *c = ud;
    char from[1400], to[1400];
    size_t dl = strlen(dir);
    int sep = dl && (dir[dl - 1] == '/' || dir[dl - 1] == '\\');
    snprintf(from, sizeof from, "%.*s/%s", (int)(sep ? dl - 1 : dl), dir, name);
    snprintf(to, sizeof to, "%s/%s", c->dst, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(from, &info))
        c->failed = 1;
    else if (info.type == SDL_PATHTYPE_DIRECTORY)
        c->failed |= copy_tree(from, to) != 0;
    else if (!SDL_CopyFile(from, to)) {
        say("saves: cannot copy %s: %s", from, SDL_GetError());
        c->failed = 1;
    }
    return SDL_ENUM_CONTINUE;
}

static int copy_tree(const char *src, const char *dst)
{
    if (!exists(dst) && !SDL_CreateDirectory(dst)) {
        say("saves: cannot make %s: %s", dst, SDL_GetError());
        return -1;
    }
    copy_ctx c = {dst, 0};
    if (!SDL_EnumerateDirectory(src, copy_one, &c))
        return -1;
    return c.failed ? -1 : 0;
}

/* at Build into a new install folder: the old one's saves and settings */
static void copy_saves(void)
{
    if (!saves_from[0])
        return;
    char src[1100], dst[1100];
    snprintf(src, sizeof src, "%s/user", saves_from);
    snprintf(dst, sizeof dst, "%s/user", game);
    if (has_files(dst)) {
        say("saves: %s has its own already; those in %s stay where they are", dst, src);
    } else {
        if (!exists(game))
            SDL_CreateDirectory(game);
        if (copy_tree(src, dst) == 0)
            say("saves: copied your saves and settings from %s", src);
        else
            say("saves: not all of %s could be copied (the old ones are still there)", src);
    }
    saves_from[0] = 0;
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
    say("no set found by itself: choose its folder");
}

/* Add to Steam edits Steam's shortcut files, which Steam rewrites when it
 * exits: it works only with Steam closed, from outside Steam. So it is not
 * offered when Steam started the launcher (Steam's own launch variables;
 * under Proton the launcher's Steam entry is already how the game is
 * played), nor to the Windows launcher under Wine or Proton, where the
 * tool would see the prefix's stand-in Steam and not the real one. */
static void steam_setup(void)
{
    static const char *const vars[] = {"SteamGameId", "SteamClientLaunch", "SteamAppId", NULL};
    for (int i = 0; vars[i]; i++) {
        const char *v = SDL_getenv(vars[i]);
        if (v && v[0]) {
            no_add_to_steam = 1;
            say("steam: started by Steam (%s=%s): no Add to Steam, the launcher is in Steam already", vars[i], v);
            return;
        }
    }
#ifdef _WIN32
    SDL_SharedObject *nt = SDL_LoadObject("ntdll.dll");
    const char *(*wine_version)(void) = nt ? (const char *(*)(void))SDL_LoadFunction(nt, "wine_get_version") : NULL;
    if (wine_version) {
        no_add_to_steam = 1;
        say("steam: Wine %s (Proton or plain Wine): no Add to Steam, which needs the real Steam closed",
            wine_version());
    }
    if (nt)
        SDL_UnloadObject(nt);
#endif
}

static void program_path(char *out, size_t n)
{
#ifdef _WIN32
    snprintf(out, n, "%s\\vf4ft.exe", game);
#else
    snprintf(out, n, "%s/%s", game, built_linux ? "vf4ft" : "vf4ft.exe");
#endif
}

/* ---- the processes: the builder, the Steam tool, the game ---- */

enum { K_NONE, K_BUILD, K_UPDATE, K_STEAM, K_STEAM_PLAY };
static SDL_Process *proc;
static SDL_Process *game_proc;      /* the game, while it runs (the launcher hides meanwhile) */
static SDL_IOStream *proc_out, *proc_in;
static int proc_kind, last_kind;     /* last_kind: the last build's (Try again does it again) */
static Uint64 proc_t0, cancel_at;   /* ms */
static int stage_n, stage_of, stage_pct = -1;
static char stage_text[96];
static double shot_elapsed = -1;    /* --shot work: the time shown */
static int shot_working;            /* --shot work: drawn as during a build (Cancel can be pressed) */
static int cancelled;
static char error_line[300];
/* the Play page's line: what the last action came to */
enum { ST_NONE, ST_WORKING, ST_GOOD, ST_BAD };
static int status_kind;
static char status_text[300];

enum { P_FILES, P_BUILD, P_WORK, P_FAIL, P_PLAY, P_SETTINGS };
static int page;
static void go(int p);

static int start(int kind, const char **args, int capture)
{
    if (proc)
        return -1;
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    if (capture) {
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        if (kind == K_BUILD || kind == K_UPDATE)    /* closing it cancels the build */
            SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    }
    SDL_Environment *env = NULL;
    if (!capture) {
        /* the game: no input or output of the launcher's. Not SDL's background
         * mode, whose exit code is always 0 (SDL_process.h; on Linux it forks
         * twice), and the launcher reads it: the menu's Open the launcher
         * (GAME_EXIT_LAUNCHER) or an error. Both programs are GUI ones on
         * Windows, so no console opens. */
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        /* it knows the launcher waits for it, so an update asked for
         * in its menu is the launcher's to do when it ends (runtime/host/updates.h).
         * An environment variable, not an option: an older game ignores it */
        env = SDL_CreateEnvironment(true);
        if (env && SDL_SetEnvironmentVariable(env, "VF4FT_LAUNCHER", "1", true))
            SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    }
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (env)
        SDL_DestroyEnvironment(env);
    if (!p) {
        snprintf(error_line, sizeof error_line, "cannot start %s: %s", args[0], SDL_GetError());
        say("%s", error_line);
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
    proc_out = SDL_GetProcessOutput(p);
    proc_in = kind == K_BUILD || kind == K_UPDATE ? SDL_GetProcessInput(p) : NULL;
    proc_kind = kind;
    proc_t0 = SDL_GetTicks();
    cancel_at = 0;
    npartial = 0;
    return 0;
}

static void cancel(void)
{
    if (!proc || cancel_at)
        return;
    say("cancelling (the builder stops its work and keeps the game as it was)");
    if (proc_in)
        SDL_CloseIO(proc_in);         /* the builder sees its input end */
    proc_in = NULL;
    cancel_at = SDL_GetTicks();
}

/* a line from the process: the builder's progress lines are kept, not shown */
static void take_line(const char *s)
{
    if ((proc_kind == K_BUILD || proc_kind == K_UPDATE) && s[0] == '@') {
        int n, of, at = 0;
        if (sscanf(s, "@stage %d %d %n", &n, &of, &at) == 2 && at) {
            stage_n = n;
            stage_of = of;
            stage_pct = -1;
            SDL_strlcpy(stage_text, s + at, sizeof stage_text);
        } else if (sscanf(s, "@pct %d", &n) == 1) {
            stage_pct = n;
        }
        return;
    }
    if (!strncmp(s, "error: ", 7)) {
        SDL_strlcpy(error_line, s + 7, sizeof error_line);
        size_t n = strlen(error_line);
        if (n && error_line[n - 1] == ':')      /* "compiling X failed:" and its output below */
            error_line[n - 1] = 0;
    }
    if (proc_kind == K_STEAM || proc_kind == K_STEAM_PLAY)
        SDL_strlcpy(status_text, s, sizeof status_text);
    say("%s", s);
}

/* reads the process's output; when it ends, its result (else -1) */
static int pump_process(void)
{
    if (!proc)
        return -1;
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
                if (partial[0])
                    take_line(partial);
                npartial = 0;
            } else {
                partial[npartial++] = b[i];
            }
        }
    }
    if (cancel_at && SDL_GetTicks() - cancel_at > 15000) {
        say("the builder did not stop in 15 seconds: ended");
        SDL_KillProcess(proc, true);
        cancel_at = SDL_GetTicks();
    }
    int code = 0;
    if (!SDL_WaitProcess(proc, false, &code))
        return -1;
    if (npartial) {
        partial[npartial] = 0;
        take_line(partial);
        npartial = 0;
    }
    SDL_DestroyProcess(proc);         /* it closes the streams still open */
    proc = NULL;
    proc_out = NULL;
    proc_in = NULL;
    load_install();
    switch (proc_kind) {
    case K_BUILD:
    case K_UPDATE:
        cancelled = cancel_at != 0 || code == 3;
        say(code == 0 ? "the game is built" : cancelled ? "cancelled" : "the build stopped (exit code %d)", code);
        if (code == 0) {
            status_kind = ST_GOOD;
            SDL_strlcpy(status_text, proc_kind == K_UPDATE ? "Updated." : "Built.", sizeof status_text);
            go(P_PLAY);
        } else {
            go(P_FAIL);
        }
        break;
    case K_STEAM:
        status_kind = code == 0 ? ST_GOOD : ST_BAD;
        if (code == 0)
            SDL_strlcpy(status_text, "Added to Steam: start Steam again and it is in your library.", sizeof status_text);
        break;
    case K_STEAM_PLAY:
        status_kind = code == 0 ? ST_GOOD : ST_BAD;
        if (code == 0)
            SDL_strlcpy(status_text, "Started through Steam.", sizeof status_text);
        break;
    }
    last_kind = proc_kind;
    proc_kind = K_NONE;
    cancel_at = 0;
    return code;
}

static void take_game_request(void);
static int upd_auto;        /* (updates, below) */

static void watch_game(void)
{
    if (!game_proc)
        return;
    int code = 0;
    if (SDL_WaitProcess(game_proc, false, &code)) {
        SDL_DestroyProcess(game_proc);
        game_proc = NULL;
        say("the game ended%s", code == GAME_EXIT_LAUNCHER ? ": its menu's Open the launcher" : "");
        take_game_request();        /* "Update when I quit" in the game's menu */
        if (direct_run && code == 0 && !upd_auto) {
            direct_close = 1;       /* started directly: no window */
            return;
        }
        direct_run = 0;
        if (code != 0 && code != GAME_EXIT_LAUNCHER) {
            status_kind = ST_BAD;
            snprintf(status_text, sizeof status_text, "The game ended with an error (code %d): its log is vf4ft.log "
                     "in the install folder.", code);
            say("%s", status_text);
        }
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);
    }
}

/* ---- updates: a newer release, from GitHub (docs/design.md section 24) ----
 * Only a release folder updates itself: release.json (version, platform,
 * repository) is beside the launcher. tools/update.py checks GitHub's list
 * of releases and downloads and unpacks a newer one into update/new; the
 * launcher then puts it in place (apply_update), at once ("Update now": it
 * starts the new launcher and quits) or when it closes. The next start
 * builds the game again with the new builder. */

#ifdef _WIN32
#define LAUNCHER_EXE "vf4ft-launcher.exe"
#else
#define LAUNCHER_EXE "vf4ft-launcher"
#endif

enum { U_OFF, U_IDLE, U_CHECKING, U_NONE, U_CHECK_FAILED, U_AVAILABLE, U_FETCHING, U_READY, U_FAILED };
enum { UW_NOW, UW_CLOSE };
static int upd = U_OFF;             /* U_OFF: not a release folder (a development build) */
static int upd_when = UW_NOW;       /* when a fetched update goes in */
static int upd_manual;              /* the check was asked for: its failure shows on the Play page */
static char rel_version[64], rel_platform[16];
static char upd_version[64], upd_error[300];
static long long upd_size;
static int upd_stage, upd_pct = -1;
static Uint64 upd_t0;
static SDL_Process *uproc;          /* tools/update.py, apart from the builder's process */
static SDL_IOStream *uproc_out, *uproc_in;
static int ukind;                   /* 1 check, 2 fetch */
static char upartial[512];
static size_t nupartial;
static int just_updated;            /* update/applied.json was there: this launcher is the new one */
static char applied_from[64];

/* a string field of a small JSON file: "key": "value" */
static int json_str(const char *s, const char *key, char *out, size_t n)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = s ? strstr(s, k) : NULL;
    if (!p)
        return 0;
    p += strlen(k);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '"')
        return 0;
    p++;
    size_t m = strcspn(p, "\"");
    snprintf(out, n, "%.*s", (int)SDL_min(m, n - 1), p);
    return 1;
}

/* versions as tools/update.py orders them: "0.6.0" > "0.6.0-dev" > "0.5.9" */
static int ver_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        int x = SDL_atoi(a), y = SDL_atoi(b);
        if (x != y)
            return x < y ? -1 : 1;
        a += strspn(a, "0123456789");
        b += strspn(b, "0123456789");
        if (i < 2) {
            a += *a == '.';
            b += *b == '.';
        }
    }
    int ra = *a != '-', rb = *b != '-';     /* no suffix: the release itself */
    if (ra != rb)
        return ra ? 1 : -1;
    int c = strcmp(a, b);
    return c < 0 ? -1 : c > 0;
}

static void upd_path(char *out, size_t n, const char *tail)
{
    snprintf(out, n, "%supdate%s%s", base, tail[0] ? "/" : "", tail);
}

/* release.json, update/applied.json (the launcher was just updated) and
 * update/ready.json (an update downloaded but not yet in place) */
static void upd_startup(void)
{
    char p[1100];
    upd_path(p, sizeof p, "applied.json");
    char *s = SDL_LoadFile(p, NULL);
    if (s) {
        just_updated = 1;
        json_str(s, "from", applied_from, sizeof applied_from);
        SDL_free(s);
        SDL_RemovePath(p);
    }
    snprintf(p, sizeof p, "%srelease.json", base);
    s = SDL_LoadFile(p, NULL);
    rel_version[0] = rel_platform[0] = 0;
    if (s && json_str(s, "version", rel_version, sizeof rel_version) &&
        json_str(s, "platform", rel_platform, sizeof rel_platform) && root[0])
        upd = U_IDLE;
    SDL_free(s);
    if (upd == U_OFF) {
        say("no release.json: a development build, which does not update itself");
        return;
    }
    say("release %s for %s", rel_version, rel_platform);
    upd_path(p, sizeof p, "ready.json");
    s = SDL_LoadFile(p, NULL);
    char v[64] = "", nd[1100];
    upd_path(nd, sizeof nd, "new");
    if (s && json_str(s, "version", v, sizeof v) && ver_cmp(v, rel_version) > 0 && exists(nd)) {
        SDL_strlcpy(upd_version, v, sizeof upd_version);
        upd = U_READY;
        upd_when = UW_CLOSE;
        say("update: %s is downloaded; it goes in when the launcher closes", v);
    }
    SDL_free(s);
}

static int upd_start(int kind);
static int upd_auto;        /* the update at once (Update now), as soon as there is one: the game asked for it */
static int after_game;      /* --after-game: the game started this launcher as it quit, for that update */

/* update/at-game-quit: "Update when I quit" in the game's menu
 * (runtime/host/updates.h). Taken (removed) when the launcher starts or
 * when the game it started ends: the update then goes as Update now, after
 * a fresh check. */
static void take_game_request(void)
{
    char p[1100];
    upd_path(p, sizeof p, "at-game-quit");
    if (upd == U_OFF || !exists(p))
        return;
    char *s = SDL_LoadFile(p, NULL), v[64] = "";
    json_str(s, "version", v, sizeof v);
    SDL_free(s);
    SDL_RemovePath(p);
    say("update: the game's menu asked for %s when the game quits: updating now", v[0] ? v : "an update");
    upd_auto = 1;
    upd_manual = 1;             /* a failed check says why on the Play page */
    if (!uproc && (upd == U_IDLE || upd == U_NONE || upd == U_CHECK_FAILED))
        upd_start(1);
}

/* starts tools/update.py: kind 1 checks, 2 downloads upd_version */
static int upd_start(int kind)
{
    if (uproc || upd == U_OFF)
        return -1;
    char script[1100], dir[900];
    snprintf(script, sizeof script, "%stools/update.py", root);
    SDL_strlcpy(dir, base, sizeof dir);
    size_t dl = strlen(dir);
    if (dl > 1 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\'))
        dir[dl - 1] = 0;          /* no separator before a closing quote on Windows' command line */
    const char *check_args[] = {python, "-I", script, "check", dir, NULL};
    const char *fetch_args[] = {python, "-I", script, "fetch", dir, upd_version, "--launcher", NULL};
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)(kind == 1 ? check_args : fetch_args));
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);   /* closing it cancels */
    uproc = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    upd_error[0] = 0;
    if (!uproc) {
        snprintf(upd_error, sizeof upd_error, "cannot start %.120s: %.150s", python, SDL_GetError());
        say("update: %s", upd_error);
        upd = kind == 1 ? U_CHECK_FAILED : U_FAILED;
        return -1;
    }
    uproc_out = SDL_GetProcessOutput(uproc);
    uproc_in = SDL_GetProcessInput(uproc);
    ukind = kind;
    nupartial = 0;
    upd_stage = 0;
    upd_pct = -1;
    upd_t0 = SDL_GetTicks();
    upd = kind == 1 ? U_CHECKING : U_FETCHING;
    return 0;
}

static void upd_cancel(void)
{
    if (uproc && uproc_in) {
        say("update: cancelling");
        SDL_CloseIO(uproc_in);      /* update.py sees its input end */
        uproc_in = NULL;
    }
}

static void upd_line(const char *s)
{
    char v[64];
    long long size = 0;
    int n, of, at = 0;
    if (!strcmp(s, "@update none")) {
        upd = U_NONE;
        say("update: %s is the newest release", rel_version);
    } else if (sscanf(s, "@update %63s %lld", v, &size) >= 1) {
        SDL_strlcpy(upd_version, v, sizeof upd_version);
        upd_size = size;
        upd = U_AVAILABLE;
        say("update: %s is out (this is %s)", v, rel_version);
    } else if (sscanf(s, "@stage %d %d %n", &n, &of, &at) == 2 && at) {
        upd_stage = n;
        upd_pct = -1;
    } else if (sscanf(s, "@pct %d", &n) == 1) {
        upd_pct = n;
    } else if (sscanf(s, "@ready %63s", v) == 1) {
        SDL_strlcpy(upd_version, v, sizeof upd_version);
        upd = U_READY;
        say("update: %s is downloaded", v);
    } else if (!strncmp(s, "error: ", 7)) {
        SDL_strlcpy(upd_error, s + 7, sizeof upd_error);
        say("update: %s", s + 7);
    } else if (s[0] != '@') {
        say("update: %s", s);
    }
}

static void upd_pump(void)
{
    if (!uproc)
        return;
    char b[512];
    for (;;) {
        size_t n = uproc_out ? SDL_ReadIO(uproc_out, b, sizeof b) : 0;
        if (n == 0)
            break;
        for (size_t i = 0; i < n; i++) {
            if (b[i] == '\n' || nupartial == sizeof upartial - 1) {
                upartial[nupartial] = 0;
                if (nupartial && upartial[nupartial - 1] == '\r')
                    upartial[nupartial - 1] = 0;
                if (upartial[0])
                    upd_line(upartial);
                nupartial = 0;
            } else {
                upartial[nupartial++] = b[i];
            }
        }
    }
    int code = 0;
    if (!SDL_WaitProcess(uproc, false, &code))
        return;
    if (nupartial) {
        upartial[nupartial] = 0;
        upd_line(upartial);
        nupartial = 0;
    }
    SDL_DestroyProcess(uproc);        /* it closes the streams still open */
    uproc = NULL;
    uproc_out = uproc_in = NULL;
    if (ukind == 1 && upd == U_CHECKING) {
        upd = U_CHECK_FAILED;
        if (!upd_error[0])
            snprintf(upd_error, sizeof upd_error, "the check ended with code %d", code);
    } else if (ukind == 2 && upd == U_FETCHING) {
        if (code == 3) {
            upd = U_AVAILABLE;
            say("update: the download was cancelled");
        } else {
            upd = U_FAILED;
            if (!upd_error[0])
                snprintf(upd_error, sizeof upd_error, "the download ended with code %d", code);
        }
    }
    if (ukind == 1 && upd_manual && upd == U_CHECK_FAILED) {
        status_kind = ST_BAD;
        snprintf(status_text, sizeof status_text, "Could not check for updates: %.260s", upd_error);
    }
    upd_manual = 0;
    ukind = 0;
}

/* moves a path, trying for a second while something holds it (a virus
 * scanner reading the new exe, say) */
static int move_path(const char *from, const char *to)
{
    for (int i = 0; i < 10; i++) {
        if (SDL_RenamePath(from, to))
            return 1;
        SDL_Delay(100);
    }
    return 0;
}

typedef struct entries { char name[16][128]; int n; } entries;

static SDL_EnumerationResult SDLCALL entry_one(void *ud, const char *dir, const char *name)
{
    (void)dir;
    entries *es = ud;
    if (es->n == 16)
        return SDL_ENUM_FAILURE;
    SDL_strlcpy(es->name[es->n++], name, sizeof es->name[0]);
    return SDL_ENUM_CONTINUE;
}

/* puts update/new in place. Each of its entries (the launcher, SDL3.dll,
 * python/, builder/, release.json...) takes the place of the release
 * folder's own, which moves to update/old-TIME/: Windows lets a running exe
 * and its DLL be moved, not written over. game/ and the launcher's own
 * files are not touched. A step that fails undoes the ones before it.
 * update.py's next check deletes the old folders. 0 when it is done. */
static int apply_update(void)
{
    char newd[1100], oldd[1100], tail[48], from[1400], to[1400], keep[1400];
    upd_path(newd, sizeof newd, "new");
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    snprintf(tail, sizeof tail, "old-%lld", (long long)(now / 1000000));
    upd_path(oldd, sizeof oldd, tail);
    entries es;
    SDL_zero(es);
    if (!SDL_EnumerateDirectory(newd, entry_one, &es) || es.n == 0) {
        snprintf(upd_error, sizeof upd_error, "%.180s cannot be read: %.100s", newd, SDL_GetError());
        say("update: %s", upd_error);
        return -1;
    }
    if (!SDL_CreateDirectory(oldd)) {
        snprintf(upd_error, sizeof upd_error, "cannot make %.180s: %.100s", oldd, SDL_GetError());
        say("update: %s", upd_error);
        return -1;
    }
    int moved_old[16] = {0}, moved_new[16] = {0}, i, ok = 1;
    for (i = 0; i < es.n; i++) {
        snprintf(from, sizeof from, "%s/%s", newd, es.name[i]);
        snprintf(to, sizeof to, "%s%s", base, es.name[i]);
        snprintf(keep, sizeof keep, "%s/%s", oldd, es.name[i]);
        if (exists(to) && !(moved_old[i] = move_path(to, keep))) {
            ok = 0;
            break;
        }
        if (!(moved_new[i] = move_path(from, to))) {
            ok = 0;
            break;
        }
    }
    if (!ok) {
        char what[160], why[200];
        SDL_strlcpy(what, es.name[i], sizeof what);
        SDL_strlcpy(why, SDL_GetError(), sizeof why);
        say("update: could not put %s in place (%s); moving back what moved", what, why);
        int stuck = 0;
        for (; i >= 0; i--) {
            snprintf(from, sizeof from, "%s/%s", newd, es.name[i]);
            snprintf(to, sizeof to, "%s%s", base, es.name[i]);
            snprintf(keep, sizeof keep, "%s/%s", oldd, es.name[i]);
            if (moved_new[i] && !move_path(to, from)) {
                say("update: could not move %s back", to);
                stuck = 1;
            }
            if (moved_old[i] && !move_path(keep, to)) {
                say("update: could not move %s back to %s", keep, to);
                stuck = 1;
            }
        }
        if (stuck)
            snprintf(upd_error, sizeof upd_error, "could not put %.100s in place, and not all of the old files "
                     "went back: they are in update/%s (launcher.log)", what, tail);
        else
            snprintf(upd_error, sizeof upd_error, "could not put %.100s in place (%.120s); nothing was changed",
                     what, why);
        say("update: %s", upd_error);
        return -1;
    }
    char p[1100], j[256];
    upd_path(p, sizeof p, "applied.json");
    snprintf(j, sizeof j, "{\n  \"from\": \"%s\",\n  \"to\": \"%s\"\n}\n", rel_version, upd_version);
    SDL_SaveFile(p, j, strlen(j));
    upd_path(p, sizeof p, "ready.json");
    SDL_RemovePath(p);
    SDL_RemovePath(newd);           /* empty now */
    say("update: %s is in place (the old files are in %s until the next check)", upd_version, oldd);
    return 0;
}

/* the new launcher, in the old one's place; this one then quits */
static int restart_launcher(const char *opt_game, int opt_exit)
{
    char exe[1100];
    snprintf(exe, sizeof exe, "%s%s", base, LAUNCHER_EXE);
    const char *args[8];
    int n = 0;
    args[n++] = exe;
    if (opt_game) {
        args[n++] = "--game";
        args[n++] = opt_game;
    }
    if (opt_exit)
        args[n++] = "--exit";
    if (after_game)
        args[n++] = "--after-game";     /* the new one closes too once the game is built */
    args[n] = NULL;
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (!p) {
        say("update: the new launcher did not start (%s): start it again yourself", SDL_GetError());
        return -1;
    }
    SDL_DestroyProcess(p);          /* it runs on */
    say("update: started the new launcher");
    return 0;
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

static void SDLCALL on_game_folder(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0]) {
        SDL_strlcpy(picked_game_path, files[0], sizeof picked_game_path);
        SDL_SetAtomicInt(&picked_game, 1);
    }
}

/* the found set is offered while no set is chosen */
static int found_offered(void)
{
    return found_path[0] && !set_path[0];
}

/* ---- the buttons ---- */

enum {
    B_CHOOSE, B_CONTINUE, B_BACK, B_TARGET, B_BUILD, B_CANCEL, B_RETRY, B_PLAY, B_SETTINGS, B_STEAM,
    B_REBUILD, B_FILES, B_UPDATE, B_DONE, B_UPD_NOW, B_UPD_CLOSE, B_UPD_CANCEL, B_CHECK, B_USE_FOUND, B_GAME_DIR,
    B_DIRECT, B_COUNT
};
static int focus = -1;
#define HITS 24
typedef struct hit { int id; float x, y, w, h; } hit;
static hit hits[HITS];
static int nhits;

/* the Play page shows a new release's card (instead of the builder's), or
 * the check's line and button at its foot */
static int upd_card(void)
{
    return upd == U_AVAILABLE || upd == U_FETCHING || upd == U_READY || upd == U_FAILED;
}

static int check_shown(void)
{
    return upd != U_OFF && !upd_card();
}

/* Start the game directly applies when Play starts the game itself (not
 * the Windows build on Linux, which plays through Steam) */
static int direct_ok(void)
{
#ifdef _WIN32
    return 1;
#else
    return built_linux;
#endif
}

static int update_ready(void);

/* the Play page shows the switch; not beside an update's or a new
 * builder's card, which the launcher is needed for anyway */
static int direct_shown(void)
{
    return direct_ok() && !upd_card() && !update_ready();
}

static int enabled(int b)
{
    switch (b) {
    case B_CONTINUE: return set_ok();
    case B_BUILD: return set_ok() && root[0] && !proc;
    case B_UPDATE: return built && root[0] && !proc;
    case B_PLAY: return built && !game_proc && !proc;
    case B_STEAM: return built && root[0] && !proc && !built_linux && !no_add_to_steam;
    case B_CANCEL: return (proc || shot_working) && !cancel_at;
    case B_REBUILD: return root[0] && !proc;
    /* a download starts only once the check's update.py has ended (it says
     * "@update" a moment before it does) */
    case B_UPD_NOW:
        return ((upd == U_AVAILABLE || upd == U_FAILED) && !uproc) ||
               ((upd == U_READY || upd == U_FETCHING) && upd_when == UW_CLOSE);
    case B_UPD_CLOSE: return upd == U_AVAILABLE && !uproc;
    case B_UPD_CANCEL: return upd == U_FETCHING && uproc_in != NULL;
    case B_CHECK: return upd == U_IDLE || upd == U_NONE || upd == U_CHECK_FAILED;
    case B_USE_FOUND: return found_offered() && !proc;
    case B_DIRECT: return built && !proc;
#ifdef _WIN32
    case B_TARGET: return 0;
#endif
    default: return !proc;
    }
}

/* the page's buttons, in the order the arrows go through them */
static int page_buttons(int *ids)
{
    int n = 0;
    switch (page) {
    case P_FILES: ids[n++] = B_CHOOSE; ids[n++] = found_offered() ? B_USE_FOUND : B_CONTINUE; break;
    case P_BUILD:
        ids[n++] = B_GAME_DIR;
#ifndef _WIN32
        ids[n++] = B_TARGET;
#endif
        ids[n++] = B_BACK;
        ids[n++] = B_BUILD;
        break;
    case P_WORK: ids[n++] = B_CANCEL; break;
    case P_FAIL: ids[n++] = B_BACK; ids[n++] = B_RETRY; break;
    case P_PLAY:
        if (upd_card()) {           /* a new release: it takes the builder card's place */
            if (enabled(B_UPD_NOW))
                ids[n++] = B_UPD_NOW;
            if (upd == U_AVAILABLE)
                ids[n++] = B_UPD_CLOSE;
            if (upd == U_FETCHING)
                ids[n++] = B_UPD_CANCEL;
        } else if (update_ready()) {
            ids[n++] = B_UPDATE;
        }
        ids[n++] = B_PLAY;
        ids[n++] = B_SETTINGS;
        if (!built_linux && !no_add_to_steam)
            ids[n++] = B_STEAM;
        ids[n++] = B_REBUILD;
        ids[n++] = B_FILES;
        if (direct_shown())
            ids[n++] = B_DIRECT;
        if (check_shown())
            ids[n++] = B_CHECK;
        break;
    case P_SETTINGS: ids[n++] = B_DONE; break;
    }
    return n;
}

static int default_focus(void)
{
    switch (page) {
    case P_FILES: return set_ok() ? B_CONTINUE : found_offered() ? B_USE_FOUND : B_CHOOSE;
    case P_BUILD: return B_BUILD;
    case P_WORK: return B_CANCEL;
    case P_FAIL: return B_RETRY;
    case P_PLAY: return built ? B_PLAY : B_REBUILD;
    }
    return -1;
}

/* ---- the settings page ---- */

enum { T_GAME, T_PICTURE, T_SOUND, T_PAD, T_KEY1, T_KEY2, T_COUNT };
static const char *const tab_names[T_COUNT] = { "Game", "Picture", "Sound", "Gamepad", "Keyboard 1", "Keyboard 2" };
static const int tab_rows[3][6] = {
    { OPT_CREDITS, OPT_CARDS, -1 },
    { OPT_FULLSCREEN, OPT_PICTURE, OPT_FILTER, OPT_RENDERER, OPT_SCALE, -1 },
    { OPT_SOUND, OPT_VOLUME, -1 },
};
static settings st;
static char settings_path[1100];
static int st_tab, st_row, st_top;
static int capturing;               /* a controls row waits for its key or button */
static int settings_from = P_PLAY;

static void settings_open(void)
{
    snprintf(settings_path, sizeof settings_path, "%s/user/settings.txt", game);
    settings_defaults(&st);
    settings_load(&st, settings_path, NULL);
    st_tab = st_row = st_top = 0;
    capturing = 0;
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

static int controls_tab(void)
{
    return st_tab >= T_PAD;
}

static int tab_count(void)
{
    if (controls_tab())
        return ACT_COUNT;
    int n = 0;
    while (n < 6 && tab_rows[st_tab][n] >= 0)
        n++;
    return n;
}

static binding *tab_binding(int act)
{
    return st_tab == T_PAD ? &st.pad[act] : &st.key[st_tab == T_KEY2][act];
}

static void settings_close(void)
{
    settings_store();
    capturing = 0;
    go(settings_from);
}

static void settings_tab(int d)
{
    st_tab = ((st_tab + d) % T_COUNT + T_COUNT) % T_COUNT;
    st_row = st_top = 0;
}

/* A, Enter or a click on a row */
static void settings_press(int row)
{
    if (controls_tab())
        capturing = 1;
    else
        option_change(&st, tab_rows[st_tab][row], 1);
}

/* what a controls row takes: replaces the row's binding */
static void capture(const char *name)
{
    binding *b = tab_binding(st_row);
    memset(b, 0, sizeof *b);
    settings_binding_add(b, name);
    capturing = 0;
}

static void settings_event(const SDL_Event *e)
{
    if (capturing) {
        if (st_tab == T_PAD) {
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
        } else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                   ui_pad_face(e->gbutton.which, e->gbutton.button) == UI_IN_BACK) {
            capturing = 0;          /* B on a pad: a keyboard row cannot take a pad button */
        }
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return;
    int k = e->type == SDL_EVENT_KEY_DOWN ? (int)e->key.key : -1;
    int g = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.button : -1;
    int face = g >= 0 ? ui_pad_face(e->gbutton.which, g) : UI_IN_NONE;     /* by label */
    int n = tab_count();
    if (k == SDLK_DOWN || g == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
        st_row = (st_row + 1) % n;
    else if (k == SDLK_UP || g == SDL_GAMEPAD_BUTTON_DPAD_UP)
        st_row = (st_row + n - 1) % n;
    else if (k == SDLK_Q || k == SDLK_PAGEUP || g == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
        settings_tab(-1);
    else if (k == SDLK_E || k == SDLK_PAGEDOWN || k == SDLK_TAB || g == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
        settings_tab(1);
    else if (!controls_tab() && (k == SDLK_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT))
        option_change(&st, tab_rows[st_tab][st_row], -1);
    else if (!controls_tab() && (k == SDLK_RIGHT || g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        option_change(&st, tab_rows[st_tab][st_row], 1);
    else if (k == SDLK_RETURN || k == SDLK_SPACE || face == UI_IN_OK)
        settings_press(st_row);
    else if (controls_tab() && (k == SDLK_DELETE || face == UI_IN_CLEAR))
        memset(tab_binding(st_row), 0, sizeof(binding));
    else if (controls_tab() && (k == SDLK_BACKSPACE || face == UI_IN_DEFAULT)) {
        /* the defaults back on this row */
        settings d;
        settings_defaults(&d);
        *tab_binding(st_row) = st_tab == T_PAD ? d.pad[st_row] : d.key[st_tab == T_KEY2][st_row];
    } else if (k == SDLK_ESCAPE || face == UI_IN_BACK)
        settings_close();
}

/* ---- what the buttons do ---- */

static void do_build(int update)
{
    char script[1100];
    snprintf(script, sizeof script, "%stools/make_game.py", root);
    const char *target = target_linux ? "linux" : "windows";
    const char *build_args[] = {python, "-I", script, "--set", set_path, "--out", game, "--target", target,
                                "--launcher", NULL};
    const char *update_args[] = {python, "-I", script, "--update", game, "--launcher", NULL};
    if (!update)
        copy_saves();
    nlines = 0;
    error_line[0] = 0;
    stage_n = stage_of = 0;
    stage_pct = -1;
    SDL_strlcpy(stage_text, "Starting", sizeof stage_text);
    cancelled = 0;
    status_kind = ST_NONE;
    if (start(update ? K_UPDATE : K_BUILD, update ? update_args : build_args, 1) == 0)
        go(P_WORK);
    else
        go(P_FAIL);
}

static void press(int b)
{
    if (!enabled(b))
        return;
    char script[1100], prog[1100];
    program_path(prog, sizeof prog);
    switch (b) {
    case B_CHOOSE:
        SDL_ShowOpenFolderDialog(on_folder, NULL, win, set_path[0] ? set_path : NULL, false);
        break;
    case B_CONTINUE: go(P_BUILD); break;
    case B_USE_FOUND:
        SDL_strlcpy(set_path, found_path, sizeof set_path);
        SDL_SaveFile(set_file, set_path, strlen(set_path));
        say("set: %s (the one found, chosen)", set_path);
        go(P_BUILD);
        break;
    case B_GAME_DIR: {
        /* the dialog opens where the install folder is (or would be) */
        static char at[1024];
        SDL_strlcpy(at, game, sizeof at);
        while (at[0] && !exists(at)) {
            char *sl = strrchr(at, '/'), *bs = strrchr(at, '\\');
            if (bs > sl)
                sl = bs;
            if (!sl)
                break;
            *sl = 0;
        }
        SDL_ShowOpenFolderDialog(on_game_folder, NULL, win, at[0] ? at : NULL, false);
        break;
    }
    case B_BACK: go(page == P_FAIL && built ? P_PLAY : page == P_FAIL ? P_BUILD : P_FILES); break;
    case B_TARGET: target_linux = !target_linux; break;
    case B_BUILD: do_build(0); break;
    case B_RETRY: do_build(last_kind == K_UPDATE); break;
    case B_UPDATE: do_build(1); break;
    case B_CANCEL: cancel(); break;
    case B_REBUILD: go(P_BUILD); break;
    case B_FILES: go(P_FILES); break;
    case B_SETTINGS:
        settings_open();
        settings_from = page;
        go(P_SETTINGS);
        break;
    case B_DONE: settings_close(); break;
    case B_UPD_NOW:                 /* it goes in as soon as it is here and nothing runs (main) */
        upd_when = UW_NOW;
        if (upd == U_AVAILABLE || upd == U_FAILED)
            upd_start(2);
        break;
    case B_UPD_CLOSE:
        upd_when = UW_CLOSE;
        upd_start(2);
        break;
    case B_UPD_CANCEL: upd_cancel(); break;
    case B_CHECK:
        upd_manual = 1;
        if (status_kind == ST_BAD && !strncmp(status_text, "Could not check", 15))
            status_kind = ST_NONE;
        upd_start(1);
        break;
    case B_PLAY: {
#ifndef _WIN32
        if (!built_linux) {
            /* the Windows exe on Linux plays through Steam (Proton, Steam Input) */
            snprintf(script, sizeof script, "%stools/steam_shortcut.py", root);
            const char *args[] = {python, "-I", script, "play", prog, NULL};
            status_kind = ST_WORKING;
            SDL_strlcpy(status_text, "Starting it through Steam\xE2\x80\xA6", sizeof status_text);
            if (start(K_STEAM_PLAY, args, 1)) {
                status_kind = ST_BAD;
                SDL_strlcpy(status_text, error_line, sizeof status_text);
            }
            break;
        }
#endif
        const char *args[] = {prog, NULL};
        if (start(K_NONE, args, 0) == 0)
            say("started %s", prog);
        else {
            status_kind = ST_BAD;
            SDL_strlcpy(status_text, error_line, sizeof status_text);
        }
        break;
    }
    case B_DIRECT: {
        char p[1100];
        direct_path(p, sizeof p);
        play_direct = !play_direct;
        if (play_direct)
            SDL_SaveFile(p, "1\n", 2);
        else
            SDL_RemovePath(p);
        say("start the game directly: %s", play_direct ? "on (from the next start; the game's menu has Open the launcher)"
                                                       : "off");
        break;
    }
    case B_STEAM: {
        snprintf(script, sizeof script, "%stools/steam_shortcut.py", root);
        const char *args[] = {python, "-I", script, "add", prog, NULL};
        status_kind = ST_WORKING;
        SDL_strlcpy(status_text, "Adding it to Steam (Steam must be closed: Steam menu, Exit)\xE2\x80\xA6",
                    sizeof status_text);
        if (start(K_STEAM, args, 1)) {
            status_kind = ST_BAD;
            SDL_strlcpy(status_text, error_line, sizeof status_text);
        }
        break;
    }
    }
}

static void go(int p)
{
    page = p;
    focus = default_focus();
}

/* ---- drawing ---- */

static void hit_add(int id, float x, float y, float w, float h)
{
    if (nhits < HITS)
        hits[nhits++] = (hit){ id, x, y, w, h };
}

static int hit_at(float x, float y)
{
    for (int i = nhits - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return hits[i].id;
    return -1;
}

/* a button; style 1 the page's main one; returns its width */
static float button(int id, float x, float y, float w, float h, const char *label, int style, int icon)
{
    int on = enabled(id), f = focus == id && on;
    float tw = ui_text_w(UI_MEDIUM, 16, label) + (icon >= 0 ? 26 : 0);
    if (w <= 0)
        w = SDL_max(tw + 44, 120);
    if (f)
        ui_round_line(x - 4, y - 4, w + 8, h + 8, 14, 2, UI_TEXT, UI_BG);
    SDL_Color fill = style == 1 ? (on ? UI_ACCENT : UI_RAISED) : (f ? UI_HOVER : UI_RAISED);
    SDL_Color tc = style == 1 && on ? UI_ON_ACCENT : on ? UI_TEXT : UI_FAINT;
    ui_round(x, y, w, h, 10, fill);
    float tx = x + (w - tw) / 2;
    if (icon >= 0) {
        ui_icon(icon, tx + 9, y + h / 2, 20, tc);
        tx += 26;
    }
    ui_text(UI_MEDIUM, 16, tx, y + h / 2, tc, label);
    hit_add(id, x, y, w, h);
    return w;
}

static float button_w(const char *label)
{
    return SDL_max(ui_text_w(UI_MEDIUM, 16, label) + 44, 120);
}

static void top_bar(void)
{
    float x = 40 + ui_text(UI_BOLD, 17, 40, 36, UI_TEXT, "VF4 Final Tuned");
    char v[96];
    snprintf(v, sizeof v, "builder %s", builder_ver[0] ? builder_ver : "?");
    ui_text(UI_REGULAR, 14, x + 12, 36, UI_FAINT, v);
    ui_fill(0, 72, LW, 1, UI_LINE);
    if (page == P_SETTINGS) {
        float w = ui_text_w(UI_MEDIUM, 15, "Settings");
        ui_text(UI_MEDIUM, 15, LW - 40 - w, 36, UI_MUTED, "Settings");
        return;
    }
    /* the stepper, from the right */
    static const char *const steps[3] = { "Game files", "Build", "Play" };
    int cur = page == P_FILES ? 0 : page == P_PLAY ? 2 : 1;
    float widths[3], total = 0;
    for (int i = 0; i < 3; i++) {
        widths[i] = 30 + ui_text_w(UI_MEDIUM, 15, steps[i]);
        total += widths[i] + (i < 2 ? 40 : 0);
    }
    float at = LW - 40 - total;
    for (int i = 0; i < 3; i++) {
        int done = i < cur;
        SDL_Color badge = done ? UI_GOOD : i == cur ? UI_ACCENT : UI_RAISED;
        ui_round(at, 36 - 11, 22, 22, 11, badge);
        if (done) {
            ui_icon(UI_I_CHECK, at + 11, 36, 18, UI_ON_ACCENT);
        } else {
            char n[4];
            snprintf(n, sizeof n, "%d", i + 1);
            float nw = ui_text_w(UI_BOLD, 12.5f, n);
            ui_text(UI_BOLD, 12.5f, at + 11 - nw / 2, 36, i == cur ? UI_ON_ACCENT : UI_MUTED, n);
        }
        ui_text(UI_MEDIUM, 15, at + 30, 36, i == cur ? UI_TEXT : done ? UI_MUTED : UI_FAINT, steps[i]);
        at += widths[i];
        if (i < 2) {
            ui_icon(UI_I_CHEVRON, at + 20, 36, 16, UI_FAINT);
            at += 40;
        }
    }
}

static void title(const char *t, const char *sub)
{
    ui_text(UI_BOLD, 32, CX, 128, UI_TEXT, t);
    if (sub && sub[0])
        ui_wrap(UI_REGULAR, 16.5f, CX, 172, CW, 25, UI_MUTED, sub);
}

static void card(float y, float h)
{
    ui_round(CX, y, CW, h, 14, UI_CARD);
}

/* the last lines of the output, in a box */
static void log_box(float y, float h)
{
    ui_round_line(CX, y, CW, h, 12, 1, UI_LINE, UI_BG);
    float lh = 19;
    int fit = (int)((h - 20) / lh);
    int first = nlines > fit ? nlines - fit : 0;
    for (int i = first; i < nlines; i++)
        ui_text_in(UI_MONO, 12.5f, CX + 18, y + 10 + lh / 2 + (float)(i - first) * lh, CW - 36, 0, 0,
                   UI_MUTED, lines[i]);
}

/* the buttons from the right edge, right to left: ids, labels, styles */
static void buttons_right(const int *ids, const char *const *labels, const int *styles, int n)
{
    float x = CX + CW;
    for (int i = n - 1; i >= 0; i--) {
        x -= button_w(labels[i]);
        button(ids[i], x, BTN_Y, 0, BTN_H, labels[i], styles[i], -1);
        x -= 14;
    }
}

static void hints_bottom(void)
{
    static const ui_hint hints[] = {
        { "Choose", { UI_IN_LEFTRIGHT, UI_IN_NONE } }, { "Select", { UI_IN_OK, UI_IN_NONE } },
    };
    if (page == P_WORK)
        return;
    ui_hints(CX, BTN_Y + BTN_H / 2, 24, hints, 2, 0);
}

static void path_line(float x, float y, float w, const char *label, const char *value)
{
    ui_text(UI_REGULAR, 14, x, y, UI_FAINT, label);
    ui_text_in(UI_MONO, 14, x + 150, y, w - 150, 0, 1, UI_TEXT, value);
}

static void draw_files(void)
{
    title("Find your game files",
          "The builder makes the game from your own MAME set of Virtua Fighter 4 Final Tuned: "
          "vf4tuned.zip and its GD-ROM, vf4tuned/gds-0036f.chd.");
    int ok = set_ok(), offer = found_offered();
    card(240, offer ? 140 : 120);
    ui_round(CX + 28, 276, 48, 48, 24, ok ? UI_GOOD : set_path[0] ? UI_BAD : offer ? UI_ACCENT : UI_RAISED);
    ui_icon(ok ? UI_I_CHECK : set_path[0] ? UI_I_FAIL : UI_I_DOT, CX + 52, 300, 36,
            ok || set_path[0] || offer ? UI_ON_ACCENT : UI_FAINT);
    ui_text(UI_MEDIUM, 19, CX + 100, 284, UI_TEXT,
            ok              ? "Your set"
            : set_path[0]   ? "This folder has no vf4tuned set"
            : offer         ? "A vf4tuned set is in this folder"
                            : "No set chosen yet");
    char shown[1024];
    ui_text_in(UI_MONO, 14, CX + 100, 316, CW - 128, 0, 1, UI_MUTED,
               set_path[0] ? home_short(set_path, shown, sizeof shown)
               : offer     ? home_short(found_path, shown, sizeof shown)
                           : "Choose the folder that holds vf4tuned.zip.");
    if (offer)
        ui_text(UI_REGULAR, 14, CX + 100, 346, UI_FAINT, "Build from this one, or choose another folder.");
    ui_wrap(UI_REGULAR, 14, CX, 420, CW, 22, UI_FAINT,
            "Nothing from your set leaves this computer, and the game the builder makes is yours alone: "
            "never share it.");
    static const int ids[] = { B_CHOOSE, B_CONTINUE };
    static const int ids_offer[] = { B_CHOOSE, B_USE_FOUND };
    static const char *const labels[] = { "Choose folder\xE2\x80\xA6", "Continue" };
    static const char *const labels_offer[] = { "Choose another folder\xE2\x80\xA6", "Use this set" };
    static const int styles[] = { 0, 1 };
    buttons_right(offer ? ids_offer : ids, offer ? labels_offer : labels, styles, 2);
    hints_bottom();
}

static void draw_build(void)
{
    const char *sub;
#ifdef _WIN32
    sub = "It takes a few minutes, and your PC stays usable. The first build also downloads the compiler, "
          "about 200 MB.";
#else
    sub = target_linux ? "It takes a few minutes, and your PC stays usable. A native build uses this "
                         "computer's own C compiler and SDL3."
                       : "It takes a few minutes, and your PC stays usable. The first Windows build also "
                         "downloads the compiler, about 90 MB.";
#endif
    title(built ? "Build the game again" : "Build the game", sub);
    float y = 246;
#ifndef _WIN32
    card(y, 176);
#else
    card(y, 120);
#endif
    path_line(CX + 28, y + 34, CW - 56, "Your set", set_path[0] ? set_path : "-");
    const char *change = "Change\xE2\x80\xA6";
    float cw = ui_text_w(UI_MEDIUM, 16, change) + 40;
    path_line(CX + 28, y + 70, CW - 56 - cw - 16, "Install folder", game);
    button(B_GAME_DIR, CX + CW - 28 - cw, y + 70 - 19, cw, 38, change, 0, -1);
    /* what Change did, and whose saves go along */
    float ny = y + 120 + 34;
#ifndef _WIN32
    ny += 56;
#endif
    char u[1100], line[1200];
    snprintf(u, sizeof u, "%s/user", game);
    if (game_note[0]) {
        ui_wrap(UI_REGULAR, 14, CX, ny, CW, 21, game_note_bad ? UI_BAD : UI_MUTED, game_note);
        ny += 21 * (float)ui_wrap_lines(UI_REGULAR, 14, CW, game_note) + 8;
    }
    if (saves_from[0] && !has_files(u)) {
        ui_text(UI_REGULAR, 14, CX, ny, UI_MUTED, "When you build, your saves and settings are copied from:");
#ifdef _WIN32
        snprintf(line, sizeof line, "%s\\user", saves_from);
#else
        snprintf(line, sizeof line, "%s/user", saves_from);
#endif
        char shown[1300];
        ui_text_in(UI_MONO, 13.5f, CX, ny + 23, CW, 0, 1, UI_MUTED, home_short(line, shown, sizeof shown));
    }
#ifndef _WIN32
    ui_text(UI_REGULAR, 14, CX + 28, y + 124, UI_FAINT, "Program");
    /* the target: a two-way switch */
    float sx = CX + 178, sw = 420, sh = 40, sy = y + 104;
    int f = focus == B_TARGET;
    if (f)
        ui_round_line(sx - 4, sy - 4, sw + 8, sh + 8, 14, 2, UI_TEXT, UI_CARD);
    ui_round_line(sx, sy, sw, sh, 10, 1, UI_LINE, UI_BG);
    ui_round(sx + 3 + (target_linux ? sw / 2 : 0), sy + 3, sw / 2 - 6, sh - 6, 8, f ? UI_ACCENT : UI_HOVER);
    SDL_Color on = f ? UI_ON_ACCENT : UI_TEXT;
    ui_text_in(UI_MEDIUM, 15, sx, sy + sh / 2, sw / 2, 1, 0, target_linux ? UI_MUTED : on, "Windows (for Steam)");
    ui_text_in(UI_MEDIUM, 15, sx + sw / 2, sy + sh / 2, sw / 2, 1, 0, target_linux ? on : UI_MUTED, "Native Linux");
    hit_add(B_TARGET, sx, sy, sw, sh);
#else
    (void)0;
#endif
    static const int ids[] = { B_BACK, B_BUILD };
    static const char *const labels[] = { "Back", "Build" };
    static const int styles[] = { 0, 1 };
    buttons_right(ids, labels, styles, 2);
    hints_bottom();
}

static void draw_work(void)
{
    title(proc_kind == K_UPDATE ? "Updating your game" : "Building your game",
          "This takes a while, and your PC stays usable.");
    card(228, 122);
    ui_text_in(UI_MEDIUM, 19, CX + 28, 266, CW - 200, 0, 0, UI_TEXT, cancel_at ? "Cancelling\xE2\x80\xA6" : stage_text);
    if (stage_of) {
        char s[40];
        snprintf(s, sizeof s, "Step %d of %d", stage_n, stage_of);
        ui_text_in(UI_REGULAR, 14, CX + CW - 228, 266, 200, 2, 0, UI_MUTED, s);
    }
    double t = shot_elapsed >= 0 ? shot_elapsed : (double)(SDL_GetTicks() - proc_t0) / 1000.0;
    ui_bar(CX + 28, 290, CW - 56, 8, stage_pct >= 0 ? (float)stage_pct / 100 : -1, t);
    char el[48];
    int secs = (int)t;
    snprintf(el, sizeof el, "%d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60);
    if (stage_pct >= 0) {
        char pc[16];
        snprintf(pc, sizeof pc, "%d%%", stage_pct);
        ui_text(UI_MEDIUM, 14, CX + 28, 324, UI_TEXT, pc);
    }
    float ew = ui_text_w(UI_MEDIUM, 14, el);
    ui_text(UI_MEDIUM, 14, CX + CW - 28 - ew, 324, UI_TEXT, el);
    float lw = ui_text_w(UI_REGULAR, 14, "Elapsed");
    ui_text(UI_REGULAR, 14, CX + CW - 28 - ew - 10 - lw, 324, UI_MUTED, "Elapsed");
    log_box(368, 130);
    static const int ids[] = { B_CANCEL };
    static const char *const labels[] = { "Cancel" };
    static const int styles[] = { 0 };
    buttons_right(ids, labels, styles, 1);
}

static void draw_fail(void)
{
    char sub[400];
    if (cancelled)
        SDL_strlcpy(sub, "Nothing was installed: the game is as it was before.", sizeof sub);
    else
        snprintf(sub, sizeof sub, "%s", error_line[0] ? error_line : "See the builder's messages below.");
    title(cancelled ? "Build cancelled" : "The build stopped", sub);
    log_box(246, 220);
    char where[1200];
    snprintf(where, sizeof where, "The whole log: %s/build.log", game);
    ui_text_in(UI_REGULAR, 13.5f, CX, 486, CW, 0, 1, UI_FAINT, where);
    static const int ids[] = { B_BACK, B_RETRY };
    static const char *const labels[] = { "Back", "Try again" };
    static const int styles[] = { 0, 1 };
    buttons_right(ids, labels, styles, 2);
    hints_bottom();
}

/* a new release's card on the Play page; returns the height it takes */
static float draw_update_card(float y)
{
    char t[200], s[400];
    const char *b1 = NULL, *b2 = NULL;
    int id1 = -1, id2 = -1, style1 = 1, bar = 0;
    switch (upd) {
    case U_AVAILABLE:
        snprintf(t, sizeof t, "Version %s is out", upd_version);
        snprintf(s, sizeof s,
                 "You have %s. The download is %lld MB. Update now restarts the launcher; either way the game is "
                 "built again before you play, and your saves and settings stay.",
                 rel_version, upd_size > 0 ? SDL_max(upd_size / 1000000, 1) : 0);
        id1 = B_UPD_NOW, b1 = "Update now";
        id2 = B_UPD_CLOSE, b2 = "When I close";
        break;
    case U_FETCHING:
        snprintf(t, sizeof t, "%s %s\xE2\x80\xA6", upd_stage == 2 ? "Unpacking" : "Downloading", upd_version);
        SDL_strlcpy(s, upd_when == UW_NOW ? "Then the launcher restarts and builds the game again."
                                          : "It goes in when you close the launcher.", sizeof s);
        bar = 1;
        if (upd_when == UW_CLOSE)
            id1 = B_UPD_NOW, b1 = "Update now", style1 = 0;
        id2 = B_UPD_CANCEL, b2 = "Cancel";
        break;
    case U_READY:
        if (upd_when == UW_CLOSE) {
            snprintf(t, sizeof t, "Version %s goes in when you close the launcher", upd_version);
            SDL_strlcpy(s, "Or now: the launcher restarts, then builds the game again.", sizeof s);
            id1 = B_UPD_NOW, b1 = "Update now";
        } else {
            snprintf(t, sizeof t, "Version %s is ready", upd_version);
            SDL_strlcpy(s, proc ? "It goes in when the build ends." : "Putting it in\xE2\x80\xA6", sizeof s);
        }
        break;
    case U_FAILED:
        snprintf(t, sizeof t, "Version %s did not go in", upd_version);
        snprintf(s, sizeof s, "%s", upd_error[0] ? upd_error : "See launcher.log.");
        id1 = B_UPD_NOW, b1 = "Try again";
        break;
    default:
        return 0;
    }
    /* the buttons from the right; the text in what is left, the card as tall as its lines */
    float w2 = b2 ? button_w(b2) : 0, w1 = b1 ? button_w(b1) : 0;
    float x2 = CX + CW - 24 - w2, x1 = (b2 ? x2 - 12 : CX + CW - 24) - w1;
    float tw = (b1 ? x1 : b2 ? x2 : CX + CW - 24) - 24 - (CX + 24);
    int lines = bar ? 1 : ui_wrap_lines(UI_REGULAR, 14, tw, s);
    float h = SDL_max(100.0f, 56 + 19.0f * (float)lines + 25);
    ui_round_line(CX, y, CW, h, 14, 1, upd == U_FAILED ? UI_BAD : UI_ACCENT, UI_CARD);
    float by = y + SDL_roundf((h - 44) / 2);
    if (b2)
        button(id2, x2, by, w2, 44, b2, 0, -1);
    if (b1)
        button(id1, x1, by, w1, 44, b1, style1, -1);
    ui_text_in(UI_MEDIUM, 17, CX + 24, y + 28, tw, 0, 0, UI_TEXT, t);
    if (bar) {
        ui_bar(CX + 24, y + 52, tw, 6, upd_pct >= 0 ? (float)upd_pct / 100 : -1,
               (double)(SDL_GetTicks() - upd_t0) / 1000.0);
        ui_text_in(UI_REGULAR, 14, CX + 24, y + 76, tw, 0, 0, UI_MUTED, s);
    } else {
        ui_wrap(UI_REGULAR, 14, CX + 24, y + 56, tw, 19, UI_MUTED, s);
    }
    return h + 20;
}

/* the check's line and button at the Play page's foot */
static void draw_check_row(void)
{
    if (!check_shown())
        return;
    const char *t = upd == U_CHECKING ? "Checking for updates\xE2\x80\xA6"
                    : upd == U_NONE    ? "Up to date"
                    : upd == U_CHECK_FAILED ? "Could not check for updates"
                                            : "";
    const char *label = "Check for updates";
    float w = button_w(label), x = CX + CW - w;
    button(B_CHECK, x, BTN_Y + 6, w, 40, label, 0, -1);
    ui_text_in(UI_REGULAR, 14, CX + 280, BTN_Y + 26, x - 16 - (CX + 280), 2, 0, UI_FAINT, t);
}

/* the Play page's switch, in the look of the menus' Off/On rows */
static void direct_row(float y)
{
    int on = enabled(B_DIRECT), f = focus == B_DIRECT && on;
    float tw = 96, th = 30, h = 50;
    if (f)
        ui_round_line(CX - 8, y - 8, CW + 16, h + 16, 14, 2, UI_TEXT, UI_BG);
    float ty = y + 14 - th / 2;
    ui_round_line(CX, ty, tw, th, th / 2, 1, UI_LINE, UI_BG);
    ui_round(CX + (play_direct ? tw / 2 : 0) + 2, ty + 2, tw / 2 - 4, th - 4, (th - 4) / 2,
             !on ? UI_LINE : play_direct ? UI_ACCENT : UI_HOVER);
    SDL_Color sel = !on ? UI_MUTED : play_direct ? UI_ON_ACCENT : UI_TEXT;
    ui_text_in(UI_MEDIUM, 14, CX, y + 14, tw / 2, 1, 0, play_direct ? UI_FAINT : sel, "Off");
    ui_text_in(UI_MEDIUM, 14, CX + tw / 2, y + 14, tw / 2, 1, 0, play_direct ? sel : UI_FAINT, "On");
    ui_text(UI_MEDIUM, 16, CX + tw + 18, y + 14, on ? UI_TEXT : UI_FAINT, "Start the game directly");
    ui_text_in(UI_REGULAR, 14, CX + tw + 18, y + 40, CW - tw - 18, 0, 0, UI_MUTED,
               "Next time the game opens without this window. Its menu has Open the launcher.");
    hit_add(B_DIRECT, CX - 8, y - 8, CW + 16, h + 16);
}

static void draw_play(void)
{
    char sub[300];
    if (built)
        snprintf(sub, sizeof sub, "Virtua Fighter 4 Final Tuned, built by builder %s as %s.", built_ver,
                 built_linux ? "a native Linux program"
#ifdef _WIN32
                             : "a Windows program");
#else
                             : "a Windows program (it plays through Steam and Proton)");
#endif
    else
        SDL_strlcpy(sub, "The game is not built yet.", sizeof sub);
    title(built ? "Ready to play" : "Not built yet", sub);
    float y = 236;
    if (upd_card()) {
        y += draw_update_card(y);
    } else if (update_ready()) {
        ui_round_line(CX, y, CW, 84, 14, 1, UI_ACCENT, UI_CARD);
        char t[160];
        snprintf(t, sizeof t, "A new builder is here: %s", builder_ver);
        ui_text(UI_MEDIUM, 17, CX + 24, y + 28, UI_TEXT, t);
        ui_wrap(UI_REGULAR, 14, CX + 24, y + 56, CW - 220, 20, UI_MUTED,
                "Update builds the game again with it. Your saves and settings stay.");
        button(B_UPDATE, CX + CW - 24 - 140, y + 20, 140, 44, "Update", 1, -1);
        y += 108;
    }
    button(B_PLAY, CX, y + 6, 220, 64, "Play", update_ready() || upd_card() ? 0 : 1, UI_I_PLAY);
    float x = CX + 244;
    x += button(B_SETTINGS, x, y + 14, 0, BTN_H, "Settings", 0, -1) + 14;
    if (!built_linux && !no_add_to_steam)
        x += button(B_STEAM, x, y + 14, 0, BTN_H, "Add to Steam", 0, -1) + 14;
    x += button(B_REBUILD, x, y + 14, 0, BTN_H, "Build again", 0, -1) + 14;
    button(B_FILES, x, y + 14, 0, BTN_H, "Game files", 0, -1);
    float sy = y + 110;
    if (direct_shown()) {
        direct_row(y + 96);
        sy = y + 166;
    }
    if (status_kind != ST_NONE) {
        int icon = status_kind == ST_GOOD ? UI_I_CHECK : status_kind == ST_BAD ? UI_I_FAIL : UI_I_DOT;
        SDL_Color c = status_kind == ST_GOOD ? UI_GOOD : status_kind == ST_BAD ? UI_BAD : UI_ACCENT;
        ui_icon(icon, CX + 10, sy, 20, c);
        ui_text_in(UI_REGULAR, 15, CX + 30, sy, CW - 30, 0, 0, UI_TEXT, status_text);
    }
    hints_bottom();
    draw_check_row();
}

static void draw_settings(void)
{
    ui_tabs(40, 92, LW - 80, 42, tab_names, T_COUNT, st_tab);
    ui_fill(40, 148, LW - 80, 1, UI_LINE);
    ui_row rows[ACT_COUNT];
    char vals[ACT_COUNT][160];
    int n = tab_count();
    for (int i = 0; i < n; i++) {
        if (!controls_tab()) {
            option_row(&st, tab_rows[st_tab][i], &rows[i], vals[i], sizeof vals[i]);
            continue;
        }
        SDL_zero(rows[i]);
        rows[i].label = settings_action_text[i];
        rows[i].kind = st_tab == T_PAD ? UI_ROW_PAD : UI_ROW_KEYS;
        settings_binding_text(tab_binding(i), vals[i], sizeof vals[i]);
        rows[i].value = vals[i];
        rows[i].waiting = capturing && i == st_row;
    }
    float ry = 164, rh = 336;
    ui_rows(40, ry, 540, rh, 42, rows, n, st_row, &st_top);
    const char *t, *text, *note = NULL;
    if (!controls_tab()) {
        int o = tab_rows[st_tab][st_row];
        t = options[o].label;
        text = options[o].about;
        if (options[o].at_start || o == OPT_CREDITS)
            note = "The game reads it when it starts.";
    } else {
        t = settings_action_text[st_row];
        text = st_tab == T_PAD
                   ? "Every gamepad's buttons: the first pad is player 1, the second player 2. The left stick "
                     "always moves. The defaults are the PlayStation 2 version's layout."
                   : st_tab == T_KEY1 ? "Player 1's keys." : "Player 2's keys (none by default).";
        note = capturing ? (st_tab == T_PAD ? "Press the button for it (Esc: leave it as it was)."
                                            : "Press the key for it (Esc: leave it as it was).")
                         : NULL;
    }
    ui_about(604, ry, LW - 40 - 604, rh, t, text, note);
    static const ui_hint opt_hints[] = {
        { "Choose", { UI_IN_UPDOWN, UI_IN_NONE } }, { "Change", { UI_IN_LEFTRIGHT, UI_IN_NONE } },
        { "Tabs", { UI_IN_PREV, UI_IN_NEXT } },
    };
    static const ui_hint ctl_hints[] = {
        { "Set", { UI_IN_OK, UI_IN_NONE } }, { "Clear", { UI_IN_CLEAR, UI_IN_NONE } },
        { "Default", { UI_IN_DEFAULT, UI_IN_NONE } }, { "Tabs", { UI_IN_PREV, UI_IN_NEXT } },
    };
    ui_fill(40, 516, LW - 80, 1, UI_LINE);
    if (controls_tab())
        ui_hints(40, 558, 24, ctl_hints, 4, 0);
    else
        ui_hints(40, 558, 24, opt_hints, 3, 0);
    static const ui_hint done_hint[] = { { "Save", { UI_IN_BACK, UI_IN_NONE } } };
    float w = button_w("Done");
    button(B_DONE, LW - 40 - w, 536, w, 44, "Done", 1, -1);
    ui_hints(LW - 40 - w - 24, 558, 24, done_hint, 1, 2);
}

static void draw(void)
{
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(ren, &pw, &ph);
    SDL_SetRenderDrawColor(ren, UI_BG.r, UI_BG.g, UI_BG.b, 255);
    SDL_RenderClear(ren);
    ui_fit(LW, LH, pw, ph);
    nhits = 0;
    top_bar();
    switch (page) {
    case P_FILES: draw_files(); break;
    case P_BUILD: draw_build(); break;
    case P_WORK: draw_work(); break;
    case P_FAIL: draw_fail(); break;
    case P_PLAY: draw_play(); break;
    case P_SETTINGS: draw_settings(); break;
    }
    /* a present that fails leaves the window showing an old page: say so
     * (once, and again when it works again), since nothing else would */
    static int present_failing;
    if (!SDL_RenderPresent(ren)) {
        if (!present_failing++)
            say("window: the picture cannot be shown (%s)", SDL_GetError());
    } else if (present_failing) {
        say("window: the picture shows again (after %d failed frames)", present_failing);
        present_failing = 0;
    }
}

/* ---- input on the main pages ---- */

static int quit_asked;

static void move_focus(int d)
{
    int ids[12], n = page_buttons(ids), at = -1;
    for (int i = 0; i < n; i++)
        if (ids[i] == focus)
            at = i;
    for (int k = 0; k < n; k++) {
        at = ((at < 0 ? (d > 0 ? -1 : 0) : at) + d + n) % n;
        if (enabled(ids[at])) {
            focus = ids[at];
            return;
        }
    }
}

static void page_event(const SDL_Event *e)
{
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return;
    int k = e->type == SDL_EVENT_KEY_DOWN ? (int)e->key.key : -1;
    int g = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.button : -1;
    int face = g >= 0 ? ui_pad_face(e->gbutton.which, g) : UI_IN_NONE;     /* by label */
    if (page == P_BUILD && focus == B_TARGET &&
        (k == SDLK_LEFT || k == SDLK_RIGHT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) {
        press(B_TARGET);
        return;
    }
    if (k == SDLK_RIGHT || k == SDLK_DOWN || k == SDLK_TAB || g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ||
        g == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
        move_focus(1);
    else if (k == SDLK_LEFT || k == SDLK_UP || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_UP)
        move_focus(-1);
    else if (k == SDLK_RETURN || k == SDLK_SPACE || face == UI_IN_OK) {
        if (focus < 0 || !enabled(focus))
            move_focus(1);
        else
            press(focus);
    } else if (k == SDLK_ESCAPE || face == UI_IN_BACK) {
        if (page == P_BUILD || page == P_FAIL)
            press(B_BACK);
        else if (k == SDLK_ESCAPE && page != P_WORK)
            quit_asked = 1;
    }
}

static void mouse_event(SDL_Event *e)
{
    SDL_ConvertEventToRenderCoordinates(ren, e);
    float x, y;
    if (e->type == SDL_EVENT_MOUSE_MOTION)
        ui_to_logical(e->motion.x, e->motion.y, &x, &y);
    else
        ui_to_logical(e->button.x, e->button.y, &x, &y);
    if (page == P_SETTINGS && !capturing && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int t = ui_tabs_hit(x, y), r = ui_rows_hit(x, y);
        if (t == UI_TAB_PREV || t == UI_TAB_NEXT)
            settings_tab(t == UI_TAB_PREV ? -1 : 1);
        else if (t >= 0) {
            st_tab = t;
            st_row = st_top = 0;
        } else if (r >= 0) {
            st_row = r;
            settings_press(r);
        }
    }
    int id = hit_at(x, y);
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        if (id >= 0 && enabled(id))
            focus = id;
    } else if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT && id >= 0) {
        focus = id;
        press(id);
    }
}

/* --shot work and fail: a build's state to draw */
static void shot_fake(void)
{
    static const char *const sample[] = {
        "[   0.0] VF4 Final Tuned recomp builder 0.5.0-dev; Python 3.14.0 on linux",
        "[   0.1] set: ~/Games/naomi2/vf4tuned/gds-0036f.chd and ~/Games/naomi2/vf4tuned.zip",
        "[   0.1] extracting VF41_S.BIN (240 MiB) from the GD-ROM",
        "[   4.9] decrypting test.bin",
        "[   5.5] compiler: clang version 23.1.4",
        "[   5.5] translating main.bin (about a minute)",
        "[  38.2] translating test.bin",
        "[  44.0] compiling 111 files, 32 at a time",
        "[  47.9]   compiled 50 of 111",
    };
    for (size_t i = 0; i < SDL_arraysize(sample); i++)
        say("%s", sample[i]);
    stage_n = 4;
    stage_of = 6;
    stage_pct = 45;
    SDL_strlcpy(stage_text, "Compiling", sizeof stage_text);
    shot_elapsed = 48.2;
    shot_working = page == P_WORK;
    if (page == P_FAIL) {
        say("error: compiling runtime/host/main.c failed:");
        SDL_strlcpy(error_line, "compiling runtime/host/main.c failed:", sizeof error_line);
    }
}

/* The window's first size and its smallest. SDL's window units are logical
 * on Wayland (the desktop scales them) but pixels on Windows and X11, where
 * SDL_GetDisplayContentScale is the desktop's scale (SDL's README-highdpi):
 * the page times that scale. Under Wine and Proton the scale is Wine's own
 * (96 dpi, 1.0, unless the prefix says otherwise), not the Linux desktop's,
 * so on a 4K screen scaled 175% that alone leaves the window about 550 x 340
 * as the desktop measures it: the window is also at least half the usable
 * height of the screen, in the page's shape, and at most 90% of it. */
static float win_scale = 1.0f;     /* for the log */
static SDL_Rect win_usable;

static void first_size(int *w, int *h, int *min_w, int *min_h)
{
    SDL_DisplayID d = SDL_GetPrimaryDisplay();
    float cs = d ? SDL_GetDisplayContentScale(d) : 0.0f;
    if (!(cs >= 0.5f && cs <= 8.0f))
        cs = 1.0f;
    SDL_Rect u = {0, 0, 0, 0};
    if (!d || !SDL_GetDisplayUsableBounds(d, &u) || u.w < 320 || u.h < 200)
        u.w = u.h = 0;
    float fh = LH * cs;
    if (u.h && fh < u.h * 0.5f)
        fh = u.h * 0.5f;
    float fw = fh * LW / LH;
    if (u.w && fw > u.w * 0.9f)
        fw = u.w * 0.9f, fh = fw * LH / LW;
    if (u.h && fh > u.h * 0.9f)
        fh = u.h * 0.9f, fw = fh * LW / LH;
    *w = (int)fw;
    *h = (int)fh;
    *min_w = SDL_min((int)(640 * cs), *w);
    *min_h = SDL_min((int)(400 * cs), *h);
    win_scale = cs;
    win_usable = u;
}

int main(int argc, char **argv)
{
    const char *opt_game = NULL, *opt_pick_game = NULL, *opt_set = NULL, *opt_do = NULL, *shot_page = NULL, *shot_file = NULL,
               *shot_device = NULL;
    int shot_scale = 1;
    const char *unknown[8];
    int opt_exit = 0, opt_show = 0, nunknown = 0;
    double opt_cancel = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) opt_game = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) opt_set = argv[++i];
        else if (!strcmp(argv[i], "--pick-game") && i + 1 < argc) opt_pick_game = argv[++i];
        else if (!strcmp(argv[i], "--do") && i + 1 < argc) opt_do = argv[++i];
        else if (!strcmp(argv[i], "--exit")) opt_exit = 1;
        else if (!strcmp(argv[i], "--after-game")) after_game = 1;
        else if (!strcmp(argv[i], "--show")) opt_show = 1;
        else if (!strcmp(argv[i], "--cancel-after") && i + 1 < argc) opt_cancel = SDL_atof(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) shot_page = argv[++i], shot_file = argv[++i];
        else if (!strcmp(argv[i], "--shot-device") && i + 1 < argc) shot_device = argv[++i];
        else if (!strcmp(argv[i], "--shot-scale") && i + 1 < argc) shot_scale = SDL_atoi(argv[++i]);
        else if (nunknown < 8) unknown[nunknown++] = argv[i];
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    if (!shot_page || shot_scale < 1 || shot_scale > 4)
        shot_scale = 1;     /* --shot-scale: the window made K times as big, as on a high-DPI screen */
    int win_w = (int)LW * shot_scale, win_h = (int)LH * shot_scale, min_w = 640, min_h = 400;
    if (!shot_page)
        first_size(&win_w, &win_h, &min_w, &min_h);     /* pictures keep the page's own size */
    {
        char p[1100];
        direct_path(p, sizeof p);
        play_direct = exists(p);
    }
    /* the switch on: the window stays hidden unless the launcher is needed */
    int want_direct = play_direct && !shot_page && !opt_show && !after_game && !opt_do && !opt_pick_game;
    if (!SDL_CreateWindowAndRenderer("VF4 Final Tuned builder", win_w, win_h,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                         (want_direct ? SDL_WINDOW_HIDDEN : 0),
                                     &win, &ren)) {
        SDL_Log("window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win, min_w, min_h);
    SDL_SetRenderVSync(ren, 1);
    if (ui_init(ren)) {
        SDL_Log("ui: %s", SDL_GetError());
        return 1;
    }
    find_paths();
    {
        char lp[1100], ap[1100];
        snprintf(lp, sizeof lp, "%slauncher.log", base);
        upd_path(ap, sizeof ap, "applied.json");
        log_file = fopen(lp, exists(ap) ? "a" : "w");     /* after an update: the old launcher's lines stay */
    }
    if (opt_game)
        SDL_strlcpy(game, opt_game, sizeof game);
    load_state();
    if (opt_set)
        SDL_strlcpy(set_path, opt_set, sizeof set_path);
    for (int i = 0; i < nunknown; i++)
        say("not an option: %s (ignored)", unknown[i]);
    say("builder files: %s", root[0] ? root : "(not found)");
    say("window: SDL %d.%d.%d, renderer %s, video %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
        SDL_GetRendererName(ren), SDL_GetCurrentVideoDriver());
    if (!shot_page)
        say("window: %d x %d to start (display scale %.2f, usable area %d x %d)", win_w, win_h, win_scale,
            win_usable.w, win_usable.h);
    say("install folder: %s", game);
    steam_setup();
    upd_startup();
    if (!shot_page)
        take_game_request();    /* the game's "Update when I quit" (a game that did not hand it over itself) */
    if (after_game && !shot_page)
        say("--after-game: %s", upd_auto       ? "started by the game for the update it asked for"
                                : just_updated ? "the new launcher: it builds the game again, then closes"
                                               : "nothing asked for: closing");
    if (opt_pick_game)
        choose_game_dir(opt_pick_game);     /* as the install folder's dialog answers */
    if (!set_path[0] && !built)
        find_set();
    go(built ? P_PLAY : set_ok() ? P_BUILD : P_FILES);
    if (just_updated && !shot_page) {
        say("the launcher was updated%s%s to %s", applied_from[0] ? " from " : "", applied_from, rel_version);
        if (built && update_ready()) {
            say("building the game again with builder %s", builder_ver);
            do_build(1);
        }
    }
    int do_failed = 0;      /* --do started nothing: --exit's result is then 1 */
    int auto_upd = 0;       /* --do check (3), upgrade (1, Update now) or upgrade-at-close (2) */
    if (opt_do && (!strcmp(opt_do, "check") || !strcmp(opt_do, "upgrade") || !strcmp(opt_do, "upgrade-at-close"))) {
        auto_upd = !strcmp(opt_do, "check") ? 3 : !strcmp(opt_do, "upgrade") ? 1 : 2;
        say("--do %s%s", opt_do, upd == U_OFF ? ": not a release folder, so no updates" : "");
        if (upd == U_OFF) {
            auto_upd = 0;
            do_failed = 1;
        }
    } else if (opt_do) {
        /* by name: the buttons' order (B_*) is not this list's */
        static const struct { const char *name; int button; } acts[] = {
            {"set", B_CHOOSE}, {"build", B_BUILD}, {"update", B_UPDATE}, {"play", B_PLAY}, {"steam", B_STEAM},
        };
        int found = 0;
        for (size_t i = 0; i < SDL_arraysize(acts); i++)
            if (!strcmp(opt_do, acts[i].name)) {
                int b = acts[i].button, on = enabled(b);
                found = 1;
                say("--do %s (%s)", opt_do, on ? "pressing" : "not available now");
                if (on)
                    press(b);
                if (b != B_CHOOSE && !proc && !game_proc)
                    do_failed = 1;
            }
        if (!found) {
            say("--do %s: not an action (set, build, update, play, steam, check, upgrade, upgrade-at-close)",
                opt_do);
            do_failed = 1;
        }
        if (do_failed && opt_exit)
            say("--do %s started nothing: exit code 1", opt_do);
    }
    if (shot_page) {
        static const struct { const char *name; int page, tab; } pages[] = {
            {"files", P_FILES, 0}, {"build", P_BUILD, 0}, {"work", P_WORK, 0}, {"fail", P_FAIL, 0},
            {"play", P_PLAY, 0}, {"settings", P_SETTINGS, T_GAME}, {"picture", P_SETTINGS, T_PICTURE},
            {"pad", P_SETTINGS, T_PAD}, {"key1", P_SETTINGS, T_KEY1}, {"key2", P_SETTINGS, T_KEY2},
        };
        for (size_t i = 0; i < SDL_arraysize(pages); i++)
            if (!strcmp(shot_page, pages[i].name)) {
                if (pages[i].page == P_SETTINGS)
                    settings_open();
                go(pages[i].page);
                st_tab = pages[i].tab;
            }
        if (page == P_WORK || page == P_FAIL)
            shot_fake();
        /* the Play page with a new release's card: update, update-fetch,
         * update-ready, update-failed; with the check's line: update-none */
        static const struct { const char *name; int state, when; } ups[] = {
            {"update", U_AVAILABLE, UW_NOW}, {"update-fetch", U_FETCHING, UW_CLOSE},
            {"update-ready", U_READY, UW_CLOSE}, {"update-failed", U_FAILED, UW_NOW}, {"update-none", U_NONE, UW_NOW},
        };
        for (size_t i = 0; i < SDL_arraysize(ups); i++)
            if (!strcmp(shot_page, ups[i].name)) {
                go(P_PLAY);
                if (!rel_version[0])
                    SDL_strlcpy(rel_version, builder_ver[0] ? builder_ver : "0.5.1", sizeof rel_version);
                SDL_strlcpy(upd_version, "0.6.0", sizeof upd_version);
                upd = ups[i].state;
                upd_when = ups[i].when;
                upd_size = 16787587;
                upd_stage = 1;
                upd_pct = 45;
                SDL_strlcpy(upd_error, "cannot reach GitHub: [Errno -3] Temporary failure in name resolution",
                            sizeof upd_error);
                focus = default_focus();
            }
        if (shot_device && ui_device_named(shot_device) >= 0)
            ui_set_device(ui_device_named(shot_device));    /* the caps' style (ui.h) */
        draw();
        draw();         /* the second time with the hits of the first (focus rings) */
        SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
        int ok = sf && SDL_SaveBMP(sf, shot_file);
        SDL_DestroySurface(sf);
        ui_quit();
        SDL_Quit();
        return ok ? 0 : 1;
    }
    if (want_direct) {
        const char *why = !built                ? "the game is not built yet"
                          : !direct_ok()        ? "this build plays through Steam"
                          : update_ready()      ? "a new builder is here"
                          : upd_auto            ? "the game asked for an update"
                          : just_updated        ? "the launcher was just updated"
                          : proc                ? "a build runs"
                                                : NULL;
        if (why) {
            say("start the game directly: not this time (%s)", why);
        } else {
            say("start the game directly (the Play page's switch): the launcher stays hidden");
            press(B_PLAY);
            direct_run = game_proc != NULL;
        }
        if (!direct_run)
            SDL_ShowWindow(win);
    }
    if (upd == U_IDLE && !after_game && !direct_run)
        upd_start(1);       /* the check at every start: a line on GitHub's API, no download */
    int running = 1, result = do_failed, restarting = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ui_note_event(&e);
            if (e.type == SDL_EVENT_RENDER_DEVICE_RESET || e.type == SDL_EVENT_RENDER_DEVICE_LOST ||
                e.type == SDL_EVENT_RENDER_TARGETS_RESET)
                say("window: %s", e.type == SDL_EVENT_RENDER_DEVICE_LOST    ? "the renderer's device is lost"
                                  : e.type == SDL_EVENT_RENDER_DEVICE_RESET ? "the renderer's device was reset"
                                                                            : "the renderer's targets were reset");
            else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                static int sizes;
                if (++sizes <= 12)
                    say("window: %d x %d pixels%s", (int)e.window.data1, (int)e.window.data2,
                        sizes == 12 ? " (no more of these)" : "");
            }
            if (e.type == SDL_EVENT_QUIT)
                running = 0;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                SDL_OpenGamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                mouse_event(&e);
            else if (page == P_SETTINGS)
                settings_event(&e);
            else
                page_event(&e);
        }
        if (quit_asked)
            running = 0;
        if (SDL_GetAtomicInt(&picked_game)) {
            SDL_SetAtomicInt(&picked_game, 0);
            choose_game_dir(picked_game_path);
        }
        if (SDL_GetAtomicInt(&picked)) {
            SDL_SetAtomicInt(&picked, 0);
            SDL_strlcpy(set_path, picked_path, sizeof set_path);
            SDL_SaveFile(set_file, set_path, strlen(set_path));
            say("set: %s", set_path);
            if (page == P_FILES)
                focus = default_focus();
        }
        if (opt_cancel >= 0 && proc && (proc_kind == K_BUILD || proc_kind == K_UPDATE) &&
            SDL_GetTicks() - proc_t0 >= (Uint64)(opt_cancel * 1000)) {
            say("--cancel-after %.1f: pressing Cancel", opt_cancel);
            press(B_CANCEL);
            opt_cancel = -1;
        }
        int was = proc != NULL;
        int code = pump_process();
        watch_game();
        if (direct_close) {
            running = 0;          /* the game started directly ended: the launcher goes with it */
            continue;
        }
        upd_pump();
        if (game_proc) {
            SDL_Delay(100);           /* hidden while the game runs: nothing to draw */
            continue;
        }
        if (was && !proc) {
            result = code == 0 ? 0 : 1;
            if (opt_exit)
                running = 0;
        } else if (opt_exit && opt_do && !auto_upd && !proc && !was) {
            running = 0;          /* the action started nothing to wait for */
        }
        /* Update now: in place as soon as it is here and nothing runs, then the new launcher */
        if (upd == U_READY && upd_when == UW_NOW && !proc && !uproc) {
            draw();
            if (apply_update() == 0) {
                restart_launcher(opt_game, opt_exit);
                restarting = 1;
                result = 0;
                running = 0;
                continue;
            }
            upd = U_FAILED;
        }
        /* the game asked for it: Update now as soon as there is one (or close: nothing newer) */
        if (upd_auto) {
            if ((upd == U_AVAILABLE || upd == U_FAILED) && !uproc) {
                press(B_UPD_NOW);
                upd_auto = 0;
            } else if (upd == U_READY || upd == U_FETCHING) {
                upd_when = UW_NOW;
                upd_auto = 0;
            } else if (upd == U_NONE || upd == U_CHECK_FAILED) {
                upd_auto = 0;
            }
        }
        /* --after-game: closes once its work is done and the Play page shows;
         * a failure stays on screen */
        if (after_game && running && !proc && !uproc && !game_proc && !upd_auto && page == P_PLAY &&
            !(upd == U_READY && upd_when == UW_NOW) && upd != U_CHECK_FAILED && upd != U_FAILED) {
            say("--after-game: done; closing");
            running = 0;
        }
        if (auto_upd == 1 || auto_upd == 2) {
            if (upd == U_AVAILABLE)
                press(auto_upd == 1 ? B_UPD_NOW : B_UPD_CLOSE);
            if (auto_upd == 2 && upd == U_READY) {
                say("--do upgrade-at-close: closing");
                result = 0;
                running = 0;
            }
        }
        if (opt_exit && auto_upd && (upd == U_NONE || upd == U_CHECK_FAILED || upd == U_FAILED ||
                                     (auto_upd == 3 && upd == U_AVAILABLE))) {
            result = auto_upd == 3 && upd != U_CHECK_FAILED ? 0 : 1;
            running = 0;
        }
        draw();
    }
    if (proc) {
        /* the launcher closes: the builder sees its input end and stops */
        cancel();
        Uint64 t = SDL_GetTicks();
        while (proc && SDL_GetTicks() - t < 10000) {
            pump_process();
            SDL_Delay(20);
        }
        if (proc)
            SDL_KillProcess(proc, true);
    }
    if (uproc) {
        /* a check or a download still going: it stops (a download is taken
         * up again by the next start's check). Once it has said @ready it is
         * only ending: wait for it (its Python must be gone before python/
         * can be moved aside) */
        if (upd != U_READY)
            upd_cancel();
        Uint64 t = SDL_GetTicks();
        while (uproc && SDL_GetTicks() - t < 5000) {
            upd_pump();
            SDL_Delay(20);
        }
        if (uproc) {
            SDL_KillProcess(uproc, true);
            SDL_WaitProcess(uproc, true, NULL);
            SDL_DestroyProcess(uproc);
            uproc = NULL;
        }
    }
    if (!restarting && upd == U_READY && upd_when == UW_CLOSE) {
        say("the launcher closes: putting %s in place", upd_version);
        if (apply_update() == 0)
            say("%s goes in use at the next start, which builds the game again", upd_version);
    }
    if (log_file)
        fclose(log_file);
    ui_quit();
    SDL_Quit();
    return result;
}
