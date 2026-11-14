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

#include "cards.h"
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
static int steam_started;           /* Steam started the launcher: Add to Steam asks for Steam closed first */
static int plain_wine;              /* the Windows launcher under Wine, not Proton: Play warns first */
static int wine_warned;             /* that warning was answered with Play anyway (this run) */
#ifdef _WIN32
static char self_exe[1024];         /* the launcher's own program: what Add to Steam adds */
static const char *argv0 = "";
#endif

/* Start the game directly (docs/design.md section 29): with the Play page's
 * switch on, the launcher starts hidden and presses Play itself, so a Steam
 * entry for the launcher plays the game. It shows itself instead when it is
 * needed: no game built, a new builder or an update to put in. --show (the
 * game's Open the launcher, for a game started on its own) never plays.
 * Whichever way the game started, the launcher closes when it ends with 0
 * (its Quit, section 40), and shows itself for the game's "Open the
 * launcher" (exit code 64, runtime/host/updates.h), an update asked for in
 * its menu, or a game that ended with an error. */
#define GAME_EXIT_LAUNCHER 64
static int play_direct;             /* the switch */
static int direct_run;              /* this run started the game directly (the window stays hidden) */
static int game_close;              /* the game it started ended with 0 (Quit): close */

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

static void say_line(const char *line)
{
    say("%s", line);
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
 * the game, Add to Steam (steam-art, art_dir) and Wine or Proton add beside
 * the game */
static const char *const install_names[] = {
    "build", "data", "user", "build.log", "build.json", "build.json.part", "READ ME.txt", "Font license.txt",
    "vf4ft.exe", "vf4ft", "SDL3.dll", "vf4ft.log", "vkd3d-proton.cache", "vkd3d-proton.cache.write",
    "vf4ft.dxvk-cache", "steam-art", NULL};

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

/* Add to Steam (docs/design.md section 33) edits Steam's shortcut files,
 * which Steam rewrites when it exits: it works only with Steam closed. The
 * tool says so with exit code 3, and the launcher asks for Steam to be
 * closed. When Steam started the launcher (Steam's own launch variables),
 * closing Steam closes the launcher too, so it asks for that and for the
 * launcher to be opened again from its folder. The Windows launcher under
 * Wine or Proton on Linux adds itself to the Linux host's Steam: the tool
 * finds that Steam through Wine's drives. */
static void steam_setup(void)
{
#ifdef _WIN32
    /* the launcher's own program: its folder (SDL's base path) and the
     * name it was started by */
    const char *n = argv0 + strlen(argv0);
    while (n > argv0 && n[-1] != '\\' && n[-1] != '/')
        n--;
    size_t nl = strlen(n);
    snprintf(self_exe, sizeof self_exe, "%s%s%s", base, nl ? n : "vf4ft-launcher.exe",
             nl && (nl < 4 || SDL_strcasecmp(n + nl - 4, ".exe")) ? ".exe" : "");
#endif
    static const char *const vars[] = {"SteamGameId", "SteamClientLaunch", "SteamAppId", NULL};
    for (int i = 0; vars[i]; i++) {
        const char *v = SDL_getenv(vars[i]);
        if (v && v[0]) {
            steam_started = 1;
            say("steam: started by Steam (%s=%s): Add to Steam asks for Steam to be closed first", vars[i], v);
            return;
        }
    }
#ifdef _WIN32
    SDL_SharedObject *nt = SDL_LoadObject("ntdll.dll");
    const char *(*wine_version)(void) = nt ? (const char *(*)(void))SDL_LoadFunction(nt, "wine_get_version") : NULL;
    if (wine_version) {
        /* plain Wine (no Steam variables, and none of Proton's): Play warns
         * first (docs/design.md section 34) */
        void (*host)(const char **, const char **) =
            (void (*)(const char **, const char **))SDL_LoadFunction(nt, "wine_get_host_version");
        const char *sys = NULL, *rel = NULL;
        if (host)
            host(&sys, &rel);
        const char *compat = SDL_getenv("STEAM_COMPAT_DATA_PATH");
        plain_wine = !(compat && compat[0]);
        say("steam: Wine %s on %s: Add to Steam adds the launcher to the host's Steam%s", wine_version(),
            sys ? sys : "an unknown system", plain_wine ? "; plain Wine, not Proton: Play warns first" : "");
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

enum { K_NONE, K_BUILD, K_UPDATE, K_STEAM, K_STEAM_PLAY, K_STEAM_CHECK, K_ART };
static SDL_Process *proc;
static SDL_Process *game_proc;      /* the game, while it runs (the launcher hides meanwhile) */
static SDL_IOStream *proc_out, *proc_in;
static int proc_kind, last_kind;     /* last_kind: the last build's (Try again does it again) */
static Uint64 proc_t0, cancel_at;   /* ms */
static int stage_n, stage_of, stage_pct = -1;
static char stage_text[96];
static double shot_elapsed = -1;    /* --shot work: the time shown */
static int shot_working;            /* --shot work: drawn as during a build (Cancel can be pressed) */
static int shot_mode;               /* a --shot picture: the text field's caret shows */
static int cancelled;
static char error_line[300];
/* the Play page's line: what the last action came to */
enum { ST_NONE, ST_WORKING, ST_GOOD, ST_BAD };
static int status_kind;
static char status_text[300];

enum { P_FILES, P_BUILD, P_WORK, P_FAIL, P_PLAY, P_SETTINGS, P_EDIT };
static int page;
static void go(int p);
static void press(int b);

/* a note over the Play page (Add to Steam's, and Play's under plain Wine): only its buttons answer */
enum { POP_NONE, POP_FROM_STEAM, POP_CLOSE_STEAM, POP_ADDED, POP_WINE, POP_RENAME, POP_DELETE, POP_RINGNAME };
static int pop;
static int steam_proton;            /* the tool set Proton for the entry */
static void pop_open(int kind);
static void steam_step(int kind);
static int steam_next;              /* Add to Steam's next step, once the process ends */
static int art_made;                /* Steam's pictures are there for the entry */
static int art_killed;

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

/* ---- Add to Steam's steps (docs/design.md section 36) ----
 * The Steam tool's check that Steam is closed (add --dry-run: exit 3 if it
 * runs, nothing written); Steam's pictures, made by the game from the
 * player's own build (vf4ft --art), unless the ones there are newer than the
 * game; then the entry, with the pictures when there are some. */

#ifdef _WIN32
#define DIR_SEP "\\"
#else
#define DIR_SEP "/"
#endif

static const char *const art_names[] = {"capsule.png", "wide.png", "hero.png", "icon.png"};
#define ART_WORKING "Making Steam's pictures from your build (about half a minute)\xE2\x80\xA6"
#define ADDED_ART "Added to Steam with its pictures: start Steam again and it is in your library."
#define ADDED_NO_ART "Added to Steam, without pictures (launcher.log says why): start Steam again and it is in your library."

static const char *art_dir(void)
{
    static char d[1100];
    snprintf(d, sizeof d, "%s%ssteam-art", game, DIR_SEP);
    return d;
}

/* every picture there, and none older than the game's program */
static int art_fresh(void)
{
    char prog[1100], p[1200];
    program_path(prog, sizeof prog);
    SDL_PathInfo exe, f;
    if (!SDL_GetPathInfo(prog, &exe))
        return 0;
    for (size_t i = 0; i < SDL_arraysize(art_names); i++) {
        snprintf(p, sizeof p, "%s%s%s", art_dir(), DIR_SEP, art_names[i]);
        if (!SDL_GetPathInfo(p, &f) || f.type != SDL_PATHTYPE_FILE || f.modify_time < exe.modify_time)
            return 0;
    }
    return 1;
}

/* the game can run here to make them: the Windows launcher runs the Windows
 * game (natively or under the same Wine). The Linux launcher's entry is the
 * Windows build, which it cannot run itself. */
static int art_possible(void)
{
#ifdef _WIN32
    return 1;
#else
    say("Steam's pictures: the Linux launcher cannot run the Windows game to make them, so the entry goes "
        "without them");
    return 0;
#endif
}

static void steam_step(int kind)
{
    char script[1100], prog[1100], logp[1200];
    program_path(prog, sizeof prog);
    snprintf(script, sizeof script, "%stools/steam_shortcut.py", root);
#ifdef _WIN32
    const char *what = self_exe;    /* the launcher: its entry can start the game directly (section 29) */
#else
    const char *what = prog;        /* the Windows build that Play starts through Steam */
#endif
    int r;
    status_kind = ST_WORKING;
    if (kind == K_ART) {
        /* the game's own log of the run goes with the pictures: vf4ft.log,
         * the player's last game, is left as it was */
        snprintf(logp, sizeof logp, "%s%sart.log", art_dir(), DIR_SEP);
        SDL_CreateDirectory(art_dir());
        const char *args[] = {prog, "--art", art_dir(), "--log", logp, NULL};
        SDL_strlcpy(status_text, ART_WORKING, sizeof status_text);
        say("Steam's pictures: %s --art %s", prog, art_dir());
        if (start(K_ART, args, 1)) {     /* without them, then */
            say("Steam's pictures: the game did not start: the entry goes without them");
            art_made = 0;
            steam_step(K_STEAM);
        }
        return;
    }
    SDL_strlcpy(status_text, "Adding it to Steam\xE2\x80\xA6", sizeof status_text);
    if (kind == K_STEAM_CHECK) {
        const char *args[] = {python, "-I", script, "add", what, "--dry-run", NULL};
        r = start(K_STEAM_CHECK, args, 1);
    } else if (art_made) {
        const char *args[] = {python, "-I", script, "add", what, "--art", art_dir(), NULL};
        r = start(K_STEAM, args, 1);
    } else {
        const char *args[] = {python, "-I", script, "add", what, NULL};
        r = start(K_STEAM, args, 1);
    }
    if (r) {
        status_kind = ST_BAD;
        SDL_strlcpy(status_text, error_line, sizeof status_text);
    }
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
    if (proc_kind == K_ART && s[0] == '[' && !strstr(s, "art: ") && !strstr(s, "failed"))
        return;                     /* the picture run's whole log is steam-art/art.log */
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
    if (proc_kind == K_STEAM && !strncmp(s, "compat tool ", 12))
        steam_proton = 1;
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
    if (proc_kind == K_ART && !art_killed && SDL_GetTicks() - proc_t0 > 300000) {
        say("Steam's pictures: the game did not finish in 5 minutes: ended");
        SDL_KillProcess(proc, true);
        art_killed = 1;
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
    case K_STEAM_CHECK:             /* add --dry-run: Steam is closed (else exit 3), nothing written */
        if (code == 0) {
            art_made = art_fresh();
            if (art_made)
                say("Steam's pictures: the ones in %s are newer than the game, so they are used", art_dir());
            steam_next = !art_made && art_possible() ? K_ART : K_STEAM;
        } else if (code == 3) {
            status_kind = ST_NONE;
            pop_open(POP_CLOSE_STEAM);
        } else {
            status_kind = ST_BAD;
            snprintf(status_text, sizeof status_text, "Could not add it to Steam: %.250s",
                     error_line[0] ? error_line : "the Steam tool failed (launcher.log)");
        }
        break;
    case K_ART:
        art_made = code == 0;
        if (art_made)
            say("Steam's pictures are made: %s", art_dir());
        else
            say("Steam's pictures could not be made (the game's exit code %d; the lines above, and %s%sart.log if it is "
                "there, say why): the entry goes without them", code, art_dir(), DIR_SEP);
        steam_next = K_STEAM;
        break;
    case K_STEAM:
        status_kind = code == 0 ? ST_GOOD : code == 3 ? ST_NONE : ST_BAD;
        if (code == 0) {
            SDL_strlcpy(status_text, art_made ? ADDED_ART : ADDED_NO_ART, sizeof status_text);
            pop_open(POP_ADDED);
        } else if (code == 3) {     /* tools/steam_shortcut.py: Steam is running */
            pop_open(POP_CLOSE_STEAM);
        }
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
    art_killed = 0;
    if (steam_next) {
        int k = steam_next;
        steam_next = 0;
        steam_step(k);
    }
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
        if (code == 0 && !upd_auto) {
            /* the game's Quit (or its window closed): the launcher goes too.
             * Its menu's Open the launcher is the way back here (code 64) */
            game_close = 1;
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
    B_DIRECT, B_POP_OK, B_POP_RETRY, B_POP_PLAY, B_POP_STEAM, B_POP_YES,
    B_CP_P1, B_CP_P2, B_CP_EDIT, B_CP_NEW, B_CP_COPY, B_CP_RENAME, B_CP_DELETE, B_ED_SAVE, B_ED_CANCEL, B_COUNT
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
static int cp_enabled(int b);

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
    case B_STEAM: return built && root[0] && !proc && !built_linux;
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
    case B_CP_P1: case B_CP_P2: case B_CP_EDIT: case B_CP_NEW: case B_CP_COPY: case B_CP_RENAME:
    case B_CP_DELETE: case B_POP_YES:
        return cp_enabled(b);
    case B_ED_SAVE: case B_ED_CANCEL: return 1;
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
    if (pop) {
        if (pop == POP_WINE) {
            ids[n++] = B_POP_STEAM;
            ids[n++] = B_POP_PLAY;
        }
        if (pop == POP_CLOSE_STEAM)
            ids[n++] = B_POP_RETRY;
        if (pop == POP_RENAME || pop == POP_DELETE || pop == POP_RINGNAME)
            ids[n++] = B_POP_YES;
        ids[n++] = B_POP_OK;
        return n;
    }
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
        if (!built_linux)
            ids[n++] = B_STEAM;
        ids[n++] = B_REBUILD;
        ids[n++] = B_FILES;
        if (direct_shown())
            ids[n++] = B_DIRECT;
        if (check_shown())
            ids[n++] = B_CHECK;
        break;
    case P_SETTINGS: ids[n++] = B_DONE; break;
    case P_EDIT: ids[n++] = B_ED_CANCEL; ids[n++] = B_ED_SAVE; break;
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

enum { T_GAME, T_CARDS, T_PICTURE, T_SOUND, T_PAD, T_KEY1, T_KEY2, T_COUNT };
static const char *const tab_names[T_COUNT] = { "Game", "Cards", "Picture", "Sound", "Gamepad", "Keyboard 1",
                                                 "Keyboard 2" };
/* the option tabs' rows (the Cards tab lists the card profiles, the
 * controls tabs the actions) */
static const int tab_rows[T_COUNT][8] = {
    [T_GAME] = { OPT_CREDITS, OPT_CARDS, OPT_NETWORK, -1 },
    [T_CARDS] = { -1 },
    [T_PICTURE] = { OPT_FULLSCREEN, OPT_WINDOW, OPT_PICTURE, OPT_FILTER, OPT_RENDERER, OPT_SCALE, -1 },
    [T_SOUND] = { OPT_SOUND, OPT_VOLUME, -1 },
    [T_PAD] = { -1 }, [T_KEY1] = { -1 }, [T_KEY2] = { -1 },
};
static settings st;
static char settings_path[1100];
static int st_tab, st_row, st_top;
static int capturing;               /* a controls row waits for its key or button */
static int settings_from = P_PLAY;

/* ---- the Cards tab: the card profiles (cards.h; docs/design.md section 37) ----
 * Each profile is a card in the game's user/cards folder. The list has every
 * card there, and a player's card the game has not made yet; the bar under
 * it gives the chosen one to player 1 or 2 (saved with the settings), makes a
 * new card, copies, renames or deletes one (a deleted card goes to
 * user/cards/Deleted, from where it can be moved back). */
#define CP_MAX (CARDS_MAX + 2)
static char cp_names[CP_MAX][CARD_NAME_MAX];
static int cp_n;
static int cp_has[CP_MAX];          /* 1: its file is there */
static card_view cp_view[CP_MAX];
static int cp_bad[CP_MAX];          /* the file is not a card */
static char cp_text[CARD_NAME_MAX]; /* the name being typed (Rename) */
static char cp_error[200];          /* why it cannot be that name */
static int cp_fresh;                /* the name as it was, chosen: the first key typed replaces it */

static int cards_tab(void)
{
    return st_tab == T_CARDS;
}

static void cp_file(char *out, size_t n, const char *name)
{
    char u[1100];
    snprintf(u, sizeof u, "%s" DIR_SEP "user" DIR_SEP, game);
    cards_path(out, n, u, name);
}

/* the player whose card the row is: 0, 1, or -1 */
static int cp_side(int i)
{
    for (int side = 0; side < 2; side++)
        if (i < cp_n && st.card[side][0] && !SDL_strcasecmp(cp_names[i], st.card[side]))
            return side;
    return -1;
}

static int cp_index(const char *name)
{
    for (int i = 0; i < cp_n; i++)
        if (!SDL_strcasecmp(cp_names[i], name))
            return i;
    return -1;
}

/* the list again; the chosen row on name when it is there */
static void cp_refresh(const char *name)
{
    char u[1100];
    snprintf(u, sizeof u, "%s" DIR_SEP "user" DIR_SEP, game);
    static char found[CARDS_MAX][CARD_NAME_MAX];
    int nf = cards_list(u, found, CARDS_MAX);
    cp_n = 0;
    for (int i = 0; i < nf; i++) {
        SDL_strlcpy(cp_names[cp_n], found[i], CARD_NAME_MAX);
        cp_has[cp_n++] = 1;
    }
    for (int side = 0; side < 2; side++)    /* a player's card the game has not made yet */
        if (st.card[side][0] && cp_index(st.card[side]) < 0 && cp_n < CP_MAX) {
            SDL_strlcpy(cp_names[cp_n], st.card[side], CARD_NAME_MAX);
            cp_has[cp_n++] = 0;
        }
    for (int i = 0; i < cp_n; i++) {
        SDL_zero(cp_view[i]);
        cp_bad[i] = 0;
        if (!cp_has[i])
            continue;
        char path[1300];
        size_t len = 0;
        cp_file(path, sizeof path, cp_names[i]);
        Uint8 *c = SDL_LoadFile(path, &len);
        if (c && len == CARD_SIZE)
            card_view_of(c, &cp_view[i]);
        else
            cp_bad[i] = 1;
        SDL_free(c);
    }
    int at = name ? cp_index(name) : -1;
    if (at >= 0)
        st_row = at;
    if (st_row >= cp_n)
        st_row = cp_n ? cp_n - 1 : 0;
}

/* name, else name 2, name 3 ... (cut to fit) that no card has */
static void cp_unique(char *out, const char *name)
{
    for (int k = 1; k < 1000; k++) {
        char tail[16] = "";
        if (k > 1)
            snprintf(tail, sizeof tail, " %d", k);
        size_t room = CARD_NAME_MAX - 1 - strlen(tail);
        char base[CARD_NAME_MAX];
        SDL_strlcpy(base, name, room + 1 < sizeof base ? room + 1 : sizeof base);
        size_t bl = strlen(base);
        while (bl && (base[bl - 1] == ' ' || base[bl - 1] == '.'))
            base[--bl] = 0;
        snprintf(out, CARD_NAME_MAX, "%s%s", base, tail);
        if (cp_index(out) < 0)
            return;
    }
}

static int cp_write(const char *name, const Uint8 *card)
{
    char dir[1200], path[1300];
    snprintf(dir, sizeof dir, "%s" DIR_SEP "user", game);
    if (!exists(dir))
        SDL_CreateDirectory(dir);
    snprintf(dir, sizeof dir, "%s" DIR_SEP "user" DIR_SEP "cards", game);
    if (!exists(dir))
        SDL_CreateDirectory(dir);
    cp_file(path, sizeof path, name);
    if (!SDL_SaveFile(path, card, CARD_SIZE)) {
        say("cards: cannot write %s: %s", path, SDL_GetError());
        return -1;
    }
    say("cards: %s", path);
    return 0;
}

/* Player 1 or Player 2: the chosen card becomes that player's; pressed on
 * the player's own, the player has none. The other player's card swaps */
static void cp_assign(int side)
{
    if (st_row >= cp_n)
        return;
    char name[CARD_NAME_MAX], old[CARD_NAME_MAX];
    SDL_strlcpy(name, cp_names[st_row], sizeof name);
    SDL_strlcpy(old, st.card[side], sizeof old);
    if (!SDL_strcasecmp(old, name)) {
        st.card[side][0] = 0;
    } else {
        if (!SDL_strcasecmp(st.card[!side], name))
            SDL_strlcpy(st.card[!side], old, sizeof st.card[!side]);
        SDL_strlcpy(st.card[side], name, sizeof st.card[side]);
    }
    say("cards: player 1 %s, player 2 %s", st.card[0][0] ? st.card[0] : "none", st.card[1][0] ? st.card[1] : "none");
    cp_refresh(name);
}

static void ed_open(void);

/* New: a ready card (card_starter: Akira, every item), then its editor */
static void cp_new(void)
{
    Uint8 c[CARD_SIZE];
    char name[CARD_NAME_MAX];
    cp_unique(name, "Card");
    card_starter(c, card_new_number());
    if (!cp_write(name, c)) {
        cp_refresh(name);
        ed_open();
    }
}

/* Copy: the chosen card under a new name, with a card number of its own */
static void cp_copy(void)
{
    char path[1300], name[CARD_NAME_MAX], base[CARD_NAME_MAX + 8];
    size_t len = 0;
    cp_file(path, sizeof path, cp_names[st_row]);
    Uint8 *c = SDL_LoadFile(path, &len);
    if (c && len == CARD_SIZE) {
        Uint32 number = card_new_number();
        c[4] = (Uint8)(number >> 24);
        c[5] = (Uint8)(number >> 16);
        c[6] = (Uint8)(number >> 8);
        c[7] = (Uint8)number;
        snprintf(base, sizeof base, "%s copy", cp_names[st_row]);
        cp_unique(name, base);
        if (!cp_write(name, c))
            cp_refresh(name);
    }
    SDL_free(c);
}

/* whether cp_text can be the chosen card's new name (cp_error says why not) */
static int cp_text_ok(void)
{
    cp_error[0] = 0;
    int at = cp_index(cp_text);
    if (!settings_card_name_ok(cp_text))
        SDL_strlcpy(cp_error, cp_text[0] ? "A name has up to 31 letters, numbers, spaces and marks, but none of "
                                           "/ \\ : * ? \" < > | #, and no space or dot at either end."
                                         : "Type a name.", sizeof cp_error);
    else if (at >= 0 && at != st_row)
        SDL_strlcpy(cp_error, "Another card has that name.", sizeof cp_error);
    return !cp_error[0];
}

static void cp_rename(void)
{
    if (st_row >= cp_n || !cp_text_ok())
        return;
    char old[CARD_NAME_MAX], from[1300], to[1300];
    SDL_strlcpy(old, cp_names[st_row], sizeof old);
    if (cp_has[st_row]) {
        cp_file(from, sizeof from, old);
        cp_file(to, sizeof to, cp_text);
        if (!SDL_RenamePath(from, to)) {
            snprintf(cp_error, sizeof cp_error, "It cannot be renamed: %s", SDL_GetError());
            return;
        }
        say("cards: %s is now %s", from, to);
    }
    for (int side = 0; side < 2; side++)
        if (!SDL_strcasecmp(st.card[side], old))
            SDL_strlcpy(st.card[side], cp_text, sizeof st.card[side]);
    SDL_StopTextInput(win);
    pop = POP_NONE;
    focus = B_CP_RENAME;
    cp_refresh(cp_text);
}

/* Delete: the card moves to user/cards/Deleted (with the time in its name) */
static void cp_delete(void)
{
    if (st_row >= cp_n)
        return;
    char name[CARD_NAME_MAX], from[1300], dir[1200], to[1400];
    SDL_strlcpy(name, cp_names[st_row], sizeof name);
    if (cp_has[st_row]) {
        cp_file(from, sizeof from, name);
        snprintf(dir, sizeof dir, "%s" DIR_SEP "user" DIR_SEP "cards" DIR_SEP "Deleted", game);
        if (!exists(dir))
            SDL_CreateDirectory(dir);
        SDL_Time now = 0;
        SDL_DateTime dt;
        SDL_zero(dt);
        if (SDL_GetCurrentTime(&now))
            SDL_TimeToDateTime(now, &dt, true);
        snprintf(to, sizeof to, "%s" DIR_SEP "%s %04d-%02d-%02d %02d%02d%02d.card", dir, name, dt.year, dt.month, dt.day,
                 dt.hour, dt.minute, dt.second);
        if (!SDL_RenamePath(from, to)) {
            say("cards: cannot move %s to %s: %s", from, to, SDL_GetError());
            return;
        }
        say("cards: %s moved to %s", from, to);
    }
    for (int side = 0; side < 2; side++)
        if (!SDL_strcasecmp(st.card[side], name))
            st.card[side][0] = 0;
    pop = POP_NONE;
    focus = B_CP_DELETE;
    cp_refresh(NULL);
}

/* the bar's buttons, in the order Left and Right go through them */
static const int cp_bar[] = { B_CP_P1, B_CP_P2, B_CP_EDIT, B_CP_NEW, B_CP_COPY, B_CP_RENAME, B_CP_DELETE, B_DONE };

static void cp_bar_move(int d)
{
    int n = (int)SDL_arraysize(cp_bar), at = 0;
    for (int i = 0; i < n; i++)
        if (cp_bar[i] == focus)
            at = i;
    for (int k = 0; k < n; k++) {
        at = (at + d + n) % n;
        if (enabled(cp_bar[at])) {
            focus = cp_bar[at];
            return;
        }
    }
}

/* the bar's focus on a button that works for the chosen card */
static void cp_focus_fix(void)
{
    int on_bar = 0;
    for (size_t i = 0; i < SDL_arraysize(cp_bar); i++)
        on_bar |= cp_bar[i] == focus;
    if (!on_bar)
        focus = B_CP_P1;
    if (!enabled(focus))
        cp_bar_move(1);
}

/* Edit takes a Final Tuned card whose checks hold; a card never used, or a
 * player's card not made yet, becomes a ready card (card_starter) on Save */
static int cp_editable(int i)
{
    const card_view *v = &cp_view[i];
    return !cp_has[i] || (!cp_bad[i] && (v->blank || (v->format == 4 && v->header_ok && v->body_ok &&
                                                       v->chara != CARD_NOT_A_PLAYER)));
}

static int cp_enabled(int b)
{
    int any = st_row < cp_n;
    switch (b) {
    case B_CP_P1: case B_CP_P2: case B_CP_RENAME: case B_CP_DELETE: return any;
    case B_CP_EDIT: return any && cp_editable(st_row);
    case B_CP_COPY: return any && cp_has[st_row] && !cp_bad[st_row];
    case B_CP_NEW: return cp_n < CARDS_MAX;
    case B_POP_YES: return pop != POP_RENAME || cp_text_ok();
    }
    return 1;
}

/* ---- the card editor (a page of its own): ring name, character, rank,
 * games, items, and each costume's colors and worn items, written as the
 * game's own changes write them (cards.h, card_fields_put) ---- */
enum { ED_NAME, ED_CHARA, ED_RANK, ED_PLAYS, ED_ITEMS, ED_CARD_ROWS };
enum { ED_COLOR = 0, ED_HAIR = 4, ED_EYES = 5, ED_SLOT = 6, ED_COSTUME_ROWS = 10 };
static const char *const ed_tab_names[3] = { "Card", "Costume 1", "Costume 2" };
static char ed_name[CARD_NAME_MAX];     /* the profile */
static Uint8 ed_card[CARD_SIZE];
static card_fields ed_f, ed_f0;         /* as edited, as it was */
static int ed_new;                      /* a ready card made on Save (the file has none yet) */
static int ed_tab, ed_row, ed_top;

static int ed_rows(void)
{
    return ed_tab == 0 ? ED_CARD_ROWS : ED_COSTUME_ROWS;
}

static void ed_open(void)
{
    if (st_row >= cp_n)
        return;
    SDL_strlcpy(ed_name, cp_names[st_row], sizeof ed_name);
    Uint8 *c = NULL;
    size_t len = 0;
    if (cp_has[st_row]) {
        char path[1300];
        cp_file(path, sizeof path, ed_name);
        c = SDL_LoadFile(path, &len);
    }
    ed_new = !(c && len == CARD_SIZE) || cp_view[st_row].blank;
    if (!ed_new) {
        memcpy(ed_card, c, CARD_SIZE);
    } else {
        /* a card never used keeps its number */
        Uint32 number = c && len == CARD_SIZE ? (Uint32)c[4] << 24 | (Uint32)c[5] << 16 | (Uint32)c[6] << 8 | c[7]
                                              : card_new_number();
        card_starter(ed_card, number);
    }
    SDL_free(c);
    card_fields_get(ed_card, &ed_f);
    ed_f0 = ed_f;
    ed_tab = ed_row = ed_top = 0;
    page = P_EDIT;
    focus = -1;
    say("card editor: %s%s", ed_name, ed_new ? " (made a ready card on Save)" : "");
}

/* Save (or Cancel): back to the Cards tab */
static void ed_close(int save)
{
    if (save && (ed_new || memcmp(&ed_f, &ed_f0, sizeof ed_f))) {
        card_fields_put(ed_card, &ed_f);
        cp_write(ed_name, ed_card);
    } else {
        say("card editor: %s %s", ed_name, save ? "unchanged" : "left as it was (Cancel)");
    }
    page = P_SETTINGS;
    st_tab = T_CARDS;
    cp_refresh(ed_name);
    focus = B_CP_EDIT;
    cp_focus_fix();
}

/* the games a card can be given: Left and Right go to the next of these */
static const int ed_plays[] = { 1, 10, 50, 100, 200, 300, 400, CARD_PLAYS };

static void ed_change(int d)
{
    card_fields *f = &ed_f;
    if (ed_tab == 0) {
        switch (ed_row) {
        case ED_CHARA:
            do
                f->chara = (f->chara + d + CARD_CHARAS) % CARD_CHARAS;
            while (f->chara == CARD_NOT_A_PLAYER);
            break;
        case ED_RANK:
            /* a title rank (VF.NET's) is left only by going down */
            if (f->level > CARD_TOP_RANK)
                f->level = d < 0 ? CARD_TOP_RANK : f->level;
            else
                f->level = (f->level + d + CARD_TOP_RANK + 1) % (CARD_TOP_RANK + 1);
            break;
        case ED_PLAYS: {
            int n = (int)SDL_arraysize(ed_plays), v = f->plays_left, to = -1;
            for (int i = 0; i < n && to < 0; i++)
                if (d > 0 && ed_plays[i] > v)
                    to = ed_plays[i];
            for (int i = n - 1; i >= 0 && to < 0; i--)
                if (d < 0 && ed_plays[i] < v)
                    to = ed_plays[i];
            f->plays_left = to >= 0 ? to : d > 0 ? ed_plays[0] : ed_plays[n - 1];
            break;
        }
        case ED_ITEMS: {
            /* every item, none, as the card had them (when that is neither) */
            static const Uint8 all[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, none[8] = { 0 };
            const Uint8 *ways[3] = { all, none, ed_f0.items };
            int n = memcmp(ed_f0.items, all, 8) && memcmp(ed_f0.items, none, 8) ? 3 : 2, at = 0;
            for (int i = 0; i < n; i++)
                if (!memcmp(f->items, ways[i], 8))
                    at = i;
            memcpy(f->items, ways[(at + d + n) % n], 8);
            break;
        }
        }
        return;
    }
    int c = ed_tab - 1;
    if (ed_row < ED_HAIR)
        f->color[c][ed_row] = (f->color[c][ed_row] + d + CARD_COLORS + 1) % (CARD_COLORS + 1);
    else if (ed_row == ED_HAIR)
        f->hair[c] = (f->hair[c] + d + CARD_HAIRS + 1) % (CARD_HAIRS + 1);
    else if (ed_row == ED_EYES)
        f->eye[c] = (f->eye[c] + d + CARD_EYES + 1) % (CARD_EYES + 1);
    else
        f->equip[c][ed_row - ED_SLOT] = (f->equip[c][ed_row - ED_SLOT] + d + CARD_ITEMS + 1) % (CARD_ITEMS + 1);
}

/* A, Enter or a click on a row: the ring name asks for the name */
static void ed_press(void)
{
    if (ed_tab == 0 && ed_row == ED_NAME) {
        char t[44];
        card_name_text(ed_f.name, t, sizeof t);
        SDL_strlcpy(cp_text, SDL_strchr(t, '?') ? "" : t, sizeof cp_text);    /* a name not in ASCII: anew */
        cp_error[0] = 0;
        pop_open(POP_RINGNAME);
    } else {
        ed_change(1);
    }
}

/* the ring name typed (POP_RINGNAME's OK): ASCII, spaces at its ends left out */
static void ed_name_set(void)
{
    const char *t = cp_text;
    while (*t == ' ')
        t++;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ')
        n--;
    memset(ed_f.name, 0, sizeof ed_f.name);
    memcpy(ed_f.name, t, SDL_min(n, sizeof ed_f.name));
    SDL_StopTextInput(win);
    pop = POP_NONE;
    focus = -1;
}

/* a row of the editor, its value written to v; its description in *about */
static void ed_row_of(int i, ui_row *r, char *v, size_t n, const char **about)
{
    SDL_zerop(r);
    r->kind = UI_ROW_CHOICE;
    r->value = v;
    const card_fields *f = &ed_f;
    if (ed_tab == 0) {
        switch (i) {
        case ED_NAME:
            r->label = "Ring name";
            r->kind = UI_ROW_TEXT;
            card_name_text(f->name, v, n);
            if (!v[0])
                SDL_strlcpy(v, "None", n);
            *about = "The name the game shows for the player, up to 20 letters, numbers and marks. A cabinet's "
                     "cards had it set on VF.NET; a card with none shows the game's own text instead. Press A or "
                     "Enter to type it.";
            break;
        case ED_CHARA:
            r->label = "Character";
            SDL_strlcpy(v, card_chara_names[f->chara], n);
            *about = "A card is one character's: with it in, the game picks that character at the select "
                     "screen. Items are numbered per character, so the worn items mean other items for another "
                     "one.";
            break;
        case ED_RANK:
            r->label = "Rank";
            card_rank_text(f->level, v, n);
            *about = "From 10th kyu to 10th dan, the ranks a card reaches without VF.NET. A new rank starts with "
                     "no progress toward the next.";
            break;
        case ED_PLAYS:
            r->label = "Games left";
            SDL_snprintf(v, n, f->plays_left ? "%d" : "None (used up)", f->plays_left);
            *about = "How many more games the card plays: each game takes one, and at none the card is used up. "
                     "A new card has 500.";
            break;
        case ED_ITEMS: {
            r->label = "Items owned";
            int k = 0;
            for (int b = 0; b < 64; b++)
                k += f->items[b / 8] >> (b & 7) & 1;
            SDL_snprintf(v, n, k == 64 ? "All" : k == 0 ? "None" : "%d of 64", k);
            *about = "The items the card has won, which the game's own item screens offer. A ready card has all "
                     "of them.";
            break;
        }
        }
        return;
    }
    int c = ed_tab - 1;
    static const char *const color_labels[4] = { "Color, part 1", "Color, part 2", "Color, part 3",
                                                 "Color, part 4" };
    static const char *const slot_labels[4] = { "Item slot 1", "Item slot 2", "Item slot 3", "Item slot 4" };
    int val;
    if (i < ED_HAIR) {
        r->label = color_labels[i];
        val = f->color[c][i];
        *about = "The costume's colors, four parts: each its own color, or one of 19 others as a cabinet's "
                 "customize screen gave them.";
    } else if (i == ED_HAIR) {
        r->label = "Hair color";
        val = f->hair[c];
        *about = "The hair: its own color, or one of 2 others.";
    } else if (i == ED_EYES) {
        r->label = "Eye color";
        val = f->eye[c];
        *about = "The eyes: their own color, or one of 3 others.";
    } else {
        r->label = slot_labels[i - ED_SLOT];
        val = f->equip[c][i - ED_SLOT];
        SDL_snprintf(v, n, val ? "Item %d" : "Empty", val);
        *about = "An item the character wears, by its number in the character's list (1 to 199), or none. A "
                 "number the character has no item for shows nothing. Hold Left or Right to go faster.";
        return;
    }
    SDL_snprintf(v, n, val ? "Color %d" : "Its own", val);
}

static void ed_tab_to(int t)
{
    ed_tab = (t % 3 + 3) % 3;
    ed_row = ed_top = 0;
}

static void ed_event(const SDL_Event *e)
{
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return;
    int k = e->type == SDL_EVENT_KEY_DOWN ? (int)e->key.key : -1;
    int g = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.button : -1;
    int face = g >= 0 ? ui_pad_face(e->gbutton.which, g) : UI_IN_NONE;
    int n = ed_rows();
    if (k == SDLK_DOWN || g == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
        ed_row = (ed_row + 1) % n;
    else if (k == SDLK_UP || g == SDL_GAMEPAD_BUTTON_DPAD_UP)
        ed_row = (ed_row + n - 1) % n;
    else if (k == SDLK_Q || k == SDLK_PAGEUP || g == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
        ed_tab_to(ed_tab - 1);
    else if (k == SDLK_E || k == SDLK_PAGEDOWN || k == SDLK_TAB || g == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
        ed_tab_to(ed_tab + 1);
    else if (k == SDLK_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
        ed_change(-1);
    else if (k == SDLK_RIGHT || g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
        ed_change(1);
    else if (k == SDLK_RETURN || k == SDLK_SPACE || face == UI_IN_OK)
        ed_press();
    else if (k == SDLK_BACKSPACE || face == UI_IN_DEFAULT) {
        ed_f = ed_f0;
        say("card editor: changes undone");
    } else if (k == SDLK_ESCAPE || face == UI_IN_BACK)
        ed_close(1);
}

/* the Cards tab's keys and buttons (its own: the list, then the bar) */
static void settings_tab(int d);
static void settings_close(void);

static void cp_event(const SDL_Event *e)
{
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return;
    int k = e->type == SDL_EVENT_KEY_DOWN ? (int)e->key.key : -1;
    int g = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.button : -1;
    int face = g >= 0 ? ui_pad_face(e->gbutton.which, g) : UI_IN_NONE;
    if ((k == SDLK_DOWN || g == SDL_GAMEPAD_BUTTON_DPAD_DOWN) && cp_n)
        st_row = (st_row + 1) % cp_n;
    else if ((k == SDLK_UP || g == SDL_GAMEPAD_BUTTON_DPAD_UP) && cp_n)
        st_row = (st_row + cp_n - 1) % cp_n;
    else if (k == SDLK_Q || k == SDLK_PAGEUP || g == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
        settings_tab(-1);
    else if (k == SDLK_E || k == SDLK_PAGEDOWN || k == SDLK_TAB || g == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
        settings_tab(1);
    else if (k == SDLK_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
        cp_bar_move(-1);
    else if (k == SDLK_RIGHT || g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
        cp_bar_move(1);
    else if (k == SDLK_RETURN || k == SDLK_SPACE || face == UI_IN_OK) {
        if (focus >= 0 && enabled(focus))
            press(focus);
    } else if (k == SDLK_DELETE || face == UI_IN_CLEAR)
        press(B_CP_DELETE);
    else if (k == SDLK_ESCAPE || face == UI_IN_BACK) {
        settings_close();
        return;
    }
    if (page == P_SETTINGS && cards_tab() && !pop)
        cp_focus_fix();
}

/* a note that takes a name: the typed text, Backspace, OK and Cancel */
static int text_pop(void)
{
    return pop == POP_RENAME || pop == POP_RINGNAME;
}

static void move_focus(int d);

static void text_event(const SDL_Event *e)
{
    size_t len = strlen(cp_text);
    if (e->type == SDL_EVENT_TEXT_INPUT && cp_fresh) {
        cp_text[0] = 0;
        len = 0;
    }
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        cp_fresh = 0;
        /* the card's name: up to 31 bytes of UTF-8, whole characters; the
         * ring name: ASCII's letters, numbers and marks, up to 20 */
        for (const unsigned char *s = (const unsigned char *)e->text.text; *s;) {
            size_t cl = *s < 0x80 ? 1 : (*s & 0xe0) == 0xc0 ? 2 : (*s & 0xf0) == 0xe0 ? 3 : (*s & 0xf8) == 0xf0 ? 4 : 1;
            int ok = pop == POP_RINGNAME ? *s >= 0x20 && *s < 0x7f && len < CARD_NAME_LEN
                                         : len + cl < sizeof cp_text && (*s >= 0x20 && *s != 0x7f);
            for (size_t i = 1; i < cl; i++)
                ok &= (s[i] & 0xc0) == 0x80;
            if (ok) {
                memcpy(cp_text + len, s, cl);
                len += cl;
                cp_text[len] = 0;
            }
            for (size_t i = 0; i < cl && *s; i++)
                s++;
        }
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return;
    int k = e->type == SDL_EVENT_KEY_DOWN ? (int)e->key.key : -1;
    int g = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.button : -1;
    int face = g >= 0 ? ui_pad_face(e->gbutton.which, g) : UI_IN_NONE;
    if (k == SDLK_BACKSPACE || face == UI_IN_CLEAR) {
        while (len && ((unsigned char)cp_text[len - 1] & 0xc0) == 0x80)
            len--;
        if (len)
            len--;
        cp_text[cp_fresh ? 0 : len] = 0;
        cp_fresh = 0;
    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || face == UI_IN_OK) {
        press(focus >= 0 ? focus : B_POP_YES);
    } else if (k == SDLK_ESCAPE || face == UI_IN_BACK) {
        press(B_POP_OK);
    } else if (k == SDLK_TAB || k == SDLK_LEFT || k == SDLK_RIGHT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT ||
               g == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
        move_focus(k == SDLK_LEFT || g == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1 : 1);
    }
}

static void settings_open(void)
{
    snprintf(settings_path, sizeof settings_path, "%s/user/settings.txt", game);
    settings_defaults(&st);
    settings_load(&st, settings_path, NULL);
    st_tab = st_row = st_top = 0;
    capturing = 0;
    cp_refresh(NULL);
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
    while (n < (int)SDL_arraysize(tab_rows[0]) && tab_rows[st_tab][n] >= 0)
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

static void settings_tab_to(int t)
{
    st_tab = t;
    st_row = st_top = 0;
    if (cards_tab()) {
        cp_refresh(NULL);
        focus = B_CP_P1;
        cp_focus_fix();
    }
}

static void settings_tab(int d)
{
    settings_tab_to(((st_tab + d) % T_COUNT + T_COUNT) % T_COUNT);
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
    if (cards_tab()) {
        cp_event(e);
        return;
    }
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
    char prog[1100];
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
        if (plain_wine && !wine_warned) {   /* what plain Wine may do, and Steam's way instead */
            pop_open(POP_WINE);
            break;
        }
#ifndef _WIN32
        if (!built_linux) {
            /* the Windows exe on Linux plays through Steam (Proton, Steam Input) */
            char script[1100];
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
        if (steam_started) {        /* closing Steam closes this launcher too */
            say("Add to Steam: Steam started the launcher, so it asks for Steam to be closed first");
            pop_open(POP_FROM_STEAM);
            break;
        }
        steam_proton = 0;
        art_made = 0;
        error_line[0] = 0;
        steam_step(K_STEAM_CHECK);
        break;
    }
    case B_POP_RETRY:
        pop = POP_NONE;
        focus = B_STEAM;
        press(B_STEAM);
        break;
    case B_POP_OK:
        if (text_pop())
            SDL_StopTextInput(win);
        focus = pop == POP_WINE ? B_PLAY : pop == POP_RENAME ? B_CP_RENAME : pop == POP_DELETE ? B_CP_DELETE
              : pop == POP_RINGNAME ? -1 : B_STEAM;
        pop = POP_NONE;
        break;
    case B_CP_P1:
    case B_CP_P2: cp_assign(b == B_CP_P2); break;
    case B_CP_EDIT: ed_open(); break;
    case B_CP_NEW: cp_new(); break;
    case B_CP_COPY: cp_copy(); break;
    case B_CP_RENAME:
        SDL_strlcpy(cp_text, cp_names[st_row], sizeof cp_text);
        pop_open(POP_RENAME);
        break;
    case B_CP_DELETE: pop_open(POP_DELETE); break;
    case B_POP_YES:
        if (pop == POP_RENAME)
            cp_rename();
        else if (pop == POP_DELETE)
            cp_delete();
        else if (pop == POP_RINGNAME)
            ed_name_set();
        break;
    case B_ED_SAVE: ed_close(1); break;
    case B_ED_CANCEL: ed_close(0); break;
    case B_POP_PLAY:
        pop = POP_NONE;
        wine_warned = 1;
        focus = B_PLAY;
        say("plain Wine: Play anyway");
        press(B_PLAY);
        break;
    case B_POP_STEAM:
        pop = POP_NONE;
        focus = B_STEAM;
        press(B_STEAM);
        break;
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
    if (page == P_SETTINGS || page == P_EDIT) {
        char t[96];
        snprintf(t, sizeof t, page == P_EDIT ? "Card: %s" : "Settings", ed_name);
        float w = ui_text_w(UI_MEDIUM, 15, t);
        ui_text(UI_MEDIUM, 15, LW - 40 - w, 36, UI_MUTED, t);
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
    if (!built_linux)
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

/* the Cards tab: the cards, the chosen one's details, the bar */
static void draw_cards(void)
{
    static ui_row rows[CP_MAX];
    for (int i = 0; i < cp_n; i++) {
        int side = cp_side(i);
        SDL_zero(rows[i]);
        rows[i].label = cp_names[i];
        rows[i].kind = UI_ROW_TEXT;
        rows[i].value = side == 0 ? "Player 1" : side == 1 ? "Player 2" : "";
        rows[i].warn = cp_bad[i] || (cp_has[i] && !cp_view[i].blank && !cp_editable(i));
    }
    float ry = 164, rh = 268;
    if (cp_n)
        ui_rows(40, ry, 540, rh, 42, rows, cp_n, st_row, &st_top);
    else
        ui_wrap(UI_REGULAR, 16, 56, ry + 24, 500, 24, UI_FAINT,
                "No cards yet. New makes one, ready to play: then choose its character and give it to a player.");
    /* the chosen card */
    static char text[600];
    const char *t = cp_n ? cp_names[st_row] : "Cards", *note = NULL;
    int i = st_row, side = cp_n ? cp_side(i) : -1;
    if (!cp_n) {
        SDL_strlcpy(text, "A card keeps a player's character, rank, record, items and colors, as a cabinet's "
                          "card did. Each one is a file in the game's user/cards folder.", sizeof text);
    } else {
        /* the game menu's Cards tab shows the same (card_view_text), less what the launcher adds */
        int kind = card_view_text(&cp_view[i], !cp_has[i] ? 0 : cp_bad[i] ? -1 : 1, text, sizeof text);
        if (kind == CARD_TEXT_NEW)
            SDL_strlcat(text, " Or Edit makes it a ready card now.", sizeof text);
        else if (kind == CARD_TEXT_OLD)
            SDL_strlcat(text, " The launcher does not edit it.", sizeof text);
    }
    if (!st.cards)
        note = "Cards are off (the Game tab): the game uses none.";
    else if (side >= 0)
        note = side == 0 ? "Player 1's card (press Player 1 again for none)." : "Player 2's card (press Player 2 again for none).";
    else if (cp_n)
        note = "Player 1 or Player 2 gives it to that player.";
    ui_about(604, ry, LW - 40 - 604, rh, t, text, note);
    /* the bar: Player 1 and Player 2 lit for the card's player */
    static const char *const labels[] = { "Player 1", "Player 2", "Edit", "New", "Copy", "Rename", "Delete" };
    float x = 40;
    for (int b = 0; b < (int)SDL_arraysize(labels); b++) {
        float w = SDL_max(ui_text_w(UI_MEDIUM, 16, labels[b]) + 36, 84);
        int lit = (b == 0 && side == 0) || (b == 1 && side == 1);
        button(cp_bar[b], x, 452, w, 44, labels[b], lit, -1);
        x += w + (b == 1 ? 26 : 12);
    }
}

/* the card editor's page */
static void draw_edit(void)
{
    ui_tabs(40, 92, LW - 80, 42, ed_tab_names, 3, ed_tab);
    ui_fill(40, 148, LW - 80, 1, UI_LINE);
    ui_row rows[ED_COSTUME_ROWS];
    static char vals[ED_COSTUME_ROWS][64];
    const char *abouts[ED_COSTUME_ROWS];
    int n = ed_rows();
    for (int i = 0; i < n; i++)
        ed_row_of(i, &rows[i], vals[i], sizeof vals[i], &abouts[i]);
    float ry = 164, rh = 336;
    ui_rows(40, ry, 540, rh, 42, rows, n, ed_row, &ed_top);
    ui_about(604, ry, LW - 40 - 604, rh, rows[ed_row].label, abouts[ed_row],
             ed_new ? "Save makes it a ready card." : memcmp(&ed_f, &ed_f0, sizeof ed_f) ? "Changed: Save keeps it." : NULL);
    static const ui_hint hints[] = {
        { "Choose", { UI_IN_UPDOWN, UI_IN_NONE } }, { "Change", { UI_IN_LEFTRIGHT, UI_IN_NONE } },
        { "Tabs", { UI_IN_PREV, UI_IN_NEXT } }, { "Undo", { UI_IN_DEFAULT, UI_IN_NONE } },
    };
    ui_fill(40, 516, LW - 80, 1, UI_LINE);
    ui_hints(40, 558, 24, hints, 4, 0);
    static const ui_hint save_hint[] = { { "Save", { UI_IN_BACK, UI_IN_NONE } } };
    float w = button_w("Save"), cw = button_w("Cancel");
    button(B_ED_SAVE, LW - 40 - w, 536, w, 44, "Save", 1, -1);
    button(B_ED_CANCEL, LW - 40 - w - 12 - cw, 536, cw, 44, "Cancel", 0, -1);
    ui_hints(LW - 40 - w - 12 - cw - 24, 558, 24, save_hint, 1, 2);
}

static void draw_settings(void)
{
    {
        /* Window size's Auto and the sizes that fit: this screen's usable
         * area in pixels, less a window's frame (the launcher's own: the
         * game's is the same kind), as the game counts it (main.c
         * screen_room). The game's window opens on this screen too, as a
         * rule */
        SDL_Rect r;
        SDL_DisplayID d = SDL_GetDisplayForWindow(win);
        window_room room = { 0, 0, SDL_GetWindowPixelDensity(win) };
        int top = 0, left = 0, bottom = 0, right = 0;
        if (room.density <= 0)
            room.density = 1;
        if (!SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right))
            top = left = bottom = right = 0;
        if (d && SDL_GetDisplayUsableBounds(d, &r)) {
            room.w = (int)((r.w - left - right) * room.density + 0.5f);
            room.h = (int)((r.h - top - bottom) * room.density + 0.5f);
        }
        options_screen(&room);
    }
    ui_tabs(40, 92, LW - 80, 42, tab_names, T_COUNT, st_tab);
    ui_fill(40, 148, LW - 80, 1, UI_LINE);
    if (cards_tab()) {
        draw_cards();
        static const ui_hint cp_hints[] = {
            { "Choose", { UI_IN_UPDOWN, UI_IN_NONE } }, { "Buttons", { UI_IN_LEFTRIGHT, UI_IN_NONE } },
            { "Tabs", { UI_IN_PREV, UI_IN_NEXT } },
        };
        ui_fill(40, 516, LW - 80, 1, UI_LINE);
        ui_hints(40, 558, 24, cp_hints, 3, 0);
        static const ui_hint done_hint[] = { { "Save", { UI_IN_BACK, UI_IN_NONE } } };
        float w = button_w("Done");
        button(B_DONE, LW - 40 - w, 536, w, 44, "Done", 1, -1);
        ui_hints(LW - 40 - w - 24, 558, 24, done_hint, 1, 2);
        return;
    }
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

/* Play's note under plain Wine (docs/design.md section 34). Paragraphs are
 * lines; one that starts with a bullet hangs under its first word. */
#define BULLET "\xE2\x80\xA2 "
static const char WINE_NOTE[] =
    "This launcher runs under plain Wine, not Steam's Proton. The game is made and tested for Proton, so "
    "here:\n"
    BULLET "Controllers go through Wine, not Steam Input: no Steam controller layouts or overlay, and a pad "
    "Wine does not know may not work.\n"
    BULLET "The picture and sound depend on your Wine version and drivers: they may stutter, glitch or not "
    "start at all.\n"
    BULLET "This launcher's window can stop updating after it is resized (Wine's Direct3D 12). Closing and "
    "opening it again fixes it.\n"
    "For the supported way, choose Add to Steam, then play it from your Steam library.";

/* a note's text, paragraph by paragraph: its height, drawn when draw is set */
static float pop_text(float x, float y, float w, const char *text, int draw)
{
    const float line_h = 24, gap = 10, hang = 16;
    float h = 0;
    char para[600];
    while (*text) {
        const char *e = SDL_strchr(text, '\n');
        size_t n = e ? (size_t)(e - text) : SDL_strlen(text);
        SDL_strlcpy(para, text, SDL_min(n + 1, sizeof para));
        int bullet = !SDL_strncmp(para, BULLET, sizeof BULLET - 1);
        const char *body = bullet ? para + sizeof BULLET - 1 : para;
        float bx = bullet ? x + hang : x, bw = bullet ? w - hang : w;
        if (h > 0)
            h += gap;
        if (draw) {
            if (bullet)
                ui_text(UI_REGULAR, 16, x + 2, y + h, UI_MUTED, "\xE2\x80\xA2");
            ui_wrap(UI_REGULAR, 16, bx, y + h, bw, line_h, UI_MUTED, body);
        }
        h += line_h * ui_wrap_lines(UI_REGULAR, 16, bw, body);
        text += n;
        if (*text == '\n')
            text++;
    }
    return h;
}

static void pop_open(int kind)
{
    say("%s", kind == POP_FROM_STEAM    ? "Add to Steam: the note to close Steam and open the launcher again"
              : kind == POP_CLOSE_STEAM ? "Add to Steam: the note to close Steam (Try again)"
              : kind == POP_WINE        ? "Play: the note on plain Wine (Add to Steam, Play anyway, Cancel)"
              : kind == POP_RENAME      ? "Cards: Rename"
              : kind == POP_DELETE      ? "Cards: Delete (it asks first)"
              : kind == POP_RINGNAME    ? "card editor: the ring name"
                                        : "Add to Steam: the note that it was added");
    pop = kind;
    focus = kind == POP_CLOSE_STEAM ? B_POP_RETRY : kind == POP_WINE ? B_POP_STEAM
          : kind == POP_RENAME || kind == POP_RINGNAME ? B_POP_YES : B_POP_OK;
    if (text_pop()) {
        cp_error[0] = 0;
        cp_fresh = 1;
        SDL_StartTextInput(win);
    }
}

/* Add to Steam's note, over the page: the page dimmed, a card with the
 * note's buttons (the only ones that answer while it is up) */
static void draw_pop(void)
{
    if (!pop)
        return;
    const char *t = "", *text = "";
    switch (pop) {
    case POP_FROM_STEAM:
        t = "Close Steam first";
        text = "Steam takes a new shortcut only while it is closed, and Steam started this launcher. Exit "
               "Steam (in Steam's menu: Steam, Exit); this window closes with it. Then open vf4ft-launcher.exe "
               "again from its folder, with Steam closed, and choose Add to Steam.";
        break;
    case POP_CLOSE_STEAM:
        t = "Close Steam first";
        text = "Steam takes a new shortcut only while it is closed. Exit Steam (in Steam's menu: Steam, Exit), "
               "then choose Try again.";
        break;
    case POP_ADDED: {
        static char added[300];
        t = "Added to Steam";
        snprintf(added, sizeof added, "Start Steam: Virtua Fighter 4 Final Tuned is in your library%s%s.%s",
                 art_made ? " with its pictures" : "", steam_proton ? ", set to run with Proton" : "",
                 art_made ? "" : " Its pictures could not be made: launcher.log says why.");
        text = added;
        break;
    }
    case POP_WINE:      /* docs/design.md section 34: what was seen under plain Wine */
        t = "Playing under plain Wine";
        text = WINE_NOTE;
        break;
    case POP_RENAME:
        t = "Rename the card";
        text = "Its new name, which is also its file's in user/cards (up to 31 characters).";
        break;
    case POP_RINGNAME:
        t = "Ring name";
        text = "The name the game shows for this player: up to 20 letters (A to Z, without accents), numbers, "
               "spaces and marks.";
        break;
    case POP_DELETE: {
        static char title[96], why[400];
        int side = cp_side(st_row);
        snprintf(title, sizeof title, "Delete %s?", st_row < cp_n ? cp_names[st_row] : "");
        snprintf(why, sizeof why, "%s%s%s",
                 st_row < cp_n && cp_has[st_row]
                     ? "The card's file moves to user/cards/Deleted, with the time in its name. To have it back, "
                       "move it into user/cards again."
                     : "This card is not made yet, so there is no file to move.",
                 side >= 0 ? "\n" : "", side == 0 ? "Player 1 has no card after this." : side == 1 ? "Player 2 has no card after this." : "");
        t = title;
        text = why;
        break;
    }
    }
    nhits = 0;                      /* the page's buttons do not answer under the note */
    SDL_SetRenderDrawColor(ren, 6, 7, 10, 210);
    SDL_RenderFillRect(ren, NULL);
    int typing = text_pop();
    float w = pop == POP_WINE ? 760 : 620, tw = w - 72;
    float text_h = pop_text(0, 0, tw, text, 0);
    float field_h = typing ? 24 + 48 + 12 + 22 + 22 : 0;     /* the field, the error line, the keyboard's line */
    float h = 88 + text_h + field_h + 28 + BTN_H + 36;
    float x0 = (LW - w) / 2, y0 = (LH - h) / 2;
    ui_round_line(x0, y0, w, h, 16, 1, UI_LINE, UI_CARD);
    ui_text(UI_BOLD, 22, x0 + 36, y0 + 48, UI_TEXT, t);
    pop_text(x0 + 36, y0 + 88, tw, text, 1);
    if (typing) {
        /* the name so far, with a caret, then why it cannot be that (Rename) */
        float fy = y0 + 88 + text_h + 24 - 12;
        ui_round_line(x0 + 36, fy, tw, 48, 10, 2, UI_ACCENT, UI_BG);
        if (cp_fresh && cp_text[0])     /* chosen: typing replaces it */
            ui_round(x0 + 48, fy + 10, ui_text_w(UI_MEDIUM, 18, cp_text) + 8, 28, 5, UI_HOVER);
        float cw = ui_text(UI_MEDIUM, 18, x0 + 52, fy + 24, UI_TEXT, cp_text);
        if (SDL_GetTicks() / 530 % 2 == 0 || shot_mode)
            ui_fill(x0 + 52 + cw + 2, fy + 12, 2, 24, UI_ACCENT);
        if (pop == POP_RENAME)
            cp_text_ok();
        if (cp_error[0])
            ui_text_in(UI_REGULAR, 14.5f, x0 + 36, fy + 48 + 22, tw, 0, 0, UI_BAD, cp_error);
        ui_text_in(UI_REGULAR, 14, x0 + 36, fy + 48 + 22 + 24, tw, 0, 0, UI_FAINT,
                   "On a Steam Deck, Steam + X shows the keyboard.");
    }
    float by = y0 + h - 36 - BTN_H, bx = x0 + w - 36;
    if (pop == POP_RENAME || pop == POP_DELETE || pop == POP_RINGNAME) {
        const char *yes = pop == POP_RENAME ? "Rename" : pop == POP_DELETE ? "Delete" : "OK";
        bx -= button_w("Cancel");
        button(B_POP_OK, bx, by, 0, BTN_H, "Cancel", 0, -1);
        bx -= 14 + button_w(yes);
        button(B_POP_YES, bx, by, 0, BTN_H, yes, pop == POP_DELETE ? 0 : 1, -1);
    } else if (pop == POP_CLOSE_STEAM) {
        bx -= button_w("Close");
        button(B_POP_OK, bx, by, 0, BTN_H, "Close", 0, -1);
        bx -= 14 + button_w("Try again");
        button(B_POP_RETRY, bx, by, 0, BTN_H, "Try again", 1, -1);
    } else if (pop == POP_WINE) {
        bx -= button_w("Cancel");
        button(B_POP_OK, bx, by, 0, BTN_H, "Cancel", 0, -1);
        bx -= 14 + button_w("Play anyway");
        button(B_POP_PLAY, bx, by, 0, BTN_H, "Play anyway", 0, -1);
        bx -= 14 + button_w("Add to Steam");
        button(B_POP_STEAM, bx, by, 0, BTN_H, "Add to Steam", 1, -1);
    } else {
        bx -= button_w("OK");
        button(B_POP_OK, bx, by, 0, BTN_H, "OK", 1, -1);
    }
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
    case P_EDIT: draw_edit(); break;
    }
    draw_pop();
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
        if (pop)
            press(B_POP_OK);
        else if (page == P_BUILD || page == P_FAIL)
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
    if (page == P_SETTINGS && !capturing && !pop && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int t = ui_tabs_hit(x, y), r = ui_rows_hit(x, y);
        if (t == UI_TAB_PREV || t == UI_TAB_NEXT)
            settings_tab(t == UI_TAB_PREV ? -1 : 1);
        else if (t >= 0) {
            settings_tab_to(t);
        } else if (r >= 0 && cards_tab()) {
            st_row = r;             /* a card is only chosen: the bar acts on it */
            cp_focus_fix();
        } else if (r >= 0) {
            st_row = r;
            settings_press(r);
        }
    }
    if (page == P_EDIT && !pop && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int t = ui_tabs_hit(x, y), r = ui_rows_hit(x, y);
        if (t == UI_TAB_PREV || t == UI_TAB_NEXT)
            ed_tab_to(ed_tab + (t == UI_TAB_PREV ? -1 : 1));
        else if (t >= 0)
            ed_tab_to(t);
        else if (r >= 0) {
            /* a choice's left half goes back, its right half on (ui_rows: the
             * value's box ends 0.35 rows in from the right) */
            float right = 40 + 540 - 42 * 0.35f, bw = SDL_min(540 * 0.42f, 42 * 5.6f);
            int was = ed_row;
            ed_row = r;
            if (ed_tab == 0 && r == ED_NAME)
                ed_press();
            else if (x >= right - bw && x <= right + 4)
                ed_change(x < right - bw / 2 ? -1 : 1);
            else if (r == was)
                ed_change(1);
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

/* an event to the page (or the note over it) */
static void route_event(SDL_Event *e)
{
    if (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        mouse_event(e);
    else if (pop && text_pop())
        text_event(e);
    else if (pop)
        page_event(e);
    else if (page == P_SETTINGS)
        settings_event(e);
    else if (page == P_EDIT)
        ed_event(e);
    else
        page_event(e);
}

/* --shot-keys: keys (SDL's names, as "Down" or "Return"), text:WORDS (typed;
 * "_" a space) and click:X,Y (the page's units), one after another, each
 * after a frame is drawn, before the picture */
static void shot_keys_run(const char *list)
{
    static char buf[2048];
    SDL_strlcpy(buf, list, sizeof buf);
    char *save = NULL;
    for (char *tok = SDL_strtok_r(buf, " ", &save); tok; tok = SDL_strtok_r(NULL, " ", &save)) {
        draw();
        SDL_Event e;
        SDL_zero(e);
        float cx, cy;
        if (!SDL_strncmp(tok, "text:", 5)) {
            for (char *c = tok + 5; *c; c++)
                if (*c == '_')
                    *c = ' ';
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.text = tok + 5;
            say("--shot-keys: typing \"%s\"", tok + 5);
            route_event(&e);
        } else if (SDL_sscanf(tok, "click:%f,%f", &cx, &cy) == 2) {
            /* the page's units to the window's (ui_fit's scale), as a click comes */
            say("--shot-keys: click at %.0f, %.0f", cx, cy);
            float s = ui_get_scale(), px = 0, py = 0, lx = 0, ly = 0;
            ui_to_logical(0, 0, &px, &py);      /* the page's origin: -px*s, -py*s */
            lx = (cx - px) * s;
            ly = (cy - py) * s;
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.x = lx;
            e.button.y = ly;
            e.button.windowID = SDL_GetWindowID(win);
            mouse_event(&e);
        } else {
            SDL_Keycode k = SDL_GetKeyFromName(tok);
            if (k == SDLK_UNKNOWN) {
                say("--shot-keys: no key %s (ignored)", tok);
                continue;
            }
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = k;
            e.key.down = true;
            route_event(&e);
        }
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
               *shot_device = NULL, *shot_keys = NULL;
    int shot_scale = 1;
    const char *unknown[8];
    int opt_exit = 0, opt_show = 0, nunknown = 0;
    double opt_cancel = -1;
#ifdef _WIN32
    if (argc > 0 && argv[0])
        argv0 = argv[0];
#endif
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
        else if (!strcmp(argv[i], "--shot-keys") && i + 1 < argc) shot_keys = argv[++i];
        else if (nunknown < 8) unknown[nunknown++] = argv[i];
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    ui_pad_logger(say_line);    /* which pad is left out: Steam Input's twins (ui.h) */
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
            {"cards", P_SETTINGS, T_CARDS},
        };
        shot_mode = 1;
        for (size_t i = 0; i < SDL_arraysize(pages); i++)
            if (!strcmp(shot_page, pages[i].name)) {
                if (pages[i].page == P_SETTINGS)
                    settings_open();
                go(pages[i].page);
                settings_tab_to(pages[i].tab);
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
        /* the Play page under its notes: steam-started, steam-close, steam-added,
         * steam-added-plain (no pictures), wine-play */
        static const struct { const char *name; int kind; } pops[] = {
            {"steam-started", POP_FROM_STEAM}, {"steam-close", POP_CLOSE_STEAM}, {"steam-added", POP_ADDED},
            {"steam-added-plain", POP_ADDED}, {"wine-play", POP_WINE},
        };
        for (size_t i = 0; i < SDL_arraysize(pops); i++)
            if (!strcmp(shot_page, pops[i].name)) {
                go(P_PLAY);
                steam_proton = 1;
                art_made = strcmp(shot_page, "steam-added-plain") != 0;
                pop_open(pops[i].kind);
            }
        /* the Play page with Add to Steam's lines: steam-art (the pictures being
         * made), steam-done, steam-done-plain (without them) */
        static const struct { const char *name; int kind; const char *text; } lines[] = {
            {"steam-art", ST_WORKING, ART_WORKING}, {"steam-done", ST_GOOD, ADDED_ART},
            {"steam-done-plain", ST_GOOD, ADDED_NO_ART},
        };
        for (size_t i = 0; i < SDL_arraysize(lines); i++)
            if (!strcmp(shot_page, lines[i].name)) {
                go(P_PLAY);
                status_kind = lines[i].kind;
                SDL_strlcpy(status_text, lines[i].text, sizeof status_text);
            }
        if (shot_device && ui_device_named(shot_device) >= 0)
            ui_set_device(ui_device_named(shot_device));    /* the caps' style (ui.h) */
        if (shot_keys)
            shot_keys_run(shot_keys);
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
            wine_warned = 1;        /* no plain Wine note: the switch was turned on from the Play page */
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
            if (ui_pad_event_hidden(&e))
                continue;       /* the real pad behind a Steam Input pad: its presses are that pad's too */
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
            else
                route_event(&e);
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
        if (game_close) {
            running = 0;          /* the game quit: the launcher goes with it */
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
