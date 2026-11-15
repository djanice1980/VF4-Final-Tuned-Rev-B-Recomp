/* Two players on two PCs (docs/design.md sections 45 and 46): each PC runs
 * the whole machine, the same run, and the two send each other only their
 * player's controls for each vertical blank, a few vertical blanks ahead
 * (the input delay). One PC hosts, as player 1 or player 2 (its choice),
 * the other joins as the other player. When the session ends, the host's
 * game can go on alone (the caller's choice), and another PC can join it
 * later (session_open, below); the joining PC's stops.
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

/* ---- a PC joining a game going on (docs/design.md section 49) ----
 *
 * Once the session has ended and the host's game goes on alone, another PC
 * (of the same build) can join it as the other player. Both PCs keep a
 * snapshot of their machine at the end of the session's start-up, the
 * reference: the host its own, and the joining PC, which replays the
 * session's start as it was (the same cabinet, both players' cards then,
 * no inputs), its copy, the same run to that vertical blank. The host
 * then sends only the difference from the reference to its machine as it
 * is now (snap_patch.h), with the joining player's card in its unit; the
 * joining PC loads it, and both go on from the next vertical blank as a
 * session again. */

/* the host, on the machine's thread, once session_frame has said the
 * session ended: the other PC is told, and the session stays open for
 * another to join. The reference is the machine at the end of vertical
 * blank ref_frame, ref_size bytes, snap_hash ref_hash. 0, or -1 when it
 * cannot (the network failed): then session_end */
int session_open(unsigned ref_frame, size_t ref_size, unsigned long long ref_hash);

/* the host, on the machine's thread while it plays alone: 1 once a
 * joining PC has its copy at the reference: *p is its player, *same 1 when
 * its copy is the host's reference to the byte (else the difference is
 * made from nothing). The join is then the machine's to finish
 * (session_late_send) or give up (session_late_cancel) */
int session_late_ready(session_player *p, int *same);

/* the host, after session_late_ready, each vertical blank it waits for
 * the game on the joining player's side to end (card_wait 1: the card of
 * the player who left is still in use there): 1 while the joining PC is
 * still there, 0 when it has gone */
int session_late_still(int card_wait);

/* the host: the difference from the reference to its machine at the end
 * of vertical blank frame (patch, n bytes: the session frees it) goes to
 * the joining PC; card_in 1 when the joining player's card went into its
 * unit. The session runs again from frame + 1 (session_frame), inputs from
 * frame + 1 + delay. 0, or -1 when the joining PC has gone (patch freed) */
int session_late_send(unsigned char *patch, size_t n, unsigned frame, int card_in);

/* the host: the join cannot go on (why goes to the joining PC); the
 * session is open again */
void session_late_cancel(const char *why);

/* the joining PC, after session_connect: 1 when it joins a game going on
 * (session_connect's start is the session's start as it was; this PC's
 * player is the one it sent), with the host's reference vertical blank */
int session_late(unsigned *ref_frame);

/* the joining PC, on the machine's thread, once it has its own snapshot
 * at the reference (ref_frame, ref_size bytes, snap_hash ref_hash): waits
 * for the host's difference. 0 with *patch (free it), *n, the vertical
 * blank it was taken at and whether this PC's card went in; -1 when the
 * session ended (session_status says why) */
int session_late_get(unsigned ref_frame, size_t ref_size, unsigned long long ref_hash, unsigned char **patch,
                     size_t *n, unsigned *frame, int *card_in);

/* the joining PC: the host's machine could not be made or loaded here
 * (why); the session ends and the host is told */
void session_late_failed(const char *why);

/* 1 while a PC is joining a game going on (both PCs), for the note */
int session_joining(void);

/* the host playing alone: 1 when another PC can join, with this PC's
 * address to join in where (n bytes; "" when it has none) */
int session_can_join(char *where, size_t n);

/* The join code (docs/design.md section 50, secure.h). The host makes a
 * code of six digits as it starts to wait, and again each time a player
 * leaves; a joining PC must have typed it (the key exchange fails
 * otherwise). A code works once. After three wrong codes typed for it, it
 * no longer works, until the host's player asks for a new one (the game's
 * menu, Join code).
 *
 * The joining PC: the code as typed (--code, or two-pc.txt), before
 * session_connect. 0, or -1 when it is not six digits */
int session_code_typed(const char *typed);

/* the host: whether another PC can join now, and with what code
 * (SESSION_CODE_SHOWN: "482 913" in buf; SESSION_CODE_GONE: three wrong
 * codes were typed, so there is none until session_code_new) */
enum { SESSION_CODE_NONE, SESSION_CODE_SHOWN, SESSION_CODE_GONE };
int session_code(char *buf, size_t n);

/* the host, while another PC can join: a new code (the old one no longer
 * works) */
void session_code_new(void);

#endif
