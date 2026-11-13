/* The machine side of the game window (milestone 3, docs/design.md section
 * 14): the translated game on runtime/machine, set up as vf4ft_headless does
 * it and run with no script. Nothing here knows about SDL; the window side
 * (main.c) gives a frame function that runs on the machine's thread once per
 * vertical blank. */
#ifndef VF4FT_GAME_MACHINE_H
#define VF4FT_GAME_MACHINE_H

#define GAME_PICTURE_W 640
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
 * blank's number; rgb the newest picture (GAME_PICTURE_W x _H, 3 bytes a
 * pixel) or NULL before the first, valid only during the call; in is to be
 * filled with the controls as they are now; booting is set until the game
 * first shows its warning screen (WARNING.DISP, vertical blank 5,832: the
 * start-up before it has nothing to see). The function also keeps the pace (it returns
 * when the frame is due). */
typedef void (*game_frame_fn)(unsigned frame, const unsigned char *rgb, int booting, game_input *in);

/* Everything up to the first instruction: memory, the program image, the
 * DIMM image, devices, the BIOS stand-in, the translated code's tables.
 * dimm_key NULL: dimm is the decrypted image (development) and image the
 * program (main.bin). dimm_key set: dimm is the disc's encrypted file, which
 * is decrypted with it (dimm_image.c), and image may be NULL to take the
 * program from it. sound_chip: the sound chip with the game's own driver
 * (1), or VF4debug's silent mailbox model (0). 0 on success; messages go to
 * stderr. Call on the thread that will run. */
int game_machine_init(const char *image, const char *dimm, const char *dimm_key, int sound_chip, game_frame_fn fn);

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

#endif
