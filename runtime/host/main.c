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
 *   --real-boot       the game's first start-up step (WARNING.INIT, about 97 s, or 35 s with the
 *                     network off) at its real pace too; by default it runs as fast as it can
 *   --fullscreen      start in fullscreen (F11 or Alt+Enter toggles)
 *   --silent          no sound chip: VF4debug's mailbox model in its place (as M3)
 *   --gpu NAME        SDL_GPU driver: direct3d12 (or d3d12) or vulkan (default: SDL's choice)
 *   --log FILE        log somewhere else
 *   --press LIST      for tests: buttons pressed for 6 vertical blanks at given ones, as
 *                     "6500:p1:start,6800:p1:p+g" (p1 or p2; up down left right p k g start coin, and
 *                     the cabinet's test and service with either)
 *   --shot N FILE     for tests: the machine waits at vertical blank N while the newest picture, as
 *                     shown (before scaling to the window), goes to a BMP file; up to 8 of them
 *   --keys LIST       for tests: keys pressed one after another (every 8 turns of the window's loop)
 *                     once the machine is at vertical blank FRAME, as "6400:Insert,Down,Right,
 *                     shot=menu.bmp,Escape": SDL key names, "@N" waits for vertical blank N,
 *                     "shot=FILE" saves the window as shown (the menu included) to a BMP file,
 *                     "pad:a+x=N" holds buttons of a virtual gamepad (SDL's names) for N turns (6),
 *                     "mouse:KIND@XxY" the mouse at window point X, Y: KIND move, left or right
 *                     (a click), up or down (a turn of the wheel),
 *                     "padtype=NAME" anywhere in the list gives that gamepad a real pad's ids (its
 *                     glyphs): xbox, xbox360, ps5, ps4, ps3, switch, gamecube, deck, steamvirtual,
 *                     "pad2:..." and "pad2type=NAME" the same for a second virtual gamepad,
 *                     "text:WORDS" types WORDS ("_" a space) as the keyboard's text
 *   --user DIR        the player's folder (default: user/ beside the executable): settings.txt,
 *                     backup.bin (the cabinet's battery-backed RAM and settings EEPROM), cards/
 *   --art DIR         no window: Steam's pictures for the launcher's entry, made from the attract
 *                     sequence into DIR (art.h; docs/design.md section 36). Exit 0 when all were
 *                     made, 3 without the GPU renderer, else 1
 *   --art-frames LIST with --art: whole pictures of those vertical blanks instead
 *                     ("2400,3000-6000/300"), for choosing scenes
 *   --kofi-notice N   for tests: the first start's Ko-fi notice at vertical blank N, even when it
 *                     was shown before (test runs, with --frames, --shot, --press, --keys or
 *                     --unpaced, show it only so)
 *   --refresh-notice N  for tests: the screen refresh notice (docs/design.md section 44) at
 *                     vertical blank N when it is due, even when it was shown before
 *   --deck-product FILE  for tests: the firmware's product name from FILE (Jupiter: a Steam
 *                     Deck LCD; Galileo: the OLED) instead of /sys/class/dmi/id/product_name
 *   --screen-hz HZ    for tests: the screen's refresh rate is HZ, whatever SDL reports and the
 *                     presents show
 *   --system-hz HZ    for tests: SDL reports HZ for the screen (a report that is wrong: it is
 *                     held against the presents' rate, docs/design.md section 61)
 *   --refresh-first HZ  for tests: the refresh's filter starts holding a screen of HZ's period,
 *                     as after presents that were not the screen's (docs/design.md section 63)
 *   --pace-ppm N      for tests: the game's own timer runs N parts in a million fast (N may be
 *                     negative), as a screen that many slow would look to it (docs/design.md
 *                     section 58)
 *   --sync-log FILE   for tests: when each frame the game drew was handed to the window's thread
 *                     ("M frame ns") and which frame each present showed ("P frame ns"), into FILE
 *   --hash-every N    for tests: logs the machine's fingerprint (main RAM and the registers) every N
 *                     vertical blanks, to compare two runs (docs/design.md section 45)
 *   --snapshot-at N FILE  for tests: the whole machine at the end of vertical blank N into FILE
 *                     (docs/design.md section 48)
 *   --resume FILE     for tests: the run goes on from FILE's snapshot (made by this build), after
 *                     the user's files are in
 *   --cpu-passes      for tests: with the GPU renderer, the passes into textures the game makes (a
 *                     stage's reflection in its water) are drawn by the machine's thread into
 *                     texture memory, as before docs/design.md section 78, not on the GPU
 *   --clock WHEN      for tests: what the board's clock shows as the machine starts, in place of
 *                     this PC's date and time (docs/design.md section 77): "fixed" (2004-11-10
 *                     07:18:24, with which every run is the same run) or YYYY-MM-DDThh:mm:ss
 *   --stall-window T:MS[:PART]  for tests: at turn T of the window's loop its thread sleeps MS
 *                     milliseconds where it waits for the screen's next buffer, as a graphics
 *                     driver that does not come back would hold it (docs/design.md section 83);
 *                     PART 1: in the first of that wait's two steps, the wait for an earlier
 *                     frame (section 85), else in the second
 *   --rollback N      for tests: once the game has booted, the machine keeps its last N frames for
 *                     going back to (docs/design.md section 55); nothing goes back by itself
 *   --synctest N[,P]  for tests: rollback, and every P vertical blanks (default 120) the machine
 *                     runs N frames (1 to 60), goes back N and runs them again with the same
 *                     controls, unseen and unheard (section 56); each check's result goes to the
 *                     log ("synctest:")
 *   --host [PORT]     a two-PC game (session.h, docs/design.md sections 46, 47, 49 and 50): this PC
 *                     hosts and waits for the other on UDP port PORT (default 29404), showing a join
 *                     code; the game starts when it joins. Should the other PC leave, this PC's game
 *                     goes on with its player alone, and another PC can join it (--join, as before,
 *                     with the new code the note and the menu show) as the other player
 *   --host-player N   with --host: this PC's player, 1 (the default) or 2
 *   --join ADDRESS    a two-PC game: this PC joins the host at ADDRESS ("192.168.1.20", "pc-name",
 *                     "192.168.1.20:29404") as the player the host is not, with --code. The host's
 *                     cabinet (backup, coins, region, network, card units) is used; this PC's own files
 *                     are not changed but for its card. Its game stops when the host's leaves
 *   --code CODE       with --join: the six digits the host's game shows (each code works once)
 *   --delay N         with --host: the input delay in vertical blanks (default 2)
 *
 * Settings (M6): user/settings.txt (settings.h), written by the launcher's
 * Settings page: free play or coins, fullscreen, picture, sound, the card
 * units and every control. The defaults:
 *   keyboard (player 1): arrows or WASD, J guard, K punch, L kick, U throw
 *     (P+G), I P+K, O P+K+G, P K+G, Enter start, 5 coin, F2 test, F3 service
 *   gamepads (a set of buttons for each player, the same by default; the first
 *     pad connected is player 1's and the second player 2's unless the Settings
 *     page gives a player one controller: section 69), the PlayStation 2
 *     versions' layout by position: left stick or D-pad, X/West (square)
 *     guard, Y/North (triangle) and A/South (cross) punch, B/East (circle)
 *     kick, LB P+G, RB P+K, LT P+K+G, RT K+G, Menu start, View coin
 *   always: 6 coin for slot 2
 * Test mode: Test starts the game's test program (runtime/native/rt_native.c),
 * where Service moves the cursor and Test chooses; its EXIT starts the game
 * again. Nothing in it is changed from the board's.
 * The menu (menu.c): Insert or Esc, a gamepad's Guide button, or View + Menu
 * held for a second; the game pauses. Settings, the operator's test menu,
 * restart and quit are in it. Closing the window quits too. The first start
 * shows a notice about the author's Ko-fi page in it, once (user/kofi-notice
 * marks it shown; docs/design.md section 26).
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

#include "art.h"
#include "card_editor.h"
#include "cards.h"
#include "compositor.h"
#include "devices.h"
#include "game_machine.h"
#include "settings.h"
#include "gpu_render.h"
#include "menu.h"
#include "options.h"
#include "session.h"
#include "snap_patch.h"
#include "sysidle.h"
#include "ui.h"
#include "updates.h"
#include "view.h"
#include "vsync.h"

#define VF4FT_TITLE "Virtua Fighter 4 Final Tuned"
#ifndef VF4FT_VERSION
#define VF4FT_VERSION "development build"   /* the builder (tools/make_game.py) gives its version */
#endif
#define PIC_W GAME_PICTURE_W
#define PIC_W_MAX GAME_PICTURE_W_MAX
#define PIC_H GAME_PICTURE_H
#define FRAME_NS (1000000000ull / 60)   /* the machine's vertical blank (runtime/machine/machine.h) */
static Uint64 frame_ns = FRAME_NS;      /* the timer's (--pace-ppm: a little off, for tests) */
static FILE *sync_log;                  /* --sync-log */

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
    /* the software pictures waiting to be shown, oldest first (2 at most): RGB, pic_w x PIC_H, each
     * with its vertical blank and when it is to be shown by the game's clock (0: as it comes) */
    unsigned char pic[2][PIC_W_MAX * PIC_H * 3];
    int pic_w[2], pic_n;
    unsigned pic_frame[2];
    Uint64 pic_pts[2];
    game_input in;                              /* the controls, as this thread last read them */
} shared;
static unsigned pics_made, pics_lost;           /* (under shared.lock) software pictures, and those never shown */
static SDL_AtomicInt presents_n;                /* the window's presents */
static SDL_AtomicInt pacing_now;                /* PACING_*: what the window's thread goes by now (in a two-PC
                                                 * game Game clock for Screen refresh) */

static const char *image_path, *dimm_path, *dimm_key;
static const char *art_dir;             /* --art: Steam's artwork, no window (art.h) */
static const unsigned *art_list;        /* the vertical blanks it takes */
static int art_n, art_i;
static int unpaced, real_boot, silent;
static settings cfg_;                   /* user/settings.txt over the defaults */
static settings start_cfg;              /* as this run started (what applies at the next start differs) */
static char user_dir[4096];             /* ends in a separator */
static char backup_path[4200];
static int backup_ok = 1;               /* 0: the file there is not a backup; never written over */
#define BACKUP_EVERY 600                /* vertical blanks: 10 seconds */

/* --shot: pictures for tests */
static struct { unsigned frame; const char *path; int done; } shots[8];
static int nshots;
static SDL_AtomicInt shot_pause;        /* 1 + the shot the machine waits for at its vertical blank */

/* the menu (menu.c): what this thread asks of the machine's */
static SDL_AtomicInt menu_pause;        /* 1: the machine waits at its vertical blank */
static SDL_AtomicInt gpu_path;          /* 1: the machine is to hand its passes to the GPU renderer (video_open) */
static int machine_path = -1;           /* the machine's thread's: what it does now (-1: not set yet) */
static SDL_AtomicInt test_frames;       /* vertical blanks the menu holds the Test switch for */
static SDL_AtomicInt restart_req;       /* GAME_COINS_*: the game starts again from power on, with it (and
                                         * start_cfg's network) */
static int net_first;                   /* a blank backup with the network wanted off: it goes in once the
                                         * game has made its settings record (on_frame) */
static SDL_AtomicInt frame_now;         /* the machine's vertical blank */
static SDL_AtomicInt program_now;       /* 1 while the test program runs */

/* The board's clock (docs/design.md section 77): this PC's date and time as the game starts, so
 * that what the game takes from the date is not the same every run (which opponents come in
 * which order, on which stages; a stage's clock). In a two-PC game both machines have the
 * host's. clock_start 0: the machine's own fixed date (--clock fixed, and the artwork's run) */
static const char *clock_arg;           /* --clock */
static unsigned clock_start;            /* seconds from 1950-01-01, as the board counts them */
static const char *clock_whose = "the fixed date";
/* a two-PC game (session.h): what both machines start from, and this PC's
 * player. Its card is written only as the fingerprints of both PCs agree
 * (own_card_*): the card as it was at the newest check vertical blank both
 * agreed on, when it changed. Once the host's game goes on alone (the
 * session ended, session_alone), its card is written as it is */
static int in_session;                  /* --host or --join */
static int session_delay = SESSION_DELAY;
static session_start sess_mine, sess;
static SDL_AtomicInt window_quit;       /* the window is closing: a machine waiting for nothing goes on to its stop */
static int session_stopped;             /* machine thread: session_frame said the session ended */
static int session_alone;               /* machine thread: the host's game goes on alone */
static SDL_AtomicInt alone_ms;          /* when it began (SDL_GetTicks, | 1; 0 not), for the window's note */
static SDL_AtomicInt session_over;      /* the session ended and this PC's game stopped */
static char own_card[CARD_NAME_MAX];    /* this PC's player's card profile ("" none in the unit) */
static unsigned char own_card_last[GAME_CARD_SIZE];     /* as written last (or put in) */
static struct { unsigned frame; int have; unsigned char c[GAME_CARD_SIZE]; } own_snap[8];
static unsigned own_card_done;          /* the check vertical blank written last */
/* latency optimization (session.h, docs/design.md section 57): the machine
 * keeps its last vertical blanks and goes back as the session asks. The
 * host's backup is taken at the start of a vertical blank as ever, and
 * written once every vertical blank before it ran with both players' real
 * controls (taken again if it runs again) */
static int rollback_test;               /* --rollback or --synctest: not in a two-PC game */
static unsigned char backup_held[GAME_BACKUP_SIZE];
static unsigned backup_held_at;         /* its vertical blank */
static int backup_held_on;              /* 1: one is held */
/* a PC joining a game going on (session.h, docs/design.md section 49): the machine at the end
 * of the session's start-up, the reference, kept by the host and by a joining PC's copy */
static int ref_step;                    /* 0 not yet, 1 asked for, 2 kept, 3 none */
static unsigned char *ref_data;
static size_t ref_size;
static unsigned ref_frame;
static unsigned long long ref_hash;
static int late_join;                   /* this PC joins a game going on, until it has the host's machine */
static int late_step;                   /* host: 0, 1 a PC is joining (the card), 2 the machine asked for */
static session_player late_p;           /* host: the joining player */
static int late_same;                   /* host: its copy is the reference */
static int late_card_in;                /* the joining player's card went into its unit */
static unsigned char *late_buf;         /* joining PC: the host's machine as loaded (freed the frame after) */
static int late_loaded;                 /* joining PC: it has just been loaded */

/* the picture in tex (w x h, RGBA) to a BMP file: waits for the GPU */
/* reads a texture back (w x h, RGBA bytes): use gets its pixels, or NULL */
static void read_back(SDL_GPUDevice *gpu, SDL_GPUTexture *tex, Uint32 w, Uint32 h,
                      void (*use)(void *ud, const void *px, Uint32 w, Uint32 h), void *ud)
{
    SDL_GPUTransferBufferCreateInfo ti;
    SDL_zero(ti);
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = w * h * 4;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu, &ti);
    SDL_GPUCommandBuffer *cmd = tb ? SDL_AcquireGPUCommandBuffer(gpu) : NULL;
    if (!cmd) {
        if (tb)
            SDL_ReleaseGPUTransferBuffer(gpu, tb);
        use(ud, NULL, w, h);
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
    use(ud, px, w, h);
    if (px)
        SDL_UnmapGPUTransferBuffer(gpu, tb);
    SDL_ReleaseGPUTransferBuffer(gpu, tb);
}

static void save_bmp(void *ud, const void *px, Uint32 w, Uint32 h)
{
    const char *path = ud;
    SDL_Surface *sf = px ? SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)px, (int)w * 4) : NULL;
    if (sf && SDL_SaveBMP(sf, path))
        logf_("--shot: %s (%ux%u)", path, w, h);
    else
        logf_("--shot %s: %s", path, SDL_GetError());
    SDL_DestroySurface(sf);
}

static void shot_save(SDL_GPUDevice *gpu, SDL_GPUTexture *tex, Uint32 w, Uint32 h, const char *path)
{
    read_back(gpu, tex, w, h, save_bmp, (void *)path);
}

/* --press: scripted presses for tests */
#define PRESS_MAX 8192
static struct { unsigned frame, side, bits, coin, sys; } press[PRESS_MAX];
static int presses;

static int press_parse(const char *list)
{
    static const struct { const char *name; unsigned bit; } names[] = {
        { "up", GAME_BTN_UP }, { "down", GAME_BTN_DOWN }, { "left", GAME_BTN_LEFT }, { "right", GAME_BTN_RIGHT },
        { "p", GAME_BTN_P }, { "k", GAME_BTN_K }, { "g", GAME_BTN_G }, { "start", GAME_BTN_START }, { "coin", 0 },
        { "test", 0 }, { "service", 0 },
    };
    static char buf[65536];    /* a whole fight's presses */
    if (SDL_strlcpy(buf, list, sizeof buf) >= sizeof buf)
        return -1;
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

static void backup_write(const unsigned char *b);

static void backup_save(void)
{
    static unsigned char b[GAME_BACKUP_SIZE];
    if (!backup_ok)
        return;
    game_machine_backup_peek(b);
    backup_write(b);
}

/* the backup b (taken earlier) into its file, when it changed */
static void backup_write(const unsigned char *b)
{
    if (!backup_ok || !game_machine_backup_new(b))
        return;
    char tmp[4300];
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", backup_path);
    if (!SDL_SaveFile(tmp, b, GAME_BACKUP_SIZE) || !SDL_RenamePath(tmp, backup_path)) {
        logf_("cannot write the backup %s: %s", backup_path, SDL_GetError());
        SDL_RemovePath(tmp);
    }
}

/* the players' cards: card profiles, user/cards/NAME.card, 128 bytes each
 * (cards.h; settings card1 and card2 say whose card is in each unit). A
 * profile without a file yet gets a card never used, which is written under
 * its name once the game has made it the player's. The menu's Cards tab
 * swaps them between games (cards_swap). */
static int card_ok[2];                  /* 1: the file is ours to write */
static char card_in[2][CARD_NAME_MAX];  /* machine thread: the profile in each unit ("" none) */
static char card_want[2][CARD_NAME_MAX];        /* the menu's choice (under shared.lock) */
static SDL_AtomicInt card_swap[2];      /* 1: card_want is to go in */
static char card_chosen[2][CARD_NAME_MAX];      /* main thread: what was last asked for */

static void card_path(char *buf, size_t n, int side)
{
    cards_path(buf, n, user_dir, card_in[side]);
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

/* card_in[side]'s card into player side's unit (it is empty) */
static void card_insert(int side)
{
    char path[4300];
    size_t n = 0;
    unsigned char *c = NULL;
    card_ok[side] = 0;
    if (!card_in[side][0]) {
        logf_("player %d: no card (the unit stays empty)", side + 1);
        return;
    }
    card_path(path, sizeof path, side);
    if (SDL_GetPathInfo(path, NULL)) {
        c = SDL_LoadFile(path, &n);
        if (!c || n != GAME_CARD_SIZE) {
            logf_("%s is not a card (%d bytes): it is left alone, and player %d's unit stays empty", path,
                  GAME_CARD_SIZE, side + 1);
            SDL_free(c);
            return;
        }
    }
    if (!game_machine_card_put(side, c)) {
        card_ok[side] = 1;
        logf_("player %d: %s (%s)", side + 1, card_in[side],
              c ? path : "a card never used, kept under that name once the game has written it");
    }
    SDL_free(c);
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
        SDL_strlcpy(card_in[side], cfg_.card[side], sizeof card_in[side]);
        if (side == 1 && card_in[1][0] && !SDL_strcasecmp(card_in[0], card_in[1])) {
            logf_("player 2's card is player 1's (%s): one card can be in only one unit, so player 2 has none",
                  card_in[1]);
            card_in[1][0] = 0;
        }
        card_insert(side);
    }
}

/* on the machine's thread, every vertical blank: the menu's new cards go
 * in. The card in the unit is written to its file first, and a card the
 * game is using stays (and is kept written) until the game lets go of it.
 * One card is in one unit at a time: a card the other player's unit still
 * holds (the menu swapped the two) goes in once that unit has let it go */
static void cards_swap(void)
{
    static int waiting[2], held[2];
    for (int side = 0; side < 2; side++) {
        if (!SDL_GetAtomicInt(&card_swap[side]))
            continue;
        cards_save();
        if (game_machine_card_take(side)) {
            if (!waiting[side]++)
                logf_("player %d: the card (%s) leaves once the game has written it", side + 1, card_in[side]);
            continue;
        }
        if (waiting[side])
            logf_("player %d: the game let go of the card (%s) after %d vertical blanks", side + 1, card_in[side],
                  waiting[side]);
        waiting[side] = 0;
        card_ok[side] = 0;      /* the unit is empty */
        card_in[side][0] = 0;
        char want[CARD_NAME_MAX];
        SDL_LockMutex(shared.lock);
        SDL_strlcpy(want, card_want[side], sizeof want);
        int other = want[0] && !SDL_strcasecmp(want, card_in[!side]);
        if (!other) {
            SDL_strlcpy(card_in[side], want, sizeof card_in[side]);
            SDL_SetAtomicInt(&card_swap[side], 0);
        }
        SDL_UnlockMutex(shared.lock);
        if (other) {
            if (!held[side]++)
                logf_("player %d: %s goes in once player %d's unit has let it go", side + 1, want, 2 - side);
            continue;
        }
        if (held[side])
            logf_("player %d: player %d's unit let %s go after %d vertical blanks", side + 1, 2 - side, want,
                  held[side]);
        held[side] = 0;
        card_insert(side);
    }
}

/* ---- the menu's card editor (docs/design.md section 42) ----
 * An edit Saved in the menu waits here, as the fields it changes, until the
 * machine thread can put it onto the card's file: as the menu closes, or,
 * for a card a game is using (hw_card_held: from the game's read at its
 * start to the lock opening at its end), once the game lets go of it, so
 * that it goes onto the game's latest and the game's own changes (its
 * record, its games left) stay. A card in a unit is taken out for it and
 * goes back in within the same vertical blank, so the game never sees it
 * leave. The menu shows a card with its waiting edit (card_peek). */
typedef struct card_pending {
    char name[CARD_NAME_MAX];
    card_edit edit;
    int fresh;              /* never used or not made yet: it becomes a ready card */
    unsigned gen;           /* changed at each Save: the machine thread drops only what it wrote */
} card_pending;
static card_pending pend[CARDS_MAX + 2];        /* under shared.lock */
static int npend;
static SDL_AtomicInt pend_n;
static char unit_card[2][CARD_NAME_MAX];        /* under shared.lock: the card in each unit, for the menu */
static int unit_held[2];                        /* and whether a game holds it */

static int pend_find(const char *name)
{
    for (int i = 0; i < npend; i++)
        if (!SDL_strcasecmp(pend[i].name, name))
            return i;
    return -1;
}

/* a card's file with an edit put onto it, in card (CARD_SIZE bytes): 1, or 0
 * when the file is not a card that can take it (left alone) */
static int card_edited(const char *name, const card_pending *p, Uint8 *card)
{
    char path[4300];
    size_t n = 0;
    Uint8 *c = NULL;
    card_view v;
    SDL_zero(v);
    cards_path(path, sizeof path, user_dir, name);
    if (SDL_GetPathInfo(path, NULL)) {
        c = SDL_LoadFile(path, &n);
        if (!c || n != CARD_SIZE) {
            SDL_free(c);
            return 0;
        }
        card_view_of(c, &v);
    }
    int ok = 1;
    if (!c || v.blank) {
        /* a ready card; a card never used keeps its number */
        ok = p->fresh;
        if (ok)
            card_starter(card, c ? (Uint32)c[4] << 24 | (Uint32)c[5] << 16 | (Uint32)c[6] << 8 | c[7]
                                 : card_new_number());
    } else if (!ced_editable(&v, 1)) {
        ok = 0;
    } else {
        memcpy(card, c, CARD_SIZE);
    }
    SDL_free(c);
    if (!ok)
        return 0;
    card_fields f;
    card_fields_get(card, &f);
    card_edit_apply(&p->edit, &f);
    card_fields_put(card, &f);
    return 1;
}

/* on the machine's thread, every vertical blank (final: as the machine
 * stops, every edit onto its file as the cards are now) */
static void cards_edits(int final)
{
    for (int side = 0; side < 2; side++) {      /* what the menu shows */
        int held = game_machine_card_held(side);
        SDL_LockMutex(shared.lock);
        SDL_strlcpy(unit_card[side], card_ok[side] ? card_in[side] : "", sizeof unit_card[side]);
        unit_held[side] = held;
        SDL_UnlockMutex(shared.lock);
    }
    if (!SDL_GetAtomicInt(&pend_n))
        return;
    static card_pending todo[CARDS_MAX + 2];
    static int waiting[2];
    SDL_LockMutex(shared.lock);
    int n = npend;
    SDL_memcpy(todo, pend, (size_t)n * sizeof todo[0]);
    SDL_UnlockMutex(shared.lock);
    for (int i = 0; i < n; i++) {
        const card_pending *p = &todo[i];
        int side = -1;
        for (int s = 0; s < 2; s++)
            if (card_ok[s] && !SDL_strcasecmp(card_in[s], p->name))
                side = s;
        if (side >= 0) {
            /* a game using the card writes back the copy it read at its
             * start: the edit waits, the card stays in (hw_card_held) */
            if (!final && game_machine_card_held(side)) {
                if (!waiting[side]++)
                    logf_("player %d: the card (%s) takes the menu's changes once the game lets go of it",
                          side + 1, p->name);
                continue;
            }
            cards_save();       /* the game's latest first */
            if (!final) {
                if (waiting[side])
                    logf_("player %d: the game let go of the card (%s) after %d vertical blanks", side + 1, p->name,
                          waiting[side]);
                waiting[side] = 0;
                game_machine_card_take(side);   /* not held: out at once */
                card_ok[side] = 0;      /* out for the change: it goes back in below, in this vertical blank */
            }
        }
        Uint8 card[CARD_SIZE];
        char path[4300], tmp[4400];
        cards_path(path, sizeof path, user_dir, p->name);
        SDL_snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (!card_edited(p->name, p, card))
            logf_("card editor: %s is not a card that can be edited now: the menu's changes are dropped", path);
        else if (!SDL_SaveFile(tmp, card, CARD_SIZE) || !SDL_RenamePath(tmp, path)) {
            logf_("card editor: cannot write %s: %s", path, SDL_GetError());
            SDL_RemovePath(tmp);
        } else
            logf_("card editor: %s written with the menu's changes%s", path, final ? " (as the game stopped)" : "");
        SDL_LockMutex(shared.lock);
        int at = pend_find(p->name);
        if (at >= 0 && pend[at].gen == p->gen) {
            pend[at] = pend[--npend];
            SDL_SetAtomicInt(&pend_n, npend);
        }
        SDL_UnlockMutex(shared.lock);
        if (side >= 0 && !final)
            card_insert(side);  /* card_in[side] still names it */
    }
}

/* the menu's callbacks, on the main thread: an edit Saved (the fields that
 * differ between from and to), and a card as the menu shows it */
static void card_edit_save(const char *name, const card_fields *from, const card_fields *to, int fresh)
{
    SDL_LockMutex(shared.lock);
    int at = pend_find(name);
    if (at < 0 && npend < (int)SDL_arraysize(pend)) {
        at = npend++;
        SDL_zero(pend[at]);
        SDL_strlcpy(pend[at].name, name, sizeof pend[at].name);
    }
    if (at >= 0) {
        card_edit_add(&pend[at].edit, from, to);
        pend[at].fresh |= fresh;
        pend[at].gen++;
    }
    SDL_SetAtomicInt(&pend_n, npend);
    SDL_UnlockMutex(shared.lock);
    if (at < 0)
        logf_("card editor: too many cards changed at once: %s's changes are dropped", name);
    else
        logf_("card editor: %s saved in the menu%s", name, fresh ? " (a ready card)" : "");
}

/* 1 and the card in card, as it will be once its waiting edit is in; 0 no
 * file (none waiting); -1 the file is not a card. *held: a game is using it */
static int card_peek(const char *name, Uint8 *card, int *held)
{
    char path[4300];
    size_t n = 0;
    int file = 0;
    cards_path(path, sizeof path, user_dir, name);
    if (SDL_GetPathInfo(path, NULL)) {
        Uint8 *c = SDL_LoadFile(path, &n);
        file = c && n == CARD_SIZE ? 1 : -1;
        if (file > 0)
            memcpy(card, c, CARD_SIZE);
        SDL_free(c);
    }
    card_pending p;
    int waiting = 0;
    *held = 0;
    SDL_LockMutex(shared.lock);
    int at = pend_find(name);
    if (at >= 0) {
        p = pend[at];
        waiting = 1;
    }
    for (int side = 0; side < 2; side++)
        if (unit_card[side][0] && !SDL_strcasecmp(unit_card[side], name))
            *held = unit_held[side];
    SDL_UnlockMutex(shared.lock);
    if (waiting && file >= 0 && card_edited(name, &p, card))
        file = 1;
    return file;
}

/* on the main thread, as the menu closes: the units' cards as chosen */
static void cards_choose(void)
{
    if (!start_cfg.cards || in_session)
        return;             /* no units this run, or a two-PC game's: the choice is for the next start */
    for (int side = 0; side < 2; side++) {
        if (!strcmp(card_chosen[side], cfg_.card[side]))
            continue;
        SDL_strlcpy(card_chosen[side], cfg_.card[side], sizeof card_chosen[side]);
        logf_("menu: player %d's card: %s", side + 1, card_chosen[side][0] ? card_chosen[side] : "none");
        SDL_LockMutex(shared.lock);
        SDL_strlcpy(card_want[side], card_chosen[side], sizeof card_want[side]);
        SDL_SetAtomicInt(&card_swap[side], 1);
        SDL_UnlockMutex(shared.lock);
    }
}

/* the backup file's bytes (SDL_free them), or NULL: a blank cabinet (no
 * file yet, or one that is not a backup: then backup_ok is 0) */
static unsigned char *backup_read(void)
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
    return b;
}

/* into the machine: the backup (NULL a blank cabinet), the coins (COINS_*)
 * and the game's NETWORK setting (0/1) */
static void backup_apply(const unsigned char *b, int coins, int network)
{
    const char *how = game_machine_backup_put(b, coins == COINS_ARCADE ? GAME_COINS_ARCADE : GAME_COINS_FREE_PLAY);
    if (how)
        logf_("coins: %s", how);
    /* the game's NETWORK setting (docs/design.md section 35) */
    int net = game_machine_network(network ? GAME_NET_ON : GAME_NET_OFF);
    if (net >= 0) {
        logf_("network: %s (the game's own NETWORK setting)", net ? "on" : "off");
    } else if (network) {
        logf_("network: on (a blank backup: the game's default as it makes its settings record)");
    } else {
        net_first = 1;
        logf_("network: off once the game has made its settings record (a blank backup: it makes it as it "
              "starts, with the network on; then it starts again)");
    }
}

static void backup_load(void)
{
    unsigned char *b = backup_read();
    backup_apply(b, cfg_.coins, cfg_.network);
    SDL_free(b);
}

/* ---- the two-PC game (session.h, docs/design.md section 46) ---- */

/* this PC's player as the other PC is told: the card of settings card1
 * (the player at this PC), none when the card units are off here */
static void session_me(session_player *p)
{
    memset(p, 0, sizeof *p);
    p->card = SESSION_CARD_NONE;
    if (!cfg_.cards || !cfg_.card[0][0])
        return;
    SDL_strlcpy(p->name, cfg_.card[0], sizeof p->name);
    char path[4300];
    cards_path(path, sizeof path, user_dir, p->name);
    if (!SDL_GetPathInfo(path, NULL)) {
        p->card = SESSION_CARD_UNUSED;
        return;
    }
    size_t n = 0;
    unsigned char *c = SDL_LoadFile(path, &n);
    if (c && n == GAME_CARD_SIZE) {
        p->card = SESSION_CARD_BYTES;
        memcpy(p->bytes, c, GAME_CARD_SIZE);
    } else {
        logf_("%s is not a card: this player has none in the two-PC game", path);
        p->name[0] = 0;
    }
    SDL_free(c);
}

/* the fingerprint the two PCs compare: main RAM, the registers and the
 * cards in both units. This PC's card is kept as it is at each check, to
 * be written once both PCs have agreed on it (session_card_write) */
static unsigned long long session_hash(unsigned frame)
{
    unsigned long long h = game_machine_hash();
    unsigned char c[GAME_CARD_SIZE];
    for (int side = 0; side < 2; side++)
        if (!game_machine_card_peek(side, c))
            for (int i = 0; i < GAME_CARD_SIZE; i++)
                h = (h ^ c[i]) * 0x100000001b3ull;
    if (own_card[0]) {
        int k = (int)(frame / SESSION_CHECK % SDL_arraysize(own_snap));
        own_snap[k].frame = frame;
        own_snap[k].have = !game_machine_card_peek(sess.side, own_snap[k].c);
    }
    return h;
}

/* this PC's card c into its file (whole, then renamed): 0, or -1 */
static int own_card_file(const unsigned char *c)
{
    char path[4300], tmp[4400];
    cards_path(path, sizeof path, user_dir, own_card);
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!SDL_SaveFile(tmp, c, GAME_CARD_SIZE) || !SDL_RenamePath(tmp, path)) {
        logf_("cannot write the card %s: %s", path, SDL_GetError());
        SDL_RemovePath(tmp);
        return -1;
    }
    memcpy(own_card_last, c, GAME_CARD_SIZE);
    return 0;
}

/* on the machine's thread: this PC's card into its file as it was at the
 * newest check both PCs agreed on, when it changed */
static void session_card_write(void)
{
    unsigned v = session_verified();
    if (!own_card[0] || !v || v == own_card_done)
        return;
    own_card_done = v;
    int k = (int)(v / SESSION_CHECK % SDL_arraysize(own_snap));
    if (own_snap[k].frame != v || !own_snap[k].have || !memcmp(own_snap[k].c, own_card_last, GAME_CARD_SIZE))
        return;
    if (!own_card_file(own_snap[k].c))
        logf_("player %d's card (%s) written as both PCs had it at vertical blank %u", sess.side + 1, own_card, v);
}

/* on the machine's thread, the host's game going on alone: this PC's card
 * into its file as it is now, when it changed (the other player's never) */
static void own_card_alone(unsigned frame)
{
    unsigned char c[GAME_CARD_SIZE];
    if (!own_card[0] || game_machine_card_peek(sess.side, c) || !memcmp(c, own_card_last, GAME_CARD_SIZE))
        return;
    if (!own_card_file(c))
        logf_("player %d's card (%s) written at vertical blank %u, this PC's game alone", sess.side + 1, own_card,
              frame);
}

/* on the machine's thread, before the machine is made: the connection and
 * what both machines start from. 0, or -1 (session_status says why) */
static int session_start_up(void)
{
    int host = session_role() == SESSION_HOST;
    session_me(&sess_mine.own);
    if (host) {
        sess_mine.coins = cfg_.coins;
        sess_mine.region = cfg_.region;
        sess_mine.network = cfg_.network;
        sess_mine.cards = cfg_.cards;
        sess_mine.delay = session_delay;
        sess_mine.clock = clock_start;
        unsigned char *b = backup_read();
        sess_mine.backup = b != NULL;
        if (b)
            memcpy(sess_mine.backup_bytes, b, GAME_BACKUP_SIZE);
        SDL_free(b);
    }
    if (session_connect(&sess_mine, &sess, session_hash))
        return -1;
    late_join = session_late(NULL);
    if (sess.region < 0 || sess.region >= REGIONS)
        sess.region = REGION_JAPAN;
    sess.coins = sess.coins == COINS_ARCADE ? COINS_ARCADE : COINS_FREE_PLAY;
    sess.network = sess.network != 0;
    sess.cards = sess.cards != 0;
    /* what this run is, for the restarts the game makes itself (net_first) and the menu */
    start_cfg.coins = sess.coins;
    start_cfg.region = sess.region;
    start_cfg.network = sess.network;
    start_cfg.cards = sess.cards;
    if (!host) {
        backup_ok = 0;          /* the host's cabinet: this PC's backup.bin is not written */
        clock_whose = sess.clock ? "the host's" : "the fixed date, as the host has it";
    }
    clock_start = sess.clock;
    logf_("two-PC game: this PC is player %d; the host's cabinet%s: coins %s, region %s, network %s, card units %s, "
          "%s; input delay %d", sess.side + 1, late_join ? " as the session began (a game going on)" : "",
          sess.coins == COINS_ARCADE ? "arcade" : "free play", settings_region_text[sess.region],
          sess.network ? "on" : "off", sess.cards ? "on" : "off", sess.backup ? "its backup" : "a blank backup",
          sess.delay);
    return 0;
}

/* after game_machine_init: the host's cabinet and both players' cards. A PC
 * joining a game going on starts from the session's start as it was: both
 * cards then are in memory only, and its own card comes with the host's
 * game (late_after_load) */
static void session_load(void)
{
    backup_apply(sess.backup ? sess.backup_bytes : NULL, sess.coins, sess.network);
    if (!sess.cards)
        return;
    char dir[4300];
    SDL_snprintf(dir, sizeof dir, "%scards", user_dir);
    if (!SDL_GetPathInfo(dir, NULL) && !SDL_CreateDirectory(dir))
        logf_("cannot make %s: %s", dir, SDL_GetError());
    for (int side = 0; side < 2; side++) {      /* in this order on both PCs (a card never used takes a number) */
        const session_player *p = &sess.player[side];
        if (p->card == SESSION_CARD_NONE) {
            logf_("player %d: no card", side + 1);
            continue;
        }
        if (game_machine_card_put(side, p->card == SESSION_CARD_BYTES ? p->bytes : NULL))
            continue;
        logf_("player %d: %s (%s%s)", side + 1, p->name, p->card == SESSION_CARD_BYTES ? "its card" : "a card never used",
              late_join ? ", as the session began: kept in memory only"
              : side == sess.side ? ", this PC's" : ", from the other PC: kept in memory only");
    }
    const session_player *me = &sess.player[sess.side];
    if (!late_join && me->card != SESSION_CARD_NONE && me->name[0]) {
        SDL_strlcpy(own_card, me->name, sizeof own_card);
        game_machine_card_peek(sess.side, own_card_last);
    }
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
static unsigned hash_every;             /* --hash-every: the machine's fingerprint every this many vertical blanks */
static const char *snap_path;           /* --snapshot-at N FILE: the machine at vertical blank snap_at into FILE */
static unsigned snap_at;
static const char *resume_path;         /* --resume FILE: the run goes on from FILE's snapshot */

/* --snapshot-at: the snapshot taken at the end of vertical blank snap_at into its file */
static void snapshot_file(void)
{
    unsigned char *data;
    size_t size;
    unsigned frame;
    int got = game_machine_snapshot_get(&data, &size, &frame);
    if (!got)
        return;
    if (got < 0)
        logf_("snapshot: the machine could not be saved at vertical blank %u", snap_at);
    else {
        char tmp[4300];
        SDL_snprintf(tmp, sizeof tmp, "%s.new", snap_path);
        if (SDL_SaveFile(tmp, data, size) && SDL_RenamePath(tmp, snap_path))
            logf_("snapshot: the machine at the end of vertical blank %u into %s (%zu bytes)", frame, snap_path, size);
        else
            logf_("snapshot: cannot write %s: %s", snap_path, SDL_GetError());
        free(data);
    }
    snap_path = NULL;
}

/* --resume: after the machine is made and the user's files are in */
static int resume_file(void)
{
    size_t n = 0;
    void *data = SDL_LoadFile(resume_path, &n);
    if (!data) {
        logf_("resume: cannot read %s: %s", resume_path, SDL_GetError());
        return 1;
    }
    int bad = game_machine_resume(data, n);
    SDL_free(data);
    if (bad) {
        logf_("resume: %s: %s", resume_path, game_machine_why());
        return 1;
    }
    net_first = 0;          /* the machine's own backup came with it */
    logf_("resume: the machine from %s (%zu bytes)", resume_path, n);
    return 0;
}
static SDL_AtomicInt machine_state;     /* 0 starting, 1 running, 2 init failed, 3 stopped, 4 a two-PC game that
                                         * did not start (no machine) */
static Uint32 machine_event;            /* an SDL event type: the machine has stopped */

/* pacing and how busy the machine is, logged every 10 seconds of frames */
static Uint64 due, busy_since, busy_ns, busy_max_ns;
static unsigned busy_frames, resyncs;

/* How long after its start a frame is handed over to the window's thread
 * (the GPU renderer's list when the game has drawn it, the software picture
 * at the vertical blank): the highest and the lowest of late, each let go
 * slowly (about 8 seconds). The pace by the screen's refresh puts their
 * middle halfway between two refreshes (vsync.h) */
static Uint64 handed_ns, lead_lo, lead_hi;
static Uint64 frame_due;        /* when the frame being run was due by the machine's own clock (0: not paced) */

/* When a frame that was due at start is to be shown, by the game's own clock (vsync.h's vsync_pick,
 * docs/design.md section 59): after the longest hand-over of late (four fifths of a frame at most)
 * and 2 ms more, so that it has come by then. 0 (as it comes) when the machine is not paced */
static Uint64 show_at(Uint64 start)
{
    if (!start)
        return 0;
    return start + (lead_hi > FRAME_NS * 4 / 5 ? FRAME_NS * 4 / 5 : lead_hi) + 2000000;
}

/* machine thread: the GPU renderer's frame (view.c's hook) */
static void gpu_frame(const struct view_gpu_frame *f)
{
    gpu_render_submit_at(f, show_at(frame_due));
    if (f->target)
        return;         /* a pass into a texture (section 78): the frame it is for comes next, and is the one timed */
    handed_ns = SDL_GetTicksNS();
    if (sync_log)
        fprintf(sync_log, "M %u %llu\n", f->frame, (unsigned long long)handed_ns);
}

static void lead_note(Uint64 s)
{
    if (s > FRAME_NS)
        s = FRAME_NS;
    if (!lead_hi)
        lead_lo = lead_hi = s;
    lead_hi = s > lead_hi ? s : lead_hi - (lead_hi - s) / 512;
    lead_lo = s < lead_lo ? s : lead_lo + (s - lead_lo) / 512;
}

/* ---- a PC joining a game going on (docs/design.md section 49) ---- */

/* machine thread: the reference, the machine at the end of the first
 * vertical blank after the start-up (the host, and a PC joining a game
 * going on, whose copy reaches the same vertical blank the same) */
static void session_reference(unsigned frame, int booting)
{
    if (ref_step == 0 && !booting && (session_role() == SESSION_HOST || late_join)) {
        game_machine_snapshot_ask();
        ref_step = 1;
        return;
    }
    if (ref_step != 1)
        return;
    unsigned f;
    int got = game_machine_snapshot_get(&ref_data, &ref_size, &f);
    if (!got)
        return;
    if (got < 0) {
        ref_step = 3;
        logf_("vertical blank %u: this PC's machine could not be kept as the session's reference: %s", frame,
              late_join ? "it cannot join the game going on" : "no other PC can join later");
        if (late_join) {
            late_join = 0;
            session_late_failed("This PC could not keep its copy of the session's start.");
        }
        return;
    }
    ref_step = 2;
    ref_frame = f;
    Uint64 t0 = SDL_GetTicksNS();
    ref_hash = snap_hash(ref_data, ref_size);
    logf_("the session's reference: the machine at the end of vertical blank %u, %zu bytes, fingerprint %016llx "
          "(%.1f ms)", f, ref_size, ref_hash, (SDL_GetTicksNS() - t0) / 1e6);
}

/* the joining PC, at the vertical blank after its reference: the host's
 * machine, its difference from the reference, then the run goes on from
 * it (this does not return); when it cannot, the session ends */
static void late_take(void)
{
    unsigned char *patch, *snap;
    size_t pn, sn;
    unsigned f;
    int card_in;
    late_join = 0;
    if (session_late_get(ref_frame, ref_size, ref_hash, &patch, &pn, &f, &card_in)) {
        free(ref_data);             /* the session ended: session_frame says so */
        ref_data = NULL;
        ref_step = 3;
        return;
    }
    Uint64 t0 = SDL_GetTicksNS();
    int bad = snap_patch_apply(ref_data, ref_size, patch, pn, &snap, &sn);
    free(patch);
    free(ref_data);                 /* a joining PC never hosts: its reference has done its work */
    ref_data = NULL;
    ref_step = 3;
    if (bad) {
        logf_("the host's game came damaged (a difference of %zu bytes)", pn);
        session_late_failed("The host's game came damaged.");
        return;
    }
    logf_("the host's game: its machine at the end of vertical blank %u, %zu bytes from a difference of %zu "
          "(%.0f ms); this PC's card %s", f, sn, pn, (SDL_GetTicksNS() - t0) / 1e6,
          card_in ? "is in its unit" : "stays out");
    late_buf = snap;
    late_loaded = 1;
    late_card_in = card_in;
    due = 0;
    busy_since = 0;
    game_machine_resume(snap, sn);  /* the run goes on from it: does not return */
    late_loaded = 0;
    late_buf = NULL;
    free(snap);
    logf_("the host's game cannot run here: %s", game_machine_why());
    session_late_failed("The host's game cannot run on this PC.");
}

/* the joining PC, at the first vertical blank of the host's game: its own
 * player's card (when it went in) is written as the session agrees on it */
static void late_after_load(unsigned frame)
{
    late_loaded = 0;
    free(late_buf);
    late_buf = NULL;
    const session_player *me = &sess_mine.own;
    sess.player[sess.side] = *me;
    if (late_card_in && me->card != SESSION_CARD_NONE && me->name[0]) {
        SDL_strlcpy(own_card, me->name, sizeof own_card);
        game_machine_card_peek(sess.side, own_card_last);
    }
    logf_("vertical blank %u: the host's game runs here; this PC is player %d (card %s)", frame, sess.side + 1,
          own_card[0] ? own_card : "none");
}

/* the host playing alone, each vertical blank: a joining PC's copy is at
 * the reference; once the card unit of its side is free, the joining
 * player's card goes in and the machine is taken at the end of this
 * vertical blank; at the next its difference from the reference goes over
 * and the session runs again from there */
static void late_host(unsigned frame)
{
    int side = !sess.side;          /* the joining player's */
    if (late_step == 0) {
        if (!session_late_ready(&late_p, &late_same))
            return;
        late_step = 1;
    }
    if (late_step == 1) {
        int held = sess.cards && game_machine_card_take(side);     /* the card of the player who left */
        if (!session_late_still(held)) {
            late_step = 0;
            return;
        }
        if (held)
            return;                 /* it leaves once the game on that side lets go of it */
        late_card_in = sess.cards && late_p.card != SESSION_CARD_NONE &&
                       !game_machine_card_put(side, late_p.card == SESSION_CARD_BYTES ? late_p.bytes : NULL);
        game_machine_snapshot_ask();
        late_step = 2;
        return;
    }
    unsigned char *snap, *patch = NULL;
    size_t sn, pn = 0;
    unsigned f;
    int got = game_machine_snapshot_get(&snap, &sn, &f);
    if (!got)
        return;
    late_step = 0;
    Uint64 t0 = SDL_GetTicksNS();
    int bad = got < 0;
    if (!bad) {
        bad = snap_patch_make(late_same ? ref_data : NULL, late_same ? ref_size : 0, snap, sn, &patch, &pn);
        free(snap);
    }
    if (bad) {
        logf_("vertical blank %u: this game could not be copied for player %d's PC", frame, side + 1);
        session_late_cancel("The host could not copy its game.");
        if (late_card_in)
            game_machine_card_take(side);
        return;
    }
    logf_("vertical blank %u: this game at the end of vertical blank %u (%zu bytes) for player %d's PC: a "
          "difference of %zu bytes from %s (%.0f ms)", frame, f, sn, side + 1, pn,
          late_same ? "the reference" : "nothing", (SDL_GetTicksNS() - t0) / 1e6);
    if (session_late_send(patch, pn, f, late_card_in)) {
        logf_("player %d's PC has gone: this game goes on alone", side + 1);
        if (late_card_in)
            game_machine_card_take(side);
        return;
    }
    sess.player[side] = late_p;
    session_alone = 0;
    session_stopped = 0;
    SDL_SetAtomicInt(&alone_ms, 0);
    due = 0;
    busy_since = 0;
    logf_("vertical blank %u: player %d joined the game going on (card %s); the two-PC game runs again from "
          "vertical blank %u", frame, side + 1, late_card_in ? late_p.name : "none", f + 1);
}

/* machine thread, with the machine's line every 600 vertical blanks: the
 * frames the game drew since the line before, those of them never shown (a
 * newer one came before a present took them), the presents, and what paces
 * the machine (docs/design.md section 58). Nothing without a window */
static int pace_text(char *by, size_t n)
{
    if (SDL_GetAtomicInt(&pacing_now) != PACING_GAME) {
        vsync_describe(by, n);
        return 0;
    }
    double hz;
    unsigned sooner, later;
    int on = vsync_pick_stats(&hz, &sooner, &later);
    if (on)
        SDL_snprintf(by, n, "the game's own clock, each frame put on a refresh of the screen (%.3f Hz)", hz);
    else
        SDL_snprintf(by, n, "the game's own clock, the newest frame at each present (the screen's refresh is "
                     "not steady, or not measured yet)");
    return on;
}

static void screen_line(void)
{
    static unsigned made0, lost0, presents0, sooner0, later0;
    unsigned sub = 0, rep = 0, presents = (unsigned)SDL_GetAtomicInt(&presents_n), sooner, later;
    double hz;
    gpu_render_counts(&sub, &rep);
    SDL_LockMutex(shared.lock);
    unsigned made = sub + pics_made, lost = rep + pics_lost;
    SDL_UnlockMutex(shared.lock);
    vsync_pick_stats(&hz, &sooner, &later);
    if (presents != presents0) {
        char by[200], steps[96] = "";
        if (pace_text(by, sizeof by))
            SDL_snprintf(steps, sizeof steps, ": %u moved a refresh sooner, %u later", sooner - sooner0,
                         later - later0);
        logf_("screen: %u frames drawn, %u of them never shown; %u presents; paced by %s%s", made - made0,
              lost - lost0, presents - presents0, by, steps);
    }
    made0 = made;
    lost0 = lost;
    presents0 = presents;
    sooner0 = sooner;
    later0 = later;
}

/* machine thread, once per vertical blank (game_frame_fn) */
static void on_frame(unsigned frame, const unsigned char *rgb, int w, int booting, game_input *in)
{
    if (hash_every && frame % hash_every == 0) {        /* --hash-every: before this vertical blank's inputs */
        Uint64 t0 = SDL_GetTicksNS();
        unsigned long long h = game_machine_hash();
        logf_("state: vertical blank %u: %016llx (%.2f ms)", frame, h, (SDL_GetTicksNS() - t0) / 1e6);
    }
    {
        int stage, chara[2];
        /* traced water (section 81): the Shrine's reflection is a mirror of its scenery in the game
         * too, and the rays take its place; the other stages' are pictures of the game's own making,
         * which stay, with the mirror over them */
        gpu_render_water(game_machine_stage() == 21 ? 1 : 2);
        if (game_machine_match(&stage, chara))
            logf_("match: stage %d; player 1's character %d, player 2's %d (the game's own numbers), loading at "
                  "vertical blank %u", stage, chara[0], chara[1], frame);
    }
    if (snap_path && frame == snap_at)
        game_machine_snapshot_ask();
    else if (snap_path && frame > snap_at)
        snapshot_file();
    Uint64 now = SDL_GetTicksNS();
    if (busy_since) {
        Uint64 b = now - busy_since;
        busy_ns += b;
        if (b > busy_max_ns)
            busy_max_ns = b;
        if (machine_path != 1)
            lead_note(b);                           /* the software picture goes over below */
        else if (handed_ns > busy_since)
            lead_note(handed_ns - busy_since);      /* a frame the game drew */
        if (++busy_frames == 600) {
            logf_("machine: vertical blank %u; %.2f ms a frame on average, %.2f ms at most; %u times behind by "
                  "more than 1/4 s; audio queue %.0f ms", frame, busy_ns / 1e6 / busy_frames, busy_max_ns / 1e6, resyncs,
                  audio_level / 44.1);
            busy_ns = busy_max_ns = 0;
            busy_frames = 0;
            screen_line();
        }
    }
    audio_frame((booting && !real_boot) || unpaced);
    SDL_LockMutex(shared.lock);
    if (rgb && machine_path != 1 && w >= PIC_W && w <= PIC_W_MAX) {     /* the GPU renderer's: not this */
        pics_made++;
        if (shared.pic_n == 2) {        /* no room: the oldest waiting is never shown */
            pics_lost++;
            memcpy(shared.pic[0], shared.pic[1], (size_t)shared.pic_w[1] * PIC_H * 3);
            shared.pic_w[0] = shared.pic_w[1];
            shared.pic_frame[0] = shared.pic_frame[1];
            shared.pic_pts[0] = shared.pic_pts[1];
            shared.pic_n = 1;
        }
        int at = shared.pic_n++;
        memcpy(shared.pic[at], rgb, (size_t)w * PIC_H * 3);
        shared.pic_w[at] = w;
        shared.pic_frame[at] = frame;
        shared.pic_pts[at] = show_at(frame_due);
    }
    *in = shared.in;
    SDL_UnlockMutex(shared.lock);
    SDL_SetAtomicInt(&frame_now, (int)frame);
    SDL_SetAtomicInt(&program_now, game_machine_program());
    if (SDL_GetAtomicInt(&test_frames) > 0) {       /* the menu's Test switch */
        SDL_AddAtomicInt(&test_frames, -1);
        in->sys |= GAME_SYS_TEST;
    }
    for (int i = 0; i < presses; i++)
        if (frame >= press[i].frame && frame < press[i].frame + 6) {
            in->player[press[i].side] |= press[i].bits;
            in->sys |= press[i].sys;
            if (press[i].coin)
                in->coin[press[i].side] = 1;
        }
    /* a two-PC game: this PC's player is its player-1 controls (and --press's p1); the inputs of
     * this vertical blank are the session's, both players', the same on both PCs. Once it has
     * ended the host's game goes on alone (docs/design.md section 47): its player's controls,
     * now with no input delay, the other player's none, and another PC can join it (section
     * 49). The joining PC's machine waits until the window closes, its note saying why (a test
     * run stops) */
    int skip_wait = 0;                      /* going back: the vertical blanks run again take this one's wait */
    Uint64 pace_more = 0;
    if (in_session) {
        game_input mine = *in;
        if (late_loaded)
            late_after_load(frame);
        session_reference(frame, booting);
        if (late_join && ref_step == 2)
            late_take();                    /* does not return once the host's game is in */
        if (session_alone)
            late_host(frame);
        if (!session_stopped && game_machine_back_failed())
            session_fail("This PC could not go back far enough to put a wrong guess right, so the two games are "
                         "no longer the same.");
        if (!session_stopped && frame % BACKUP_EVERY == 0 && backup_ok) {
            game_machine_backup_peek(backup_held);
            backup_held_at = frame;
            backup_held_on = 1;
        }
        session_back back = { .kept_from = game_machine_kept_from() };
        int playing = !session_stopped && !session_frame(frame, booting, mine.player[0], (int)mine.coin[0], in, &back);
        if (playing) {
            game_machine_keep(back.keep);
            if (back.redo) {
                game_machine_go_back(back.redo);
                skip_wait = 1;
            }
            if (backup_held_on && back.settled + 1 >= backup_held_at) {
                backup_write(backup_held);
                backup_held_on = 0;
            }
            pace_more = back.pace_ns;
            if (back.guessing && back.waited_ns > FRAME_NS / 2)
                due = 0;                    /* it waited: the pace starts again from now, no catching up */
            if (frame % 600 == 0) {
                game_back_stats bs;
                game_machine_back_stats(&bs);
                if (bs.backs)
                    logf_("rollback: vertical blank %u: went back %u times, %u vertical blanks run again; %.2f ms "
                          "each time on average, %.2f ms at most", frame, bs.backs, bs.frames, bs.ms / bs.backs,
                          bs.ms_most);
            }
        } else if (!session_stopped) {
            session_stopped = 1;
            game_machine_keep(0);
            backup_held_on = 0;
            backup_save();
            if (session_role() == SESSION_HOST && !SDL_GetAtomicInt(&window_quit)) {
                session_alone = 1;
                logf_("vertical blank %u: the two-PC game is over; this PC's game goes on alone (player %d)", frame,
                      sess.side + 1);
                /* the other PC is told; another can join, or (no reference) the network thread stops */
                if (ref_step != 2 || session_open(ref_frame, ref_size, ref_hash))
                    session_end();
                SDL_SetAtomicInt(&alone_ms, (int)((Uint32)SDL_GetTicks() | 1));
            } else {
                logf_("vertical blank %u: the two-PC game is over", frame);
                SDL_SetAtomicInt(&session_over, 1);
            }
        }
        if (session_alone) {
            memset(in, 0, sizeof *in);
            in->player[sess.side] = mine.player[0];
            in->coin[sess.side] = mine.coin[0];
            if (frame % SESSION_CHECK == 0)
                own_card_alone(frame);
        } else if (session_stopped) {
            memset(in, 0, sizeof *in);
            if (frames_max)
                game_machine_stop();
            else
                while (!SDL_GetAtomicInt(&window_quit))
                    SDL_Delay(10);
            return;
        } else {
            session_card_write();
        }
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
    while (art_i < art_n && art_list[art_i] <= frame) {
        if (art_list[art_i] == frame) {
            SDL_SetAtomicInt(&shot_pause, art_i + 1);
            while (SDL_GetAtomicInt(&shot_pause))
                SDL_Delay(1);
        }
        art_i++;
    }
    if (frame % BACKUP_EVERY == 0) {
        if (!in_session || session_stopped)
            backup_save();                  /* a two-PC game's: held above until it is sure */
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

    /* the menu: the machine waits while it is open; a restart from it
     * leaves this function for the power-on state (rt_native_restart) */
    if (SDL_GetAtomicInt(&menu_pause)) {
        cards_save();       /* the units' cards in their files, for the menu's Cards tab */
        while (SDL_GetAtomicInt(&menu_pause))
            SDL_Delay(5);
        due = 0;
    }
    /* the renderer the window's thread has now (section 53): its passes to
     * the GPU renderer, or the software picture, from the next one on */
    int path = SDL_GetAtomicInt(&gpu_path);
    if (path != machine_path) {
        game_machine_gpu(path ? gpu_frame : NULL);
        if (machine_path >= 0)
            logf_("vertical blank %u: the machine draws for the %s from now on", frame,
                  path ? "GPU renderer" : "software picture");
        machine_path = path;
    }
    cards_edits(0);         /* the menu's card edits, then its card choices */
    cards_swap();
    int restart = SDL_GetAtomicInt(&restart_req);
    if (restart) {
        SDL_SetAtomicInt(&restart_req, 0);
        logf_("vertical blank %u: the game starts again (the menu's Restart), region %s", frame,
              settings_region_text[start_cfg.region]);
        backup_save();
        cards_save();
        due = 0;
        busy_since = 0;
        game_machine_region(start_cfg.region);      /* the game asks for it as it starts again */
        game_machine_restart(restart, start_cfg.network ? GAME_NET_ON : GAME_NET_OFF);
        logf_("restart: the machine's power-on state was not kept");
    }
    /* a blank backup: the game has just made its settings record, with its
     * default (the network on), early in its first start-up step. The
     * setting goes in, and the board starts again, once, as an operator's
     * would after changing it in the test menu */
    if (net_first && !booting) {
        net_first = 0;
        logf_("vertical blank %u: no settings record by the end of the game's start-up: the network stays on "
              "this time", frame);
    }
    if (net_first && !game_machine_program() && game_machine_net_ready()) {
        net_first = 0;
        logf_("vertical blank %u: the game made its settings record; the network goes off in it, and the game "
              "starts again", frame);
        due = 0;
        busy_since = 0;
        game_machine_restart(start_cfg.coins == COINS_ARCADE ? GAME_COINS_ARCADE : GAME_COINS_FREE_PLAY, GAME_NET_OFF);
        logf_("restart: the machine's power-on state was not kept");
    }

    /* the frame is due 1/60 s after the one before; a long stall (loading)
     * is not caught up in a burst. The game's first start-up step runs as
     * fast as it can unless --real-boot: pacing never changes the run. The
     * time is taken again here: the two-PC game may have waited above. Ahead
     * of the other PC (both guessing), a little later (pace_more).
     * Frame pacing (docs/design.md sections 58 and 59): Game clock keeps
     * this timer, and the window's thread puts each frame on a refresh;
     * Screen refresh has the frame due by the screen's refresh when that
     * can be followed (a two-PC game keeps the timer, since both PCs must
     * go at one rate); Off is the timer and the newest frame */
    if (booting && !real_boot) {
        due = 0;
        frame_due = 0;
        vsync_free("the game is starting up");
    } else if (unpaced) {
        frame_due = 0;
        vsync_free("--unpaced");
    } else {
        now = SDL_GetTicksNS();
        int fresh = 0;
        if (!due || now > due + 250000000ull) {
            if (due)
                resyncs++;
            due = now;
            fresh = 1;
        }
        Uint64 by_screen = 0;
        int pacing = SDL_GetAtomicInt(&pacing_now);
        if (in_session)
            vsync_free("a two-PC game");
        else if (pacing == PACING_SCREEN)
            by_screen = vsync_next(now, due, frame_ns, (lead_lo + lead_hi) / 2, fresh);
        else
            vsync_free(pacing == PACING_OFF ? "Frame pacing is off" : "Frame pacing: Game clock");
        due = by_screen ? by_screen : due + frame_ns + pace_more;
        frame_due = due;
        /* what paces it, said when it changes (not more than every 2 seconds) */
        static int said = -1;
        static Uint64 said_at;
        double hz;
        unsigned sooner, later;
        int state = pacing * 4 + (by_screen != 0) * 2 + (pacing == PACING_GAME && vsync_pick_stats(&hz, &sooner, &later));
        if (state != said && (said < 0 || now - said_at > 2000000000ull)) {
            char by[200];
            pace_text(by, sizeof by);
            logf_("pace: vertical blank %u: by %s", frame, by);
            said = state;
            said_at = now;
        }
        if (due > now && !skip_wait)
            SDL_DelayPrecise(due - now);
    }
    busy_since = SDL_GetTicksNS();
}

/* machine thread: a two-PC game's vertical blank run again (session_back's
 * redo; game_replay_fn): its controls from the session, and its fingerprint
 * and the host's backup taken again (the first run's were from a guess) */
static void on_replay(unsigned frame, game_input *in)
{
    if (hash_every && frame % hash_every == 0)
        logf_("state: vertical blank %u: %016llx (run again)", frame, game_machine_hash());
    if (frame % BACKUP_EVERY == 0 && backup_ok) {
        game_machine_backup_peek(backup_held);
        backup_held_at = frame;
        backup_held_on = 1;
    }
    session_replay(frame, in);
}

static int SDLCALL machine_main(void *arg)
{
    (void)arg;
    /* a two-PC game: connected first, then the host's cabinet; the sound chip runs on both PCs
     * alike (with sound off nothing is played), since its model is part of the machine */
    if (in_session && session_start_up()) {
        SDL_SetAtomicInt(&machine_state, 4);
        return 0;           /* the window shows why until it is closed */
    }
    game_machine_cards(in_session ? sess.cards : cfg_.cards);
    game_machine_region(in_session ? sess.region : cfg_.region);
    game_machine_clock(clock_start);
    int made = !game_machine_init(image_path, dimm_path, dimm_key, in_session || !silent, on_frame);
    if (made) {
        if (in_session) {
            game_machine_replay(on_replay);
            session_load();
        } else {
            backup_load();
            cards_load();
        }
        if (resume_path && resume_file())
            made = 0;
    }
    if (made) {
        int d[6];
        game_machine_clock_get(d);
        logf_("clock: the board's clock started at %04d-%02d-%02d %02d:%02d:%02d (%s)", d[0], d[1], d[2], d[3], d[4],
              d[5], resume_path ? "the snapshot's" : clock_whose);
    }
    if (!made) {
        SDL_SetAtomicInt(&machine_state, 2);
    } else {
        SDL_SetAtomicInt(&machine_state, 1);
        int bad = game_machine_run();
        logf_("machine stopped%s: %s", bad ? " by itself" : "", game_machine_why());
        if (snap_path)
            snapshot_file();
        backup_save();
        cards_save();
        if (session_alone)
            own_card_alone((unsigned)SDL_GetAtomicInt(&frame_now));
        cards_edits(1);     /* edits still waiting (a game held the card): onto its last state */
        SDL_SetAtomicInt(&machine_state, 3);
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = machine_event;
    SDL_PushEvent(&ev);
    return 0;
}

/* the machine goes on to its stop at its next vertical blank (at once if it
 * is not running), whatever it waits for: the window closing, or the
 * compositor's watch (window_stuck). Its thread saves the backup and the
 * cards as it ends (machine_main) */
static void machine_stop_ask(void)
{
    SDL_SetAtomicInt(&shot_pause, 0);
    SDL_SetAtomicInt(&menu_pause, 0);       /* a machine waiting for the menu goes on to its stop */
    SDL_SetAtomicInt(&window_quit, 1);      /* and one whose two-PC game ended */
    if (in_session)
        session_cancel();                   /* stops waiting (to connect, or for the other's inputs) */
    int stopping = SDL_GetAtomicInt(&machine_state) < 2;
    game_machine_stop();
    if (stopping)
        logf_("stopping the machine");
}

/* ---- the compositor going away (docs/design.md section 54) ----
 * A frame waits for a swapchain buffer (SDL_GPU's acquire, with no time
 * limit) that a compositor gone never gives back: killed or crashed, and on
 * the Radeon even ending cleanly. The machine runs on, but the main thread
 * never comes back to close the game. compositor.c's watch ends it: once the
 * compositor has closed its connection, a frame still being drawn 3 seconds
 * on has the watch's thread stop the machine, which saves as on any quit,
 * and end the game (window_stuck). A compositor gone between frames ends the
 * loop as usual (display_gone); the window is then not closed, since NVIDIA's
 * driver crashes destroying its swapchain (a workaround: gone_close). Wayland
 * only */
#define STOP_WAIT_MS 5000   /* how long the watch waits for the machine's stop */

/* the compositor has gone (compositor.c): said once */
static int display_gone(void)
{
    static int said;
    int err = 0;
    if (!compositor_gone(&err))
        return 0;
    if (!said++) {
        if (err)
            logf_("window: the compositor's connection has failed (%s): the game closes", strerror(err));
        else
            logf_("window: the compositor has closed its connection: the game closes");
    }
    return 1;
}

/* the game ends with the compositor gone, the machine stopped (its saves
 * made): what does not need the window (an update asked for in the menu
 * goes to the launcher), then the caller ends the process without closing
 * the window. The window's closing would give buffers back to a compositor
 * that is not there, and NVIDIA's driver (620.70) crashes in it then:
 * vkDestroySwapchainKHR, under SDL_ReleaseWindowFromGPUDevice. A workaround
 * for the driver's fault; nothing is lost by it, as the process is ending */
static void gone_close(int code)
{
    gu_quit();
    logf_("the compositor has gone: the game ends without closing its window (exit code %d)", code);
}

/* The window's thread, watched (docs/design.md section 83). A picture that freezes while the
 * game runs on underneath is the window's thread not coming back from one of its steps (issue 3:
 * on one PC, in full screen; never seen here). The thread says which step it is at (win_at). A
 * thread of its own (win_watch) looks four times a second, and when the step has stood for
 * WIN_STUCK_MS it says so in the log, with what the window and the machine were last known to
 * be; again while it lasts; and when the thread goes on. It only reports: nothing is done about
 * the wait, and nothing is asked of SDL or the window from the watch's thread. */
enum { WIN_LOOP, WIN_SWITCH, WIN_EARLIER, WIN_ACQUIRE, WIN_DRAW, WIN_SUBMIT, WIN_AFTER, WIN_STOP, WIN_CLOSE, WIN_STEPS };
static const char *const win_step_name[WIN_STEPS] = {
    "between frames (events, the menu)",
    "starting the renderer again",
    "waiting for the graphics chip to finish an earlier frame (SDL_WaitForGPUSwapchain)",
    "waiting for the screen's next buffer (SDL_AcquireGPUSwapchainTexture)",
    "drawing the frame",
    "handing the frame to the graphics driver (SDL_SubmitGPUCommandBuffer)",
    "after the frame (pictures read back)",
    "ending (the machine stops, its records are saved)",
    "closing the window",
};
#define WIN_STUCK_MS 2000
static SDL_AtomicInt win_step;          /* WIN_*: what the window's thread is doing */
static SDL_AtomicInt win_step_at;       /* since when (SDL_GetTicks) */
static SDL_AtomicInt win_turn;          /* turns of its loop */
static SDL_AtomicInt win_flags_now;     /* the window's flags at the turn's start (SDL_WINDOW_*, the low 32 bits) */
static SDL_AtomicInt win_watch_on;
static SDL_Semaphore *win_watch_wake;   /* given once, as the game closes: the watch does not sleep its time out */
static int win_stall_turn, win_stall_ms, win_stall_part;   /* --stall-window */

static void win_at(int step)
{
    SDL_SetAtomicInt(&win_step, step);
    SDL_SetAtomicInt(&win_step_at, (int)(Uint32)SDL_GetTicks());
}

static int win_watch(void *arg)
{
    (void)arg;
    Uint32 held_at = (Uint32)SDL_GetAtomicInt(&win_step_at), next_ms = WIN_STUCK_MS;
    int held_step = SDL_GetAtomicInt(&win_step), told = 0;
    while (SDL_GetAtomicInt(&win_watch_on)) {
        if (!win_watch_wake)
            SDL_Delay(250);
        else if (SDL_WaitSemaphoreTimeout(win_watch_wake, 250))
            break;                      /* the game is closing */
        Uint32 at = (Uint32)SDL_GetAtomicInt(&win_step_at), now = (Uint32)SDL_GetTicks();
        int step = SDL_GetAtomicInt(&win_step);
        if (step < 0 || step >= WIN_STEPS)
            step = WIN_LOOP;
        if (at != held_at || step != held_step) {
            if (told)
                logf_("window: its thread went on after %.1f s (it was %s)", (double)(at - held_at) / 1000.0,
                      win_step_name[held_step]);
            held_at = at;
            held_step = step;
            told = 0;
            next_ms = WIN_STUCK_MS;
            continue;
        }
        Uint32 held = now - at;
        if (held < next_ms)
            continue;
        char idle[80] = "";             /* section 85: Windows' own count, where there is one */
        int idle_s = sys_idle_seconds();
        if (idle_s >= 0)
            SDL_snprintf(idle, sizeof idle, "; the system's last key or mouse input: %d s ago", idle_s);
        if (!told) {
            Uint32 fl = (Uint32)SDL_GetAtomicInt(&win_flags_now);
            int st = SDL_GetAtomicInt(&machine_state);
            logf_("window: its thread has not gone on for %.0f s: it is %s (turn %d of its loop, %d presents so far; "
                  "the window: %s%s%s%s; the machine: vertical blank %d, %s%s)", (double)held / 1000.0, win_step_name[step],
                  SDL_GetAtomicInt(&win_turn), SDL_GetAtomicInt(&presents_n),
                  fl & SDL_WINDOW_FULLSCREEN ? "fullscreen" : "not fullscreen", fl & SDL_WINDOW_MINIMIZED ? ", minimized" : "",
                  fl & SDL_WINDOW_OCCLUDED ? ", covered" : "", fl & SDL_WINDOW_INPUT_FOCUS ? ", in front" : ", not in front",
                  SDL_GetAtomicInt(&frame_now),
                  st != 1 ? "not running" : SDL_GetAtomicInt(&menu_pause) ? "held by the menu" : "running", idle);
        } else {
            logf_("window: its thread is still %s after %.0f s (the machine: vertical blank %d%s)", win_step_name[step],
                  (double)held / 1000.0, SDL_GetAtomicInt(&frame_now), idle);
        }
        told = 1;
        next_ms = held < 10000 ? 10000 : held < 30000 ? 30000 : (held / 60000 + 1) * 60000;
    }
    return 0;
}

/* the watch's thread (compositor.c): the compositor has gone and the main
 * thread's frame has not finished in COMPOSITOR_STUCK_MS. The machine
 * stops and saves as on any quit, then the game ends: the exit code. The
 * main thread never goes on (compositor_busy) */
static int window_stuck(void)
{
    logf_("window: the compositor has closed its connection and the frame being drawn has not finished in %d s: "
          "the machine stops, and the game ends once it has saved", COMPOSITOR_STUCK_MS / 1000);
    machine_stop_ask();
    Uint64 t = SDL_GetTicks();
    int st;
    while ((st = SDL_GetAtomicInt(&machine_state)) < 2 && SDL_GetTicks() - t < STOP_WAIT_MS)
        SDL_Delay(10);
    if (st < 2)
        logf_("the machine has not stopped in %d s: the backup it wrote last (every 10 seconds) stays",
              STOP_WAIT_MS / 1000);
    else if (in_session)
        session_end();                      /* the other PC is told */
    gone_close(1);
    return 1;
}

/* --art: the machine runs unpaced and waits at each vertical blank of the
 * artwork (on_frame); its newest picture is drawn and read back, and art.c
 * makes the files. No window: nothing is shown. Exit 0 once every picture
 * was made, else 1 */
static void art_say(const char *msg)
{
    logf_("%s", msg);
}

typedef struct { unsigned frame; int made; } art_ctx;

static void art_use(void *ud, const void *px, Uint32 w, Uint32 h)
{
    art_ctx *c = ud;
    c->made += art_take(art_dir, c->frame, px, w, h, art_say);
}

static int art_loop(SDL_GPUDevice *gpu, SDL_Thread *machine)
{
    int running = 1, turns = 0, taken = 0;
    art_ctx ctx = { 0, 0 };
    Uint64 t0 = SDL_GetTicksNS();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == machine_event || ev.type == SDL_EVENT_QUIT)
                running = 0;
        int sp = SDL_GetAtomicInt(&shot_pause);
        if (!sp) {
            SDL_Delay(1);
            continue;
        }
        /* the picture of the vertical blank the machine waits at: drawn on
         * the third turn, as a --shot's (frames still in flight come first) */
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
        if (!cmd) {
            logf_("art: %s", SDL_GetError());
            break;
        }
        Uint32 w = 0, h = 0;
        unsigned gf = 0;
        SDL_GPUTexture *t = gpu_render_frame(cmd, &w, &h, &gf, ~0u);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (t && ++turns >= 3) {
            ctx.frame = art_list[sp - 1];
            read_back(gpu, t, w, h, art_use, &ctx);
            taken++;
            turns = 0;
            SDL_SetAtomicInt(&shot_pause, 0);
            if (sp == art_n)
                running = 0;
        }
    }
    SDL_SetAtomicInt(&shot_pause, 0);
    game_machine_stop();
    SDL_WaitThread(machine, NULL);
    gpu_render_quit();
    SDL_DestroyGPUDevice(gpu);
    logf_("art: %d of %d scenes taken, %d files, in %.1f s", taken, art_n, ctx.made,
          (SDL_GetTicksNS() - t0) / 1e9);
    SDL_Quit();
    if (log_file && log_file != stderr)
        fclose(log_file);
    return taken == art_n && ctx.made ? 0 : 1;
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
static pad_set pads[2][ACT_COUNT];
static const unsigned act_bits[ACT_COUNT] = {
    GAME_BTN_UP, GAME_BTN_DOWN, GAME_BTN_LEFT, GAME_BTN_RIGHT, GAME_BTN_G, GAME_BTN_P, GAME_BTN_K,
    GAME_BTN_P | GAME_BTN_G, GAME_BTN_P | GAME_BTN_K, GAME_BTN_P | GAME_BTN_K | GAME_BTN_G, GAME_BTN_K | GAME_BTN_G,
    GAME_BTN_START, 0,
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
        for (int p = 0; p < 2; p++) {
            pad_set *g = &pads[p][a];
            g->n = 0;
            for (int i = 0; i < BIND_MAX; i++) {
                const char *name = cfg_.pad[p][a].name[i];
                if (!name[0])
                    continue;
                SDL_GamepadButton bt = SDL_GetGamepadButtonFromString(name);
                SDL_GamepadAxis ax = SDL_GetGamepadAxisFromString(name);
                if (bt != SDL_GAMEPAD_BUTTON_INVALID)
                    g->in[g->n++] = bt;
                else if (ax == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ax == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                    g->in[g->n++] = -1 - ax;
                else
                    logf_("settings: pad%d.%s: no gamepad button is called \"%s\"", p + 1, settings_action_name[a],
                          name);
            }
        }
    }
}

/* inputs kept from the game: gamepad buttons (any pad) and keys, while the
 * menu's chord or operator bar has them, or until they are let go after the
 * menu closed (the button that closed it is not the game's) */
static unsigned pad_ignore;
static int pad_ignore_bar;              /* and each pad's operator bar buttons (they go by label: ui_pad_face) */
static bool key_ignore[SDL_SCANCODE_COUNT];
static unsigned pads_raw;               /* the buttons of both players' gamepads, together */
static unsigned pad_raw[2];             /* each player's gamepad's buttons */
static SDL_JoystickID pad_id[2];        /* each player's gamepad now (0: none): ui_pad_players */
#define CHORD ((1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START))     /* View + Menu: the menu */

/* the log says whose gamepad is whose, each time that changes */
static void pads_note(const SDL_JoystickID ids[2])
{
    static SDL_JoystickID was[2];
    static int said;
    if (said && ids[0] == was[0] && ids[1] == was[1])
        return;
    said = 1;
    was[0] = ids[0];
    was[1] = ids[1];
    char t[2][96];
    for (int p = 0; p < 2; p++) {
        if (ids[p])
            ui_pad_title(ids[p], t[p], sizeof t[p]);
        else
            SDL_strlcpy(t[p], "none", sizeof t[p]);
    }
    logf_("gamepads: player 1: %s; player 2: %s", t[0], t[1]);
}

/* everything at once; returns CHORD when a player's gamepad holds View + Menu */
static unsigned read_controls(game_input *in)
{
    memset(in, 0, sizeof *in);
    const bool *k = SDL_GetKeyboardState(NULL);
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < ACT_COUNT; a++)
            for (int i = 0; i < keys[p][a].n; i++)
                if (k[keys[p][a].sc[i]] && !key_ignore[keys[p][a].sc[i]]) {
                    in->player[p] |= act_bits[a];
                    act_extra(in, p, a);
                }
    /* fixed: slot 2's coin as M3 had it */
    if (k[SDL_SCANCODE_6]) in->coin[1] = 1;

    unsigned first = 0;
    pads_raw = pad_raw[0] = pad_raw[1] = 0;
    /* each player's gamepad: the one the settings give them, else the pads
     * in the order they connected, without Steam Input's twins (ui.h) */
    const char *const wish[2] = { cfg_.pad_wish[0], cfg_.pad_wish[1] };
    SDL_JoystickID ids[2];
    ui_pad_players(wish, ids);
    pads_note(ids);
    for (int i = 0; i < 2; i++) {
        pad_id[i] = ids[i];
        SDL_Gamepad *g = ids[i] ? SDL_GetGamepadFromID(ids[i]) : NULL;
        if (!g)
            continue;
        unsigned b = 0, raw = 0, ignore = pad_ignore;
        for (int j = 0; j < SDL_GAMEPAD_BUTTON_COUNT && j < 32; j++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)j))
                raw |= 1u << j;
        if (pad_ignore_bar)     /* A, X and B by label: Y stays the game's */
            for (int f = SDL_GAMEPAD_BUTTON_SOUTH; f <= SDL_GAMEPAD_BUTTON_NORTH; f++)
                if (ui_pad_face(ids[i], f) != UI_IN_DEFAULT)
                    ignore |= 1u << f;
        b |= stick_bits(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f,
                        SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f);
        for (int a = 0; a < ACT_COUNT; a++)
            for (int j = 0; j < pads[i][a].n; j++) {
                int x = pads[i][a].in[j];
                int on = x >= 0 ? (x < 32 && (raw >> x & 1) && !(ignore >> x & 1))
                                : SDL_GetGamepadAxis(g, (SDL_GamepadAxis)(-1 - x)) > 16000;
                if (on) {
                    b |= act_bits[a];
                    act_extra(in, i, a);
                }
            }
        in->player[i] |= b;
        pads_raw |= raw;
        pad_raw[i] = raw;
        if ((raw & CHORD) == CHORD)
            first |= CHORD;
    }
    /* a stick held between two directions that cancel gives neither */
    for (int i = 0; i < 2; i++) {
        if ((in->player[i] & (GAME_BTN_LEFT | GAME_BTN_RIGHT)) == (GAME_BTN_LEFT | GAME_BTN_RIGHT))
            in->player[i] &= ~(unsigned)(GAME_BTN_LEFT | GAME_BTN_RIGHT);
        if ((in->player[i] & (GAME_BTN_UP | GAME_BTN_DOWN)) == (GAME_BTN_UP | GAME_BTN_DOWN))
            in->player[i] &= ~(unsigned)(GAME_BTN_UP | GAME_BTN_DOWN);
    }
    return first;
}

/* ui.c's lines (which pad is left out) */
static void log_line(const char *line)
{
    logf_("%s", line);
}

static void log_gamepad_added(SDL_JoystickID which)
{
    /* Under Steam with Steam Input on, a pad seen here can be Steam's
     * virtual one (Valve vendor 28de, product 11ff: an Xbox pad to SDL on
     * Windows and under Proton). Under Proton the real pad behind it is seen
     * too, and ui.c leaves it out (design.md s30, its own log lines). Logged
     * so a run shows which pads were there. Valve's own pads (Steam
     * Controller, the Deck's 1205) share the vendor, so only 11ff is tagged
     * virtual. */
    SDL_Gamepad *g = SDL_OpenGamepad(which);
    if (!g) {
        logf_("gamepad %u: open failed: %s", (unsigned)which, SDL_GetError());
        return;
    }
    Uint16 vid = SDL_GetGamepadVendor(g), pid = SDL_GetGamepadProduct(g);
    const char *path = SDL_GetGamepadPath(g);
    logf_("gamepad added: id %u \"%s\" type %s vid %04x pid %04x%s steam-handle %llx player %d path %s",
          (unsigned)which, SDL_GetGamepadName(g), SDL_GetGamepadStringForType(SDL_GetGamepadType(g)), vid, pid,
          vid == 0x28de && pid == 0x11ff ? " (Steam virtual)" : "", (unsigned long long)SDL_GetGamepadSteamHandle(g),
          SDL_GetGamepadPlayerIndex(g), path ? path : "-");
}

/* ---- the menu's drawing ----
 * SDL's software renderer draws it (runtime/ui: sharp text at any size) into
 * a surface the window's size, which is uploaded into a texture and blitted
 * over the picture: no shaders, so any device. Behind the menu is the window
 * as it was when the menu opened, taken once (a frame without the menu),
 * blurred and darkened. The operator bar is a strip as wide as the picture,
 * at its top. */
static struct {
    int w, h;                       /* the surface's and the texture's size: the window's, in pixels */
    SDL_Surface *surf;
    SDL_Renderer *ren;              /* ui_init's */
    SDL_GPUTexture *tex;
    SDL_GPUTransferBuffer *up;
    SDL_Texture *backdrop;          /* on ren; NULL until taken (or when it could not be) */
    int taken;                      /* the window was taken for this opening (and this size) */
    SDL_GPUTexture *grab;           /* the window, taken for the backdrop */
    Uint32 grab_w, grab_h;
    int ready;                      /* what tex holds: 0 nothing, 1 the menu, 2 the bar */
    int bar_w, bar_h;
} ovl;

/* ---- the menu (menu.c) ---- */

static SDL_Window *main_win;
static char settings_path[4300];
static int menu_on, bar_on, ov_dirty;   /* bar_on: the operator bar's buttons (A Test, X Service, by label) */
static Uint64 bar_until;                /* the bar is drawn until then (ms); its buttons work until it closes */
static unsigned pad_hold;               /* buttons held as the menu closed: not the game's until let go */
static int live_fullscreen;
static int live_window, live_picture;   /* the Window size and Picture the window has */
static int window_later;                /* Window size changed in fullscreen: it applies as that ends */
static int to_launcher;                 /* the menu's Open the launcher: the game quits for it */
static char live_display[DISPLAY_NAME_MAX];     /* the Display the window was put on */
static SDL_DisplayID win_on;                    /* the screen the log last said the window is on */

static int fullscreen_wanted(void)
{
    return settings_fullscreen_wanted(&cfg_);
}

/* ---- the GPU and Display settings (devices.h, docs/design.md section 51) ---- */

static gpu_info gpus[GPUS_MAX];
static const char *gpu_names[GPUS_MAX];
static int gpus_n = -1;                 /* -1: not listed yet */
static char gpu_now[GPU_NAME_MAX];      /* the GPU the GPU renderer draws on ("": none, Software) */

/* Vulkan's GPUs, listed once (at the start for a GPU chosen by name, else
 * as the menu first opens) */
static void gpus_need(void)
{
    if (gpus_n >= 0)
        return;
    char why[120];
    gpus_n = gpus_list(gpus, GPUS_MAX, why, sizeof why);
    for (int i = 0; i < gpus_n; i++) {
        gpu_names[i] = gpus[i].name;
        logf_("GPU %d: %s (%s, PCI %04x:%04x)", i + 1, gpus[i].name,
              gpus[i].type == GPU_INTEGRATED ? "integrated" : gpus[i].type == GPU_DISCRETE ? "discrete" : "virtual",
              gpus[i].vendor, gpus[i].device);
    }
    if (!gpus_n)
        logf_("GPUs: none listed (%s)", why);
}

/* SDL_GPU's Vulkan device, with low power preferred or not */
static SDL_GPUDevice *gpu_open(int lowpower)
{
    if (!lowpower)
        return SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");     /* SDL's own choice */
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetStringProperty(p, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);    /* as the other's (it defaults to on) */
    SDL_GPUDevice *d = SDL_CreateGPUDeviceWithProperties(p);
    SDL_DestroyProperties(p);
    return d;
}

/* the GPU device d is on, as the GPU setting names it */
static const char *gpu_name_of(SDL_GPUDevice *d, char *buf, size_t n)
{
    const char *s = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(d), SDL_PROP_GPU_DEVICE_NAME_STRING, "");
    settings_name_clean(buf, n, s ? s : "");
    return buf;
}

/* The GPU g as SDL_GPU's Vulkan device, or NULL (logged). SDL 3.4 takes no
 * device name or number: it ranks the GPUs by kind, a separate one first,
 * or one in the processor first when low power is preferred. So a GPU in
 * the processor is asked for with low power preferred and any other
 * without; and when SDL still opens another (two separate GPUs), Vulkan's
 * loader is told to list that one only (VK_LOADER_DEVICE_ID_FILTER, read by
 * the Linux and Windows loaders; under Wine the Linux loader cannot see
 * it). The name of the device SDL opened says which it is. */
static SDL_GPUDevice *gpu_open_named(const gpu_info *g)
{
    char name[GPU_NAME_MAX];
    int lowpower = g->type == GPU_INTEGRATED;
    SDL_GPUDevice *d = gpu_open(lowpower);
    if (d && !SDL_strcasecmp(gpu_name_of(d, name, sizeof name), g->name))
        return d;
    if (d)
        SDL_DestroyGPUDevice(d);
    char id[16], old[64];
    const char *was = SDL_getenv("VK_LOADER_DEVICE_ID_FILTER");
    SDL_strlcpy(old, was ? was : "", sizeof old);
    SDL_snprintf(id, sizeof id, "0x%04x", (unsigned)g->device);
    SDL_setenv_unsafe("VK_LOADER_DEVICE_ID_FILTER", id, 1);
    d = gpu_open(lowpower);
    if (was)
        SDL_setenv_unsafe("VK_LOADER_DEVICE_ID_FILTER", old, 1);
    else
        SDL_unsetenv_unsafe("VK_LOADER_DEVICE_ID_FILTER");
    if (d && !SDL_strcasecmp(gpu_name_of(d, name, sizeof name), g->name)) {
        logf_("GPU: %s through Vulkan's loader (SDL opens another first)", g->name);
        return d;
    }
    logf_("GPU: %s cannot be had (SDL opens %s for it)", g->name, d ? name : "no device");
    if (d)
        SDL_DestroyGPUDevice(d);
    return NULL;
}

/* ---- the window's device and renderer (docs/design.md section 53) ---- */

static const char *gpu_driver;          /* --gpu: SDL_GPU's driver (NULL: Vulkan, else SDL's choice) */
static SDL_GPUDevice *gpu;              /* the window's device */
static int use_gpu;                     /* the GPU renderer draws on it; else the software picture is shown */
static SDL_GPUTexture *pic_tex;         /* the software picture, uploaded */
static SDL_GPUTransferBuffer *upload;
static SDL_GPUTexture *shot_tex;        /* --keys shot=: the window as shown */
static Uint32 shot_w, shot_h;
static struct {
    int renderer, scale;
    char gpu[GPU_NAME_MAX];
} video_for;                            /* the settings the device and the renderer were made for */
static int video_redo;                  /* the menu changed them: made again before the next frame */

static void ovl_free(SDL_GPUDevice *dev);
static void ovl_gpu_free(SDL_GPUDevice *dev);
static int ovl_gpu_make(SDL_GPUDevice *dev);

/* the window for device d (--art has none: true) */
static int video_claim(SDL_GPUDevice *d)
{
    return !main_win || SDL_ClaimWindowForGPUDevice(d, main_win);
}

/* ---- one device a GPU for the whole run (docs/design.md section 60) ----
 *
 * A device that has shown the window is not closed before the game ends.
 * When Renderer or GPU changes it gives the window back and is held, and
 * video_open takes it again when its order comes to that GPU. Under Wine,
 * NVIDIA's driver (620.70) takes 33 seconds to close the second device a
 * process makes on a GeForce, once the first has drawn the game and been
 * closed (vkDestroySwapchainKHR 22 s, vkDestroyDevice 11 s): at the quit, or
 * at the next change in the menu. So no GPU gets a second device */
enum { DEV_LISTED, DEV_OWN, DEV_ANY };      /* how video_open came by one: a GPU of the list, SDL's own choice, any driver */
static struct {
    SDL_GPUDevice *dev;
    int how;
    char name[GPU_NAME_MAX];
} held[GPUS_MAX + 2];
static int held_n;
static int gpu_how;                     /* the window's device: how video_open came by it */

/* the device held for a GPU of the list (by its name), or the one held of
 * another kind (name NULL); NULL when there is none */
static SDL_GPUDevice *held_take(int how, const char *name)
{
    for (int i = 0; i < held_n; i++) {
        if (held[i].how != how || (name && SDL_strcasecmp(held[i].name, name)))
            continue;
        SDL_GPUDevice *d = held[i].dev;
        logf_("GPU: %s: the device opened before is used again", held[i].name);
        held[i] = held[--held_n];
        return d;
    }
    return NULL;
}

static void held_put(SDL_GPUDevice *d, int how)
{
    if (held_n == (int)SDL_arraysize(held)) {   /* never: one a GPU and kind */
        SDL_DestroyGPUDevice(d);
        return;
    }
    held[held_n].dev = d;
    held[held_n].how = how;
    gpu_name_of(d, held[held_n].name, sizeof held[held_n].name);
    held_n++;
}

/* a device video_open does not take: closed if it never showed the window
 * (it drew nothing), else held as before */
static void dev_drop(SDL_GPUDevice *d, int how, int old)
{
    if (old)
        held_put(d, how);
    else
        SDL_DestroyGPUDevice(d);
}

/* a closing's step that takes a second or more is logged by its name: a
 * driver that stalls is found by the step (section 60) */
static Uint64 close_t0;

static void close_begin(void)
{
    close_t0 = SDL_GetTicksNS();
}

static void close_step(const char *what, const char *on)
{
    Uint64 now = SDL_GetTicksNS();
    if (now - close_t0 >= (Uint64)SDL_NS_PER_SECOND)
        logf_("closing: %s%s%s took %.1f s", what, on && on[0] ? " on " : "", on && on[0] ? on : "",
              (double)(now - close_t0) / SDL_NS_PER_SECOND);
    close_t0 = now;
}

/* The device for the window and the renderer on it, by Renderer and GPU:
 * the GPU chosen, then the others in the list's order (Vulkan's; Auto
 * starts at its top), then a device SDL opens itself (a GPU not listed:
 * one that draws on the processor). The first that opens, shows the window
 * and starts the GPU renderer is taken. If the GPU renderer starts on none,
 * the software picture is shown through the first that showed the window.
 * Software passes the GPU setting by: the first in the list that shows the
 * window. Last, for the software picture, any device SDL_GPU has (Direct3D
 * 12 on Windows without Vulkan; --gpu names one). A device held from
 * before (section 60) is taken in place of opening its GPU again. Sets gpu,
 * use_gpu, gpu_now, pic_tex and upload, the window claimed; 0, or -1 when
 * nothing can show the window */
static int video_open(void)
{
    SDL_Window *win = main_win;
    char name[GPU_NAME_MAX];
    int want_gpu = cfg_.renderer == RENDERER_GPU && !(gpu_driver && SDL_strcasecmp(gpu_driver, "vulkan"));
    SDL_GPUDevice *keep = NULL;     /* the first that showed the window, where the GPU renderer did not start */
    int keep_how = 0, keep_old = 0;
    const char *why = "";
    gpu = NULL;
    use_gpu = 0;
    gpu_now[0] = 0;
    video_for.renderer = cfg_.renderer;
    video_for.scale = cfg_.scale;
    SDL_strlcpy(video_for.gpu, cfg_.gpu, sizeof video_for.gpu);
    if (!gpu_driver || !SDL_strcasecmp(gpu_driver, "vulkan")) {
        gpus_need();
        const gpu_info *chosen = want_gpu && cfg_.gpu[0] ? gpus_find(gpus, gpus_n, cfg_.gpu) : NULL;
        if (want_gpu && cfg_.gpu[0] && !chosen)
            logf_("settings: GPU \"%s\" is not here: the first that works instead (the setting is kept)", cfg_.gpu);
        const gpu_info *order[GPUS_MAX + 1];
        int n = 0;
        if (chosen)
            order[n++] = chosen;
        for (int i = 0; i < gpus_n; i++)
            if (&gpus[i] != chosen)
                order[n++] = &gpus[i];
        order[n++] = NULL;          /* SDL's own choice */
        for (int i = 0; i < n && !gpu; i++) {
            int how = order[i] ? DEV_LISTED : DEV_OWN;
            SDL_GPUDevice *d = held_take(how, order[i] ? order[i]->name : NULL);
            int old = d != NULL;        /* it showed the window before: not closed before the end */
            if (!d)
                d = order[i] ? gpu_open_named(order[i]) : gpu_open(0);
            if (!d) {
                if (!order[i])
                    logf_("GPU: SDL opens no Vulkan device (%s)", SDL_GetError());
                continue;
            }
            gpu_name_of(d, name, sizeof name);
            if (!old && !order[i] && gpus_find(gpus, gpus_n, name)) {
                SDL_DestroyGPUDevice(d);        /* one of the list's, tried already */
                continue;
            }
            if (!video_claim(d)) {
                /* on X11 (and Proton) the X server draws on one GPU and takes
                 * another's pictures only as that GPU's driver allows (Mesa's
                 * swapchain on the Radeon when Xwayland is on an NVIDIA card fails so) */
                logf_("GPU: %s cannot show the window (%s)", name, SDL_GetError());
                dev_drop(d, how, old);
                continue;
            }
            if (!want_gpu) {
                gpu = d;
                gpu_how = how;
                break;
            }
            if (gpu_render_init(d, cfg_.scale) == 0) {
                gpu = d;
                gpu_how = how;
                use_gpu = 1;
                why = chosen && order[i] == chosen ? " (the GPU chosen)"
                      : chosen                     ? " (the GPU chosen could not be used)"
                      : cfg_.gpu[0]                ? " (the first that works: the GPU chosen is not here)"
                                                   : " (Auto: the first that works)";
                break;
            }
            logf_("GPU: the GPU renderer cannot start on %s (%s)", name, SDL_GetError());
            gpu_render_quit();
            if (win)
                SDL_ReleaseWindowFromGPUDevice(d, win);
            if (!keep) {
                keep = d;
                keep_how = how;
                keep_old = old;
            } else {
                dev_drop(d, how, old);
            }
        }
    }
    if (!gpu && keep) {
        if (video_claim(keep)) {
            gpu = keep;
            gpu_how = keep_how;
            keep = NULL;
        } else {
            logf_("GPU: %s cannot show the window again (%s)", gpu_name_of(keep, name, sizeof name), SDL_GetError());
        }
    }
    if (keep)
        dev_drop(keep, keep_how, keep_old);
    if (!gpu) {
        SDL_GPUDevice *d = held_take(DEV_ANY, NULL);
        int old = d != NULL;
        if (!d)
            d = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, false, gpu_driver);
        if (!d)
            logf_("SDL_CreateGPUDevice(%s) failed: %s", gpu_driver ? gpu_driver : "any", SDL_GetError());
        else if (!video_claim(d)) {
            logf_("claim window failed: %s", SDL_GetError());
            dev_drop(d, DEV_ANY, old);
        } else {
            gpu = d;
            gpu_how = DEV_ANY;
        }
    }
    if (!gpu)
        return -1;
    logf_("GPU driver: %s", SDL_GetGPUDeviceDriver(gpu));
    {
        /* which graphics driver, and its version: a fault of one driver's shows only on its PCs (section 83) */
        SDL_PropertiesID gp = SDL_GetGPUDeviceProperties(gpu);
        const char *dn = SDL_GetStringProperty(gp, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, NULL);
        const char *dv = SDL_GetStringProperty(gp, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, NULL);
        const char *di = SDL_GetStringProperty(gp, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, NULL);
        if (dn || dv || di)
            logf_("graphics driver: %s%s%s%s%s", dn ? dn : "?", dv ? ", version " : "", dv ? dv : "", di ? ", " : "", di ? di : "");
    }
    int shown_w = art_dir ? PIC_W : settings_picture_width(cfg_.picture);
    if (use_gpu) {
        gpu_name_of(gpu, gpu_now, sizeof gpu_now);
        logf_("renderer: GPU, %d x %d, on %s%s", shown_w * cfg_.scale, PIC_H * cfg_.scale, gpu_now, why);
    } else {
        logf_("renderer: software, %d x %d, shown through %s%s", shown_w, PIC_H, gpu_name_of(gpu, name, sizeof name),
              want_gpu ? " (the GPU renderer could not start on any GPU)" : "");
    }

    SDL_SetAtomicInt(&gpu_path, use_gpu);
    if (art_dir)
        return 0;                       /* no window: art_loop takes the GPU renderer's pictures */
    /* the software picture: uploaded into a texture whenever a new one is
     * there, then blitted to the window, scaled */
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = PIC_W_MAX;               /* room for a wide screen */
    ti.height = PIC_H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    pic_tex = SDL_CreateGPUTexture(gpu, &ti);
    SDL_GPUTransferBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = PIC_W_MAX * PIC_H * 4;
    upload = SDL_CreateGPUTransferBuffer(gpu, &bi);
    if (!pic_tex || !upload) {
        logf_("picture texture: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

/* everything on the device: the window is free for another. The device
 * itself is held for the run (section 60), with none of the renderer's
 * memory */
static void video_close(void)
{
    if (!gpu)
        return;
    char on[GPU_NAME_MAX];
    gpu_name_of(gpu, on, sizeof on);
    if (use_gpu)
        gpu_render_quit();
    use_gpu = 0;
    if (pic_tex)
        SDL_ReleaseGPUTexture(gpu, pic_tex);
    if (upload)
        SDL_ReleaseGPUTransferBuffer(gpu, upload);
    if (shot_tex)
        SDL_ReleaseGPUTexture(gpu, shot_tex);
    pic_tex = shot_tex = NULL;
    upload = NULL;
    shot_w = shot_h = 0;
    ovl_gpu_free(gpu);
    close_begin();
    if (main_win)
        SDL_ReleaseWindowFromGPUDevice(gpu, main_win);
    close_step("giving the window back", on);
    /* SDL gives a device's memory back as it submits a command buffer: an
     * empty one, so that the device held has none of what was released */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (cmd)
        SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(gpu);
    close_step("giving memory back", on);
    held_put(gpu, gpu_how);
    gpu = NULL;
    gpu_now[0] = 0;
}

/* the game's end: the window's device and every one held are closed */
static void video_end(void)
{
    video_close();
    close_begin();
    while (held_n > 0) {
        held_n--;
        SDL_DestroyGPUDevice(held[held_n].dev);
        close_step("closing the GPU device", held[held_n].name);
    }
}

/* Renderer, GPU or Internal resolution changed in the menu (the machine
 * waits while it is open): the renderer starts again, at once. A new
 * Internal resolution alone starts it again on the same device; anything
 * else gives the window to the device video_open's order comes to (a GPU's
 * device is opened once and held: section 60). The machine takes the
 * GPU renderer or the software picture at its next vertical blank
 * (on_frame). 0, or -1 when nothing can show the window any more */
static int video_switch(void)
{
    logf_("menu: Renderer %s, GPU %s, Internal resolution %dx: the renderer starts again",
          cfg_.renderer == RENDERER_GPU ? "GPU" : "Software", cfg_.gpu[0] ? cfg_.gpu : "Auto", cfg_.scale);
    if (use_gpu && cfg_.renderer == RENDERER_GPU && video_for.renderer == RENDERER_GPU &&
        !SDL_strcasecmp(cfg_.gpu, video_for.gpu)) {
        gpu_render_quit();
        if (gpu_render_init(gpu, cfg_.scale) == 0) {
            video_for.scale = cfg_.scale;
            int shown_w = settings_picture_width(cfg_.picture);
            logf_("renderer: GPU, %d x %d, on %s", shown_w * cfg_.scale, PIC_H * cfg_.scale, gpu_now);
            return 0;
        }
        logf_("GPU: the GPU renderer cannot start again on %s (%s)", gpu_now, SDL_GetError());
        gpu_render_quit();
        use_gpu = 0;
    }
    video_close();
    if (video_open()) {
        logf_("no GPU device can show the window: the game ends");
        return -1;
    }
    if (ovl.surf && ovl_gpu_make(gpu)) {
        logf_("the menu's drawing: %s", SDL_GetError());
        ovl_free(gpu);
    }
    ov_dirty = 1;
    return 0;
}

/* the menu's change: does the renderer start again for it? */
static int video_changed(void)
{
    if (cfg_.renderer != video_for.renderer)
        return 1;
    return cfg_.renderer == RENDERER_GPU && (cfg_.scale != video_for.scale || SDL_strcasecmp(cfg_.gpu, video_for.gpu));
}

/* the displays as last listed: their names and ids, for the menu's Display
 * row and to name one that has gone */
static char disp_names[DISPLAYS_MAX][DISPLAY_NAME_MAX];
static const char *disp_list[DISPLAYS_MAX];
static SDL_DisplayID disp_ids[DISPLAYS_MAX];
static int disp_n;

/* lists the displays again; log 1: says which came or went since */
static void displays_refresh(int log)
{
    char names[DISPLAYS_MAX][DISPLAY_NAME_MAX];
    SDL_DisplayID ids[DISPLAYS_MAX];
    int n = displays_list(names, ids, DISPLAYS_MAX);
    for (int i = 0; log && i < disp_n; i++) {
        int still = 0;
        for (int k = 0; k < n; k++)
            still |= ids[k] == disp_ids[i];
        if (!still)
            logf_("display: %s disconnected%s", disp_names[i],
                  !SDL_strcasecmp(disp_names[i], cfg_.display) ? " (Display's: Auto until it is back)" : "");
    }
    for (int k = 0; log && k < n; k++) {
        int had = 0;
        for (int i = 0; i < disp_n; i++)
            had |= ids[k] == disp_ids[i];
        if (!had)
            logf_("display: %s connected%s", names[k], !SDL_strcasecmp(names[k], cfg_.display) ? " (Display's)" : "");
    }
    disp_n = n;
    for (int i = 0; i < n; i++) {
        SDL_memcpy(disp_names[i], names[i], sizeof disp_names[i]);
        disp_ids[i] = ids[i];
        disp_list[i] = disp_names[i];
    }
}

/* the display Display names now, or 0: Auto, or that display is not
 * connected (the window stays where the system puts it) */
static SDL_DisplayID display_wanted(void)
{
    return cfg_.display[0] ? displays_find(cfg_.display) : 0;
}

/* fullscreen on or off: on goes to Display's display when it is connected
 * (SDL takes a window centered on a display as one for its fullscreen,
 * Wayland's too), else to the one the window is on */
static void fullscreen_to(SDL_Window *w, bool on)
{
    SDL_DisplayID want = on ? display_wanted() : 0;
    if (want)
        SDL_SetWindowPosition(w, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(want), (int)SDL_WINDOWPOS_CENTERED_DISPLAY(want));
    SDL_SetWindowFullscreen(w, on);
}

/* the room for the window's picture on w's screen (before there is a
 * window, Display's, or the primary one) in pixels: the usable area less the window's frame
 * when SDL knows it (an X11 window manager keeps a title bar on the screen,
 * so 1080 lines on a 1080 screen were 1052), in w's coordinates times its
 * pixel density (2 on a Wayland screen at 200%; 1 on Windows, whose
 * coordinates are pixels) */
static window_room screen_room(SDL_Window *w)
{
    SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : display_wanted();
    if (!d)
        d = SDL_GetPrimaryDisplay();
    window_room room = { 0, 0, w ? SDL_GetWindowPixelDensity(w) : 1.0f };
    SDL_Rect r;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (room.density <= 0)
        room.density = 1;
    if (w && !SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right))
        top = left = bottom = right = 0;
    if (d && SDL_GetDisplayUsableBounds(d, &r)) {
        room.w = (int)((r.w - left - right) * room.density + 0.5f);
        room.h = (int)((r.h - top - bottom) * room.density + 0.5f);
    }
    return room;
}

/* the Window size setting (docs/design.md section 40): the window to its
 * size in pixels at the picture's shape (settings_window_size), in the
 * middle of its screen when it was there already (move 1): of Display's
 * screen when that is connected (section 51; moved there first, so that
 * the size is that screen's). Fullscreen keeps the screen: the size waits
 * for it to end. A maximized window is restored to it. Window size
 * Maximized, the default (section 70), maximizes it instead */
static void window_to_setting(SDL_Window *w, int move)
{
    SDL_strlcpy(live_display, cfg_.display, sizeof live_display);
    live_window = cfg_.window;
    live_picture = cfg_.picture;
    if (!w)
        return;
    SDL_WindowFlags fl = SDL_GetWindowFlags(w);
    if (fl & SDL_WINDOW_FULLSCREEN) {
        window_later = 1;
        return;
    }
    window_later = 0;
    int maxed = cfg_.window == WINDOW_MAXIMIZED;
    int pw, ph, cw, ch, ow = 0, oh = 0;
    SDL_DisplayID want = move ? display_wanted() : 0, on = SDL_GetDisplayForWindow(w);
    if ((fl & SDL_WINDOW_MAXIMIZED) && (!maxed || (want && want != on))) {
        SDL_RestoreWindow(w);       /* to a size, or to move it to Display's screen first */
        SDL_SyncWindow(w);
    }
    if (want && want != on) {
        /* to Display's screen first (Wayland: SDL cannot place a window, and it stays) */
        if (SDL_SetWindowPosition(w, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(want),
                                  (int)SDL_WINDOWPOS_CENTERED_DISPLAY(want)))
            SDL_SyncWindow(w);
        else
            logf_("window: cannot be placed on %s (%s): it stays where the system put it", cfg_.display,
                  SDL_GetError());
        on = SDL_GetDisplayForWindow(w);
    }
    if (maxed) {                    /* Window size Maximized (section 70): the screen's room, with its frame */
        if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED)) {
            SDL_MaximizeWindow(w);
            logf_("window: maximized (Window size Maximized)");
        }
        return;
    }
    window_room room = screen_room(w);
    settings_window_size(&cfg_, &room, &pw, &ph);
    cw = (int)(pw / room.density + 0.5f);
    ch = (int)(ph / room.density + 0.5f);
    SDL_GetWindowSize(w, &ow, &oh);
    if (ow == cw && oh == ch)
        return;
    SDL_SetWindowSize(w, cw, ch);
    if (move) {
        SDL_DisplayID d = want && want == on ? want : on;
        SDL_SetWindowPosition(w, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d), (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d));
    }
    const char *dn = SDL_GetDisplayName(on);
    logf_("window: %d x %d pixels on %s (Window size %s, room on the screen %d x %d, pixel density %.2f)", pw, ph,
          dn ? dn : "?", cfg_.window == WINDOW_AUTO ? "Auto" : "set", room.w, room.h, room.density);
}

/* the Display setting changed in the menu: a window goes to the middle of
 * that display at its Window size there, a fullscreen one to that display
 * (Auto: it stays where it is) */
static void display_to_setting(SDL_Window *w)
{
    SDL_DisplayID want = display_wanted();
    if (!w)
        return;
    if (!want) {
        SDL_strlcpy(live_display, cfg_.display, sizeof live_display);
        if (cfg_.display[0])
            logf_("display: %s is not connected: the window stays where it is", cfg_.display);
        return;
    }
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) {
        SDL_strlcpy(live_display, cfg_.display, sizeof live_display);
        SDL_SetWindowPosition(w, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(want), (int)SDL_WINDOWPOS_CENTERED_DISPLAY(want));
        SDL_SyncWindow(w);
        logf_("display: fullscreen on %s", cfg_.display);
        return;
    }
    window_to_setting(w, 1);
}

static float screen_hz_test;        /* --screen-hz */
static float system_hz_test;        /* --system-hz */

/* the refresh rate the system reports for w's screen in Hz (0: not known) */
static float screen_refresh(SDL_Window *w)
{
    if (screen_hz_test > 0)
        return screen_hz_test;
    if (system_hz_test > 0)
        return system_hz_test;
    SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : 0;
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    return m && m->refresh_rate > 0 ? m->refresh_rate : 0;
}

/* The screen's refresh rate for its notice and the menu's note (docs/design.md
 * sections 44 and 61), in Hz; 0: not known. The system's report can be
 * wrong (under Proton 60 was reported on screens at 120 and at 50 Hz), so
 * it is held against the presents' own steady rate (vsync_screen_hz) by
 * settings_refresh_choose, which says when that rate is taken: as a guess,
 * and the notice says it as one. The presents' rate counts only once it is
 * settled, and only while the window has the keyboard: behind another
 * window it may be covered. *sys and *seen: the report and the presents'
 * rate (0: none, or not to be counted); *settled: it is settled, or nothing
 * is to be waited for; *own: the rate returned is the presents', not the
 * report */
static float screen_rate(float *sys_out, double *seen_out, int *settled_out, int *own_out)
{
    float sys = screen_refresh(main_win);
    int settled = 1;
    double seen = screen_hz_test > 0 ? 0 : vsync_screen_hz(SDL_GetTicksNS(), &settled);
    if (screen_hz_test > 0) {
        settled = 1;
    } else if (!settled) {
        seen = 0;                               /* not settled: it says nothing yet */
    } else if (main_win && !(SDL_GetWindowFlags(main_win) & SDL_WINDOW_INPUT_FOCUS)) {
        seen = 0;                               /* nor behind another window */
        settled = 0;
    }
    if (sys_out)
        *sys_out = sys;
    if (seen_out)
        *seen_out = seen;
    if (settled_out)
        *settled_out = settled;
    return settings_refresh_choose(sys, (float)seen, own_out);
}

/* Frame pacing as the window's thread goes by it (docs/design.md section 59): this PC's own,
 * but in a two-PC game the host's (each PC says its own with its controls: the joining PC
 * goes by the host's once it has heard it), and Game clock there for Screen refresh, since
 * the machine keeps its own clock in a two-PC game */
static int pacing_in_use(void)
{
    int pacing = cfg_.pacing;
    if (!in_session)
        return pacing;
    session_pacing(cfg_.pacing);
    if (session_role() != SESSION_HOST && session_pacing_theirs() >= 0)
        pacing = session_pacing_theirs();
    return pacing == PACING_SCREEN ? PACING_GAME : pacing;
}

/* the menu's note on Frame pacing in a two-PC game */
static void pacing_note(char *buf, size_t n)
{
    static const char *const name[PACINGS] = { "Game clock", "Screen refresh", "Off" };
    int now = pacing_in_use(), host = session_role() == SESSION_HOST;
    if (session_stopped)
        buf[0] = 0;
    else if (!host && session_pacing_theirs() < 0)
        SDL_snprintf(buf, n, "A two-PC game uses the host's choice. It has not come yet: %s for now.", name[now]);
    else if (!host)
        SDL_snprintf(buf, n, "A two-PC game uses the host's choice: %s now. Yours is kept for your own games.",
                     name[now]);
    else if (cfg_.pacing == PACING_SCREEN)
        SDL_snprintf(buf, n, "A two-PC game keeps its own time: Game clock is used, on both PCs.");
    else
        SDL_snprintf(buf, n, "A two-PC game uses the host's choice: yours, on both PCs.");
}

/* the menu's note on Latency optimization in a two-PC game: what it does
 * now, and the round trip measured (session.h) */
static void latency_now(char *buf, size_t n)
{
    double rt, jit;
    int on, measured = !session_round_trip(&rt, &jit, &on);
    if (session_stopped)
        buf[0] = 0;
    else if (!measured)
        SDL_snprintf(buf, n, "Now %s. The round trip to the other PC is not measured yet.", on ? "on" : "off");
    else
        SDL_snprintf(buf, n, "Now %s. The round trip to the other PC is %.0f ms, varying by %.0f ms.", on ? "on" : "off",
                     rt, jit);
}

/* the menu, or (notice: MENU_NOTICE_*) a notice in its place */
static void menu_show_as(int notice)
{
    /* the card profiles there now, for Player 1 card and Player 2 card */
    static char names[CARDS_MAX][CARD_NAME_MAX];
    static const char *list[CARDS_MAX];
    int n = cards_list(user_dir, names, CARDS_MAX);
    for (int i = 0; i < n; i++)
        list[i] = names[i];
    options_card_names(list, n);
    {
        window_room room = screen_room(main_win);
        options_screen(&room);
    }
    gpus_need();
    options_gpu_names(gpu_names, gpus_n);
    displays_refresh(0);
    options_display_names(disp_list, disp_n);
    menu_run r;
    r.start = start_cfg;
    r.sound = audio != NULL;
    r.no_device = !silent && !audio;    /* silent: sound off in the settings, or --silent */
    r.test_program = SDL_GetAtomicInt(&program_now);
    r.card_peek = card_peek;
    r.card_edit = card_edit_save;
    r.refresh = screen_rate(NULL, NULL, NULL, &r.refresh_own);
    r.gpu_now = gpu_now;                /* as it is now: video_switch changes it while the menu is open */
    r.deck = settings_steam_deck();
    r.session = in_session;
    r.join_code = in_session && session_role() == SESSION_HOST ? session_code : NULL;    /* the same values */
    r.join_code_new = session_code_new;
    r.latency_now = in_session ? latency_now : NULL;
    r.pacing_now = in_session ? pacing_note : NULL;
    session_paused(1);              /* the other PC waits, and says why */
    if (notice)
        menu_open_notice(&cfg_, &r, notice);
    else
        menu_open(&cfg_, &r);
    menu_on = ov_dirty = 1;
    SDL_SetAtomicInt(&menu_pause, 1);
    if (audio)
        SDL_PauseAudioStreamDevice(audio);
    logf_("menu: %s at vertical blank %d",
          notice == MENU_NOTICE_KOFI ? "the Ko-fi notice" : notice == MENU_NOTICE_REFRESH ? "the refresh notice" : "open",
          SDL_GetAtomicInt(&frame_now));
}

static void menu_show(void)
{
    menu_show_as(0);
}

/* ---- the first start's Ko-fi notice (docs/design.md section 26) ---- */
static unsigned kofi_frame;         /* --kofi-notice: at this vertical blank */
static int kofi_due;                /* 1 until it is shown */
static Uint64 kofi_after;           /* not before this (ms): set at the first picture */

static void kofi_mark(void)
{
    char path[4200];
    SDL_snprintf(path, sizeof path, "%skofi-notice", user_dir);
    SDL_IOStream *f = SDL_IOFromFile(path, "wb");
    if (!f) {
        logf_("kofi: cannot write %s: %s", path, SDL_GetError());
        return;
    }
    SDL_IOprintf(f, "The game showed its Ko-fi notice. Remove this file to see it again at the next start.\n");
    SDL_CloseIO(f);
}

/* whether this run shows it: once ever, never in a test run (test) unless asked */
static void kofi_setup(int test)
{
    if (kofi_frame) {
        kofi_due = 1;
        return;
    }
    if (test)
        return;
    char path[4200];
    SDL_snprintf(path, sizeof path, "%skofi-notice", user_dir);
    kofi_due = !SDL_GetPathInfo(path, NULL);
}

/* called every turn of the window's loop: shows it when it is time */
static void kofi_turn(int have_picture, Uint64 now_ms)
{
    if (!kofi_due || !have_picture || menu_on)
        return;
    if (kofi_frame) {
        if ((unsigned)SDL_GetAtomicInt(&frame_now) < kofi_frame)
            return;
    } else {
        if (!kofi_after)
            kofi_after = now_ms + 2000;     /* two seconds into the picture */
        if (now_ms < kofi_after)
            return;
    }
    kofi_due = 0;
    if (!kofi_frame)
        kofi_mark();
    menu_show_as(MENU_NOTICE_KOFI);
}

/* ---- the screen's refresh rate notice (docs/design.md section 44) ----
 * The game shows 60 pictures a second; a screen refreshing at a rate that
 * is not a multiple of 60 (90 on a Steam Deck OLED, 144, 75) shows some of
 * them longer than others. The game cannot change the screen's rate, so it
 * says how, once for each screen it is shown for (user/refresh-notice holds
 * which), after the Ko-fi notice. On a Steam Deck OLED it is shown whatever
 * rate the screen reports, since the per-game Refresh rate is Steam's. */
static unsigned refresh_frame;      /* --refresh-notice: at this vertical blank (a test: nothing is written) */
static int refresh_due;             /* 1 until it is checked */
static Uint64 refresh_after;        /* not before this (ms) */
static Uint64 refresh_limit;        /* and at this, settled or not (ms) */

#define REFRESH_WAIT_MS 20000       /* how long the presents' rate is waited for (section 61) */

#define REFRESH_FILE "refresh-notice"

/* what it is shown for: "Steam Deck OLED", or the screen's rate ("144 Hz") */
static void refresh_key(float hz, char *key, size_t n)
{
    if (settings_steam_deck() == STEAM_DECK_OLED)
        SDL_strlcpy(key, "Steam Deck OLED", n);
    else
        SDL_snprintf(key, n, "%.0f Hz", (double)hz);
}

/* 1 when user/refresh-notice says it was shown for key */
static int refresh_seen(const char *key)
{
    char path[4200];
    SDL_snprintf(path, sizeof path, "%s" REFRESH_FILE, user_dir);
    size_t len = 0;
    char *t = SDL_LoadFile(path, &len);
    if (!t)
        return 0;
    size_t k = strlen(key);
    int seen = len >= k && !memcmp(t, key, k) && (len == k || t[k] == '\n' || t[k] == '\r');
    SDL_free(t);
    return seen;
}

static void refresh_mark(const char *key)
{
    char path[4200];
    SDL_snprintf(path, sizeof path, "%s" REFRESH_FILE, user_dir);
    SDL_IOStream *f = SDL_IOFromFile(path, "wb");
    if (!f) {
        logf_("screen: cannot write %s: %s", path, SDL_GetError());
        return;
    }
    SDL_IOprintf(f, "%s\nThe game showed its refresh rate notice for this screen. Remove this file to see it "
                    "again at the next start.\n", key);
    SDL_CloseIO(f);
}

/* whether this run checks: always, but never in a test run (test) unless asked */
static void refresh_setup(int test)
{
    refresh_due = refresh_frame || !test;
}

/* called every turn of the window's loop, after kofi_turn: checks the
 * screen's rate (screen_rate) once the presents' rate has settled, two
 * seconds into the picture (or the Ko-fi notice's closing) at the earliest
 * and REFRESH_WAIT_MS at the latest, and shows the notice when it is
 * uneven */
static void refresh_turn(int have_picture, Uint64 now_ms)
{
    if (!refresh_due || !have_picture || menu_on || kofi_due)
        return;
    int deck = settings_steam_deck();
    float sys = 0;
    double seen = 0;
    int settled = 1;
    if (refresh_frame) {
        if ((unsigned)SDL_GetAtomicInt(&frame_now) < refresh_frame)
            return;
    } else {
        if (!refresh_after) {
            refresh_after = now_ms + 2000;
            refresh_limit = now_ms + REFRESH_WAIT_MS;
        }
        if (now_ms < refresh_after)
            return;
    }
    float hz = screen_rate(&sys, &seen, &settled, NULL);
    /* the presents' rate has not stood long enough yet (an OLED Deck's notice does not go by it) */
    if (!refresh_frame && !settled && deck != STEAM_DECK_OLED && now_ms < refresh_limit)
        return;
    refresh_due = 0;
    int uneven = settings_refresh_uneven(hz);
    char key[48], how[112] = "";
    refresh_key(hz, key, sizeof key);
    if (seen > 0 && hz != sys && sys > 0)
        SDL_snprintf(how, sizeof how, ", by its presents (the system says %.2f Hz)", (double)sys);
    else if (seen > 0 && hz != sys)
        SDL_snprintf(how, sizeof how, ", by its presents (the system reports no rate)");
    else if (seen > 0 && SDL_fabs(seen - (double)sys) > 0.01 * (double)sys)
        SDL_snprintf(how, sizeof how, " (the presents come at %.2f Hz, which is not taken for the screen's rate)", seen);
    else if (seen <= 0 && hz > 0 && screen_hz_test <= 0)
        SDL_snprintf(how, sizeof how, " (the system's report: the presents gave no settled rate)");
    logf_("screen: %.2f Hz%s%s", (double)hz, how,
          hz <= 0 ? " (not known)" : uneven ? " (not a multiple of 60: uneven)" : "");
    if (!uneven && deck != STEAM_DECK_OLED)
        return;
    if (!refresh_frame) {
        if (refresh_seen(key)) {
            logf_("screen: the refresh notice was shown for %s before", key);
            return;
        }
        refresh_mark(key);
    }
    menu_show_as(MENU_NOTICE_REFRESH);
}

/* opens the Ko-fi page in the browser and tells the menu what it did */
static void kofi_open(void)
{
    bool ok = SDL_OpenURL(MENU_KOFI_URL);
    if (ok)
        logf_("menu: Ko-fi: opened %s", MENU_KOFI_URL);
    else
        logf_("menu: Ko-fi: cannot open %s: %s", MENU_KOFI_URL, SDL_GetError());
    menu_kofi(ok);
}

static void menu_hide(void)
{
    menu_on = 0;
    ov_dirty = 1;
    if (ovl.backdrop) {
        SDL_DestroyTexture(ovl.backdrop);
        ovl.backdrop = NULL;
    }
    ovl.taken = 0;          /* taken again at the next opening */
    if (menu_changed()) {
        if (settings_save(&cfg_, settings_path))
            logf_("menu: cannot write %s", settings_path);
        else
            logf_("menu: settings saved to %s", settings_path);
    }
    cards_choose();
    /* what is held now (the button or key that closed it) waits to be let go */
    pad_hold = pads_raw;
    int nk = 0;
    const bool *k = SDL_GetKeyboardState(&nk);
    for (int i = 0; i < nk && i < SDL_SCANCODE_COUNT; i++)
        if (k[i])
            key_ignore[i] = true;
    SDL_SetAtomicInt(&menu_pause, 0);
    session_paused(0);
    if (audio)
        SDL_ResumeAudioStreamDevice(audio);
}

/* The picture the game leaves its far objects out for (section 68): the
 * picture shown, or in a two-PC game the widest there is, on both PCs: the
 * machines must do the same, whatever each player's picture. */
static int cull_width(void)
{
    return settings_picture_width(in_session ? PICTURE_21_9 : cfg_.picture);
}

/* The ray tracing the setting names (sections 74 and 75), or NULL for none. DirectX 12's is in
 * the Windows game only: the game built for Linux never has that value (it reads it as off). */
static const rt_backend *raytracing_wanted(void)
{
    switch (cfg_.raytracing) {
    case RAYTRACING_VULKAN: return &rt_vulkan;
#ifdef _WIN32
    case RAYTRACING_DIRECTX: return &rt_directx;
#endif
    default: return NULL;
    }
}

/* what the menu asked for; 0 to quit */
static int menu_do(int a)
{
    switch (a) {
    case MENU_LIVE:
        game_machine_wide(settings_picture_width(cfg_.picture));
        game_machine_cull(cull_width());
        game_machine_lighting(cfg_.lighting == LIGHTING_GPU);
        game_machine_every(cfg_.skip == SKIP_HALF ? 2 : 1);
        gpu_render_occlusion(cfg_.occlusion == OCCLUSION_SSAO);
        gpu_render_raytracing(raytracing_wanted(), cfg_.rayquality);
        bindings_compile();     /* the menu's Gamepad and Keyboard tabs (section 72) */
        if (SDL_strcasecmp(cfg_.display, live_display)) {
            display_to_setting(main_win);       /* another screen: its room for Window size too */
            window_room room = screen_room(main_win);
            options_screen(&room);
        }
        if (cfg_.window != live_window ||
            settings_window_width(cfg_.picture, PIC_H) != settings_window_width(live_picture, PIC_H))
            window_to_setting(main_win, 1);     /* the size, or the new picture's shape */
        live_picture = cfg_.picture;
        if (cfg_.fullscreen != live_fullscreen) {
            live_fullscreen = cfg_.fullscreen;
            fullscreen_to(main_win, fullscreen_wanted());
        }
        if (audio)
            SDL_SetAudioStreamGain(audio, cfg_.volume / 100.0f);
        session_rollback(cfg_.latency);
        if (video_changed())
            video_redo = 1;     /* in the main loop, before the next frame */
        ov_dirty = 1;
        return 1;
    case MENU_RESUME:
        menu_hide();
        logf_("menu: back to the game");
        return 1;
    case MENU_TEST:
        menu_hide();
        SDL_SetAtomicInt(&test_frames, 6);
        bar_on = 1;
        bar_until = SDL_GetTicks() + 6000;
        logf_("menu: the Test switch (the operator's test menu); A is Test and X Service until B or EXIT");
        return 1;
    case MENU_OPERATOR:
        menu_hide();
        bar_on = 1;
        bar_until = SDL_GetTicks() + 6000;
        logf_("menu: the operator's buttons: A is Test and X Service until B or EXIT");
        return 1;
    case MENU_RESTART:
        bar_on = 0;
        start_cfg.coins = cfg_.coins;           /* the restart takes them (set before the request: the */
        start_cfg.network = cfg_.network;       /* game's thread reads them as it sees it) */
        start_cfg.region = cfg_.region;
        SDL_SetAtomicInt(&restart_req, cfg_.coins == COINS_ARCADE ? GAME_COINS_ARCADE : GAME_COINS_FREE_PLAY);
        menu_hide();
        return 1;
    case MENU_QUIT:
        menu_hide();
        logf_("quit: the menu");
        return 0;
    case MENU_LAUNCHER:
        menu_hide();
        to_launcher = 1;
        logf_("quit: the menu's Open the launcher");
        return 0;
    case MENU_KOFI:
        kofi_open();
        ov_dirty = 1;
        return 1;
    case MENU_KOFI_CLOSE:
        kofi_open();
        menu_hide();
        logf_("menu: back to the game");
        return 1;
    default:
        ov_dirty = 1;
        return 1;
    }
}


/* the overlay's textures on the device; its drawing on the processor (the
 * surface, ui's renderer, the backdrop) stays, for another device
 * (video_switch) */
static void ovl_gpu_free(SDL_GPUDevice *dev)
{
    if (ovl.tex)
        SDL_ReleaseGPUTexture(dev, ovl.tex);
    if (ovl.up)
        SDL_ReleaseGPUTransferBuffer(dev, ovl.up);
    if (ovl.grab)
        SDL_ReleaseGPUTexture(dev, ovl.grab);
    ovl.tex = NULL;
    ovl.up = NULL;
    ovl.grab = NULL;
    ovl.grab_w = ovl.grab_h = 0;
    ovl.ready = 0;
}

/* the overlay's textures at its size (ovl.w x ovl.h); 0 or -1 */
static int ovl_gpu_make(SDL_GPUDevice *dev)
{
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)ovl.w;
    ti.height = (Uint32)ovl.h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ovl.tex = SDL_CreateGPUTexture(dev, &ti);
    SDL_GPUTransferBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = (Uint32)ovl.w * (Uint32)ovl.h * 4;
    ovl.up = SDL_CreateGPUTransferBuffer(dev, &bi);
    return ovl.tex && ovl.up ? 0 : -1;
}

static void ovl_free(SDL_GPUDevice *dev)
{
    if (ovl.ren) {
        ui_quit();
        if (ovl.backdrop)
            SDL_DestroyTexture(ovl.backdrop);
        SDL_DestroyRenderer(ovl.ren);
    }
    SDL_DestroySurface(ovl.surf);
    ovl_gpu_free(dev);
    SDL_zero(ovl);
}

/* made again at the window's size, w x h pixels; 0 or -1 */
static int ovl_size(SDL_GPUDevice *dev, int w, int h)
{
    ovl_free(dev);
    ovl.w = w;
    ovl.h = h;
    ovl.surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
    ovl.ren = ovl.surf ? SDL_CreateSoftwareRenderer(ovl.surf) : NULL;
    if (!ovl.ren || ovl_gpu_make(dev) || ui_init(ovl.ren)) {
        logf_("the menu's drawing at %dx%d: %s", w, h, SDL_GetError());
        ovl_free(dev);
        return -1;
    }
    return 0;
}

/* the backdrop from the window taken (RGBA, w x h): made small, blurred,
 * made big again and darkened, as a texture on ovl.ren */
static void ovl_backdrop(void *ud, const void *pixels, Uint32 w, Uint32 h)
{
    (void)ud;
    if (!pixels || !ovl.ren || ovl.backdrop)
        return;
    const Uint8 *px = pixels;
    int f = (int)h / 90 > 1 ? (int)h / 90 : 1, sw = (int)w / f, sh = (int)h / f;
    if (sw < 2 || sh < 2)
        return;
    float *a = SDL_malloc(sizeof(float) * 3 * (size_t)sw * (size_t)sh);
    float *b = SDL_malloc(sizeof(float) * 3 * (size_t)sw * (size_t)sh);
    SDL_Surface *out = SDL_CreateSurface((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888);
    if (a && b && out) {
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                float c[3] = { 0, 0, 0 };
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) {
                        const Uint8 *p = px + ((size_t)(y * f + j) * w + (size_t)(x * f + i)) * 4;
                        c[0] += p[0], c[1] += p[1], c[2] += p[2];
                    }
                for (int k = 0; k < 3; k++)
                    a[(y * sw + x) * 3 + k] = c[k] / (float)(f * f);
            }
        for (int pass = 0; pass < 2; pass++) {     /* a 3 x 3 box, twice */
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    for (int k = 0; k < 3; k++) {
                        float sum = 0;
                        int n = 0;
                        for (int j = y - 1; j <= y + 1; j++)
                            for (int i = x - 1; i <= x + 1; i++)
                                if (i >= 0 && i < sw && j >= 0 && j < sh) {
                                    sum += a[(j * sw + i) * 3 + k];
                                    n++;
                                }
                        b[(y * sw + x) * 3 + k] = sum / (float)n;
                    }
            float *t = a;
            a = b;
            b = t;
        }
        static const float bg[3] = { 14, 16, 22 };
        for (Uint32 y = 0; y < h; y++) {
            float fy = ((float)y + 0.5f) / (float)f - 0.5f;
            fy = fy < 0 ? 0 : fy > (float)(sh - 1) ? (float)(sh - 1) : fy;
            int y0 = (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            float ty = fy - (float)y0;
            Uint8 *d = (Uint8 *)out->pixels + (size_t)y * out->pitch;
            for (Uint32 x = 0; x < w; x++) {
                float fx = ((float)x + 0.5f) / (float)f - 0.5f;
                fx = fx < 0 ? 0 : fx > (float)(sw - 1) ? (float)(sw - 1) : fx;
                int x0 = (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
                float tx = fx - (float)x0;
                for (int k = 0; k < 3; k++) {
                    float top = a[(y0 * sw + x0) * 3 + k] * (1 - tx) + a[(y0 * sw + x1) * 3 + k] * tx;
                    float bot = a[(y1 * sw + x0) * 3 + k] * (1 - tx) + a[(y1 * sw + x1) * 3 + k] * tx;
                    d[4 * x + k] = (Uint8)((top * (1 - ty) + bot * ty) * 0.42f + bg[k] * 0.58f);
                }
                d[4 * x + 3] = 255;
            }
        }
        ovl.backdrop = SDL_CreateTextureFromSurface(ovl.ren, out);
    }
    SDL_DestroySurface(out);
    SDL_free(a);
    SDL_free(b);
}

/* the shape of the picture last shown: 640 x 480, or a wide screen's */
static Uint32 shape_w = PIC_W, shape_h = PIC_H;

/* the picture's place in a w x h target: at its own shape (4:3, or a wide
 * screen's) in the middle, or all of it */
static void picture_rect(Uint32 w, Uint32 h, Uint32 *x0, Uint32 *y0, Uint32 *dw, Uint32 *dh)
{
    *dw = w;
    *dh = h;
    if (cfg_.picture == PICTURE_STRETCH)
        ;       /* the whole window */
    else if ((Uint64)w * shape_h > (Uint64)h * shape_w) *dw = (Uint32)((Uint64)h * shape_w / shape_h);
    else *dh = (Uint32)((Uint64)w * shape_h / shape_w);
    *x0 = (w - *dw) / 2;
    *y0 = (h - *dh) / 2;
}

/* the picture at its shape (or stretched) in the middle of a w x h target,
 * and the menu (ov 1) or the operator bar (2) over it when ovl holds it */
static void compose(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, Uint32 w, Uint32 h, SDL_GPUTexture *src,
                    Uint32 src_w, Uint32 src_h, int have_picture, int ov)
{
    Uint32 x0, y0, dw, dh;
    if (have_picture && src_w && src_h) {
        shape_w = src_w;
        shape_h = src_h;
    }
    picture_rect(w, h, &x0, &y0, &dw, &dh);
    if (have_picture) {
        SDL_GPUBlitInfo bl;
        SDL_zero(bl);
        bl.source.texture = src;
        bl.source.w = src_w;
        bl.source.h = src_h;
        bl.destination.texture = target;
        bl.destination.x = x0;
        bl.destination.y = y0;
        bl.destination.w = dw;
        bl.destination.h = dh;
        bl.load_op = SDL_GPU_LOADOP_CLEAR;
        bl.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
        bl.filter = cfg_.filter == FILTER_SHARP ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &bl);
    } else {
        SDL_GPUColorTargetInfo t;
        SDL_zero(t);
        t.texture = target;
        t.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
        t.load_op = SDL_GPU_LOADOP_CLEAR;
        t.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &t, 1, NULL));
    }
    if (!ov || ovl.ready != ov)
        return;
    /* drawn for this size only: after a resize it is drawn again first */
    if (ov == 1 ? (Uint32)ovl.w != w || (Uint32)ovl.h != h : (Uint32)ovl.bar_w != dw || (Uint32)ovl.bar_h > h)
        return;
    SDL_GPUBlitInfo bl;
    SDL_zero(bl);
    bl.source.texture = ovl.tex;
    bl.source.w = ov == 1 ? w : dw;
    bl.source.h = ov == 1 ? h : (Uint32)ovl.bar_h;
    bl.destination.texture = target;
    bl.destination.x = ov == 1 ? 0 : x0;
    bl.destination.y = ov == 1 ? 0 : y0;
    bl.destination.w = bl.source.w;
    bl.destination.h = bl.source.h;
    bl.load_op = SDL_GPU_LOADOP_LOAD;
    bl.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &bl);
}

/* --keys: keys for tests */
#define KEY_ITEMS 96
static char key_list[8192];
static const char *key_item[KEY_ITEMS];
static int key_items, key_at, key_wait;

/* --keys pad:... and pad2:...: SDL's virtual gamepads, so the gamepad paths
 * run as real pads' do */
static SDL_Joystick *vpad[2];
static int vpad_turns[2];               /* turns left of the buttons held */

/* made at start-up, before the first press: SDL sends no press for a button
 * already down when a gamepad is opened. "padtype=NAME" and "pad2type=NAME"
 * give each a real pad's ids (ui_test_pad) */
static void vpad_attach(int which)
{
    if (vpad[which])
        return;
    const char *key = which ? "pad2type=" : "padtype=", *type = NULL;
    size_t kn = strlen(key);
    for (int i = 0; i < key_items; i++)
        if (!strncmp(key_item[i], key, kn))
            type = key_item[i] + kn;
    /* with a type, named as that pad is; else by what it is */
    SDL_JoystickID id = ui_test_pad(type, type ? NULL : which ? "vf4ft --keys pad 2" : "vf4ft --keys pad");
    vpad[which] = id ? SDL_OpenJoystick(id) : NULL;
    if (!vpad[which])
        logf_("--keys pad: no virtual gamepad: %s", SDL_GetError());
}

static void vpad_press(int which, const char *what)
{
    if (!vpad[which])
        return;
    char buf[64];
    SDL_strlcpy(buf, what, sizeof buf);
    char *eq = SDL_strchr(buf, '=');
    vpad_turns[which] = eq ? atoi(eq + 1) : 6;
    if (eq)
        *eq = 0;
    char *save = NULL;
    for (char *b = SDL_strtok_r(buf, "+", &save); b; b = SDL_strtok_r(NULL, "+", &save)) {
        SDL_GamepadButton bt = SDL_GetGamepadButtonFromString(b);
        if (bt == SDL_GAMEPAD_BUTTON_INVALID)
            logf_("--keys pad: no gamepad button is called \"%s\"", b);
        else
            SDL_SetJoystickVirtualButton(vpad[which], bt, true);
    }
}

static void vpad_turn(void)
{
    for (int w = 0; w < 2; w++)
        if (vpad[w] && vpad_turns[w] > 0 && --vpad_turns[w] == 0)
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
                SDL_SetJoystickVirtualButton(vpad[w], b, false);
}

/* --keys mouse:KIND@XxY: the mouse's event at window point X, Y, as SDL
 * gives it (the press and the release of a click) */
static void key_mouse(SDL_Window *w, const char *what)
{
    char kind[16] = "";
    float x = 0, y = 0;
    if (SDL_sscanf(what, "%15[a-z]@%fx%f", kind, &x, &y) != 3) {
        logf_("--keys: \"mouse:%s\" is not KIND@XxY", what);
        return;
    }
    SDL_Event e;
    SDL_zero(e);
    if (!strcmp(kind, "move")) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(w);
        e.motion.x = x, e.motion.y = y;
        SDL_PushEvent(&e);
    } else if (!strcmp(kind, "left") || !strcmp(kind, "right")) {
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.windowID = SDL_GetWindowID(w);
        e.button.button = kind[0] == 'l' ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
        e.button.down = true;
        e.button.clicks = 1;
        e.button.x = x, e.button.y = y;
        SDL_PushEvent(&e);
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.down = false;
        SDL_PushEvent(&e);
    } else if (!strcmp(kind, "up") || !strcmp(kind, "down")) {
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = SDL_GetWindowID(w);
        e.wheel.y = kind[0] == 'u' ? 1 : -1;
        e.wheel.integer_y = (Sint32)e.wheel.y;
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        e.wheel.mouse_x = x, e.wheel.mouse_y = y;
        SDL_PushEvent(&e);
    } else
        logf_("--keys: no mouse event is called \"%s\"", kind);
}

static int keys_parse(const char *list)
{
    SDL_strlcpy(key_list, list, sizeof key_list);
    char *colon = SDL_strchr(key_list, ':');
    if (!colon)
        return -1;
    *colon = 0;
    key_wait = atoi(key_list);
    char *save = NULL;
    for (char *it = SDL_strtok_r(colon + 1, ",", &save); it; it = SDL_strtok_r(NULL, ",", &save)) {
        if (key_items == KEY_ITEMS)
            return -1;
        if (!strncmp(it, "text:", 5))       /* typed text: "_" a space */
            for (char *c = it + 5; *c; c++)
                if (*c == '_')
                    *c = ' ';
        key_item[key_items++] = it;
    }
    return 0;
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
    const char *log_path = NULL, *user_arg = NULL;
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
            gpu_driver = argv[++i];
            if (!SDL_strcasecmp(gpu_driver, "d3d12")) gpu_driver = "direct3d12";   /* SDL's name for it */
        }
        else if (!strcmp(a, "--log") && v) log_path = argv[++i];
        else if (!strcmp(a, "--user") && v) user_arg = argv[++i];
        else if (!strcmp(a, "--kofi-notice") && v) kofi_frame = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--refresh-notice") && v) refresh_frame = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--deck-product") && v) settings_steam_deck_test(argv[++i]);
        else if (!strcmp(a, "--screen-hz") && v) screen_hz_test = (float)SDL_atof(argv[++i]);
        else if (!strcmp(a, "--system-hz") && v) system_hz_test = (float)SDL_atof(argv[++i]);
        else if (!strcmp(a, "--refresh-first") && v) vsync_test_first(SDL_atof(argv[++i]));
        else if (!strcmp(a, "--pace-ppm") && v)
            frame_ns = (Uint64)((double)FRAME_NS / (1.0 + SDL_atof(argv[++i]) / 1e6));
        else if (!strcmp(a, "--sync-log") && v) sync_log = fopen(argv[++i], "w");
        else if (!strcmp(a, "--stall-window") && v) {
            if (sscanf(argv[++i], "%d:%d:%d", &win_stall_turn, &win_stall_ms, &win_stall_part) < 2 || win_stall_turn < 1 ||
                win_stall_ms < 1) {
                fprintf(stderr, "vf4ft: --stall-window: TURN:MILLISECONDS[:PART]\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--hash-every") && v) hash_every = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--snapshot-at") && v && i + 2 < argc) {
            snap_at = (unsigned)strtoul(argv[++i], NULL, 10);
            snap_path = argv[++i];
        }
        else if (!strcmp(a, "--resume") && v) resume_path = argv[++i];
        else if (!strcmp(a, "--cpu-passes")) gpu_render_passes(0);
        else if (!strcmp(a, "--clock") && v) {
            int y, mo, d, h, mi, sec;
            clock_arg = argv[++i];
            if (strcmp(clock_arg, "fixed") &&
                (SDL_sscanf(clock_arg, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6 ||
                 !(clock_start = game_machine_clock_seconds(y, mo, d, h, mi, sec)))) {
                fprintf(stderr, "vf4ft: --clock: fixed, or YYYY-MM-DDThh:mm:ss (1950 to 2085)\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--rollback") && v) {
            int n = atoi(argv[++i]);
            if (n < 1 || n > 60) { fprintf(stderr, "vf4ft: --rollback: 1 to 60\n"); return 2; }
            game_machine_rollback(n, 0, 0);
            rollback_test = 1;
        }
        else if (!strcmp(a, "--net-lag") && v) {
            int ms = 0, jit = 0, loss = 0;
            if (SDL_sscanf(argv[++i], "%d,%d,%d", &ms, &jit, &loss) < 1 || ms < 0 || ms > 2000 || jit < 0 ||
                jit > 1000 || loss < 0 || loss > 50) {
                fprintf(stderr, "vf4ft: --net-lag: MS[,JITTER[,LOSS]]: 0 to 2000 ms, jitter to 1000 ms, loss to 50 in 100\n");
                return 2;
            }
            session_test_lag(ms, jit, loss);
        }
        else if (!strcmp(a, "--synctest") && v) {
            char *end = NULL;
            int n = (int)strtol(argv[++i], &end, 10), p = 120;
            if (end && *end == ',')
                p = atoi(end + 1);
            if (n < 1 || n > 60 || p < n + 1) { fprintf(stderr, "vf4ft: --synctest: N[,P], N 1 to 60, P more than N\n"); return 2; }
            game_machine_rollback(n, n, p);
            rollback_test = 1;
        }
        else if (!strcmp(a, "--host")) {
            int port = 0;
            if (v && v[0] >= '0' && v[0] <= '9')
                port = atoi(argv[++i]);
            session_config(SESSION_HOST, port, NULL);
        }
        else if (!strcmp(a, "--host-player") && v) {
            int n = atoi(argv[++i]);
            if (n != 1 && n != 2) { fprintf(stderr, "vf4ft: --host-player: 1 or 2\n"); return 2; }
            session_host_side(n - 1);
        }
        else if (!strcmp(a, "--join") && v) session_config(SESSION_GUEST, 0, argv[++i]);
        else if (!strcmp(a, "--code") && v) {
            if (session_code_typed(argv[++i])) { fprintf(stderr, "vf4ft: --code: the host's six digits\n"); return 2; }
        }
        else if (!strcmp(a, "--delay") && v) {
            session_delay = atoi(argv[++i]);
            if (session_delay < 1 || session_delay > 30) { fprintf(stderr, "vf4ft: --delay: 1 to 30\n"); return 2; }
        }
        else if (!strcmp(a, "--shot") && v && i + 2 < argc && nshots < 8) {
            shots[nshots].frame = (unsigned)strtoul(argv[++i], NULL, 10);
            shots[nshots++].path = argv[++i];
        }
        else if (!strcmp(a, "--keys") && v) {
            if (keys_parse(argv[++i])) { fprintf(stderr, "vf4ft: --keys: FRAME:KEY,KEY,shot=FILE,@FRAME,...\n"); return 2; }
        }
        else if (!strcmp(a, "--art") && v) art_dir = argv[++i];
        else if (!strcmp(a, "--art-frames") && v) {
            if (art_frames_parse(argv[++i])) { fprintf(stderr, "vf4ft: --art-frames: N,N-M/STEP,...\n"); return 2; }
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
        int model = settings_steam_deck();
        if (model != STEAM_DECK_NONE)
            logf_("device: %s%s", settings_steam_deck_name[model],
                  model == STEAM_DECK_UNKNOWN_MODEL ? " (SteamDeck=1; the model is not known)" : "");
    }
    gu_init(SDL_GetBasePath(), say_line);   /* the menu's Check for updates (updates.h) */
    session_logger(log_line);

    if (art_dir) {
        /* Steam's artwork (art.h): a blank cabinet with the default settings,
         * unpaced and silent, the GPU renderer at 4 x; nothing of the
         * player's is read or written (no settings, backup or cards) */
        art_list = art_frames(&art_n);
        settings_defaults(&cfg_);
        cfg_.sound = 0;
        cfg_.cards = 0;
        cfg_.renderer = RENDERER_GPU;
        cfg_.scale = 4;
        start_cfg = cfg_;
        unpaced = 1;
        backup_ok = 0;
        frames_max = art_list[art_n - 1] + 1;
        logf_("art: %d scenes into %s, the last at vertical blank %u", art_n, art_dir, art_list[art_n - 1]);
    } else {
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
    /* the launcher's Two PCs (docs/design.md sections 46, 47 and 50): user/two-pc.txt asks for a
     * two-PC game once ("host", "host 2" as player 2, or "join ADDRESS CODE", then the time it
     * was written), however the game is started (through Steam too). It is removed as it is
     * read; one older than two minutes is not used */
    if (session_role() == SESSION_OFF) {
        char rp[4300];
        SDL_snprintf(rp, sizeof rp, "%stwo-pc.txt", user_dir);
        size_t len = 0;
        char *t = SDL_GetPathInfo(rp, NULL) ? SDL_LoadFile(rp, &len) : NULL;
        if (t) {
            SDL_RemovePath(rp);
            char what[300] = "", *nl = SDL_strchr(t, '\n');
            long long when = nl ? SDL_strtoll(nl + 1, NULL, 10) : 0;
            SDL_Time now = 0;
            SDL_GetCurrentTime(&now);
            long long age = now / 1000000000 - when;
            SDL_strlcpy(what, t, nl && (size_t)(nl - t) < sizeof what ? (size_t)(nl - t) + 1 : sizeof what);
            what[strcspn(what, "\r\n")] = 0;
            int join = !SDL_strncmp(what, "join ", 5) && what[5];
            int host = !SDL_strcmp(what, "host") || !SDL_strcmp(what, "host 1") ? 1
                       : !SDL_strcmp(what, "host 2") ? 2 : 0;
            /* "join ADDRESS CODE": the code after the address (not logged: it is the joining
             * player's to type) */
            char *code_at = join ? SDL_strchr(what + 5, ' ') : NULL;
            if (code_at)
                *code_at++ = 0;
            if (age < 0 || age > 120)
                logf_("two-pc.txt is %lld seconds old: not used (\"%s\")", age, what);
            else if (!host && !join)
                logf_("two-pc.txt: \"%s\" is not \"host\", \"host 2\" or \"join ADDRESS CODE\"", what);
            else {
                logf_("two-pc.txt (the launcher's Two PCs): \"%s\"%s", what,
                      join ? code_at ? ", with a code" : ", with no code" : "");
                session_config(join ? SESSION_GUEST : SESSION_HOST, 0, join ? what + 5 : NULL);
                if (host)
                    session_host_side(host - 1);
                if (code_at && session_code_typed(code_at))
                    logf_("two-pc.txt: the code is not six digits");
            }
            if (code_at)
                SDL_memset(code_at, 0, SDL_strlen(code_at));
            SDL_free(t);
        }
    }
    /* the notices pause the game: not in a two-PC game, where they would pause both PCs */
    in_session = session_role() != SESSION_OFF;
    if (in_session && (snap_path || resume_path)) {     /* the session takes its own snapshots (section 49) */
        fprintf(stderr, "vf4ft: --snapshot-at and --resume are not for a two-PC game\n");
        return 2;
    }
    if (in_session && rollback_test) {                  /* the session goes back itself (section 57) */
        fprintf(stderr, "vf4ft: --rollback and --synctest are not for a two-PC game\n");
        return 2;
    }
    kofi_setup(frames_max || nshots || presses || key_items || unpaced || in_session);
    refresh_setup(frames_max || nshots || presses || key_items || unpaced || in_session);
    {
        char *sp = settings_path;
        SDL_snprintf(sp, sizeof settings_path, "%ssettings.txt", user_dir);
        settings_defaults_here(&cfg_);      /* a Steam Deck's 16:10 picture (section 44) */
        if (!SDL_GetPathInfo(sp, NULL))
            logf_("no %s: the defaults", sp);
        else if (!settings_load(&cfg_, sp, say_line))
            logf_("settings from %s", sp);
        /* Ray tracing through DirectX 12 (section 75) is the Windows game's alone: its menu
         * offers it; this game, built for Linux, takes the file's word for it as off */
#ifdef _WIN32
        options_directx(1);
#else
        if (cfg_.raytracing == RAYTRACING_DIRECTX) {
            cfg_.raytracing = RAYTRACING_OFF;
            logf_("settings: raytracing = directx is the Windows game's alone: off in this game (vulkan traces the "
                  "same light here)");
        }
#endif
        SDL_snprintf(backup_path, sizeof backup_path, "%sbackup.bin", user_dir);
        start_cfg = cfg_;
        live_fullscreen = cfg_.fullscreen;
        SDL_memcpy(card_chosen, cfg_.card, sizeof card_chosen);
    }
    }
    if (!cfg_.sound)
        silent = 1;
    if (!fullscreen)
        fullscreen = fullscreen_wanted();
    logf_("settings: renderer %s x%d, coins %s, fullscreen %s, picture %s, filter %s, sound %s (volume %d%%), cards %s, "
          "network %s, region %s, lighting %s, frame skip %s, ambient occlusion %s, ray tracing %s (quality %s)",
          cfg_.renderer == RENDERER_GPU ? "gpu" : "software", cfg_.scale,
          cfg_.coins == COINS_ARCADE ? "arcade" : "free play", fullscreen ? "on" : "off",
          settings_picture_name(cfg_.picture), cfg_.filter == FILTER_SHARP ? "sharp" : "smooth",
          silent ? "off" : "on", cfg_.volume, cfg_.cards ? "on" : "off", cfg_.network ? "on" : "off",
          settings_region_text[cfg_.region], settings_lighting_name[cfg_.lighting], settings_skip_name[cfg_.skip],
          settings_occlusion_name[cfg_.occlusion], settings_raytracing_name[cfg_.raytracing],
          settings_rayquality_name[cfg_.rayquality]);
    logf_("settings: gpu %s, display %s", cfg_.gpu[0] ? cfg_.gpu : "auto", cfg_.display[0] ? cfg_.display : "auto");
    /* the board's clock (section 77): this PC's, but for --clock and the artwork's run (art.h:
     * its scenes are found by their vertical blanks, in the run the fixed date gives) */
    if (clock_arg) {
        if (clock_start)
            clock_whose = "--clock";
    } else if (!art_dir) {
        SDL_Time now;
        SDL_DateTime dt;
        if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
            clock_start = game_machine_clock_seconds(dt.year, dt.month, dt.day, dt.hour, dt.minute,
                                                     dt.second > 59 ? 59 : dt.second);
        if (clock_start)
            clock_whose = "this PC's date and time";
        else
            logf_("clock: this PC's date and time could not be read: the fixed date instead");
    }
    if (in_session)
        logf_("settings: a two-PC game: the stage's far objects are drawn for a 21:9 picture on both PCs");
    session_rollback(cfg_.latency);
    if (in_session)
        logf_("settings: latency optimization %s%s", settings_latency_name[cfg_.latency],
              settings_steam_deck() != STEAM_DECK_NONE && cfg_.latency != LATENCY_OFF
                  ? " (a Steam Deck may not be fast enough for it: Off if the game slows down)" : "");
    logf_("image %s; DIMM %s%s%s", image_path ? image_path : "(from the DIMM image)", dimm_path,
          dimm_key ? ", key " : "", dimm_key ? dimm_key : "");

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (silent ? 0 : SDL_INIT_AUDIO))) {
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    logf_("video driver: %s", SDL_GetCurrentVideoDriver());
    {
        /* section 85: what this PC does when left alone, which a player with a gamepad alone is
         * to Windows. SDL asks the system to keep the screen saver and the display's switching
         * off away while the game runs (SDL_DisableScreenSaver, its default): said here too */
        char alone[320];
        sys_idle_describe(alone, sizeof alone);
        if (alone[0])
            logf_("this PC left alone: %s; kept away by the game while it runs (SDL): %s", alone,
                  SDL_ScreenSaverEnabled() ? "no" : "yes");
    }
    ui_pad_logger(log_line);
    bindings_compile();     /* key names need the video subsystem */
    for (int w = 0; w < 2; w++)         /* the first before the second: the order they connect in */
        for (int i = 0; i < key_items; i++)
            if (!strncmp(key_item[i], w ? "pad2:" : "pad:", w ? 5 : 4) ||
                !strncmp(key_item[i], w ? "pad2type=" : "padtype=", w ? 9 : 8)) {
                vpad_attach(w);
                break;
            }
    if (!silent) {
        SDL_AudioSpec as = { SDL_AUDIO_S16, 2, 44100 };
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &as, NULL, NULL);
        if (!audio)
            logf_("no sound: cannot open the audio device: %s", SDL_GetError());
        else {
            static short quiet[2 * AUDIO_TARGET];
            SDL_PutAudioStreamData(audio, quiet, sizeof quiet);
            SDL_SetAudioStreamGain(audio, cfg_.volume / 100.0f);
            SDL_ResumeAudioStreamDevice(audio);
            logf_("audio driver: %s", SDL_GetCurrentAudioDriver());
        }
    }
    shared.lock = SDL_CreateMutex();
    machine_event = SDL_RegisterEvents(1);

    SDL_Window *win = NULL;
    if (!art_dir) {
        /* the wide screen (Steam's pictures, --art, are always 4:3). The
         * window starts at Window size on the primary screen, its pixels
         * taken as coordinates; then the size is set again for the screen
         * it opened on, in its own pixels (a scaled Wayland screen) */
        int ww, wh;
        displays_refresh(0);
        for (int i = 0; i < disp_n; i++)
            logf_("display %d: %s%s", i + 1, disp_names[i], disp_ids[i] == SDL_GetPrimaryDisplay() ? " (primary)" : "");
        SDL_DisplayID want = display_wanted();      /* Display (section 51); 0 Auto: the system places it */
        if (cfg_.display[0] && !want)
            logf_("settings: display \"%s\" is not connected: Auto for this run (it is kept for the next)",
                  cfg_.display);
        window_room room = screen_room(NULL);
        game_machine_wide(settings_picture_width(cfg_.picture));
        game_machine_cull(cull_width());
        game_machine_lighting(cfg_.lighting == LIGHTING_GPU);
        game_machine_every(cfg_.skip == SKIP_HALF ? 2 : 1);
        gpu_render_occlusion(cfg_.occlusion == OCCLUSION_SSAO);
        gpu_render_raytracing(raytracing_wanted(), cfg_.rayquality);
        settings_window_size(&cfg_, &room, &ww, &wh);
        const char *title = !in_session ? VF4FT_TITLE
                            : session_role() == SESSION_GUEST ? VF4FT_TITLE " (joining)"    /* its player once joined */
                            : session_host_side_get() ? VF4FT_TITLE " (player 2, hosting)"
                                                      : VF4FT_TITLE " (player 1, hosting)";
        SDL_PropertiesID wp = SDL_CreateProperties();
        SDL_SetStringProperty(wp, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, ww);
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wh);
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  (fullscreen ? SDL_WINDOW_FULLSCREEN
                                   : cfg_.window == WINDOW_MAXIMIZED ? SDL_WINDOW_MAXIMIZED : 0));
        if (want) {
            /* in the middle of Display's display, and fullscreen there (SDL takes the display a
             * window is centered on for its fullscreen, on Wayland too, which places windows itself) */
            SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(want));
            SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(want));
        }
        win = SDL_CreateWindowWithProperties(wp);
        SDL_DestroyProperties(wp);
        if (!win) { logf_("SDL_CreateWindow failed: %s", SDL_GetError()); return 1; }
        main_win = win;
        SDL_SetWindowMinimumSize(win, MENU_MIN_W, MENU_MIN_H);  /* the menu always fits */
        logf_("window: %d x %d pixels (Window size %s, %s screen %d x %d)%s", ww, wh,
              cfg_.window == WINDOW_MAXIMIZED ? "Maximized" : cfg_.window == WINDOW_AUTO ? "Auto" : "set",
              want ? "Display's" : "primary", room.w, room.h,
              fullscreen ? "; fullscreen" : cfg_.window == WINDOW_MAXIMIZED ? "; maximized" : "");
        SDL_SyncWindow(win);        /* its frame, for the room left (X11) */
        window_to_setting(win, 1);
        {
            /* where it opened. A Wayland window's screen is the compositor's
             * choice and SDL knows it only once the window shows ("now on"
             * below); a fullscreen one goes to Display's */
            const char *drv = SDL_GetCurrentVideoDriver();
            int fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0;
            if (!fs && drv && !SDL_strcmp(drv, "wayland")) {
                logf_("window: on the screen Wayland chooses%s", display_wanted() ? " (Display: fullscreen only)" : "");
            } else {
                int wx = 0, wy = 0;
                win_on = SDL_GetDisplayForWindow(win);
                const char *dn = SDL_GetDisplayName(win_on);
                SDL_GetWindowPosition(win, &wx, &wy);
                logf_("window: on %s at %d, %d%s", dn ? dn : "?", wx, wy, fs ? ", fullscreen" : "");
            }
        }
    }
    /* The device and the renderer on it (video_open, docs/design.md section
     * 53). The GPU renderer's shaders are SPIR-V: a Vulkan device (natively,
     * or Proton's winevulkan) */
    if (video_open()) {
        logf_("no GPU device can show the window");
        return 1;
    }
    if (use_gpu)
        game_machine_gpu(gpu_frame);
    if (art_dir && !use_gpu) {
        logf_("art: no GPU renderer, so no artwork");
        return 3;
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
    if (art_dir)
        return art_loop(gpu, machine);


    /* the picture (video_open's pic_tex) is blitted to the window, scaled;
     * the menu's backdrop and --keys shot= take the window into textures of
     * this kind */
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = PIC_W_MAX;
    ti.height = PIC_H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    static unsigned char rgb[PIC_W_MAX * PIC_H * 3];
    int rgb_w = PIC_W;                  /* the software picture's width */

    Uint64 t0 = SDL_GetTicksNS(), chord_since = 0, dir_next = 0;
    unsigned shown_frame = 0, pictures = 0, presents = 0;
    int last_sc = 0;                    /* the turn before presented (the window is not minimized) */
    int have_picture = 0, running = 1, reported_start = 0;
    int chord_used = 0, last_dir = -1, last_program = 0, key_turns = 0;
    compositor_watch_start(logf_, window_stuck);    /* section 54 */
    win_at(WIN_LOOP);
    SDL_SetAtomicInt(&win_watch_on, 1);
    win_watch_wake = SDL_CreateSemaphore(0);
    SDL_Thread *win_watcher = SDL_CreateThread(win_watch, "window watch", NULL);    /* section 83 */
    while (running) {
        const char *window_shot = NULL;
        win_at(WIN_LOOP);
        SDL_AddAtomicInt(&win_turn, 1);
        SDL_SetAtomicInt(&win_flags_now, (int)(Uint32)SDL_GetWindowFlags(win));
        if (gu_poll() && menu_on)
            ov_dirty = 1;           /* the Updates row's check answered */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ui_pad_event_hidden(&ev))
                continue;           /* the real pad behind a Steam Input pad (ui.h) */
            if (ui_note_event(&ev) && (menu_on || bar_on))
                ov_dirty = 1;       /* its hints show the keys or the buttons of the pad last used */
            if (ev.type == machine_event) {
                running = 0;
                continue;
            }
            /* a cell of the menu's Gamepad or Keyboard tab waits for its
             * button or key: this one is the menu's first (section 72) */
            int menu_took = 0;
            if (menu_on && menu_capturing() &&
                (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                 ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)) {
                int took = menu_capture(&ev);
                menu_took = took != 0;
                if (took) {
                    ov_dirty = 1;
                    if (took == 2)
                        menu_do(MENU_LIVE);
                }
            }
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = 0;
                break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                logf_("window: fullscreen off");
                if (window_later)
                    window_to_setting(win, 1);  /* Window size, changed while it was fullscreen */
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                logf_("window: fullscreen");    /* its screen: "now on" when it moved (Wayland: after this) */
                break;
            case SDL_EVENT_WINDOW_MAXIMIZED:
            case SDL_EVENT_WINDOW_RESTORED: {
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(win, &pw, &ph);
                logf_("window: %s (%d x %d pixels)",
                      ev.type == SDL_EVENT_WINDOW_MAXIMIZED ? "maximized" : "no longer maximized or minimized", pw, ph);
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                logf_("window: in front (it has the keyboard)");
                vsync_rate_forget();            /* what its presents did behind others says nothing (section 61) */
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                logf_("window: no longer in front");
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
                logf_("window: minimized");
                break;
            case SDL_EVENT_WINDOW_OCCLUDED:
                logf_("window: covered by others");
                break;
            case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: {
                const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(ev.display.displayID);
                const char *dn = SDL_GetDisplayName(ev.display.displayID);
                if (dm)
                    logf_("display: %s is now at %d x %d, %.2f Hz (%s)", dn ? dn : "?", dm->w, dm->h, (double)dm->refresh_rate,
                          ev.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED ? "its mode changed" : "the desktop's mode changed");
                break;
            }
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED: {
                /* where it is now (moved, shown by Wayland, fullscreen on
                 * Display's screen), and Window size's sizes for that screen */
                SDL_DisplayID on = SDL_GetDisplayForWindow(win);
                if (on == win_on)
                    break;                      /* SDL tells some moves more than once */
                win_on = on;
                vsync_reset();                  /* another screen's refresh */
                const char *dn = SDL_GetDisplayName(on);
                logf_("window: now on %s", dn ? dn : "?");
                if (menu_on && win) {
                    window_room room = screen_room(win);
                    options_screen(&room);
                    ov_dirty = 1;
                }
                break;
            }
            case SDL_EVENT_DISPLAY_ADDED:
            case SDL_EVENT_DISPLAY_REMOVED:
                displays_refresh(1);            /* the window stays where the system puts it */
                if (menu_on && win) {
                    window_room room = screen_room(win);
                    options_display_names(disp_list, disp_n);
                    options_screen(&room);
                    ov_dirty = 1;
                }
                break;
            case SDL_EVENT_KEY_DOWN: {
                SDL_Keycode key = ev.key.key;
                int cmd = -1;
                if (menu_took)
                    break;                      /* a controls cell's key (section 72) */
                if (!ev.key.repeat && (key == SDLK_F11 || (key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
                    fullscreen_to(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                else if (menu_on && menu_typing()) {
                    /* the ring name's note: the letters come as text (SDL_EVENT_TEXT_INPUT) */
                    if (!ev.key.repeat && (key == SDLK_RETURN || key == SDLK_KP_ENTER))
                        cmd = MENU_OK;
                    else if (!ev.key.repeat && (key == SDLK_ESCAPE || key == SDLK_INSERT))
                        cmd = MENU_ESCAPE;
                    else if (key == SDLK_BACKSPACE)
                        cmd = MENU_ERASE;
                    else if (!ev.key.repeat && (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_TAB))
                        cmd = key == SDLK_LEFT ? MENU_LEFT : MENU_RIGHT;
                } else if (!ev.key.repeat && (key == SDLK_INSERT || key == SDLK_ESCAPE)) {
                    if (menu_on)
                        cmd = MENU_ESCAPE;      /* the card editor first, else the menu closes */
                    else
                        menu_show();
                } else if (menu_on) {
                    if (key == SDLK_UP) cmd = MENU_UP;
                    else if (key == SDLK_DOWN) cmd = MENU_DOWN;
                    else if (key == SDLK_LEFT) cmd = MENU_LEFT;
                    else if (key == SDLK_RIGHT) cmd = MENU_RIGHT;
                    else if (!ev.key.repeat && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE))
                        cmd = MENU_OK;
                    else if (!ev.key.repeat && key == SDLK_BACKSPACE)
                        cmd = MENU_ERASE;       /* back to the game; the card editor's Undo */
                    else if (!ev.key.repeat && key == SDLK_DELETE)
                        cmd = MENU_CLEAR;       /* a controls cell emptied */
                    else if (!ev.key.repeat && (key == SDLK_Q || key == SDLK_PAGEUP))
                        cmd = MENU_PREV;
                    else if (!ev.key.repeat && (key == SDLK_E || key == SDLK_PAGEDOWN))
                        cmd = MENU_NEXT;
                }
                if (cmd >= 0 && !menu_do(menu_command(cmd)))
                    running = 0;
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                int b = ev.gbutton.button;
                int face = ui_pad_face(ev.gbutton.which, b);    /* A selects, B goes back: by label */
                if (menu_took)
                    break;                      /* a controls cell's button (section 72) */
                if (b == SDL_GAMEPAD_BUTTON_GUIDE) {
                    if (menu_on)
                        menu_do(menu_command(MENU_ESCAPE));
                    else
                        menu_show();
                } else if (menu_on && face == UI_IN_OK) {
                    if (!menu_do(menu_command(MENU_OK)))
                        running = 0;
                } else if (menu_on && (face == UI_IN_BACK || b == SDL_GAMEPAD_BUTTON_START)) {
                    menu_do(menu_command(MENU_BACK));
                } else if (menu_on && (face == UI_IN_DEFAULT || face == UI_IN_CLEAR)) {
                    menu_do(menu_command(face == UI_IN_DEFAULT ? MENU_UNDO : MENU_CLEAR));
                } else if (menu_on && (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
                    menu_do(menu_command(b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ? MENU_PREV : MENU_NEXT));
                } else if (!menu_on && bar_on && face == UI_IN_BACK) {
                    bar_on = 0;
                    pad_hold |= 1u << b;
                    logf_("menu: the operator's buttons are the game's again");
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_WHEEL: {
                if (!menu_on)
                    break;
                /* the menu is drawn over the whole window in pixels; the
                 * mouse is in window units (logical on Wayland) */
                float k = SDL_GetWindowPixelDensity(win), mx, my;
                int what, d = 0;
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    what = MENU_MOUSE_MOVE;
                    mx = ev.motion.x, my = ev.motion.y;
                } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    what = MENU_MOUSE_WHEEL;
                    mx = ev.wheel.mouse_x, my = ev.wheel.mouse_y;
                    float wy = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y;
                    d = wy > 0 ? -1 : wy < 0 ? 1 : 0;   /* up the list for a turn away */
                    if (!d)
                        break;
                } else {
                    if (ev.button.button != SDL_BUTTON_LEFT && ev.button.button != SDL_BUTTON_RIGHT)
                        break;
                    what = ev.button.button == SDL_BUTTON_LEFT ? MENU_MOUSE_LEFT : MENU_MOUSE_RIGHT;
                    mx = ev.button.x, my = ev.button.y;
                }
                int a = menu_mouse(what, mx * (k > 0 ? k : 1), my * (k > 0 ? k : 1), d);
                if (a >= 0 && !menu_do(a))
                    running = 0;
                break;
            }
            case SDL_EVENT_TEXT_INPUT:
                if (menu_on && menu_typing()) {
                    menu_text(ev.text.text);
                    ov_dirty = 1;
                }
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
        {
            /* the keyboard's text while the ring name's note is up (on a
             * Steam Deck, Steam + X shows its keyboard) */
            static int text_on;
            int want = menu_on && menu_typing();
            if (want != text_on) {
                text_on = want;
                if (want)
                    SDL_StartTextInput(win);
                else
                    SDL_StopTextInput(win);
            }
        }
        int st = SDL_GetAtomicInt(&machine_state);
        if (st == 1 && !reported_start) {
            logf_("machine running");
            reported_start = 1;
            if (in_session && session_role() == SESSION_GUEST)
                SDL_SetWindowTitle(win, sess.side ? VF4FT_TITLE " (player 2)" : VF4FT_TITLE " (player 1)");
        }
        Uint64 now_ms = SDL_GetTicks();

        /* the controls: the menu's chord and the operator bar's buttons are
         * kept from the game, as is what was held when the menu closed */
        static unsigned last_pad;
        pad_ignore = pad_hold | ((last_pad & CHORD) == CHORD ? CHORD : 0);
        pad_ignore_bar = bar_on;
        game_input in;
        unsigned pad = read_controls(&in);
        last_pad = pad;
        pad_hold &= pads_raw;
        {
            int nk = 0;
            const bool *k = SDL_GetKeyboardState(&nk);
            for (int i = 0; i < nk && i < SDL_SCANCODE_COUNT; i++)
                if (key_ignore[i] && !k[i])
                    key_ignore[i] = false;
        }
        /* View + Menu held for a second opens the menu (once a hold) */
        if ((pad & CHORD) == CHORD) {
            if (!chord_since)
                chord_since = now_ms;
            if (!chord_used && now_ms - chord_since >= 1000) {
                chord_used = 1;
                if (!menu_on) {
                    logf_("menu: View + Menu held");
                    menu_show();
                }
            }
        } else {
            chord_since = 0;
            chord_used = 0;
        }
        /* the operator bar: A the Test switch, X Service; it ends with B or
         * when the test program's EXIT starts the game again */
        int program = SDL_GetAtomicInt(&program_now);
        if (program != last_program) {
            if (!program && bar_on) {
                bar_on = 0;
                logf_("menu: the game again: the operator's buttons are the game's again");
            }
            last_program = program;
        }
        if (bar_on && !menu_on)
            for (int i = 0; i < 2; i++) {
                unsigned b = pad_raw[i] & ~pad_hold;
                for (int f = SDL_GAMEPAD_BUTTON_SOUTH; f <= SDL_GAMEPAD_BUTTON_NORTH; f++)
                    if (b >> f & 1) {
                        int face = ui_pad_face(pad_id[i], f);
                        if (face == UI_IN_OK)
                            in.sys |= GAME_SYS_TEST;
                        else if (face == UI_IN_CLEAR)       /* the X button */
                            in.sys |= GAME_SYS_SERVICE;
                    }
            }
        /* in the menu: the D-pad or left stick of any pad moves, held it repeats.
         * Not while a controls cell waits for its button, nor after it until
         * the D-pad or stick that may have been the button is let go */
        static int dir_wait;
        if (menu_on && menu_capturing())
            dir_wait = 1;
        if (menu_on) {
            memset(&in, 0, sizeof in);
            int dir = -1, n = 0;
            SDL_JoystickID *ids = ui_gamepads(&n);
            for (int i = 0; ids && i < n && dir < 0; i++) {
                SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
                if (!g)
                    continue;
                float x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
                float y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
                if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -0.6f) dir = MENU_UP;
                else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 0.6f) dir = MENU_DOWN;
                else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -0.6f) dir = MENU_LEFT;
                else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 0.6f) dir = MENU_RIGHT;
            }
            SDL_free(ids);
            int fire = 0;
            if (dir_wait) {
                if (dir < 0 && !menu_capturing())
                    dir_wait = 0;
                last_dir = dir;
            } else if (dir != last_dir) {
                fire = dir >= 0;
                dir_next = now_ms + 350;
            } else if (dir >= 0 && now_ms >= dir_next) {
                fire = 1;
                dir_next = now_ms + 110;
            }
            last_dir = dir;
            if (fire && !menu_do(menu_command(dir)))
                running = 0;
        } else {
            last_dir = -1;
        }

        /* --keys */
        vpad_turn();
        if (key_at < key_items && !vpad_turns[0] && !vpad_turns[1] && SDL_GetAtomicInt(&frame_now) >= key_wait &&
            ++key_turns >= 8) {
            const char *it = key_item[key_at++];
            key_turns = 0;
            if (it[0] == '@')
                key_wait = atoi(it + 1);
            else if (!strncmp(it, "shot=", 5))
                window_shot = it + 5;
            else if (!strncmp(it, "pad:", 4))
                vpad_press(0, it + 4);
            else if (!strncmp(it, "pad2:", 5))
                vpad_press(1, it + 5);
            else if (!strncmp(it, "padtype=", 8) || !strncmp(it, "pad2type=", 9))
                ;                   /* taken at start-up (vpad_attach) */
            else if (!strncmp(it, "mouse:", 6))
                key_mouse(win, it + 6);
            else if (!strncmp(it, "text:", 5)) {
                SDL_Event t;
                SDL_zero(t);
                t.type = SDL_EVENT_TEXT_INPUT;
                t.text.windowID = SDL_GetWindowID(win);
                t.text.text = it + 5;           /* key_list's: it lasts the run */
                SDL_PushEvent(&t);
            }
            else {
                SDL_Event k;
                SDL_zero(k);
                k.type = SDL_EVENT_KEY_DOWN;
                k.key.windowID = SDL_GetWindowID(win);
                k.key.key = SDL_GetKeyFromName(it);
                k.key.scancode = SDL_GetScancodeFromName(it);
                k.key.down = true;
                if (k.key.key == SDLK_UNKNOWN)
                    logf_("--keys: no key is called \"%s\"", it);
                else
                    SDL_PushEvent(&k);
            }
        }

        /* the compositor has gone: no more frames, as one could wait forever (section 54) */
        if (display_gone())
            break;

        /* the menu changed Renderer, GPU or Internal resolution: the renderer
         * starts again before this frame (section 53); no picture until the
         * next one comes, from the renderer now in use */
        if (video_redo && running) {
            video_redo = 0;
            compositor_busy(1);         /* it gives the window back and claims it again */
            win_at(WIN_SWITCH);
            int bad = video_switch();
            win_at(WIN_LOOP);
            compositor_busy(0);
            if (bad) {
                running = 0;
                break;
            }
            have_picture = 0;
            SDL_LockMutex(shared.lock);
            if (shared.pic_n)               /* the software pictures made so far are not shown */
                shown_frame = shared.pic_frame[shared.pic_n - 1];
            shared.pic_n = 0;
            SDL_UnlockMutex(shared.lock);
        }
        unsigned frame;
        /* Frame pacing (docs/design.md sections 58 and 59). This present takes a frame: the
         * software picture here, the GPU renderer's after the swapchain's wait. Game clock: the
         * newest waiting whose refresh has come (vsync_pick). Screen refresh: the machine's pace
         * follows these moments, and on a screen that refreshes several times to a frame only
         * the first of them takes a new one (vsync_take). Off: the newest, as it comes. A two-PC
         * game goes by the host's choice on both PCs (pacing_in_use) */
        int pacing = pacing_in_use();
        SDL_SetAtomicInt(&pacing_now, pacing);
        int take = 1;
        if (!use_gpu && last_sc)
            take = vsync_take(SDL_GetTicksNS());
        unsigned wait_frame[2], upto = take ? ~0u : 0;
        Uint64 wait_pts[2];
        SDL_LockMutex(shared.lock);
        shared.in = in;
        int waiting = shared.pic_n;
        for (int i = 0; i < waiting; i++) {
            wait_frame[i] = shared.pic_frame[i];
            wait_pts[i] = shared.pic_pts[i];
        }
        SDL_UnlockMutex(shared.lock);
        if (!use_gpu && last_sc && pacing == PACING_GAME)
            upto = vsync_pick(wait_frame, wait_pts, waiting);
        SDL_LockMutex(shared.lock);
        int got = -1;
        for (int i = 0; i < shared.pic_n; i++)
            if (shared.pic_frame[i] <= upto)
                got = i;
        frame = shown_frame;
        if (got >= 0) {
            pics_lost += (unsigned)got;             /* the one before it is passed over */
            frame = shared.pic_frame[got];
            rgb_w = shared.pic_w[got];
            memcpy(rgb, shared.pic[got], (size_t)rgb_w * PIC_H * 3);
            if (got == 0 && shared.pic_n == 2) {
                memcpy(shared.pic[0], shared.pic[1], (size_t)shared.pic_w[1] * PIC_H * 3);
                shared.pic_w[0] = shared.pic_w[1];
                shared.pic_frame[0] = shared.pic_frame[1];
                shared.pic_pts[0] = shared.pic_pts[1];
            }
            shared.pic_n -= got + 1;
        }
        SDL_UnlockMutex(shared.lock);

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
        if (!cmd) { logf_("acquire command buffer failed: %s", SDL_GetError()); break; }
        if (frame != shown_frame) {
            unsigned char *dst = SDL_MapGPUTransferBuffer(gpu, upload, true);
            if (dst) {
                for (int i = 0; i < rgb_w * PIC_H; i++) {
                    dst[4 * i] = rgb[3 * i];
                    dst[4 * i + 1] = rgb[3 * i + 1];
                    dst[4 * i + 2] = rgb[3 * i + 2];
                    dst[4 * i + 3] = 255;
                }
                SDL_UnmapGPUTransferBuffer(gpu, upload);
                SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureTransferInfo src = { .transfer_buffer = upload, .offset = 0 };
                SDL_GPUTextureRegion reg = { .texture = pic_tex, .w = (Uint32)rgb_w, .h = PIC_H, .d = 1 };
                SDL_UploadToGPUTexture(cp, &src, &reg, true);
                SDL_EndGPUCopyPass(cp);
                have_picture = 1;
                pictures++;
            }
            shown_frame = frame;
        }
        kofi_turn(have_picture, now_ms);
        refresh_turn(have_picture, now_ms);
        /* a two-PC game's note: waiting for the other player, or why it ended and what now (the
         * host's game going on alone: for 8 seconds) */
        static char note[320], said[320];
        static Uint64 said_ms;          /* when the session's words last changed */
        if (in_session) {
            char now_note[320];
            session_status(now_note, sizeof now_note);
            if (strcmp(now_note, said)) {
                SDL_strlcpy(said, now_note, sizeof said);
                said_ms = now_ms;
            }
            /* a PC joining (section 49) is said until it has joined */
            int alone = SDL_GetAtomicInt(&alone_ms) != 0, joining = session_joining();
            char where[96];
            if (!joining && alone && now_ms - said_ms > 8000) {
                now_note[0] = 0;
            } else if (!joining && now_note[0] && !alone && SDL_GetAtomicInt(&session_over)) {
                SDL_strlcat(now_note, " The game stopped.", sizeof now_note);
            } else if (!joining && now_note[0] && alone) {
                /* section 50: the join code with the address */
                char code[16];
                int cs = session_code(code, sizeof code);
                size_t l = SDL_strlen(now_note);
                if (!session_can_join(where, sizeof where) || cs == SESSION_CODE_NONE)
                    SDL_strlcat(now_note, " Your game goes on: you play alone.", sizeof now_note);
                else if (cs == SESSION_CODE_GONE)
                    SDL_strlcat(now_note, " Your game goes on: you play alone. For another PC to join, get a new "
                                "code in the game's menu (Join code).", sizeof now_note);
                else if (!where[0])
                    SDL_snprintf(now_note + l, sizeof now_note - l, " Your game goes on: you play alone until another "
                                 "PC joins, with the code %s.", code);
                else
                    SDL_snprintf(now_note + l, sizeof now_note - l, " Your game goes on: you play alone, and another "
                                 "PC can join %s with the code %s.", where, code);
            }
            if (strcmp(now_note, note)) {
                SDL_strlcpy(note, now_note, sizeof note);
                ov_dirty = 1;
            }
        }
        /* the menu (1), the operator bar (2) or the two-PC game's note (3), drawn again when it
         * changed; the menu's first turn shows the picture alone and takes it (grab) */
        int ov = menu_on ? 1 : bar_on && now_ms < bar_until ? 2 : note[0] ? 3 : 0, grab = 0;
        if (ov) {
            int ww = 0, wh = 0;
            SDL_GetWindowSizeInPixels(win, &ww, &wh);
            if (ww > 0 && wh > 0 && (ww != ovl.w || wh != ovl.h) && ovl_size(gpu, ww, wh) == 0)
                ov_dirty = 1;
            grab = ov == 1 && ovl.ren && !ovl.taken;
        }
        if (ov && ovl.ren && !grab && (ov_dirty || ov != ovl.ready)) {
            SDL_SetRenderDrawColor(ovl.ren, 0, 0, 0, 0);
            SDL_RenderClear(ovl.ren);
            Uint32 px0, py0, pdw, pdh;
            picture_rect((Uint32)ovl.w, (Uint32)ovl.h, &px0, &py0, &pdw, &pdh);
            int rw = ovl.w, rh = ovl.h;
            if (ov == 1) {
                menu_draw(ovl.ren, ovl.w, ovl.h, ovl.backdrop);
            } else {
                rw = (int)pdw;
                rh = menu_bar_height((int)pdh) < ovl.h ? menu_bar_height((int)pdh) : ovl.h;
                if (ov == 3)
                    menu_draw_note(ovl.ren, rw, rh, note);
                else
                    menu_draw_bar(ovl.ren, rw, rh);
            }
            SDL_FlushRenderer(ovl.ren);
            unsigned char *dst = SDL_MapGPUTransferBuffer(gpu, ovl.up, true);
            int locked = dst && SDL_LockSurface(ovl.surf);
            if (locked) {
                for (int y = 0; y < rh; y++)
                    memcpy(dst + (size_t)y * (size_t)rw * 4,
                           (unsigned char *)ovl.surf->pixels + (size_t)y * ovl.surf->pitch, (size_t)rw * 4);
                SDL_UnlockSurface(ovl.surf);
            }
            if (dst)
                SDL_UnmapGPUTransferBuffer(gpu, ovl.up);
            if (locked) {
                SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureTransferInfo src = { .transfer_buffer = ovl.up, .offset = 0,
                                                   .pixels_per_row = (Uint32)rw, .rows_per_layer = (Uint32)rh };
                SDL_GPUTextureRegion reg = { .texture = ovl.tex, .w = (Uint32)rw, .h = (Uint32)rh, .d = 1 };
                SDL_UploadToGPUTexture(cp, &src, &reg, true);
                SDL_EndGPUCopyPass(cp);
                ovl.ready = ov;
                ovl.bar_w = rw;
                ovl.bar_h = rh;
            }
            ov_dirty = 0;
        }
        SDL_GPUTexture *sc = NULL;
        Uint32 w = 0, h = 0;
        compositor_busy(1);     /* the frame, to its end: it may never finish once the compositor has gone */
        /* The wait for the screen's next buffer, in its two parts (section 85), each a step the
         * watch can name: SDL_WaitAndAcquireGPUSwapchainTexture is these two in one call. First
         * the frame handed over a few turns before must be finished by the graphics chip (SDL
         * keeps no more than that many in flight); then the driver gives the next buffer, and SDL
         * waits for it without an end. The second call would give no buffer where the first
         * frame was not finished: after the first it is */
        int stall = win_stall_ms && SDL_GetAtomicInt(&win_turn) == win_stall_turn;
        win_at(WIN_EARLIER);
        if (stall && win_stall_part == 1)
            SDL_Delay((Uint32)win_stall_ms);    /* --stall-window T:MS:1 */
        if (!SDL_WaitForGPUSwapchain(gpu, win)) {
            logf_("wait for the swapchain failed: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        win_at(WIN_ACQUIRE);
        if (stall && win_stall_part != 1)
            SDL_Delay((Uint32)win_stall_ms);    /* --stall-window */
        if (!SDL_AcquireGPUSwapchainTexture(cmd, win, &sc, &w, &h)) {
            logf_("acquire swapchain failed: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        win_at(WIN_DRAW);
        last_sc = sc != NULL;
        SDL_GPUTexture *src_tex = pic_tex;
        Uint32 src_w = (Uint32)rgb_w, src_h = PIC_H;
        if (use_gpu) {
            unsigned gf = 0;
            if (sc)
                take = vsync_take(SDL_GetTicksNS());
            upto = take ? ~0u : 0;
            if (sc && pacing == PACING_GAME) {
                waiting = gpu_render_waiting(wait_frame, wait_pts);
                upto = vsync_pick(wait_frame, wait_pts, waiting);
            }
            SDL_GPUTexture *t = gpu_render_frame(cmd, &src_w, &src_h, &gf, upto);
            if (sync_log && sc && t)
                fprintf(sync_log, "P %u %llu\n", gf, (unsigned long long)SDL_GetTicksNS());
            if (t) {
                src_tex = t;
                if (gf != shown_frame)
                    pictures++;
                shown_frame = gf;
                have_picture = 1;
            }
        }
        if (sc) {
            compose(cmd, sc, w, h, src_tex, src_w, src_h, have_picture, grab ? 0 : ov);
            if (grab) {
                if (!ovl.grab || ovl.grab_w != w || ovl.grab_h != h) {
                    if (ovl.grab)
                        SDL_ReleaseGPUTexture(gpu, ovl.grab);
                    SDL_GPUTextureCreateInfo gi = ti;
                    gi.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    gi.width = ovl.grab_w = w;
                    gi.height = ovl.grab_h = h;
                    ovl.grab = SDL_CreateGPUTexture(gpu, &gi);
                }
                if (ovl.grab)
                    compose(cmd, ovl.grab, w, h, src_tex, src_w, src_h, have_picture, 0);
            }
            SDL_AddAtomicInt(&presents_n, 1);
            if (presents++ == 0)
                logf_("first present: swapchain %ux%u", w, h);
            if (window_shot) {
                if (!shot_tex || shot_w != w || shot_h != h) {
                    if (shot_tex)
                        SDL_ReleaseGPUTexture(gpu, shot_tex);
                    SDL_GPUTextureCreateInfo si = ti;
                    si.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    si.width = shot_w = w;
                    si.height = shot_h = h;
                    shot_tex = SDL_CreateGPUTexture(gpu, &si);
                }
                if (shot_tex)
                    compose(cmd, shot_tex, w, h, src_tex, src_w, src_h, have_picture, grab ? 0 : ov);
            }
        }
        win_at(WIN_SUBMIT);
        SDL_SubmitGPUCommandBuffer(cmd);
        win_at(WIN_AFTER);
        if (grab && sc) {
            if (ovl.grab)
                read_back(gpu, ovl.grab, ovl.grab_w, ovl.grab_h, ovl_backdrop, NULL);
            ovl.taken = 1;          /* without a backdrop when it failed: the menu shows anyway */
            ov_dirty = 1;
        }
        if (window_shot) {
            if (sc && shot_tex)
                shot_save(gpu, shot_tex, w, h, window_shot);
            else
                logf_("--keys shot=%s: no window to take", window_shot);
        }
        /* a shot: two turns of this loop after the machine stopped for it, the newest picture is shown */
        static int shot_turns;
        int sp = SDL_GetAtomicInt(&shot_pause);
        if (sp && ++shot_turns >= 3 && have_picture) {
            shot_save(gpu, src_tex, src_w, src_h, shots[sp - 1].path);
            shots[sp - 1].done = 1;
            shot_turns = 0;
            SDL_SetAtomicInt(&shot_pause, 0);
        }
        compositor_busy(0);
        if (!sc)
            SDL_Delay(10);      /* minimized: nothing to present to */
        /* a two-PC game that did not start (4) keeps the window, its note saying why, until it is
         * closed; a test run (--frames) ends */
        if (st >= 2 && (st != 4 || frames_max))
            running = 0;
    }
    compositor_busy(0);                     /* the loop left in a frame (its acquire failed) */
    win_at(WIN_STOP);

    machine_stop_ask();
    SDL_WaitThread(machine, NULL);
    if (in_session)
        session_end();                      /* the other PC is told */
    double secs = (SDL_GetTicksNS() - t0) / 1e9;
    if (sync_log)
        fclose(sync_log);
    logf_("%u pictures shown, %u presents in %.2f s (%.1f presents a second)", pictures, presents, secs,
          secs > 0 ? presents / secs : 0.0);
    int failed = SDL_GetAtomicInt(&machine_state) == 2 ? 1 : SDL_GetAtomicInt(&machine_state) == 4 ? 4 : 0;
    if (failed == 4)
        logf_("the two-PC game did not start: see the session's lines above");
    else if (failed)
        logf_("the machine did not start: see the messages above (or, on Windows, this log)");

    if (audio) {
        logf_("audio: %u frames dropped (boot, unpaced or a stall); last queue %.0f ms", audio_dropped,
              audio_level / 44.1);
        SDL_DestroyAudioStream(audio);
    }
    {
        unsigned gf, gt, gv;
        gpu_render_stats(&gf, &gt, &gv);
        if (use_gpu || gf)
            logf_("GPU renderer: %u frames drawn, at most %u triangles; %u frames had translucent volumes (not drawn)",
                  gf, gt, gv);
        unsigned gp, gs;
        gpu_render_pass_stats(&gp, &gs);
        if (gp)
            logf_("GPU renderer: %u passes into textures drawn (a stage's reflection); %u frames used one not drawn "
                  "for them", gp, gs);
        unsigned rf;
        double rms, rfirst;
        gpu_render_rt_stats(&rf, &rms, &rfirst);
        if (rf)
            logf_("ray tracing: %u frames with traced light, %.2f ms each to trace and bring back (the first after it "
                  "started: %.0f ms)", rf, rms, rfirst);
    }
    /* the compositor has gone: the window is not closed (gone_close; section 54) */
    if (display_gone()) {
        gone_close(failed);
        _Exit(failed);
    }
    compositor_busy(1);     /* closing the window: it may never finish if the compositor goes now */
    win_at(WIN_CLOSE);
    ovl_free(gpu);
    for (int w = 0; w < 2; w++)
        if (vpad[w]) {
            SDL_JoystickID id = SDL_GetJoystickID(vpad[w]);
            SDL_CloseJoystick(vpad[w]);
            SDL_DetachVirtualJoystick(id);
        }
    video_end();
    close_begin();
    SDL_DestroyWindow(win);
    close_step("closing the window", NULL);
    compositor_busy(0);
    compositor_watch_end();
    SDL_SetAtomicInt(&win_watch_on, 0);
    if (win_watch_wake)
        SDL_SignalSemaphore(win_watch_wake);
    if (win_watcher)
        SDL_WaitThread(win_watcher, NULL);
    if (win_watch_wake)
        SDL_DestroySemaphore(win_watch_wake);
    win_watch_wake = NULL;
    SDL_DestroyMutex(shared.lock);
    gu_quit();      /* the records are saved: an update asked for in the menu goes to the launcher */
    if (to_launcher) {
        gu_open_launcher();
        if (!failed && gu_by_launcher())
            failed = GU_EXIT_LAUNCHER;      /* the launcher that started the game shows itself */
    }
    logf_("clean exit");
    SDL_Quit();
    if (log_file && log_file != stderr)
        fclose(log_file);
    return failed;
}
