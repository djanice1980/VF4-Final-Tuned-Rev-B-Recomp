/* The machine side of the game window: see game_machine.h. The set-up is
 * vf4ft_headless's (runtime/headless/main.c, from VF4debug's src/main.c)
 * without the harness's options: no script, no call log, no reports. */
#include "sh4.h"
#include "mem.h"
#include "hw.h"
#include "dispatch.h"
#include "observe.h"
#include "machine.h"
#include "control.h"
#include "hle.h"
#include "view.h"
#include "net.h"
#include "rt_native.h"
#include "aica.h"
#include "game_machine.h"

#include <stdio.h>
#include <string.h>

_Static_assert(GAME_PICTURE_W == VIEW_W && GAME_PICTURE_H == VIEW_H, "the picture's size");
_Static_assert(GAME_BTN_UP == BTN_UP && GAME_BTN_DOWN == BTN_DOWN && GAME_BTN_LEFT == BTN_LEFT && GAME_BTN_RIGHT == BTN_RIGHT && GAME_BTN_P == BTN_P &&
               GAME_BTN_K == BTN_K && GAME_BTN_G == BTN_G && GAME_BTN_E == BTN_E &&
               GAME_BTN_START == BTN_START && GAME_SYS_TEST == SYS_TEST && GAME_SYS_SERVICE == SYS_SERVICE,
               "control.h's bits");

static game_frame_fn frame_fn;
static char why[700];

/* control.c's "save" writes the harness's reports; the game has no script */
void write_reports(void)
{
}

/* machine_host_frame: after the controller's step of this vertical blank */
static void host_frame(sh4 *c)
{
    (void)c;
    game_input in;
    memset(&in, 0, sizeof in);
    /* booting until the game first shows its warning screen (WARNING.DISP) or
     * anything after it; before it sets its mode (vertical blank 429) the mode
     * words hold other values */
    static int booted;
    if (!booted) {
        int m = game_mode_main(), s = game_mode_sub();
        booted = (m == 0 && s >= 1 && s < 16) || (m >= 1 && m < 8);
    }
    frame_fn(machine_frame, view_host_picture(), !booted, &in);
    inputs.player[0] = in.player[0];
    inputs.player[1] = in.player[1];
    inputs.sys = in.sys;
    /* a coin is the switch closing, counted as the controller's "press coin" does */
    for (int ch = 0; ch < 2; ch++) {
        u32 on = in.coin[ch] != 0;
        if (on && !inputs.coin_switch[ch])
            inputs.coins[ch]++;
        inputs.coin_switch[ch] = on;
    }
}

unsigned game_machine_audio(short *dst, unsigned max)
{
    return hw_sound_chip ? aica_audio_take(dst, max) : 0;
}

int game_machine_init(const char *image, const char *dimm, int sound_chip, game_frame_fn fn)
{
    memset(&cfg, 0, sizeof cfg);
    cfg.image = image;
    cfg.dimm = dimm;
    cfg.run_name = "game";
    cfg.report_dir = "out";
    frame_fn = fn;
    hw_sound_chip = sound_chip;

    if (mem_init() || mem_add_ram("main-ram", RAM_BASE, RAM_SIZE) ||
        mem_add_ram("harness-scratch", CONTROL_SCRATCH, CONTROL_SCRATCH_SIZE))
        return 1;
    u32 loaded = 0;
    if (mem_load_file(image, IMAGE_BASE, 0x340000, &loaded) || loaded != 0x340000) {
        fprintf(stderr, "vf4ft: %s is not the 3,407,872-byte program image (main.bin)\n", image);
        return 1;
    }
    if (disp_init() || rt_native_names_check())
        return 1;
    machine_init();
    hw_init(&cpu);
    if (hw_cart_init(dimm))
        return 1;
    sh4_reset(&cpu, IMAGE_BASE);
    if (control_init(NULL) || hle_init(&cpu))
        return 1;
    hw_net_boot(&cpu);
    view_host_start();
    mem_guards();
    if (rt_native_init(image))
        return 1;
    machine_host_frame = host_frame;
    return 0;
}

int game_machine_run(void)
{
    int code = rt_native_run(&cpu);
    char mode[160];
    snprintf(why, sizeof why, "%s%s%s at vertical blank %u, %llu instructions; mode %s", sh4_stop_name(code),
             machine_stop_text[0] ? ": " : "", machine_stop_text, machine_frame, (unsigned long long)cpu.insns,
             game_mode_name(game_mode_main(), game_mode_sub(), mode, sizeof mode));
    if (code != STOP_LIMIT || !machine_interrupted) {
        machine_where(&cpu, stderr);
        machine_backtrace(&cpu, stderr);
        return 1;
    }
    return 0;
}

void game_machine_stop(void)
{
    machine_interrupted = 1;
}

const char *game_machine_why(void)
{
    return why;
}
