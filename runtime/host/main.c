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
 *   --screen-hz HZ    for tests: the screen's refresh rate is HZ, not the one SDL reports
 *
 * Settings (M6): user/settings.txt (settings.h), written by the launcher's
 * Settings page: free play or coins, fullscreen, picture, sound, the card
 * units and every control. The defaults:
 *   keyboard (player 1): arrows or WASD, J guard, K punch, L kick, U throw
 *     (P+G), I P+K, O P+K+G, P K+G, Enter start, 5 coin, F2 test, F3 service
 *   gamepads (first is player 1, second player 2), the PlayStation 2
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
#include "game_machine.h"
#include "settings.h"
#include "gpu_render.h"
#include "menu.h"
#include "options.h"
#include "ui.h"
#include "updates.h"

#define VF4FT_TITLE "Virtua Fighter 4 Final Tuned"
#ifndef VF4FT_VERSION
#define VF4FT_VERSION "development build"   /* the builder (tools/make_game.py) gives its version */
#endif
#define PIC_W GAME_PICTURE_W
#define PIC_W_MAX GAME_PICTURE_W_MAX
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
    unsigned char pic[PIC_W_MAX * PIC_H * 3];   /* the newest picture, RGB, pic_w x PIC_H */
    int pic_w;
    unsigned pic_frame;                         /* its vertical blank; 0 = none yet */
    game_input in;                              /* the controls, as this thread last read them */
} shared;

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
static SDL_AtomicInt test_frames;       /* vertical blanks the menu holds the Test switch for */
static SDL_AtomicInt restart_req;       /* GAME_COINS_*: the game starts again from power on, with it (and
                                         * start_cfg's network) */
static int net_first;                   /* a blank backup with the network wanted off: it goes in once the
                                         * game has made its settings record (on_frame) */
static SDL_AtomicInt frame_now;         /* the machine's vertical blank */
static SDL_AtomicInt program_now;       /* 1 while the test program runs */

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
    if (!start_cfg.cards)
        return;             /* no units this run: the choice is for the next start */
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
    /* the game's NETWORK setting (docs/design.md section 35) */
    int net = game_machine_network(cfg_.network ? GAME_NET_ON : GAME_NET_OFF);
    if (net >= 0) {
        logf_("network: %s (the game's own NETWORK setting)", net ? "on" : "off");
    } else if (cfg_.network) {
        logf_("network: on (a blank backup: the game's default as it makes its settings record)");
    } else {
        net_first = 1;
        logf_("network: off once the game has made its settings record (a blank backup: it makes it as it "
              "starts, with the network on; then it starts again)");
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
static SDL_AtomicInt machine_state;     /* 0 starting, 1 running, 2 init failed, 3 stopped */
static Uint32 machine_event;            /* an SDL event type: the machine has stopped */

/* pacing and how busy the machine is, logged every 10 seconds of frames */
static Uint64 due, busy_since, busy_ns, busy_max_ns;
static unsigned busy_frames, resyncs;

/* machine thread, once per vertical blank (game_frame_fn) */
static void on_frame(unsigned frame, const unsigned char *rgb, int w, int booting, game_input *in)
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
    if (rgb && w >= PIC_W && w <= PIC_W_MAX) {
        memcpy(shared.pic, rgb, (size_t)w * PIC_H * 3);
        shared.pic_w = w;
        shared.pic_frame = frame;
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

    /* the menu: the machine waits while it is open; a restart from it
     * leaves this function for the power-on state (rt_native_restart) */
    if (SDL_GetAtomicInt(&menu_pause)) {
        cards_save();       /* the units' cards in their files, for the menu's Cards tab */
        while (SDL_GetAtomicInt(&menu_pause))
            SDL_Delay(5);
        due = 0;
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
    game_machine_region(cfg_.region);
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
        cards_edits(1);     /* edits still waiting (a game held the card): onto its last state */
        SDL_SetAtomicInt(&machine_state, 3);
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = machine_event;
    SDL_PushEvent(&ev);
    return 0;
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
        SDL_GPUTexture *t = gpu_render_frame(cmd, &w, &h, &gf);
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
static pad_set pads[ACT_COUNT];
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

/* inputs kept from the game: gamepad buttons (any pad) and keys, while the
 * menu's chord or operator bar has them, or until they are let go after the
 * menu closed (the button that closed it is not the game's) */
static unsigned pad_ignore;
static int pad_ignore_bar;              /* and each pad's operator bar buttons (they go by label: ui_pad_face) */
static bool key_ignore[SDL_SCANCODE_COUNT];
static unsigned pads_raw;               /* the buttons of every gamepad, together */
static unsigned pad_raw[2];             /* the first two gamepads' buttons */
static SDL_JoystickID pad_id[2];
#define CHORD ((1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START))     /* View + Menu: the menu */

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
    int n = 0;
    pads_raw = pad_raw[0] = pad_raw[1] = 0;
    SDL_JoystickID *ids = ui_gamepads(&n);     /* without Steam Input's twins (ui.h) */
    for (int i = 0; ids && i < n && i < 2; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
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
            for (int j = 0; j < pads[a].n; j++) {
                int x = pads[a].in[j];
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
        pad_id[i] = ids[i];
        if ((raw & CHORD) == CHORD)
            first |= CHORD;
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

static int fullscreen_wanted(void)
{
    const char *tenfoot = SDL_getenv("SteamTenfoot");
    return cfg_.fullscreen == FULLSCREEN_ON ||
           (cfg_.fullscreen == FULLSCREEN_AUTO &&
            (settings_steam_deck() != STEAM_DECK_NONE || (tenfoot && atoi(tenfoot))));
}

/* the room for the window's picture on w's screen (the primary one before
 * there is a window) in pixels: the usable area less the window's frame
 * when SDL knows it (an X11 window manager keeps a title bar on the screen,
 * so 1080 lines on a 1080 screen were 1052), in w's coordinates times its
 * pixel density (2 on a Wayland screen at 200%; 1 on Windows, whose
 * coordinates are pixels) */
static window_room screen_room(SDL_Window *w)
{
    SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay();
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
 * middle of its screen when it was there already (move 1). Fullscreen
 * keeps the screen: the size waits for it to end. A maximized window is
 * restored to it */
static void window_to_setting(SDL_Window *w, int move)
{
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
    if (fl & SDL_WINDOW_MAXIMIZED) {
        SDL_RestoreWindow(w);
        SDL_SyncWindow(w);
    }
    int pw, ph, cw, ch, ow = 0, oh = 0;
    window_room room = screen_room(w);
    settings_window_size(&cfg_, &room, &pw, &ph);
    cw = (int)(pw / room.density + 0.5f);
    ch = (int)(ph / room.density + 0.5f);
    SDL_GetWindowSize(w, &ow, &oh);
    if (ow == cw && oh == ch)
        return;
    SDL_SetWindowSize(w, cw, ch);
    if (move) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(w);
        SDL_SetWindowPosition(w, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d), (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d));
    }
    logf_("window: %d x %d pixels (Window size %s, room on the screen %d x %d, pixel density %.2f)", pw, ph,
          cfg_.window == WINDOW_AUTO ? "Auto" : "set", room.w, room.h, room.density);
}

static float screen_hz_test;        /* --screen-hz */

/* the refresh rate of w's screen in Hz (0: not known) */
static float screen_refresh(SDL_Window *w)
{
    if (screen_hz_test > 0)
        return screen_hz_test;
    SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : 0;
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    return m && m->refresh_rate > 0 ? m->refresh_rate : 0;
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
    menu_run r;
    r.start = start_cfg;
    r.sound = audio != NULL;
    r.no_device = !silent && !audio;    /* silent: sound off in the settings, or --silent */
    r.test_program = SDL_GetAtomicInt(&program_now);
    r.card_peek = card_peek;
    r.card_edit = card_edit_save;
    r.refresh = screen_refresh(main_win);
    r.deck = settings_steam_deck();
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
 * screen's rate two seconds into the picture (or the Ko-fi notice's
 * closing), and shows the notice when it is uneven */
static void refresh_turn(int have_picture, Uint64 now_ms)
{
    if (!refresh_due || !have_picture || menu_on || kofi_due)
        return;
    if (refresh_frame) {
        if ((unsigned)SDL_GetAtomicInt(&frame_now) < refresh_frame)
            return;
    } else {
        if (!refresh_after)
            refresh_after = now_ms + 2000;
        if (now_ms < refresh_after)
            return;
    }
    refresh_due = 0;
    float hz = screen_refresh(main_win);
    int deck = settings_steam_deck();
    int uneven = settings_refresh_uneven(hz);
    char key[48];
    refresh_key(hz, key, sizeof key);
    logf_("screen: %.2f Hz%s", (double)hz, hz <= 0 ? " (not known)" : uneven ? " (not a multiple of 60: uneven)" : "");
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
    if (audio)
        SDL_ResumeAudioStreamDevice(audio);
}

/* what the menu asked for; 0 to quit */
static int menu_do(int a)
{
    switch (a) {
    case MENU_LIVE:
        game_machine_wide(settings_picture_width(cfg_.picture));
        if (cfg_.window != live_window ||
            settings_window_width(cfg_.picture, PIC_H) != settings_window_width(live_picture, PIC_H))
            window_to_setting(main_win, 1);     /* the size, or the new picture's shape */
        live_picture = cfg_.picture;
        if (cfg_.fullscreen != live_fullscreen) {
            live_fullscreen = cfg_.fullscreen;
            SDL_SetWindowFullscreen(main_win, fullscreen_wanted());
        }
        if (audio)
            SDL_SetAudioStreamGain(audio, cfg_.volume / 100.0f);
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


static void ovl_free(SDL_GPUDevice *gpu)
{
    if (ovl.ren) {
        ui_quit();
        if (ovl.backdrop)
            SDL_DestroyTexture(ovl.backdrop);
        SDL_DestroyRenderer(ovl.ren);
    }
    SDL_DestroySurface(ovl.surf);
    if (ovl.tex)
        SDL_ReleaseGPUTexture(gpu, ovl.tex);
    if (ovl.up)
        SDL_ReleaseGPUTransferBuffer(gpu, ovl.up);
    if (ovl.grab)
        SDL_ReleaseGPUTexture(gpu, ovl.grab);
    SDL_zero(ovl);
}

/* made again at the window's size, w x h pixels; 0 or -1 */
static int ovl_size(SDL_GPUDevice *gpu, int w, int h)
{
    ovl_free(gpu);
    ovl.surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
    ovl.ren = ovl.surf ? SDL_CreateSoftwareRenderer(ovl.surf) : NULL;
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ovl.tex = SDL_CreateGPUTexture(gpu, &ti);
    SDL_GPUTransferBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = (Uint32)w * (Uint32)h * 4;
    ovl.up = SDL_CreateGPUTransferBuffer(gpu, &bi);
    if (!ovl.ren || !ovl.tex || !ovl.up || ui_init(ovl.ren)) {
        logf_("the menu's drawing at %dx%d: %s", w, h, SDL_GetError());
        ovl_free(gpu);
        return -1;
    }
    ovl.w = w;
    ovl.h = h;
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

/* --keys pad:...: SDL's virtual gamepad, so the gamepad paths run as a real pad's do */
static SDL_Joystick *vpad;
static int vpad_turns;                  /* turns left of the buttons held */

/* made at start-up, before its first press: SDL sends no press for a button
 * already down when a gamepad is opened */
static void vpad_attach(void)
{
    if (!vpad) {
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        d.name = "vf4ft --keys pad";
        /* "padtype=NAME": a real pad's vendor and product ids, so that SDL
         * takes it for one and its glyphs show */
        static const struct { const char *name; Uint16 vendor, product; } types[] = {
            { "xbox", 0x045E, 0x0B12 },         /* Xbox Series X|S */
            { "xbox360", 0x045E, 0x028E },
            { "ps5", 0x054C, 0x0CE6 },          /* DualSense */
            { "ps4", 0x054C, 0x09CC },          /* DualShock 4 */
            { "ps3", 0x054C, 0x0268 },          /* DualShock 3 */
            { "switch", 0x057E, 0x2009 },       /* Switch Pro */
            { "gamecube", 0x057E, 0x2073 },     /* Switch 2 GameCube controller */
            { "deck", 0x28DE, 0x1205 },         /* Steam Deck */
            { "steamvirtual", 0x28DE, 0x11FF }, /* Steam Input's own pad */
        };
        for (int i = 0; i < key_items; i++)
            for (size_t k = 0; k < SDL_arraysize(types); k++)
                if (!strncmp(key_item[i], "padtype=", 8) && !strcmp(key_item[i] + 8, types[k].name))
                    d.vendor_id = types[k].vendor, d.product_id = types[k].product;
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
        vpad = id ? SDL_OpenJoystick(id) : NULL;
        if (!vpad)
            logf_("--keys pad: no virtual gamepad: %s", SDL_GetError());
    }
}

static void vpad_press(const char *what)
{
    if (!vpad)
        return;
    char buf[64];
    SDL_strlcpy(buf, what, sizeof buf);
    char *eq = SDL_strchr(buf, '=');
    vpad_turns = eq ? atoi(eq + 1) : 6;
    if (eq)
        *eq = 0;
    char *save = NULL;
    for (char *b = SDL_strtok_r(buf, "+", &save); b; b = SDL_strtok_r(NULL, "+", &save)) {
        SDL_GamepadButton bt = SDL_GetGamepadButtonFromString(b);
        if (bt == SDL_GAMEPAD_BUTTON_INVALID)
            logf_("--keys pad: no gamepad button is called \"%s\"", b);
        else
            SDL_SetJoystickVirtualButton(vpad, bt, true);
    }
}

static void vpad_turn(void)
{
    if (vpad && vpad_turns > 0 && --vpad_turns == 0)
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            SDL_SetJoystickVirtualButton(vpad, b, false);
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
        else if (!strcmp(a, "--kofi-notice") && v) kofi_frame = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--refresh-notice") && v) refresh_frame = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--deck-product") && v) settings_steam_deck_test(argv[++i]);
        else if (!strcmp(a, "--screen-hz") && v) screen_hz_test = (float)SDL_atof(argv[++i]);
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
    kofi_setup(frames_max || nshots || presses || key_items || unpaced);
    refresh_setup(frames_max || nshots || presses || key_items || unpaced);
    {
        char *sp = settings_path;
        SDL_snprintf(sp, sizeof settings_path, "%ssettings.txt", user_dir);
        settings_defaults_here(&cfg_);      /* a Steam Deck's 16:10 picture (section 44) */
        if (!SDL_GetPathInfo(sp, NULL))
            logf_("no %s: the defaults", sp);
        else if (!settings_load(&cfg_, sp, say_line))
            logf_("settings from %s", sp);
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
          "network %s, region %s",
          cfg_.renderer == RENDERER_GPU ? "gpu" : "software", cfg_.scale,
          cfg_.coins == COINS_ARCADE ? "arcade" : "free play", fullscreen ? "on" : "off",
          settings_picture_name(cfg_.picture), cfg_.filter == FILTER_SHARP ? "sharp" : "smooth",
          silent ? "off" : "on", cfg_.volume, cfg_.cards ? "on" : "off", cfg_.network ? "on" : "off",
          settings_region_text[cfg_.region]);
    logf_("image %s; DIMM %s%s%s", image_path ? image_path : "(from the DIMM image)", dimm_path,
          dimm_key ? ", key " : "", dimm_key ? dimm_key : "");

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (silent ? 0 : SDL_INIT_AUDIO))) {
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    logf_("video driver: %s", SDL_GetCurrentVideoDriver());
    ui_pad_logger(log_line);
    bindings_compile();     /* key names need the video subsystem */
    for (int i = 0; i < key_items; i++)
        if (!strncmp(key_item[i], "pad:", 4) || !strncmp(key_item[i], "padtype=", 8)) {
            vpad_attach();
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
        window_room room = screen_room(NULL);
        game_machine_wide(settings_picture_width(cfg_.picture));
        settings_window_size(&cfg_, &room, &ww, &wh);
        win = SDL_CreateWindow(VF4FT_TITLE, ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                               (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!win) { logf_("SDL_CreateWindow failed: %s", SDL_GetError()); return 1; }
        main_win = win;
        SDL_SetWindowMinimumSize(win, MENU_MIN_W, MENU_MIN_H);  /* the menu always fits */
        logf_("window: %d x %d pixels (Window size %s, primary screen %d x %d)%s", ww, wh,
              cfg_.window == WINDOW_AUTO ? "Auto" : "set", room.w, room.h, fullscreen ? "; fullscreen" : "");
        SDL_SyncWindow(win);        /* its frame, for the room left (X11) */
        window_to_setting(win, 1);
    }
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
    if (win && !SDL_ClaimWindowForGPUDevice(gpu, win)) { logf_("claim window failed: %s", SDL_GetError()); return 1; }
    if (use_gpu && gpu_render_init(gpu, cfg_.scale)) {
        logf_("the GPU renderer cannot start (%s): the software picture instead", SDL_GetError());
        gpu_render_quit();
        use_gpu = 0;
    }
    int shown_w = art_dir ? PIC_W : settings_picture_width(cfg_.picture);
    if (use_gpu) {
        game_machine_gpu(gpu_render_submit);
        logf_("renderer: GPU, %d x %d", shown_w * cfg_.scale, PIC_H * cfg_.scale);
    } else {
        logf_("renderer: software, %d x %d", shown_w, PIC_H);
    }
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


    /* the picture: uploaded into a texture whenever a new one is there, then
     * blitted to the window, scaled */
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = PIC_W_MAX;               /* room for a wide screen */
    ti.height = PIC_H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *pic_tex = SDL_CreateGPUTexture(gpu, &ti);
    SDL_GPUTransferBufferCreateInfo bi;
    SDL_zero(bi);
    bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bi.size = PIC_W_MAX * PIC_H * 4;
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &bi);
    if (!pic_tex || !upload) { logf_("picture texture: %s", SDL_GetError()); return 1; }
    static unsigned char rgb[PIC_W_MAX * PIC_H * 3];
    int rgb_w = PIC_W;                  /* the software picture's width */

    SDL_GPUTexture *shot_tex = NULL;    /* --keys shot=: the window as shown */
    Uint32 shot_w = 0, shot_h = 0;

    Uint64 t0 = SDL_GetTicksNS(), chord_since = 0, dir_next = 0;
    unsigned shown_frame = 0, pictures = 0, presents = 0;
    int have_picture = 0, running = 1, stopping = 0, reported_start = 0;
    int chord_used = 0, last_dir = -1, last_program = 0, key_turns = 0;
    while (running) {
        const char *window_shot = NULL;
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
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = 0;
                break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                if (window_later)
                    window_to_setting(win, 1);  /* Window size, changed while it was fullscreen */
                break;
            case SDL_EVENT_KEY_DOWN: {
                SDL_Keycode key = ev.key.key;
                int cmd = -1;
                if (!ev.key.repeat && (key == SDLK_F11 || (key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
                    SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
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
        /* in the menu: the D-pad or left stick of any pad moves, held it repeats */
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
            if (dir != last_dir) {
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
        if (key_at < key_items && !vpad_turns && SDL_GetAtomicInt(&frame_now) >= key_wait && ++key_turns >= 8) {
            const char *it = key_item[key_at++];
            key_turns = 0;
            if (it[0] == '@')
                key_wait = atoi(it + 1);
            else if (!strncmp(it, "shot=", 5))
                window_shot = it + 5;
            else if (!strncmp(it, "pad:", 4))
                vpad_press(it + 4);
            else if (!strncmp(it, "padtype=", 8))
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

        unsigned frame;
        SDL_LockMutex(shared.lock);
        shared.in = in;
        frame = shared.pic_frame;
        if (frame != shown_frame) {
            rgb_w = shared.pic_w;
            memcpy(rgb, shared.pic, (size_t)rgb_w * PIC_H * 3);
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
        /* the menu (1) or the operator bar (2), drawn again when it changed;
         * the menu's first turn shows the picture alone and takes it (grab) */
        int ov = menu_on ? 1 : bar_on && now_ms < bar_until ? 2 : 0, grab = 0;
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
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &sc, &w, &h)) {
            logf_("acquire swapchain failed: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        SDL_GPUTexture *src_tex = pic_tex;
        Uint32 src_w = (Uint32)rgb_w, src_h = PIC_H;
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
        SDL_SubmitGPUCommandBuffer(cmd);
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
        if (!sc)
            SDL_Delay(10);      /* minimized: nothing to present to */
        if (st >= 2)
            running = 0;
    }

    SDL_SetAtomicInt(&shot_pause, 0);
    SDL_SetAtomicInt(&menu_pause, 0);       /* a machine waiting for the menu goes on to its stop */
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
    ovl_free(gpu);
    if (shot_tex)
        SDL_ReleaseGPUTexture(gpu, shot_tex);
    if (vpad) {
        SDL_JoystickID id = SDL_GetJoystickID(vpad);
        SDL_CloseJoystick(vpad);
        SDL_DetachVirtualJoystick(id);
    }
    SDL_ReleaseWindowFromGPUDevice(gpu, win);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(win);
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
