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
 *   --image FILE      the program image (default: data/main.bin beside the executable, or $VF4_IMAGE)
 *   --dimm FILE       the decrypted DIMM image (default: data/vf41_s.dec beside the executable, or $VF4_DIMM)
 *   --frames N        quit after N vertical blanks (0 = until the window closes)
 *   --unpaced         no waiting: the machine runs as fast as it can (for measuring)
 *   --real-boot       the game's first start-up step (WARNING.INIT, about 97 s) at its real pace too;
 *                     by default it runs as fast as it can
 *   --fullscreen      start in fullscreen (F11 or Alt+Enter toggles)
 *   --silent          no sound chip: VF4debug's mailbox model in its place (as M3)
 *   --gpu NAME        SDL_GPU driver: direct3d12 (or d3d12) or vulkan (default: SDL's choice)
 *   --log FILE        log somewhere else
 *
 * Controls (rebinding is M6):
 *   keyboard (player 1): arrows or WASD, J guard, K punch, L kick, Enter start,
 *     5 / 6 coin for slot 1 / 2, F2 test, F3 service
 *   gamepads (first is player 1, second player 2): left stick or D-pad,
 *     X/West guard, Y/North punch, B/East kick, A/South punch+guard (throw),
 *     Menu start, View coin
 * Quit: Esc, closing the window, or holding View + Menu on a controller.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_machine.h"

#define VF4FT_TITLE "Virtua Fighter 4 Final Tuned"
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

static const char *image_path, *dimm_path;
static int unpaced, real_boot, silent;
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
    if (frames_max && frame >= frames_max)
        game_machine_stop();

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
    if (game_machine_init(image_path, dimm_path, !silent, on_frame)) {
        SDL_SetAtomicInt(&machine_state, 2);
    } else {
        SDL_SetAtomicInt(&machine_state, 1);
        int bad = game_machine_run();
        logf_("machine stopped%s: %s", bad ? " by itself" : "", game_machine_why());
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

/* everything at once; returns the buttons of the first gamepad (for the quit chord) */
static unsigned read_controls(game_input *in)
{
    memset(in, 0, sizeof *in);
    const bool *k = SDL_GetKeyboardState(NULL);
    unsigned *p1 = &in->player[0];
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) *p1 |= GAME_BTN_UP;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) *p1 |= GAME_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) *p1 |= GAME_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) *p1 |= GAME_BTN_RIGHT;
    if (k[SDL_SCANCODE_J]) *p1 |= GAME_BTN_G;
    if (k[SDL_SCANCODE_K]) *p1 |= GAME_BTN_P;
    if (k[SDL_SCANCODE_L]) *p1 |= GAME_BTN_K;
    if (k[SDL_SCANCODE_RETURN]) *p1 |= GAME_BTN_START;
    if (k[SDL_SCANCODE_5]) in->coin[0] = 1;
    if (k[SDL_SCANCODE_6]) in->coin[1] = 1;
    if (k[SDL_SCANCODE_F2]) in->sys |= GAME_SYS_TEST;
    if (k[SDL_SCANCODE_F3]) in->sys |= GAME_SYS_SERVICE;

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
        if (raw & 1u << SDL_GAMEPAD_BUTTON_DPAD_UP) b |= GAME_BTN_UP;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN) b |= GAME_BTN_DOWN;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT) b |= GAME_BTN_LEFT;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT) b |= GAME_BTN_RIGHT;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_WEST) b |= GAME_BTN_G;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_NORTH) b |= GAME_BTN_P;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_EAST) b |= GAME_BTN_K;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_SOUTH) b |= GAME_BTN_P | GAME_BTN_G;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_START) b |= GAME_BTN_START;
        if (raw & 1u << SDL_GAMEPAD_BUTTON_BACK) in->coin[i] = 1;
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

static const char *beside_exe(char *buf, size_t n, const char *name)
{
    const char *base = SDL_GetBasePath();
    snprintf(buf, n, "%s%s", base ? base : "", name);
    return buf;
}

int main(int argc, char **argv)
{
    const char *gpu_name = NULL, *log_path = NULL;
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
    if (!image_path) image_path = beside_exe(default_image, sizeof default_image, "data/main.bin");
    if (!dimm_path) dimm_path = SDL_getenv("VF4_DIMM");
    if (!dimm_path) dimm_path = beside_exe(default_dimm, sizeof default_dimm, "data/vf41_s.dec");

    SDL_SetLogOutputFunction(sdl_log, NULL);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_INFO);
    int sdl_ver = SDL_GetVersion();
    logf_("vf4ft M4; SDL %d.%d.%d (%s); platform %s", SDL_VERSIONNUM_MAJOR(sdl_ver), SDL_VERSIONNUM_MINOR(sdl_ver),
          SDL_VERSIONNUM_MICRO(sdl_ver), SDL_GetRevision(), SDL_GetPlatform());
    {
        const char *appid = SDL_getenv("SteamAppId"), *deck = SDL_getenv("SteamDeck");
        logf_("steam: SteamAppId=%s SteamDeck=%s", appid ? appid : "-", deck ? deck : "-");
    }
    logf_("image %s; DIMM %s", image_path, dimm_path);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (silent ? 0 : SDL_INIT_AUDIO))) {
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    logf_("video driver: %s", SDL_GetCurrentVideoDriver());
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

    SDL_Window *win = SDL_CreateWindow(VF4FT_TITLE, 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                       (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) { logf_("SDL_CreateWindow failed: %s", SDL_GetError()); return 1; }
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, false, gpu_name);
    if (!gpu) { logf_("SDL_CreateGPUDevice(%s) failed: %s", gpu_name ? gpu_name : "default", SDL_GetError()); return 1; }
    logf_("GPU driver: %s", SDL_GetGPUDeviceDriver(gpu));
    if (!SDL_ClaimWindowForGPUDevice(gpu, win)) { logf_("claim window failed: %s", SDL_GetError()); return 1; }

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
        if (sc) {
            /* 4:3, as large as fits, black bars on the other sides */
            Uint32 dw = w, dh = h;
            if ((Uint64)w * 3 > (Uint64)h * 4) dw = h * 4 / 3; else dh = w * 3 / 4;
            if (have_picture) {
                SDL_GPUBlitInfo bl;
                SDL_zero(bl);
                bl.source.texture = pic_tex;
                bl.source.w = PIC_W;
                bl.source.h = PIC_H;
                bl.destination.texture = sc;
                bl.destination.x = (w - dw) / 2;
                bl.destination.y = (h - dh) / 2;
                bl.destination.w = dw;
                bl.destination.h = dh;
                bl.load_op = SDL_GPU_LOADOP_CLEAR;
                bl.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
                bl.filter = SDL_GPU_FILTER_LINEAR;
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
        if (!sc)
            SDL_Delay(10);      /* minimized: nothing to present to */
        if (st >= 2)
            running = 0;
    }

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
