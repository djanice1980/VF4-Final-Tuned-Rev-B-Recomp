/* The game's side of updates: see updates.h. */
#include "updates.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define LAUNCHER_EXE "vf4ft-launcher.exe"
#else
#define LAUNCHER_EXE "vf4ft-launcher"
#endif

static int state = GU_OFF, by_launcher;
static char rel[1024];                      /* the release folder, a separator at its end */
static char python[1100], script[1100], launcher[1100], marker[1100];
static char version[64], newer[64], error[300];
static long long size;
static SDL_Process *proc;                   /* update.py check */
static SDL_IOStream *out;
static char part[512];
static size_t npart;
static void (*say)(const char *line);

static void sayf(const char *fmt, ...)
{
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (say)
        say(b);
}

static int exists(const char *p)
{
    return SDL_GetPathInfo(p, NULL);
}

/* a string field of a small JSON file: "key": "value" */
static int json_str(const char *s, const char *key, char *dst, size_t n)
{
    char k[64];
    SDL_snprintf(k, sizeof k, "\"%s\"", key);
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
    SDL_snprintf(dst, n, "%.*s", (int)SDL_min(m, n - 1), p);
    return 1;
}

void gu_init(const char *exe_dir, void (*log)(const char *line))
{
    say = log;
    state = GU_OFF;
    const char *env = SDL_getenv("VF4FT_LAUNCHER");
    by_launcher = env && !strcmp(env, "1");
    /* the release folder: the game's folder's parent */
    SDL_strlcpy(rel, exe_dir ? exe_dir : "", sizeof rel);
    size_t n = strlen(rel);
    if (n > 1 && (rel[n - 1] == '/' || rel[n - 1] == '\\'))
        rel[--n] = 0;
    char *sl = strrchr(rel, '/'), *bs = strrchr(rel, '\\');
    if (bs > sl)
        sl = bs;
    if (!sl)
        return;
    sl[1] = 0;
    char p[1100];
    SDL_snprintf(p, sizeof p, "%srelease.json", rel);
    char *s = SDL_LoadFile(p, NULL);
    int ok = s && json_str(s, "version", version, sizeof version);
    SDL_free(s);
    if (!ok)
        return;         /* a development build, or a game folder that is not a release's game/ */
#ifdef _WIN32
    SDL_snprintf(python, sizeof python, "%spython\\pythonw.exe", rel);
#else
    SDL_strlcpy(python, "python3", sizeof python);
#endif
    SDL_snprintf(script, sizeof script, "%sbuilder/tools/update.py", rel);
    SDL_snprintf(launcher, sizeof launcher, "%s%s", rel, LAUNCHER_EXE);
    SDL_snprintf(marker, sizeof marker, "%supdate/at-game-quit", rel);
#ifdef _WIN32
    if (!exists(python)) {
        sayf("updates: no %s, so no updates from the menu", python);
        return;
    }
#endif
    if (!exists(script) || !exists(launcher)) {
        sayf("updates: no %s or %s, so no updates from the menu", script, launcher);
        return;
    }
    state = GU_IDLE;
    s = SDL_LoadFile(marker, NULL);
    if (s && json_str(s, "version", newer, sizeof newer)) {
        state = GU_ASKED;
        sayf("updates: release %s; %s goes in when the game quits (asked before)", version, newer);
    } else {
        sayf("updates: release %s in %s%s", version, rel, by_launcher ? "; the launcher started the game" : "");
    }
    SDL_free(s);
}

int gu_state(void) { return state; }
const char *gu_version(void) { return version; }
const char *gu_newer(void) { return newer; }
long long gu_size(void) { return size; }
const char *gu_error(void) { return error; }
int gu_by_launcher(void) { return by_launcher; }

static void check(void)
{
    if (proc)
        return;
    char dir[1024];
    SDL_strlcpy(dir, rel, sizeof dir);
    size_t n = strlen(dir);
    if (n > 1)
        dir[n - 1] = 0;     /* no separator before a closing quote on Windows' command line */
    const char *args[] = {python, "-I", script, "check", dir, NULL};
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    proc = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    error[0] = 0;
    if (!proc) {
        SDL_snprintf(error, sizeof error, "cannot start %.120s: %.150s", python, SDL_GetError());
        sayf("updates: %s", error);
        state = GU_FAILED;
        return;
    }
    out = SDL_GetProcessOutput(proc);
    npart = 0;
    state = GU_CHECKING;
    sayf("updates: checking");
}

static void line(const char *s)
{
    char v[64];
    long long sz = 0;
    if (!strcmp(s, "@update none")) {
        state = GU_NONE;
        sayf("updates: %s is the newest release", version);
    } else if (sscanf(s, "@update %63s %lld", v, &sz) >= 1) {
        SDL_strlcpy(newer, v, sizeof newer);
        size = sz;
        state = GU_AVAILABLE;
        sayf("updates: %s is out (this is %s)", v, version);
    } else if (!strncmp(s, "error: ", 7)) {
        SDL_strlcpy(error, s + 7, sizeof error);
        sayf("updates: %s", s + 7);
    }
}

int gu_poll(void)
{
    if (!proc)
        return 0;
    int was = state;
    char b[512];
    for (;;) {
        size_t n = out ? SDL_ReadIO(out, b, sizeof b) : 0;
        if (n == 0)
            break;
        for (size_t i = 0; i < n; i++) {
            if (b[i] == '\n' || npart == sizeof part - 1) {
                part[npart] = 0;
                if (npart && part[npart - 1] == '\r')
                    part[npart - 1] = 0;
                if (part[0])
                    line(part);
                npart = 0;
            } else {
                part[npart++] = b[i];
            }
        }
    }
    int code = 0;
    if (SDL_WaitProcess(proc, false, &code)) {
        if (npart) {
            part[npart] = 0;
            line(part);
            npart = 0;
        }
        SDL_DestroyProcess(proc);
        proc = NULL;
        out = NULL;
        if (state == GU_CHECKING) {
            state = GU_FAILED;
            if (!error[0])
                SDL_snprintf(error, sizeof error, "the check ended with code %d", code);
        }
    }
    return state != was;
}

void gu_press(void)
{
    switch (state) {
    case GU_IDLE:
    case GU_NONE:
    case GU_FAILED:
        check();
        break;
    case GU_AVAILABLE: {
        char d[1100];
        SDL_snprintf(d, sizeof d, "%supdate", rel);
        SDL_CreateDirectory(d);
        char j[160];
        SDL_snprintf(j, sizeof j, "{\n  \"version\": \"%s\"\n}\n", newer);
        if (!SDL_SaveFile(marker, j, strlen(j))) {
            SDL_snprintf(error, sizeof error, "cannot write %.200s: %.80s", marker, SDL_GetError());
            sayf("updates: %s", error);
            state = GU_FAILED;
            break;
        }
        state = GU_ASKED;
        sayf("updates: %s goes in when the game quits", newer);
        break;
    }
    case GU_ASKED:
        SDL_RemovePath(marker);
        state = GU_AVAILABLE;
        sayf("updates: not when the game quits after all");
        break;
    }
}

void gu_quit(void)
{
    if (proc) {
        SDL_KillProcess(proc, true);
        SDL_WaitProcess(proc, true, NULL);
        SDL_DestroyProcess(proc);
        proc = NULL;
    }
    if (state != GU_ASKED)
        return;
    if (by_launcher) {
        sayf("updates: the launcher puts %s in when it sees the game end", newer);
        return;
    }
    /* the game started on its own: the launcher, in the background, does the
     * update and builds the game again, then closes */
    const char *args[] = {launcher, "--after-game", NULL};
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (!p) {
        sayf("updates: cannot start %s (%s): %s goes in at the launcher's next start", launcher, SDL_GetError(),
             newer);
        return;
    }
    SDL_DestroyProcess(p);      /* it runs on */
    sayf("updates: started %s --after-game for %s", launcher, newer);
}
