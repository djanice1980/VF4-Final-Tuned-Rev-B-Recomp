/* The machine side of the game window (milestone 3, docs/design.md section
 * 14): the translated game on runtime/machine, set up as vf4ft_headless does
 * it and run with no script. Nothing here knows about SDL; the window side
 * (main.c) gives a frame function that runs on the machine's thread once per
 * vertical blank. */
#ifndef VF4FT_GAME_MACHINE_H
#define VF4FT_GAME_MACHINE_H

#include <stddef.h>

#define GAME_PICTURE_W 640
#define GAME_PICTURE_W_MAX 1280     /* the widest wide screen (game_machine_wide) */
#define GAME_PICTURE_H 480

/* what the cabinet's controls are doing, in runtime/machine/control.h's bits */
typedef struct game_input {
    unsigned player[2];         /* BTN_* */
    unsigned sys;               /* SYS_TEST, SYS_SERVICE */
    unsigned coin[2];           /* the coin switch of each slot is closed */
} game_input;

#define GAME_BTN_UP 0x1
#define GAME_BTN_DOWN 0x2
#define GAME_BTN_LEFT 0x4
#define GAME_BTN_RIGHT 0x8
#define GAME_BTN_P 0x10
#define GAME_BTN_K 0x20
#define GAME_BTN_G 0x40
#define GAME_BTN_E 0x80
#define GAME_BTN_START 0x100
#define GAME_SYS_TEST 0x1
#define GAME_SYS_SERVICE 0x2

/* Once per vertical blank, on the machine's thread: frame is the vertical
 * blank's number; rgb the newest picture (w x GAME_PICTURE_H, 3 bytes a
 * pixel; w is GAME_PICTURE_W, or wider with game_machine_wide) or NULL
 * before the first, valid only during the call; in is to be
 * filled with the controls as they are now; booting is set until the game
 * first shows its warning screen (WARNING.DISP, vertical blank 5,832: the
 * start-up before it has nothing to see). The function also keeps the pace (it returns
 * when the frame is due). */
typedef void (*game_frame_fn)(unsigned frame, const unsigned char *rgb, int w, int booting, game_input *in);

/* The wide screen (docs/design.md section 39): the game's screen is drawn
 * `width` pixels wide (even, GAME_PICTURE_W to GAME_PICTURE_W_MAX), its own
 * 640 in the middle and what the camera sees past its sides around it;
 * GAME_PICTURE_W is the cabinet's 4:3. Any thread, at any time: the next
 * picture the game draws takes it. */
void game_machine_wide(int width);
/* The GPU renderer's lights by the GPU's vertex shader (1) or by the machine's
 * thread as ever (0): docs/design.md section 67. Any thread, at any time. */
void game_machine_lighting(int gpu);

/* The GPU renderer (runtime/host/gpu_render.c): set before game_machine_init,
 * a pass to the screen goes to fn (view.h, view_gpu_frame) instead of the
 * software picture, which then stays as it was. After game_machine_init it
 * changes the renderer as the game runs (docs/design.md section 53), and
 * only on the machine's thread, between passes (from the game_frame_fn):
 * NULL, the software picture from the next pass on */
struct view_gpu_frame;
typedef void (*game_gpu_fn)(const struct view_gpu_frame *f);
void game_machine_gpu(game_gpu_fn fn);

/* Everything up to the first instruction: memory, the program image, the
 * DIMM image, devices, the BIOS stand-in, the translated code's tables.
 * dimm_key NULL: dimm is the decrypted image (development) and image the
 * program (main.bin). dimm_key set: dimm is the disc's encrypted file, which
 * is decrypted with it (dimm_image.c), and image may be NULL to take the
 * program from it. sound_chip: the sound chip with the game's own driver
 * (1), or VF4debug's silent mailbox model (0). 0 on success; messages go to
 * stderr. Call on the thread that will run. */
int game_machine_init(const char *image, const char *dimm, const char *dimm_key, int sound_chip, game_frame_fn fn);

/* The cabinet's memory that outlives a power-off: the battery-backed RAM
 * (64 KiB at 0x00200000: records, rankings, the operator's bookkeeping)
 * and the settings EEPROM (128 bytes: the board and coin settings). The
 * file is the two one after the other, 65,664 bytes, the format of the
 * harness's "backup save" (runtime/machine/control.c). */
#define GAME_BACKUP_SIZE (0x10000 + 0x80)

/* how credits work, set the way an operator sets it: the coin setting in
 * the EEPROM's two system copies (the BIOS stand-in reads setting 26 as
 * free play, runtime/machine/hle_boot.c coin_config_fix) */
enum { GAME_COINS_KEEP, GAME_COINS_FREE_PLAY, GAME_COINS_ARCADE };

/* after game_machine_init, before game_machine_run: the backup as a file
 * held it (b NULL: the machine's blank one), then the coin choice. Returns
 * what the coin setting became ("free play", "arcade coins") or NULL for
 * GAME_COINS_KEEP */
const char *game_machine_backup_put(const unsigned char *b, int coins);

/* The game's own NETWORK setting (its test menu, Game Assignments), in its
 * settings record in the battery-backed RAM (docs/design.md section 35):
 * off, the game skips its network check as it starts. */
enum { GAME_NET_KEEP, GAME_NET_OFF, GAME_NET_ON };

/* after game_machine_backup_put (and before game_machine_run), or from the
 * frame function: the setting put in the record. Returns what it is now (0
 * off, 1 on), or -1 when the backup has no settings record yet: the game
 * makes one with its defaults (the network on) as it starts, and the
 * setting can go in once it has (game_machine_net_ready) */
int game_machine_network(int net);

/* from the frame function: 1 when the battery-backed RAM holds the game's
 * settings record */
int game_machine_net_ready(void);

/* on the machine's thread (the frame function, or after game_machine_run):
 * the backup as it is now into b (GAME_BACKUP_SIZE bytes); 1 when it
 * differs from what was last put or got, else 0 */
int game_machine_backup_get(unsigned char *b);

/* The two card reader/writer units (VF.NET cards: ring name, rank, record,
 * items). game_machine_cards before game_machine_init fits them (the game
 * looks for them once, early in its boot). A card is 128 bytes. */
#define GAME_CARD_SIZE 128
void game_machine_cards(int on);

/* The board's region (docs/design.md section 41): the country the BIOS
 * stand-in's board settings give the game (0 Japan, 1 USA, 2 export, 3
 * Korea, 4 Australia). The game asks for them as it starts (board_start),
 * so: before game_machine_init, or on the machine's thread before
 * game_machine_restart. */
void game_machine_region(int country);

/* after init: a card goes into player side's unit (0 or 1) and stays; card
 * NULL is a card never used. 0, or -1 (no units, or a card in already) */
int game_machine_card_put(int side, const unsigned char *card);

/* on the machine's thread: the card in the unit as it is now (or, once it
 * has left, as it left: the game's last write to a card taken while it was
 * using it); 1 when it differs from what was last put or got, 0 when not,
 * -1 when no card was ever in */
int game_machine_card_get(int side, unsigned char *card);

/* on the machine's thread: the card in the unit as game_machine_card_get
 * would give it, without marking it got (the two-PC game's fingerprint).
 * 0, or -1 when no card was ever in */
int game_machine_card_peek(int side, unsigned char *card);

/* on the machine's thread: 1 while a game is using the card in the unit
 * (from its read at the start of a game until the lock opens at the end;
 * hw_card_held) */
int game_machine_card_held(int side);

/* on the machine's thread: the player takes the card out. 0: the unit is
 * empty; 1: the game is using the card (its lock is on), and it leaves when
 * the game lets go of it: ask again later */
int game_machine_card_take(int side);

/* from the frame function: the sound the chip made since the last call, up
 * to max stereo frames (44,100 a second); with sound_chip 0 there is none */
unsigned game_machine_audio(short *dst, unsigned max);

/* Runs until game_machine_stop (or a fatal stop); returns 0 for a stop that
 * was asked for, else 1, with the reason in game_machine_why. */
int game_machine_run(void);

/* from any thread: the run ends at the next vertical blank */
void game_machine_stop(void);

/* why the run stopped, and where the game was (its mode), for the log */
const char *game_machine_why(void);

/* 1 while the game's test program runs (test mode: the Test switch starts
 * it, its EXIT starts the game again), else 0; from the frame function */
int game_machine_program(void);

/* a fingerprint of the machine: main RAM and the processor's registers.
 * Two runs that are the same run have the same one at each vertical blank
 * (docs/design.md section 45); from the frame function */
unsigned long long game_machine_hash(void);

/* from the frame function only: the game starts again from power on, as the
 * test program's EXIT starts it, keeping what the cabinet keeps; coins
 * (GAME_COINS_*) is put in the settings EEPROM and net (GAME_NET_*) in the
 * game's settings record first, which the game reads as it starts. Does not
 * return (the frame function is left, as the run goes on from the power-on
 * state) */
void game_machine_restart(int coins, int net);

/* Snapshots of the whole machine (docs/design.md section 48): another
 * process of this build takes the machine as it is and goes on from there,
 * the same run. From the frame function: the machine is saved at the end
 * of this vertical blank, once its inputs have gone in. */
void game_machine_snapshot_ask(void);

/* on the machine's thread, after the vertical blank asked for: 1 with the
 * snapshot in *data (*size bytes; the caller frees it with free) and its
 * vertical blank in *frame; 0 when none is waiting; -1 when it could not be
 * taken (the test program was running, or memory ran out) */
int game_machine_snapshot_get(unsigned char **data, size_t *size, unsigned *frame);

/* the machine from a snapshot of this build, and the run goes on from it,
 * its next frame function at the vertical blank after the snapshot's.
 * After game_machine_init and whatever goes in after it (backup, cards),
 * before game_machine_run: 0. From the frame function while the game (not
 * its test program) runs: does not return (the frame function is left, as
 * game_machine_restart leaves it), so call it holding nothing. Else -1 with
 * the reason in game_machine_why (the machine is as it was) */
int game_machine_resume(const unsigned char *data, size_t size);

/* Rollback, for tests (docs/design.md sections 55 and 56): before
 * game_machine_run. Once the game has booted the machine keeps its last
 * depth frames (1 to 60). synctest N (at most depth): every period vertical
 * blanks it runs N frames, goes back N and runs them again with the same
 * controls, without the frame function, sound or screens, and says on
 * stderr ("synctest:") whether going back gave the machine as it was, the
 * second run the machine the first one made, and its sound was all left
 * out. 0 for either: off */
void game_machine_rollback(int depth, int synctest, int period);

/* Rollback in the two-PC game (docs/design.md section 57), not with the
 * tests above. All from the frame function but the first.
 *
 * Before game_machine_run: the controls of a vertical blank run again come
 * from fn (on the machine's thread, in place of the frame function) */
typedef void (*game_replay_fn)(unsigned frame, game_input *in);
void game_machine_replay(game_replay_fn fn);

/* keep the machine's last depth vertical blanks from the end of this one on
 * (0: none kept, from now). Only once the game has booted, and not in its
 * test program */
void game_machine_keep(int depth);

/* the oldest vertical blank the machine can go back to the end of now, or
 * ~0u (none kept) */
unsigned game_machine_kept_from(void);

/* at the end of this vertical blank, once its controls have gone in: the
 * machine goes back to the end of vertical blank frame - 1 (it must be kept)
 * and runs frame .. this one again, their controls from the replay function,
 * no screen drawn and no sound kept (but the last one's lists: the next
 * screen shows them). Returns; the frame function is next called for the
 * vertical blank after this one */
void game_machine_go_back(unsigned frame);

/* 1 when a going back could not be done since the last call (the vertical
 * blank was not kept): the machine went on from where it was */
int game_machine_back_failed(void);

/* since the last call: times gone back, vertical blanks run again, and the
 * time that took (ms in all, and the longest) */
typedef struct game_back_stats {
    unsigned backs, frames;
    double ms, ms_most;
} game_back_stats;
void game_machine_back_stats(game_back_stats *s);

/* on the machine's thread: the backup as it is now into b, as
 * game_machine_backup_get does without marking it got; then
 * game_machine_backup_new(b) later, which marks it got: 1 when it differs
 * from what was last put or got */
void game_machine_backup_peek(unsigned char *b);
int game_machine_backup_new(const unsigned char *b);

#endif
