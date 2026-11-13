/* vf4ft: the game in a window (milestone 3, docs/design.md section 14).
 *
 * The machine (game_machine.c) runs on its own thread in virtual time; once
 * per vertical blank it hands this side the newest picture, takes the
 * controls as they are, and waits until the frame is due (60 a second).
 * This thread runs SDL: events, keyboard and gamepads (Steam Input arrives
 * as an SDL gamepad), and the picture through SDL_GPU, scaled to the window
 * at 4:3. Everything it has to say goes to vf4ft.log beside the executable,
 * so a run under Proton can be checked afterwards.
 *
 * Options:
 *   --image FILE      the program image (main.bin; or $VF4_IMAGE). By default the program comes out of
 *                     the DIMM image when that is the disc's file, else data/main.bin beside the executable
 *   --dimm FILE       the DIMM image (or $VF4_DIMM): the disc's encrypted file (a name ending in .BIN, any
 *                     case: VF41_S.BIN, decrypted with dimm.key beside it, as the builder installs it), or
 *                     a decrypted image (development). Default: data/VF41_S.BIN beside the executable if
 *                     it is there, else data/vf41_s.dec
 *   --frames N        quit after N vertical blanks (0 = until the window closes)
 *   --unpaced         no waiting: the machine runs as fast as it can (for measuring)
 *   --real-boot       the game's first start-up step (WARNING.INIT, about 97 s) at its real pace too;
 *                     by default it runs as fast as it can
 *   --fullscreen      start in fullscreen (F11 or Alt+Enter toggles)
 *   --silent          no sound chip: VF4debug's mailbox model in its place (as M3)
 *   --gpu NAME        SDL_GPU driver: direct3d12 (or d3d12) or vulkan (default: SDL's choice)
 *   --log FILE        log somewhere else
 *   --press LIST      for tests: buttons pressed for 6 vertical blanks at given ones, as
 *                     "6500:p1:start,6800:p1:p+g" (p1 or p2; up down left right p k g start coin, and
 *                     the cabinet's test and service with either)
 *   --shot N FILE     for tests: the machine waits at vertical blank N while the newest picture, as
 *                     shown (before scaling to the window), goes to a BMP file; up to 8 of them
 *   --user DIR        the player's folder (default: user/ beside the executable): settings.txt,
 *                     backup.bin (the cabinet's battery-backed RAM and settings EEPROM), cards/
 *
 * Settings (M6): user/settings.txt (settings.h), written by the launcher's
 * Settings page: free play or coins, fullscreen, picture, sound, the card
 * units and every control. The defaults:
 *   keyboard (player 1): arrows or WASD, J guard, K punch, L kick, U throw
 *     (P+G), I P+K, O P+K+G, Enter start, 5 coin, F2 test, F3 service
 *   gamepads (first is player 1, second player 2): left stick or D-pad,
 *     X/West guard, Y/North punch, B/East kick, A/South throw (P+G), LB P+K,
 *     RB P+K+G, Menu start, View coin
 *   always: 6 coin for slot 2
 * Test mode: Test starts the game's test program (runtime/native/rt_native.c),
 * where Service moves the cursor and Test chooses; its EXIT starts the game
 * again. Nothing in it is changed from the board's.
 * Quit: Esc, closing the window, or holding View + Menu on a controller.
 *
 * The backup is written every 10 seconds when it has changed, and when the
 * game ends; a file that is there and not a backup is left alone (the run
 * starts blank and does not write over it).
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_machine.h"
#include "settings.h"
#include "gpu_render.h"

#define VF4FT_TITLE "Virtua Fighter 4 Final Tuned"
#ifndef VF4FT_VERSION
#define VF4FT_VERSION "development build"   /* the builder (tools/make_game.py) gives its version */
#endif
#define PIC_W GAME_PICTURE_W
#define PIC_H GAME_PICTURE_H
#define FRAME_NS (1000000000ull / 60)   /* the machine's vertical blank (runtime/machine/machine.h) */

static FILE *log_file;

static void logf_(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    Uint64 ms = SDL_GetTicks();
    fprintf(stdout, "[%7.3f] %s\n", ms / 1000.0, line);
    fflush(stdout);
    if (log_file && log_file != stdout) {
        fprintf(log_file, "[%7.3f] %s\n", ms / 1000.0, line);
        fflush(log_file);
    }
}

/* SDL's own warnings (why a GPU driver was refused, device trouble) go to
 * the same log: under Proton nobody sees stderr. */
static void sdl_log(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    static const char *const pri[] = { "?", "trace", "verbose", "debug", "info", "warn", "error", "critical" };
    logf_("SDL %s (cat %d): %s", priority < (int)SDL_arraysize(pri) ? pri[priority] : "?", category, message);
}

/* ---- what the two threads share ---- */

static struct {
    SDL_Mutex *lock;
    unsigned char pic[PIC_W * PIC_H * 3];       /* the newest picture, RGB */
    unsigned pic_frame;                         /* its vertical blank; 0 = none yet */
    game_input in;                              /* the controls, as this thread last read them */
} shared;

static const char *image_path, *dimm_path, *dimm_key;
static int unpaced, real_boot, silent;
static settings cfg_;                   /* user/settings.txt over the defaults */
static char user_dir[4096];             /* ends in a separator */
static char backup_path[4200];
static int backup_ok = 1;               /* 0: the file there is not a backup; never written over */
#define BACKUP_EVERY 600                /* vertical blanks: 10 seconds */

/* --shot: pictures for tests */
static struct { unsigned frame; const char *path; int done; } shots[8];
static int nshots;
static SDL_AtomicInt shot_pause;        /* 1 + the shot the machine waits for at its vertical blank */

/* the picture in tex (w x h, RGBA) to a BMP file: waits for the GPU */
static void shot_save(SDL_GPUDevice *gpu, SDL_GPUTexture *tex, Uint32 w, Uint32 h, const char *path)
{
    SDL_GPUTransferBufferCreateInfo ti;
    SDL_zero(ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = w * h * 4;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu, &ti);
    SDL_GPUCommandBuffer *cmd = tb ? SDL_AcquireGPUCommandBuffer(gpu) : NULL;
    if (!cmd) {
        logf_("--shot %s: %s", path, SDL_GetError());
        return;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg = { .texture = tex, .w = w, .h = h, .d = 1 };
    SDL_GPUTextureTransferInfo dst = { .transfer_buffer = tb, .offset = 0 };
    SDL_DownloadFromGPUTexture(cp, &reg, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(gpu, true, &fence, 1);
        SDL_ReleaseGPUFence(gpu, fence);
    }
    void *px = SDL_MapGPUTransferBuffer(gpu, tb, false);
    SDL_Surface *sf = px ? SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, px, (int)w * 4) : NULL;
    if (sf && SDL_SaveBMP(sf, path))
        logf_("--shot: %s (%ux%u)", path, w, h);
    else
        logf_("--shot %s: %s", path, SDL_GetError());
    SDL_DestroySurface(sf);
    if (px)
        SDL_UnmapGPUTransferBuffer(gpu, tb);
    SDL_ReleaseGPUTransferBuffer(gpu, tb);
}

/* --press: scripted presses for tests */
#define PRESS_MAX 64
static struct { unsigned frame, side, bits, coin, sys; } press[PRESS_MAX];
static int presses;

static int press_parse(const char *list)
{
    static const struct { const char *name; unsigned bit; } names[] = {
        { "up", GAME_BTN_UP }, { "down", GAME_BTN_DOWN }, { "left", GAME_BTN_LEFT }, { "right", GAME_BTN_RIGHT },
        { "p", GAME_BTN_P }, { "k", GAME_BTN_K }, { "g", GAME_BTN_G }, { "start", GAME_BTN_START }, { "coin", 0 },
        { "test", 0 }, { "service", 0 },
    };
    char buf[2048];
    SDL_strlcpy(buf, list, sizeof buf);
    char *save = NULL;
    for (char *item = SDL_strtok_r(buf, ",", &save); item; item = SDL_strtok_r(NULL, ",", &save)) {
        char *who = SDL_strchr(item, ':'), *what = who ? SDL_strchr(who + 1, ':') : NULL;
        if (!what || presses == PRESS_MAX)
            return -1;
        *who++ = 0;
        *what++ = 0;
        press[presses].frame = (unsigned)SDL_strtoul(item, NULL, 10);
        press[presses].side = !SDL_strcasecmp(who, "p2");
        char *save2 = NULL;
        for (char *b = SDL_strtok_r(what, "+", &save2); b; b = SDL_strtok_r(NULL, "+", &save2)) {
            size_t i = 0;
            while (i < SDL_arraysize(names) && SDL_strcasecmp(b, names[i].name))
                i++;
            if (i == SDL_arraysize(names))
                return -1;
            press[presses].bits |= names[i].bit;
            if (!SDL_strcasecmp(b, "coin"))
                press[presses].coin = 1;
            else if (!SDL_strcasecmp(b, "test"))
                press[presses].sys |= GAME_SYS_TEST;
            else if (!SDL_strcasecmp(b, "service"))
                press[presses].sys |= GAME_SYS_SERVICE;
        }
        presses++;
    }
    return 0;
}

static void backup_save(void)
{
    static unsigned char b[GAME_BACKUP_SIZE];
    if (!backup_ok || !game_machine_backup_get(b))
        return;
    char tmp[4300];
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", backup_path);
    if (!SDL_SaveFile(tmp, b, sizeof b) || !SDL_RenamePath(tmp, backup_path)) {
        logf_("cannot write the backup %s: %s", backup_path, SDL_GetError());
        SDL_RemovePath(tmp);
    }
}

/* the players' cards: user/cards/p1.card and p2.card, 128 bytes each. A
 * player without one gets a card never used, which is written to the file
 * once the game has made it theirs. */
static int card_ok[2];                  /* 1: the file is ours to write */

static void card_path(char *buf, size_t n, int side)
{
    SDL_snprintf(buf, n, "%scards/p%d.card", user_dir, side + 1);
}

static void cards_save(void)
{
    for (int side = 0; side < 2; side++) {
        unsigned char c[GAME_CARD_SIZE];
        char path[4300], tmp[4400];
        if (!card_ok[side] || game_machine_card_get(side, c) != 1)
            continue;
        card_path(path, sizeof path, side);
        SDL_snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (!SDL_SaveFile(tmp, c, sizeof c) || !SDL_RenamePath(tmp, path)) {
            logf_("cannot write the card %s: %s", path, SDL_GetError());
            SDL_RemovePath(tmp);
        }
    }
}

static void cards_load(void)
{
    if (!cfg_.cards)
        return;
    char dir[4300];
    SDL_snprintf(dir, sizeof dir, "%scards", user_dir);
    if (!SDL_GetPathInfo(dir, NULL) && !SDL_CreateDirectory(dir))
        logf_("cannot make %s: %s", dir, SDL_GetError());
    for (int side = 0; side < 2; side++) {
        char path[4300];
        size_t n = 0;
        unsigned char *c = NULL;
        card_path(path, sizeof path, side);
        card_ok[side] = 1;
        if (SDL_GetPathInfo(path, NULL)) {
            c = SDL_LoadFile(path, &n);
            if (!c || n != GAME_CARD_SIZE) {
                logf_("%s is not a card (%d bytes): it is left alone, and player %d's unit stays empty", path,
                      GAME_CARD_SIZE, side + 1);
                SDL_free(c);
                card_ok[side] = 0;
                continue;
            }
        }
        if (game_machine_card_put(side, c))
            card_ok[side] = 0;
        else
            logf_("player %d: %s", side + 1, c ? path : "a new card (kept once the game has written it)");
        SDL_free(c);
    }
}

static void backup_load(void)
{
    size_t n = 0;
    unsigned char *b = NULL;
    if (SDL_GetPathInfo(backup_path, NULL)) {
        b = SDL_LoadFile(backup_path, &n);
        if (!b || n != GAME_BACKUP_SIZE) {
            logf_("%s is not a backup (%d bytes; it has %u): it is left alone, the run starts blank and "
                  "nothing is saved", backup_path, GAME_BACKUP_SIZE, (unsigned)n);
            SDL_free(b);
            b = NULL;
            backup_ok = 0;
        } else {
            logf_("backup loaded from %s", backup_path);
        }
    } else {
        logf_("no backup yet (%s): the cabinet starts blank", backup_path);
    }
    const char *how = game_machine_backup_put(b, cfg_.coins == COINS_ARCADE ? GAME_COINS_ARCADE : GAME_COINS_FREE_PLAY);
    if (how)
        logf_("coins: %s", how);
    SDL_free(b);
}
static SDL_AudioStream *audio;          /* NULL: no sound */

/* Sound: the chip's samples of this vertical blank go to the stream. The
 * device's clock is not the host's: the stream's frequency ratio is nudged
 * (at most 0.5%) to keep about 50 ms queued, and audio is dropped while the
 * machine runs ahead of real time (the boot, --unpaced) or the queue has
 * grown past 200 ms (a stall). */
#define AUDIO_TARGET 2205               /* frames: 50 ms */
static double audio_level = AUDIO_TARGET;
static unsigned audio_dropped;

static void audio_frame(int ahead)
{
    static short buf[2 * 4096];
    unsigned n, total = 0;
    int queued = audio ? SDL_GetAudioStreamQueued(audio) / 4 : 0;
    int keep = audio && !ahead && queued < 4 * AUDIO_TARGET;
    while ((n = game_machine_audio(buf, 4096)) > 0) {
        if (keep)
            SDL_PutAudioStreamData(audio, buf, (int)n * 4);
        else
            audio_dropped += n;
        total += n;
    }
    if (!keep || !total)
        return;
    audio_level += (queued - audio_level) * 0.05;
    double ratio = 1.0 + (audio_level - AUDIO_TARGET) / AUDIO_TARGET * 0.005;
    if (ratio < 0.995) ratio = 0.995;
    if (ratio > 1.005) ratio = 1.005;
    SDL_SetAudioStreamFrequencyRatio(audio, (float)ratio);
}
static unsigned frames_max;
static SDL_AtomicInt machine_state;     /* 0 starting, 1 running, 2 init failed, 3 stopped */
static Uint32 machine_event;            /* an SDL event type: the machine has stopped */

/* pacing and how busy the machine is, logged every 10 seconds of frames */
static Uint64 due, busy_since, busy_ns, busy_max_ns;
static unsigned busy_frames, resyncs;

/* machine thread, once per vertical blank (game_frame_fn) */
static void on_frame(unsigned frame, const unsigned char *rgb, int booting, game_input *in)
{
    Uint64 now = SDL_GetTicksNS();
    if (busy_since) {
        Uint64 b = now - busy_since;
        busy_ns += b;
        if (b > busy_max_ns)
            busy_max_ns = b;
        if (++busy_frames == 600) {
            logf_("machine: vertical blank %u; %.2f ms a frame on average, %.2f ms at most; %u times behind by "
                  "more than 1/4 s; audio queue %.0f ms", frame, busy_ns / 1e6 / busy_frames, busy_max_ns / 1e6, resyncs,
                  audio_level / 44.1);
            busy_ns = busy_max_ns = 0;
            busy_frames = 0;
        }
    }
    audio_frame((booting && !real_boot) || unpaced);
    SDL_LockMutex(shared.lock);
    if (rgb) {
        memcpy(shared.pic, rgb, sizeof shared.pic);
        shared.pic_frame = frame;
    }
    *in = shared.in;
    SDL_UnlockMutex(shared.lock);
    for (int i = 0; i < presses; i++)
        if (frame >= press[i].frame && frame < press[i].frame + 6) {
            in->player[press[i].side] |= press[i].bits;
            in->sys |= press[i].sys;
            if (press[i].coin)
                in->coin[press[i].side] = 1;
        }
    if (frames_max && frame >= frames_max)
        game_machine_stop();
    for (int i = 0; i < nshots; i++)
        if (shots[i].frame == frame) {
            SDL_SetAtomicInt(&shot_pause, i + 1);
            while (SDL_GetAtomicInt(&shot_pause))
                SDL_Delay(5);
            due = 0;
        }
    if (frame % BACKUP_EVERY == 0) {
        backup_save();
        cards_save();
    }
    /* test mode: the test program writes the game's settings and can clear
     * the backup; whatever it did is kept as soon as the game starts again */
    static int program;
    if (game_machine_program() != program) {
        program = game_machine_program();
        logf_("vertical blank %u: %s", frame, program ? "test mode (the game's test program)" : "the game starts again");
        backup_save();
        cards_save();
    }

    /* the frame is due 1/60 s after the one before; a long stall (loading)
     * is not caught up in a burst. The game's first start-up step runs as
     * fast as it can unless --real-boot: pacing never changes the run */
    if (booting && !real_boot)
        due = 0;
    else if (!unpaced) {
        if (!due || now > due + 250000000ull) {
            if (due)
                resyncs++;
            due = now;
        }
        due += FRAME_NS;
        if (due > now)
            SDL_DelayPrecise(due - now);
    }
    busy_since = SDL_GetTicksNS();
}

static int SDLCALL machine_main(void *arg)
{
    (void)arg;
    game_machine_cards(cfg_.cards);
    if (game_machine_init(image_path, dimm_path, dimm_key, !silent, on_frame)) {
        SDL_SetAtomicInt(&machine_state, 2);
    } else {
        backup_load();
        cards_load();
        SDL_SetAtomicInt(&machine_state, 1);
        int bad = game_machine_run();
        logf_("machine stopped%s: %s", bad ? " by itself" : "", game_machine_why());
        backup_save();
        cards_save();
        SDL_SetAtomicInt(&machine_state, 3);
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = machine_event;
    SDL_PushEvent(&ev);
    return 0;
}

/* ---- controls ---- */

static unsigned stick_bits(float x, float y)
{
    unsigned b = 0;
    if (x < -0.5f) b |= GAME_BTN_LEFT;
    if (x > 0.5f) b |= GAME_BTN_RIGHT;
    if (y < -0.5f) b |= GAME_BTN_UP;
    if (y > 0.5f) b |= GAME_BTN_DOWN;
    return b;
}

/* the bindings of settings.txt, turned into SDL's numbers once */
typedef struct { int n; SDL_Scancode sc[BIND_MAX]; } key_set;
typedef struct { int n; int in[BIND_MAX]; } pad_set;   /* >= 0 a button, < 0 a trigger: -1 - its axis */
static key_set keys[2][ACT_COUNT];
static pad_set pads[ACT_COUNT];
static const unsigned act_bits[ACT_COUNT] = {
    GAME_BTN_UP, GAME_BTN_DOWN, GAME_BTN_LEFT, GAME_BTN_RIGHT, GAME_BTN_G, GAME_BTN_P, GAME_BTN_K,
    GAME_BTN_P | GAME_BTN_G, GAME_BTN_P | GAME_BTN_K, GAME_BTN_P | GAME_BTN_K | GAME_BTN_G, GAME_BTN_START, 0,
    0, 0,                   /* test and service: the cabinet's switches (act_sys) */
};

/* what an action sets besides a player's bits: a coin, or a cabinet switch */
static void act_extra(game_input *in, int player, int a)
{
    if (a == ACT_COIN)
        in->coin[player] = 1;
    else if (a == ACT_TEST)
        in->sys |= GAME_SYS_TEST;
    else if (a == ACT_SERVICE)
        in->sys |= GAME_SYS_SERVICE;
}

static void bindings_compile(void)
{
    for (int a = 0; a < ACT_COUNT; a++) {
        for (int p = 0; p < 2; p++) {
            key_set *k = &keys[p][a];
            k->n = 0;
            for (int i = 0; i < BIND_MAX; i++) {
                const char *name = cfg_.key[p][a].name[i];
                if (!name[0])
                    continue;
                SDL_Scancode sc = SDL_GetScancodeFromName(name);
                if (sc == SDL_SCANCODE_UNKNOWN)
                    logf_("settings: key%d.%s: no key is called \"%s\"", p + 1, settings_action_name[a], name);
                else
                    k->sc[k->n++] = sc;
            }
        }
        pad_set *g = &pads[a];
        g->n = 0;
        for (int i = 0; i < BIND_MAX; i++) {
            const char *name = cfg_.pad[a].name[i];
            if (!name[0])
                continue;
            SDL_GamepadButton bt = SDL_GetGamepadButtonFromString(name);
            SDL_GamepadAxis ax = SDL_GetGamepadAxisFromString(name);
            if (bt != SDL_GAMEPAD_BUTTON_INVALID)
                g->in[g->n++] = bt;
            else if (ax == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ax == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                g->in[g->n++] = -1 - ax;
            else
                logf_("settings: pad.%s: no gamepad button is called \"%s\"", settings_action_name[a], name);
        }
    }
}

/* everything at once; returns the buttons of the first gamepad (for the quit chord) */
static unsigned read_controls(game_input *in)
{
    memset(in, 0, sizeof *in);
    const bool *k = SDL_GetKeyboardState(NULL);
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < ACT_COUNT; a++)
            for (int i = 0; i < keys[p][a].n; i++)
                if (k[keys[p][a].sc[i]]) {
                    in->player[p] |= act_bits[a];
                    act_extra(in, p, a);
                }
    /* fixed: slot 2's coin as M3 had it */
    if (k[SDL_SCANCODE_6]) in->coin[1] = 1;

    unsigned first = 0;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && i < 2; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g)
            continue;
        unsigned b = 0, raw = 0;
        for (int j = 0; j < SDL_GAMEPAD_BUTTON_COUNT && j < 32; j++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)j))
                raw |= 1u << j;
        b |= stick_bits(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f,
                        SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f);
        for (int a = 0; a < ACT_COUNT; a++)
            for (int j = 0; j < pads[a].n; j++) {
                int x = pads[a].in[j];
                int on = x >= 0 ? (x < 32 && (raw >> x & 1))
                                : SDL_GetGamepadAxis(g, (SDL_GamepadAxis)(-1 - x)) > 16000;
                if (on) {
                    b |= act_bits[a];
                    act_extra(in, i, a);
                }
            }
        in->player[i] |= b;
        if (i == 0)
            first = raw;
    }
    SDL_free(ids);
    /* a stick held between two directions that cancel gives neither */
    for (int i = 0; i < 2; i++) {
        if ((in->player[i] & (GAME_BTN_LEFT | GAME_BTN_RIGHT)) == (GAME_BTN_LEFT | GAME_BTN_RIGHT))
            in->player[i] &= ~(unsigned)(GAME_BTN_LEFT | GAME_BTN_RIGHT);
        if ((in->player[i] & (GAME_BTN_UP | GAME_BTN_DOWN)) == (GAME_BTN_UP | GAME_BTN_DOWN))
            in->player[i] &= ~(unsigned)(GAME_BTN_UP | GAME_BTN_DOWN);
    }
    return first;
}

static void log_gamepad_added(SDL_JoystickID which)
{
    /* Under Steam with Steam Input on, the pad seen here is Steam's virtual
     * one (Valve vendor 28de; on Windows and under Proton it looks like an
     * Xbox 360 pad, product 11ff), and the physical device is hidden from
     * SDL through SDL_GAMECONTROLLER_IGNORE_DEVICES. Logged so a run can show
     * which it was. */
    SDL_Gamepad *g = SDL_OpenGamepad(which);
    if (!g) {
        logf_("gamepad %u: open failed: %s", (unsigned)which, SDL_GetError());
        return;
    }
    Uint16 vid = SDL_GetGamepadVendor(g), pid = SDL_GetGamepadProduct(g);
    const char *path = SDL_GetGamepadPath(g);
    logf_("gamepad added: id %u \"%s\" type %s vid %04x pid %04x%s steam-handle %llx path %s", (unsigned)which,
          SDL_GetGamepadName(g), SDL_GetGamepadStringForType(SDL_GetGamepadType(g)), vid, pid,
          vid == 0x28de ? " (Steam virtual)" : "", (unsigned long long)SDL_GetGamepadSteamHandle(g),
          path ? path : "-");
}

/* ---- set-up ---- */

static void say_line(const char *msg)
{
    logf_("%s", msg);
}

static const char *beside_exe(char *buf, size_t n, const char *name)
{
    const char *base = SDL_GetBasePath();
    snprintf(buf, n, "%s%s", base ? base : "", name);
    return buf;
}

int main(int argc, char **argv)
{
    const char *gpu_name = NULL, *log_path = NULL, *user_arg = NULL;
    int fullscreen = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--frames") && v) frames_max = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--image") && v) image_path = argv[++i];
        else if (!strcmp(a, "--dimm") && v) dimm_path = argv[++i];
        else if (!strcmp(a, "--unpaced")) unpaced = 1;
        else if (!strcmp(a, "--real-boot")) real_boot = 1;
        else if (!strcmp(a, "--silent")) silent = 1;
        else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
        else if (!strcmp(a, "--gpu") && v) {
            gpu_name = argv[++i];
            if (!SDL_strcasecmp(gpu_name, "d3d12")) gpu_name = "direct3d12";   /* SDL's name for it */
        }
        else if (!strcmp(a, "--log") && v) log_path = argv[++i];
        else if (!strcmp(a, "--user") && v) user_arg = argv[++i];
        else if (!strcmp(a, "--shot") && v && i + 2 < argc && nshots < 8) {
            shots[nshots].frame = (unsigned)strtoul(argv[++i], NULL, 10);
            shots[nshots++].path = argv[++i];
        }
        else if (!strcmp(a, "--press") && v) {
            if (press_parse(argv[++i])) { fprintf(stderr, "vf4ft: --press: FRAME:p1|p2:BUTTON[+BUTTON],...\n"); return 2; }
        }
        else { fprintf(stderr, "vf4ft: unknown option %s (see the top of runtime/host/main.c)\n", a); return 2; }
    }

    static char default_log[4096], default_image[4096], default_dimm[4096];
    if (!log_path)
        log_path = beside_exe(default_log, sizeof default_log, "vf4ft.log");
#ifdef _WIN32
    /* a windowed program has no console: the machine's own messages (stderr) go to the log too */
    log_file = freopen(log_path, "w", stderr);
#else
    log_file = fopen(log_path, "w");
#endif
    if (!image_path) image_path = SDL_getenv("VF4_IMAGE");
    if (!dimm_path) dimm_path = SDL_getenv("VF4_DIMM");
    if (!dimm_path) {
        /* the builder's install (M5), else a development build's links */
        dimm_path = beside_exe(default_dimm, sizeof default_dimm, "data/VF41_S.BIN");
        if (!SDL_GetPathInfo(dimm_path, NULL))
            dimm_path = beside_exe(default_dimm, sizeof default_dimm, "data/vf41_s.dec");
    }
    size_t dl = strlen(dimm_path);
    if (dl >= 4 && !SDL_strcasecmp(dimm_path + dl - 4, ".bin")) {
        /* the disc's file: its key beside it */
        static char key[4096];
        const char *slash = SDL_strrchr(dimm_path, '/'), *bslash = SDL_strrchr(dimm_path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        size_t dir = slash ? (size_t)(slash - dimm_path + 1) : 0;
        snprintf(key, sizeof key, "%.*sdimm.key", (int)dir, dimm_path);
        dimm_key = key;
    } else if (!image_path) {
        image_path = beside_exe(default_image, sizeof default_image, "data/main.bin");
    }

    SDL_SetLogOutputFunction(sdl_log, NULL);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_INFO);
    int sdl_ver = SDL_GetVersion();
    logf_("vf4ft " VF4FT_VERSION "; SDL %d.%d.%d (%s); platform %s", SDL_VERSIONNUM_MAJOR(sdl_ver), SDL_VERSIONNUM_MINOR(sdl_ver),
          SDL_VERSIONNUM_MICRO(sdl_ver), SDL_GetRevision(), SDL_GetPlatform());
    {
        const char *appid = SDL_getenv("SteamAppId"), *deck = SDL_getenv("SteamDeck");
        logf_("steam: SteamAppId=%s SteamDeck=%s", appid ? appid : "-", deck ? deck : "-");
    }

    /* the player's folder: settings, the backup, cards */
    if (user_arg) {
        size_t ul = strlen(user_arg);
        SDL_snprintf(user_dir, sizeof user_dir, "%s%s", user_arg,
                     ul && (user_arg[ul - 1] == '/' || user_arg[ul - 1] == '\\') ? "" : "/");
    } else {
        beside_exe(user_dir, sizeof user_dir, "user/");
    }
    if (!SDL_GetPathInfo(user_dir, NULL) && !SDL_CreateDirectory(user_dir))
        logf_("cannot make %s: %s", user_dir, SDL_GetError());
    {
        char sp[4300];
        SDL_snprintf(sp, sizeof sp, "%ssettings.txt", user_dir);
        settings_defaults(&cfg_);
        if (!SDL_GetPathInfo(sp, NULL))
            logf_("no %s: the defaults", sp);
        else if (!settings_load(&cfg_, sp, say_line))
            logf_("settings from %s", sp);
        SDL_snprintf(backup_path, sizeof backup_path, "%sbackup.bin", user_dir);
    }
    if (!cfg_.sound)
        silent = 1;
    if (!fullscreen) {
        const char *deck = SDL_getenv("SteamDeck"), *tenfoot = SDL_getenv("SteamTenfoot");
        fullscreen = cfg_.fullscreen == FULLSCREEN_ON ||
                     (cfg_.fullscreen == FULLSCREEN_AUTO && ((deck && atoi(deck)) || (tenfoot && atoi(tenfoot))));
    }
    logf_("settings: renderer %s x%d, coins %s, fullscreen %s, picture %s, filter %s, sound %s, cards %s",
          cfg_.renderer == RENDERER_GPU ? "gpu" : "software", cfg_.scale,
          cfg_.coins == COINS_ARCADE ? "arcade" : "free play", fullscreen ? "on" : "off",
          cfg_.picture == PICTURE_STRETCH ? "stretch" : "4:3", cfg_.filter == FILTER_SHARP ? "sharp" : "smooth",
          silent ? "off" : "on", cfg_.cards ? "on" : "off");
    logf_("image %s; DIMM %s%s%s", image_path ? image_path : "(from the DIMM image)", dimm_path,
          dimm_key ? ", key " : "", dimm_key ? dimm_key : "");

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (silent ? 0 : SDL_INIT_AUDIO))) {
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    logf_("video driver: %s", SDL_GetCurrentVideoDriver());
    bindings_compile();     /* key names need the video subsystem */
    if (!silent) {
        SDL_AudioSpec as = { SDL_AUDIO_S16, 2, 44100 };
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &as, NULL, NULL);
        if (!audio)
            logf_("no sound: cannot open the audio device: %s", SDL_GetError());
        else {
            static short quiet[2 * AUDIO_TARGET];
            SDL_PutAudioStreamData(audio, quiet, sizeof quiet);
            SDL_ResumeAudioStreamDevice(audio);
            logf_("audio driver: %s", SDL_GetCurrentAudioDriver());
        }
    }
    shared.lock = SDL_CreateMutex();
    machine_event = SDL_RegisterEvents(1);

    SDL_Window *win = SDL_CreateWindow(VF4FT_TITLE, 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                       (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) { logf_("SDL_CreateWindow failed: %s", SDL_GetError()); return 1; }
    /* The GPU renderer's shaders are SPIR-V: a Vulkan device (natively, or
     * Proton's winevulkan). Without one the software picture is shown, on
     * whatever device SDL picks. */
    SDL_GPUDevice *gpu = NULL;
    int use_gpu = cfg_.renderer == RENDERER_GPU && !(gpu_name && SDL_strcasecmp(gpu_name, "vulkan"));
    if (use_gpu) {
        gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
        if (!gpu) {
            logf_("no Vulkan device (%s): the software picture instead", SDL_GetError());
            use_gpu = 0;
        }
    }
    if (!gpu)
        gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, false, gpu_name);
    if (!gpu) { logf_("SDL_CreateGPUDevice(%s) failed: %s", gpu_name ? gpu_name : "default", SDL_GetError()); return 1; }
    logf_("GPU driver: %s", SDL_GetGPUDeviceDriver(gpu));
    if (!SDL_ClaimWindowForGPUDevice(gpu, win)) { logf_("claim window failed: %s", SDL_GetError()); return 1; }
    if (use_gpu && gpu_render_init(gpu, cfg_.scale)) {
        logf_("the GPU renderer cannot start (%s): the software picture instead", SDL_GetError());
        gpu_render_quit();
        use_gpu = 0;
    }
    if (use_gpu) {
        game_machine_gpu(gpu_render_submit);
        logf_("renderer: GPU, %d x %d", PIC_W * cfg_.scale, PIC_H * cfg_.scale);
    } else {
        logf_("renderer: software, %d x %d", PIC_W, PIC_H);
    }

    /* the machine's thread: the translated code nests a C call for every guest
     * call and every interrupt, so it gets a large stack (Windows' default is 1 MB) */
    SDL_PropertiesID tp = SDL_CreateProperties();
    SDL_SetPointerProperty(tp, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)machine_main);
    SDL_SetStringProperty(tp, SDL_PROP_THREAD_CREATE_NAME_STRING, "machine");
    SDL_SetNumberProperty(tp, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 64 << 20);
    SDL_Thread *machine = SDL_CreateThreadWithProperties(tp);
    SDL_DestroyProperties(tp);
    if (!machine) {
        logf_("cannot start the machine's thread: %s", SDL_GetError());
        return 1;
    }


    /* the picture: uploaded into a texture whenever a new one is there, then
     * blitted to the window, scaled */
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = PIC_W;
    ti.height = PIC_H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *pic_tex = SDL_CreateGPUTexture(gpu, &ti);
    SDL_GPUTransferBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = PIC_W * PIC_H * 4;
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &bi);
    if (!pic_tex || !upload) { logf_("picture texture: %s", SDL_GetError()); return 1; }
    static unsigned char rgb[PIC_W * PIC_H * 3];

    Uint64 t0 = SDL_GetTicksNS(), quit_held_since = 0;
    unsigned shown_frame = 0, pictures = 0, presents = 0, last_pad = 0;
    int have_picture = 0, running = 1, stopping = 0, reported_start = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == machine_event) {
                running = 0;
                continue;
            }
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = 0;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE)
                    running = 0;
                else if (!ev.key.repeat && (ev.key.key == SDLK_F11 || (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
                    SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                log_gamepad_added(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                logf_("gamepad removed: id %u", (unsigned)ev.gdevice.which);
                break;
            default:
                break;
            }
        }
        int st = SDL_GetAtomicInt(&machine_state);
        if (st == 1 && !reported_start) {
            logf_("machine running");
            reported_start = 1;
        }

        game_input in;
        unsigned pad = read_controls(&in);
        if (pad != last_pad) {
            const unsigned chord = (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START);
            if ((pad & chord) == chord)
                quit_held_since = SDL_GetTicks();
            else
                quit_held_since = 0;
            last_pad = pad;
        }
        if (quit_held_since && SDL_GetTicks() - quit_held_since >= 1000) {
            logf_("quit: View + Menu held");
            running = 0;
        }

        unsigned frame;
        SDL_LockMutex(shared.lock);
        shared.in = in;
        frame = shared.pic_frame;
        if (frame != shown_frame)
            memcpy(rgb, shared.pic, sizeof rgb);
        SDL_UnlockMutex(shared.lock);

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
        if (!cmd) { logf_("acquire command buffer failed: %s", SDL_GetError()); break; }
        if (frame != shown_frame) {
            unsigned char *dst = SDL_MapGPUTransferBuffer(gpu, upload, true);
            if (dst) {
                for (int i = 0; i < PIC_W * PIC_H; i++) {
                    dst[4 * i] = rgb[3 * i];
                    dst[4 * i + 1] = rgb[3 * i + 1];
                    dst[4 * i + 2] = rgb[3 * i + 2];
                    dst[4 * i + 3] = 255;
                }
                SDL_UnmapGPUTransferBuffer(gpu, upload);
                SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureTransferInfo src = { .transfer_buffer = upload, .offset = 0 };
                SDL_GPUTextureRegion reg = { .texture = pic_tex, .w = PIC_W, .h = PIC_H, .d = 1 };
                SDL_UploadToGPUTexture(cp, &src, &reg, true);
                SDL_EndGPUCopyPass(cp);
                have_picture = 1;
                pictures++;
            }
            shown_frame = frame;
        }
        SDL_GPUTexture *sc = NULL;
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &sc, &w, &h)) {
            logf_("acquire swapchain failed: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        SDL_GPUTexture *src_tex = pic_tex;
        Uint32 src_w = PIC_W, src_h = PIC_H;
        if (use_gpu) {
            unsigned gf = 0;
            SDL_GPUTexture *t = gpu_render_frame(cmd, &src_w, &src_h, &gf);
            if (t) {
                src_tex = t;
                if (gf != shown_frame)
                    pictures++;
                shown_frame = gf;
                have_picture = 1;
            }
        }
        if (sc) {
            /* 4:3, as large as fits, black bars on the other sides (or stretched) */
            Uint32 dw = w, dh = h;
            if (cfg_.picture == PICTURE_STRETCH)
                ;       /* the whole window */
            else if ((Uint64)w * 3 > (Uint64)h * 4) dw = h * 4 / 3;
            else dh = w * 3 / 4;
            if (have_picture) {
                SDL_GPUBlitInfo bl;
                SDL_zero(bl);
                bl.source.texture = src_tex;
                bl.source.w = src_w;
                bl.source.h = src_h;
                bl.destination.texture = sc;
                bl.destination.x = (w - dw) / 2;
                bl.destination.y = (h - dh) / 2;
                bl.destination.w = dw;
                bl.destination.h = dh;
                bl.load_op = SDL_GPU_LOADOP_CLEAR;
                bl.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
                bl.filter = cfg_.filter == FILTER_SHARP ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmd, &bl);
            } else {
                SDL_GPUColorTargetInfo target;
                SDL_zero(target);
                target.texture = sc;
                target.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
                target.load_op = SDL_GPU_LOADOP_CLEAR;
                target.store_op = SDL_GPU_STOREOP_STORE;
                SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &target, 1, NULL));
            }
            if (presents++ == 0)
                logf_("first present: swapchain %ux%u", w, h);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
        /* a shot: two turns of this loop after the machine stopped for it, the newest picture is shown */
        static int shot_turns;
        int sp = SDL_GetAtomicInt(&shot_pause);
        if (sp && ++shot_turns >= 3 && have_picture) {
            shot_save(gpu, src_tex, src_w, src_h, shots[sp - 1].path);
            shots[sp - 1].done = 1;
            shot_turns = 0;
            SDL_SetAtomicInt(&shot_pause, 0);
        }
        if (!sc)
            SDL_Delay(10);      /* minimized: nothing to present to */
        if (st >= 2)
            running = 0;
    }

    SDL_SetAtomicInt(&shot_pause, 0);
    /* the machine ends at its next vertical blank (at once if it is not running) */
    stopping = SDL_GetAtomicInt(&machine_state) < 2;
    game_machine_stop();
    if (stopping)
        logf_("stopping the machine");
    SDL_WaitThread(machine, NULL);
    double secs = (SDL_GetTicksNS() - t0) / 1e9;
    logf_("%u pictures shown, %u presents in %.2f s (%.1f presents a second)", pictures, presents, secs,
          secs > 0 ? presents / secs : 0.0);
    int failed = SDL_GetAtomicInt(&machine_state) == 2;
    if (failed)
        logf_("the machine did not start: see the messages above (or, on Windows, this log)");

    if (audio) {
        logf_("audio: %u frames dropped (boot, unpaced or a stall); last queue %.0f ms", audio_dropped,
              audio_level / 44.1);
        SDL_DestroyAudioStream(audio);
    }
    if (use_gpu) {
        unsigned gf, gt, gv;
        gpu_render_stats(&gf, &gt, &gv);
        logf_("GPU renderer: %u frames drawn, at most %u triangles; %u frames had translucent volumes (not drawn)",
              gf, gt, gv);
        gpu_render_quit();
    }
    SDL_ReleaseGPUTransferBuffer(gpu, upload);
    SDL_ReleaseGPUTexture(gpu, pic_tex);
    SDL_ReleaseWindowFromGPUDevice(gpu, win);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(win);
    SDL_DestroyMutex(shared.lock);
    logf_("clean exit");
    SDL_Quit();
    if (log_file && log_file != stderr)
        fclose(log_file);
    return failed;
}
