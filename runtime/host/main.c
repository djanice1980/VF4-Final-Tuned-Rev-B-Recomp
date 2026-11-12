/* M0: the platform layer on its own. Opens the game's window through
 * SDL_GPU, reads keyboard and gamepads (Steam Input arrives as an SDL
 * gamepad), and shows that it hears them: the window's colour follows the
 * stick and the buttons. Everything it sees goes to vf4ft.log beside the
 * executable, so a run under Proton can be checked afterwards.
 *
 * Options:
 *   --frames N        present N frames, then exit (0 = until the window closes)
 *   --gpu NAME        SDL_GPU driver: direct3d12 (or d3d12) or vulkan (default: SDL's choice)
 *   --log FILE        log somewhere else
 *
 * Quit: Esc, closing the window, or holding View + Menu on a controller.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VF4FT_TITLE "Virtua Fighter 4 Final Tuned"

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
    if (log_file) {
        fprintf(log_file, "[%7.3f] %s\n", ms / 1000.0, line);
        fflush(log_file);
    }
}

/* What the inputs look like this frame, as the JVS board will see them
 * later: a stick and the buttons. */
struct inputs {
    float x, y;          /* -1..1 */
    unsigned buttons;    /* bit per SDL_GamepadButton, plus keyboard */
};

enum { KEY_PUNCH = 1u << 24, KEY_KICK = 1u << 25, KEY_GUARD = 1u << 26, KEY_START = 1u << 27, KEY_COIN = 1u << 28 };

static void keyboard_inputs(struct inputs *in)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) in->x -= 1;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) in->x += 1;
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) in->y -= 1;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) in->y += 1;
    if (k[SDL_SCANCODE_J]) in->buttons |= KEY_GUARD;
    if (k[SDL_SCANCODE_K]) in->buttons |= KEY_PUNCH;
    if (k[SDL_SCANCODE_L]) in->buttons |= KEY_KICK;
    if (k[SDL_SCANCODE_RETURN]) in->buttons |= KEY_START;
    if (k[SDL_SCANCODE_5]) in->buttons |= KEY_COIN;
}

static void gamepad_inputs(struct inputs *in)
{
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        float ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        if (ax * ax + ay * ay > 0.06f) { in->x += ax; in->y += ay; }
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 24; b++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) in->buttons |= 1u << b;
        if (in->buttons & (1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT)) in->x -= 1;
        if (in->buttons & (1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) in->x += 1;
        if (in->buttons & (1u << SDL_GAMEPAD_BUTTON_DPAD_UP)) in->y -= 1;
        if (in->buttons & (1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN)) in->y += 1;
    }
    SDL_free(ids);
}

/* SDL's own warnings (why a GPU driver was refused, device trouble) go to
 * the same log: under Proton nobody sees stderr. */
static void sdl_log(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    static const char *const pri[] = { "?", "trace", "verbose", "debug", "info", "warn", "error", "critical" };
    logf_("SDL %s (cat %d): %s", priority < (int)SDL_arraysize(pri) ? pri[priority] : "?", category, message);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

int main(int argc, char **argv)
{
    long frames_max = 0;
    const char *gpu_name = NULL;
    const char *log_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames_max = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
            gpu_name = argv[++i];
            if (!SDL_strcasecmp(gpu_name, "d3d12")) gpu_name = "direct3d12";   /* SDL's name for it */
        }
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    char default_log[4096];
    if (!log_path) {
        const char *base = SDL_GetBasePath();
        snprintf(default_log, sizeof default_log, "%svf4ft.log", base ? base : "");
        log_path = default_log;
    }
    log_file = fopen(log_path, "w");
    SDL_SetLogOutputFunction(sdl_log, NULL);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_INFO);

    int sdl_ver = SDL_GetVersion();
    logf_("vf4ft M0 platform test; SDL %d.%d.%d (%s)", SDL_VERSIONNUM_MAJOR(sdl_ver),
          SDL_VERSIONNUM_MINOR(sdl_ver), SDL_VERSIONNUM_MICRO(sdl_ver), SDL_GetRevision());
    logf_("platform: %s", SDL_GetPlatform());
    {
        /* Set by Steam when it runs the game with Steam Input. */
        const char *ignore = SDL_getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
        const char *appid = SDL_getenv("SteamAppId");
        const char *deck = SDL_getenv("SteamDeck");
        int hidden = 0;
        for (const char *p = ignore; p && *p; p++) hidden += *p == ',';
        if (ignore && *ignore) hidden++;
        logf_("steam: SteamAppId=%s SteamDeck=%s ignore-devices: %d entries",
              appid ? appid : "-", deck ? deck : "-", hidden);
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    logf_("video driver: %s; audio driver: %s", SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver());

    SDL_Window *win = SDL_CreateWindow(VF4FT_TITLE, 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) { logf_("SDL_CreateWindow failed: %s", SDL_GetError()); return 1; }

    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL,
                                             false, gpu_name);
    if (!gpu) { logf_("SDL_CreateGPUDevice(%s) failed: %s", gpu_name ? gpu_name : "default", SDL_GetError()); return 1; }
    logf_("GPU driver: %s", SDL_GetGPUDeviceDriver(gpu));
    if (!SDL_ClaimWindowForGPUDevice(gpu, win)) { logf_("claim window failed: %s", SDL_GetError()); return 1; }

    long frames = 0;
    Uint64 t0 = SDL_GetTicksNS();
    unsigned last_buttons = 0;
    Uint64 quit_held_since = 0;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) running = false;
                if (!ev.key.repeat) logf_("key down: %s", SDL_GetKeyName(ev.key.key));
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                /* Under Steam with Steam Input on, the pad seen here is
                 * Steam's virtual one (Valve vendor 28de; on Windows and
                 * under Proton it looks like an Xbox 360 pad, product 11ff)
                 * and the physical device is hidden from SDL through the
                 * variables Steam sets (SDL_GAMECONTROLLER_IGNORE_DEVICES).
                 * A native Linux run from Steam also gets a Steam Input
                 * handle. Logged so a run can show which it was. */
                SDL_Gamepad *g = SDL_OpenGamepad(ev.gdevice.which);
                if (!g) { logf_("gamepad %u: open failed: %s", (unsigned)ev.gdevice.which, SDL_GetError()); break; }
                Uint16 vid = SDL_GetGamepadVendor(g), pid = SDL_GetGamepadProduct(g);
                const char *path = SDL_GetGamepadPath(g);
                logf_("gamepad added: id %u \"%s\" type %s vid %04x pid %04x%s steam-handle %llx path %s",
                      (unsigned)ev.gdevice.which, SDL_GetGamepadName(g),
                      SDL_GetGamepadStringForType(SDL_GetGamepadType(g)), vid, pid,
                      vid == 0x28de ? " (Steam virtual)" : "",
                      (unsigned long long)SDL_GetGamepadSteamHandle(g), path ? path : "-");
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED:
                logf_("gamepad removed: id %u", (unsigned)ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                logf_("gamepad %u button down: %s", (unsigned)ev.gbutton.which,
                      SDL_GetGamepadStringForButton((SDL_GamepadButton)ev.gbutton.button));
                break;
            default:
                break;
            }
        }

        struct inputs in = {0};
        keyboard_inputs(&in);
        gamepad_inputs(&in);
        in.x = clampf(in.x, -1, 1);
        in.y = clampf(in.y, -1, 1);
        if (in.buttons != last_buttons) {
            logf_("inputs: buttons %08x stick %.2f %.2f", in.buttons, in.x, in.y);
            last_buttons = in.buttons;
        }
        /* Quit from a controller alone (Steam Deck, couch): hold View and
         * Menu (Back + Start) for a second. */
        {
            const unsigned chord = (1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START);
            if ((in.buttons & chord) == chord) {
                if (!quit_held_since) quit_held_since = SDL_GetTicks();
                else if (SDL_GetTicks() - quit_held_since >= 1000) {
                    logf_("quit: View + Menu held");
                    running = false;
                }
            } else {
                quit_held_since = 0;
            }
        }

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
        if (!cmd) { logf_("acquire command buffer failed: %s", SDL_GetError()); break; }
        SDL_GPUTexture *tex = NULL;
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &tex, &w, &h)) {
            logf_("acquire swapchain failed: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        if (tex) {
            /* Dark blue at rest; the stick moves red/green, any button
             * brightens it. */
            float lit = in.buttons ? 0.35f : 0.0f;
            SDL_GPUColorTargetInfo target = {0};
            target.texture = tex;
            target.clear_color = (SDL_FColor){ clampf(0.05f + lit + 0.4f * (in.x > 0 ? in.x : 0), 0, 1),
                                               clampf(0.05f + lit + 0.4f * (in.y > 0 ? in.y : 0), 0, 1),
                                               clampf(0.20f + lit + 0.4f * (in.x < 0 ? -in.x : 0), 0, 1), 1.0f };
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
            SDL_EndGPURenderPass(pass);
            if (frames == 0) logf_("first frame: swapchain %ux%u", w, h);
            frames++;
        }
        SDL_SubmitGPUCommandBuffer(cmd);

        if (frames_max && frames >= frames_max) running = false;
    }

    double secs = (SDL_GetTicksNS() - t0) / 1e9;
    logf_("presented %ld frames in %.2f s (%.1f fps)", frames, secs, secs > 0 ? frames / secs : 0.0);

    SDL_ReleaseWindowFromGPUDevice(gpu, win);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(win);
    logf_("clean exit");
    SDL_Quit();
    if (log_file) fclose(log_file);
    return 0;
}
