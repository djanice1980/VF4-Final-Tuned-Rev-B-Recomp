/* Two players on two PCs (docs/design.md sections 45 and 46): each PC runs
 * the whole machine, the same run, and the two send each other only their
 * player's controls for each vertical blank, a few vertical blanks ahead
 * (the input delay). One PC hosts, as player 1 or player 2 (its choice),
 * the other joins as the other player. When the session ends, the host's
 * game can go on alone (the caller's choice); the joining PC's stops.
 *
 * The start: the joining PC says which build it is and sends its player's
 * card; the host answers with the cabinet (its backup, coins, region,
 * network, card units) and its own player's card. Both machines start from
 * that. Nothing of the host's is written on the joining PC: its own backup
 * and settings stay as they were, and each PC keeps only its own player's
 * card.
 *
 * While it runs: inputs before the game's start-up has ended (the
 * fast-forwarded boot) are none on both PCs; from then on each vertical
 * blank's inputs are both players'. Every SESSION_CHECK vertical blanks
 * both PCs send a fingerprint of their machine (the caller's hash); a
 * difference ends the session at once (the two runs went apart).
 *
 * Over UDP (udp.h), one socket a PC. */
#ifndef VF4FT_SESSION_H
#define VF4FT_SESSION_H

#include <stddef.h>

#include "cards.h"
#include "game_machine.h"

enum { SESSION_OFF, SESSION_HOST, SESSION_GUEST };
enum { SESSION_CARD_NONE, SESSION_CARD_UNUSED, SESSION_CARD_BYTES };

#define SESSION_CHECK 60            /* vertical blanks between fingerprints */
#define SESSION_DELAY 2             /* the input delay on a LAN */

typedef struct session_player {
    int card;                       /* SESSION_CARD_*: none, a card never used, or its bytes */
    unsigned char bytes[GAME_CARD_SIZE];
    char name[CARD_NAME_MAX];       /* its card profile ("" none) */
} session_player;

/* what both machines start from */
typedef struct session_start {
    int coins, region, network, cards;  /* the host's settings (settings.h's COINS_*, REGION_*; 0/1) */
    int backup;                         /* 1: backup_bytes is the host's backup.bin; 0: a blank cabinet */
    unsigned char backup_bytes[GAME_BACKUP_SIZE];
    session_player own;                 /* mine: this PC's player's card (both roles) */
    session_player player[2];           /* out: player 1's and player 2's */
    int delay;                          /* vertical blanks from a press to its frame */
    int side;                           /* out: this PC's player, 0 (player 1) or 1 */
} session_start;

/* from the command line, before anything else: SESSION_HOST on port (0:
 * UDP_PORT), or SESSION_GUEST to addr ("host[:port]") */
void session_config(int role, int port, const char *addr);
int session_role(void);

/* the host's player: 0 (player 1, the default) or 1 (player 2); the joining
 * PC is the other */
void session_host_side(int side);
int session_host_side_get(void);

/* where the session's log lines go (any thread) */
void session_logger(void (*fn)(const char *line));

/* The connection, on the machine's thread before the machine starts. The
 * host gives the cabinet's fields and own; the joining PC only own. out is
 * the start both agreed on (player[] by player, side this PC's). hash is called at every
 * check vertical blank (from session_frame) for the machine's fingerprint.
 * Blocks until the two have agreed (0), or the connection failed or was
 * cancelled (session_cancel) (-1, with session_status saying why) */
int session_connect(const session_start *mine, session_start *out, unsigned long long (*hash)(unsigned frame));

/* On the machine's thread, every vertical blank: this PC's player's
 * controls now (game_input's player bits and coin) go to the other PC for
 * vertical blank frame + delay, and in gets both players' for this one
 * (it waits for the other's). booting: the game's start-up step (no
 * inputs). 0, or -1 when the session has ended (session_status says why):
 * the machine should stop */
int session_frame(unsigned frame, int booting, unsigned bits, int coin, game_input *in);

/* the newest check vertical blank whose fingerprints agreed (0 none yet) */
unsigned session_verified(void);

/* from the window: this PC's menu is open (the other PC waits, and says so) */
void session_paused(int on);

/* from any thread: stop waiting and leave (the other PC is told) */
void session_cancel(void);

/* what the window shows over the picture now ("" nothing): waiting for
 * the other player, the other PC's menu, why the session ended. 1 when the
 * session has ended (or never began) */
int session_status(char *buf, size_t n);

/* at the end: the socket closes, the other PC is told */
void session_end(void);

#endif
