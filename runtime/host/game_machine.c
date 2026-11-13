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
#include "dimm_image.h"

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

int game_machine_init(const char *image, const char *dimm, const char *dimm_key, int sound_chip, game_frame_fn fn)
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
    /* The installed game has the disc's file and its key; the image is
     * decrypted into memory here, and the program comes out of it as the
     * BIOS takes it (the Naomi header's load table: 0x20000, 0x340000 bytes).
     * A development build has a decrypted image and main.bin instead. */
    u8 *dimm_mem = NULL;
    u32 dimm_size = 0;
    if (dimm_key) {
        if (dimm_image_load(dimm, dimm_key, &dimm_mem, &dimm_size))
            return 1;
        if (dimm_size < 0x360000)
            return 1;
    }
    if (image) {
        u32 loaded = 0;
        if (mem_load_file(image, IMAGE_BASE, 0x340000, &loaded) || loaded != 0x340000) {
            fprintf(stderr, "vf4ft: %s is not the 3,407,872-byte program image (main.bin)\n", image);
            return 1;
        }
    } else if (dimm_mem) {
        memcpy(mem_ptr(IMAGE_BASE), dimm_mem + 0x20000, 0x340000);
    } else {
        fprintf(stderr, "vf4ft: no program image\n");
        return 1;
    }
    if (disp_init() || rt_native_names_check())
        return 1;
    machine_init();
    hw_init(&cpu);
    if (dimm_mem ? hw_cart_init_memory(dimm_mem, dimm_size) : hw_cart_init(dimm))
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

/* ---- the backup: battery-backed RAM and the settings EEPROM ---- */

#define BACKUP_RAM 0xa0200000u
#define BACKUP_RAM_SIZE 0x10000u
#define EEPROM_SIZE 0x80u
_Static_assert(GAME_BACKUP_SIZE == BACKUP_RAM_SIZE + EEPROM_SIZE, "the backup file's size");

/* The EEPROM's system area, as the game's sdk/libam/eeprom.c keeps it: two
 * copies of 0x12 bytes at 0x00 and 0x12, each a 16-bit check (stored low
 * byte first) over the 0x10 bytes after it, then the board settings (5
 * bytes) and the coin settings (11; coin_config_pack: maximum credit,
 * chutes, the coin setting, ...). eeprom_check_copies takes copy A when its
 * check holds, else B, else clears both. */
#define EEP_COPY_A 0x00u
#define EEP_COPY_B 0x12u
#define EEP_COPY_LEN 0x12u
#define EEP_COIN_SETTING 9u     /* copy + 2 (check) + 5 (board) + 2 */
#define COIN_SETTING_FREE_PLAY 26u
#define COIN_SETTING_DEFAULT 0u /* the BIOS stand-in's default: 1 coin, 1 credit */

/* the check the game computes (crc16_ccitt, sdk/libam/getcrc.c): CCITT's
 * polynomial on a register that starts at 0xdeb, the data followed by a
 * zero byte */
static unsigned eeprom_check(const u8 *p, unsigned n)
{
    u32 c = 0xdebdeb00u;
    for (unsigned i = 0; i <= n; i++) {
        c = (c & 0xffffff00u) + (i < n ? p[i] : 0);
        for (int j = 0; j < 8; j++)
            c = (c & 0x80000000u) ? (c << 1) + 0x10210000u : c << 1;
    }
    return c >> 16;
}

static int eeprom_copy_good(const u8 *e, unsigned at)
{
    return (unsigned)(e[at] | e[at + 1] << 8) == eeprom_check(e + at + 2, 0x10);
}

static void eeprom_copy_seal(u8 *e, unsigned at)
{
    unsigned c = eeprom_check(e + at + 2, 0x10);
    e[at] = (u8)c;
    e[at + 1] = (u8)(c >> 8);
}

/* the system area the game writes when it starts on a blank part with this
 * BIOS stand-in (board settings of country 0, which is all the window's
 * machine runs; coin settings from the stand-in's service 0x2c): read back
 * from a run, not made up */
static const u8 eeprom_first_start[EEP_COPY_LEN] = {
    0xf0, 0xc6, 0x10, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x11, 0x11,
};

static const char *eeprom_coins(u8 *e, int coins)
{
    if (coins == GAME_COINS_KEEP)
        return NULL;
    int a = eeprom_copy_good(e, EEP_COPY_A), b = eeprom_copy_good(e, EEP_COPY_B);
    if (!a && !b) {
        /* blank (or both copies bad, which the game would clear): what it
         * would write on its first start */
        memcpy(e + EEP_COPY_A, eeprom_first_start, EEP_COPY_LEN);
        a = 1;
    }
    u8 *src = e + (a ? EEP_COPY_A : EEP_COPY_B);
    u8 setting = src[EEP_COIN_SETTING], want = setting;
    if (coins == GAME_COINS_FREE_PLAY)
        want = COIN_SETTING_FREE_PLAY;
    else if (setting == COIN_SETTING_FREE_PLAY)
        want = COIN_SETTING_DEFAULT;    /* arcade coins: an operator's own setting is kept */
    /* both copies the same, both sealed: as eeprom_sync_from_a leaves them */
    memmove(e + EEP_COPY_A, src, EEP_COPY_LEN);
    e[EEP_COPY_A + EEP_COIN_SETTING] = want;
    eeprom_copy_seal(e, EEP_COPY_A);
    memcpy(e + EEP_COPY_B, e + EEP_COPY_A, EEP_COPY_LEN);
    return want == COIN_SETTING_FREE_PLAY ? "free play" : "arcade coins";
}

static u8 backup_last[GAME_BACKUP_SIZE];        /* what was last put or got */

static void backup_take(u8 *b)
{
    g_read(BACKUP_RAM, b, BACKUP_RAM_SIZE);
    memcpy(b + BACKUP_RAM_SIZE, hw_maple_eeprom(), EEPROM_SIZE);
}

const char *game_machine_backup_put(const unsigned char *b, int coins)
{
    if (b) {
        g_write(BACKUP_RAM, b, BACKUP_RAM_SIZE);
        memcpy(hw_maple_eeprom(), b + BACKUP_RAM_SIZE, EEPROM_SIZE);
    }
    /* what is in the machine before the coin choice counts as saved: a run
     * that changes nothing has nothing to save, and a blank one no file */
    backup_take(backup_last);
    return eeprom_coins(hw_maple_eeprom(), coins);
}

int game_machine_backup_get(unsigned char *b)
{
    backup_take(b);
    if (!memcmp(b, backup_last, GAME_BACKUP_SIZE))
        return 0;
    memcpy(backup_last, b, GAME_BACKUP_SIZE);
    return 1;
}

/* ---- the card units ---- */

static u8 card_last[2][GAME_CARD_SIZE];

void game_machine_cards(int on)
{
    hw_card_option = on;
}

int game_machine_card_put(int side, const unsigned char *card)
{
    if (!hw_card_option || hw_card_put(side, card))
        return -1;
    hw_card_get(side, card_last[side]);
    return 0;
}

int game_machine_card_get(int side, unsigned char *card)
{
    if (side < 0 || side > 1 || hw_card_get(side, card))
        return -1;
    if (!memcmp(card, card_last[side], GAME_CARD_SIZE))
        return 0;
    memcpy(card_last[side], card, GAME_CARD_SIZE);
    return 1;
}
