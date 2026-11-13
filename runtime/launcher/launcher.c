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
 * For testing: --game DIR (another install folder), --set DIR, --do ACTION
 * (build, update, play or steam: pressed at the start) and --exit (quit when
 * that action's process ends, with its result as the exit code).
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>

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
        SDL_DestroyProcess(p);        /* the game runs on by itself */
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

static void SDLCALL on_folder(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0]) {
        SDL_strlcpy(picked_path, files[0], sizeof picked_path);
        SDL_SetAtomicInt(&picked, 1);
    }
}

/* ---- the buttons ---- */

enum { B_SET, B_BUILD, B_UPDATE, B_PLAY, B_STEAM, B_TARGET, B_COUNT };
static const char *const names[B_COUNT] = {"Choose set", "Build", "Update", "Play", "Add to Steam", "Target"};
static SDL_FRect rects[B_COUNT];
static int focus = B_SET;

static int enabled(int b)
{
    switch (b) {
    case B_BUILD: return set_path[0] && root[0] && !proc;
    case B_UPDATE: return built && root[0] && !proc;
    case B_PLAY: return built;
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
    }
}

static void text(float x, float y, const char *s, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderDebugText(ren, x, y, s);
}

static void draw(void)
{
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
    const char *opt_game = NULL, *opt_set = NULL, *opt_do = NULL;
    int opt_exit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) opt_game = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) opt_set = argv[++i];
        else if (!strcmp(argv[i], "--do") && i + 1 < argc) opt_do = argv[++i];
        else if (!strcmp(argv[i], "--exit")) opt_exit = 1;
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
    say(built ? "Play, or Update after a new release." : "Choose your MAME set's folder, then Build.");
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
    int running = 1, result = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                running = 0;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                SDL_OpenGamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
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
