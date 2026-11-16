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
#include "tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(GAME_PICTURE_W == VIEW_W && GAME_PICTURE_H == VIEW_H && GAME_PICTURE_W_MAX == VIEW_W_MAX,
               "the picture's size");
_Static_assert(GAME_BTN_UP == BTN_UP && GAME_BTN_DOWN == BTN_DOWN && GAME_BTN_LEFT == BTN_LEFT && GAME_BTN_RIGHT == BTN_RIGHT && GAME_BTN_P == BTN_P &&
               GAME_BTN_K == BTN_K && GAME_BTN_G == BTN_G && GAME_BTN_E == BTN_E &&
               GAME_BTN_START == BTN_START && GAME_SYS_TEST == SYS_TEST && GAME_SYS_SERVICE == SYS_SERVICE,
               "control.h's bits");

static game_frame_fn frame_fn;
static game_gpu_fn gpu_fn;
static int machine_up;      /* game_machine_init has set the view up */

void game_machine_gpu(game_gpu_fn fn)
{
    gpu_fn = fn;
    if (machine_up)
        view_host_gpu(fn);
}

void game_machine_wide(int width)
{
    view_host_wide(width);
}
static char why[700];

/* control.c's "save" writes the harness's reports; the game has no script */
void write_reports(void)
{
}

/* game_machine_snapshot_ask: taken at the end of host_frame */
static int snap_wanted, snap_state;     /* snap_state: 0 none waiting, 1 one, -1 it failed */
static u8 *snap_data;
static size_t snap_size;
static unsigned snap_frame;

void game_machine_snapshot_ask(void)
{
    snap_wanted = 1;
}

int game_machine_snapshot_get(unsigned char **data, size_t *size, unsigned *frame)
{
    int s = snap_state;
    if (s == 1) {
        *data = snap_data;
        *size = snap_size;
        *frame = snap_frame;
        snap_data = NULL;
    }
    snap_state = 0;
    return s;
}

static void snapshot_take(void)
{
    snap_wanted = 0;
    free(snap_data);
    snap_data = NULL;
    snap_frame = machine_frame;
    /* the test program's run is not resumed (rt_native_load) */
    snap_state = rt_native_program() == PROGRAM_MAIN && !snapshot_save_mem(&snap_data, &snap_size) ? 1 : -1;
    if (snap_state < 0)
        snap_data = NULL;
}

int game_machine_resume(const unsigned char *data, size_t size)
{
    if (rt_native_load(data, size)) {
        snprintf(why, sizeof why, "%s", snapshot_error);
        return -1;
    }
    return 0;
}

/* machine_host_frame: after the controller's step of this vertical blank */
static void host_frame(sh4 *c)
{
    (void)c;
    game_input in;
    memset(&in, 0, sizeof in);
    /* booting until the game first shows its warning screen (WARNING.DISP) or
     * anything after it; before it sets its mode (vertical blank 429) the mode
     * words hold other values. Test mode: the game boots again when it comes
     * back from the test program, which has no such start-up (and no mode
     * words) */
    static int booted;
    static unsigned switches;
    if (switches != rt_native_switches) {
        switches = rt_native_switches;
        booted = 0;
    }
    if (!booted && rt_native_program() == PROGRAM_TEST)
        booted = 1;
    if (!booted) {
        int m = game_mode_main(), s = game_mode_sub();
        booted = (m == 0 && s >= 1 && s < 16) || (m >= 1 && m < 8);
    }
    int pw = VIEW_W;
    const u8 *rgb = view_host_picture(&pw);
    frame_fn(machine_frame, rgb, pw, !booted, &in);
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
    if (snap_wanted)
        snapshot_take();
}

unsigned long long game_machine_hash(void)
{
    /* four lanes of FNV-1a over 64-bit words: the 32 MiB in about a
     * millisecond, where one lane's chain of multiplies takes several */
    const u64 m = 0x100000001b3ull;
    const u8 *ram = mem_ptr(RAM_BASE);
    u64 h[4] = { 0xcbf29ce484222325ull, 0x84222325cbf29ce4ull, 0x9e3779b97f4a7c15ull, 0x7f4a7c159e3779b9ull };
    for (u32 i = 0; i < RAM_SIZE; i += 32)
        for (int j = 0; j < 4; j++) {
            u64 w;
            memcpy(&w, ram + i + 8 * j, 8);
            h[j] = (h[j] ^ w) * m;
        }
    u64 r = h[0];
    for (int j = 1; j < 4; j++)
        r = (r ^ h[j]) * m;
    const u32 regs[] = { cpu.pc, cpu.pr, cpu.sr, cpu.t, cpu.gbr, cpu.vbr, cpu.mach, cpu.macl, cpu.fpul, cpu.fpscr };
    for (int i = 0; i < 16; i++)
        r = (((r ^ cpu.r[i]) * m ^ cpu.fr[i].u) * m ^ cpu.xf[i].u) * m;
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
        r = (r ^ regs[i]) * m;
    return r;
}

unsigned game_machine_audio(short *dst, unsigned max)
{
    return hw_sound_chip ? aica_audio_take(dst, max) : 0;
}

static int region;      /* game_machine_region's country: game_machine_init clears cfg */

int game_machine_init(const char *image, const char *dimm, const char *dimm_key, int sound_chip, game_frame_fn fn)
{
    memset(&cfg, 0, sizeof cfg);
    cfg.country = region;   /* game_machine_region */
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
    if (gpu_fn)
        view_host_gpu(gpu_fn);
    machine_up = 1;
    mem_guards();
    if (rt_native_init(image))
        return 1;
    rt_native_power_on();       /* test mode: the BIOS stand-in starts the other program from it */
    machine_host_frame = host_frame;
    return 0;
}

int game_machine_program(void)
{
    return rt_native_program() == PROGRAM_TEST;
}

static const char *eeprom_coins(u8 *e, int coins);

void game_machine_restart(int coins, int net)
{
    eeprom_coins(hw_maple_eeprom(), coins);     /* the game reads them as it starts */
    game_machine_network(net);
    rt_native_restart();
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
 * BIOS stand-in (board settings of country 0; coin settings from the
 * stand-in's service 0x2c): read back from a run, not made up. It holds no
 * country (board_config_unpack takes it as an argument), so it serves every
 * region (docs/design.md section 41) */
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

/* The game's settings record (vf4lib user_back.c's user_back_data: 13,012
 * bytes), as sdk/libam/userbkup.c keeps it in the battery-backed RAM: two
 * headers of four words (the check, the size, the size padded to words, 0)
 * at 0x1f8 and 0x208, then copy A at 0x218 and copy B right after it. A
 * copy's check is the EEPROM's, over its padded size. sram_save_load wants
 * the size in both headers, then takes copy A when its check holds, else B,
 * else the defaults (a blank part: the game writes them as it starts, with
 * the network on). sram_save_store writes both headers and both copies. */
#define SAVE_HEADER_A 0x1f8u
#define SAVE_HEADER_B 0x208u
#define SAVE_COPIES 0x218u
#define SAVE_SIZE 13012u            /* sizeof (user_back_data_rec): whole words, so padded the same */
#define SAVE_NET_CONNECT 0x162au    /* user_back_data.net_connect: Game Assignments, NETWORK (0 off, 1 on) */

static u8 save_rec[SAVE_SIZE];

static u32 le32(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* the copy the game would take into save_rec: 1, or 0 when it would start
 * from its defaults */
static int save_read(void)
{
    u8 h[2][16];
    g_read(BACKUP_RAM + SAVE_HEADER_A, h[0], 16);
    g_read(BACKUP_RAM + SAVE_HEADER_B, h[1], 16);
    for (int c = 0; c < 2; c++)
        if (le32(h[c] + 4) != SAVE_SIZE || le32(h[c] + 8) != SAVE_SIZE)
            return 0;
    for (int c = 0; c < 2; c++) {
        g_read(BACKUP_RAM + SAVE_COPIES + c * SAVE_SIZE, save_rec, SAVE_SIZE);
        if ((le32(h[c]) & 0xffffu) == eeprom_check(save_rec, SAVE_SIZE))
            return 1;
    }
    return 0;
}

/* save_rec as both copies, each with its header, as sram_save_store leaves them */
static void save_write(void)
{
    u32 c = eeprom_check(save_rec, SAVE_SIZE);
    u8 h[16] = { (u8)c, (u8)(c >> 8), 0, 0 };
    for (int i = 0; i < 4; i++) {
        h[4 + i] = (u8)(SAVE_SIZE >> 8 * i);
        h[8 + i] = (u8)(SAVE_SIZE >> 8 * i);
    }
    g_write(BACKUP_RAM + SAVE_HEADER_A, h, 16);
    g_write(BACKUP_RAM + SAVE_COPIES, save_rec, SAVE_SIZE);
    g_write(BACKUP_RAM + SAVE_HEADER_B, h, 16);
    g_write(BACKUP_RAM + SAVE_COPIES + SAVE_SIZE, save_rec, SAVE_SIZE);
}

int game_machine_net_ready(void)
{
    return save_read();
}

int game_machine_network(int net)
{
    if (!save_read())
        return -1;
    if (net != GAME_NET_KEEP) {
        save_rec[SAVE_NET_CONNECT] = net == GAME_NET_ON;
        save_write();
    }
    return save_rec[SAVE_NET_CONNECT] != 0;
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

/* ---- the board's region ---- */

void game_machine_region(int country)
{
    region = country;
    cfg.country = country;  /* hle_boot.c's service 0x28 gives it to the game */
}

int game_machine_card_put(int side, const unsigned char *card)
{
    if (!hw_card_option || hw_card_put(side, card))
        return -1;
    hw_card_get(side, card_last[side]);
    return 0;
}

int game_machine_card_held(int side)
{
    return hw_card_option && hw_card_held(side);
}

int game_machine_card_take(int side)
{
    if (!hw_card_option)
        return 0;
    return hw_card_take(side) > 0;
}

int game_machine_card_peek(int side, unsigned char *card)
{
    if (side < 0 || side > 1 || hw_card_last(side, card))
        return -1;
    return 0;
}

int game_machine_card_get(int side, unsigned char *card)
{
    if (side < 0 || side > 1 || hw_card_last(side, card))
        return -1;
    if (!memcmp(card, card_last[side], GAME_CARD_SIZE))
        return 0;
    memcpy(card_last[side], card, GAME_CARD_SIZE);
    return 1;
}
