/* Two players on two PCs: see session.h and docs/design.md sections 46, 49
 * and 50.
 *
 * Every datagram starts with "VF4N", the protocol's version, its kind and
 * the session's number (0 in a HELLO, a CHALLENGE and a REFUSE). The join
 * code's exchange (section 50, secure.h) goes in the clear:
 *   HELLO    joining -> host, every 250 ms until answered: the build, then
 *            the try's number (CPace's sid) and the joining PC's point
 *            (zeros after them, so a build from before section 50 reads it
 *            as a HELLO of another build and says so)
 *   CHALLENGE host -> joining: the host's point
 *   REFUSE   host -> joining: why (another build, a game going on, the code
 *            not right)
 * and all the others are sealed (the kind's top bit set; secure_seal: the
 * header in the clear, then a counter, the body encrypted, the seal), first
 * with the try's keys:
 *   PROOF    joining -> host, every 250 ms until answered: the joining
 *            player's card. That the host can open it says the joining PC
 *            typed the code; that the host's answer opens says the host has
 *            it
 * then with the session's (the same keys):
 *   REFUSE   host -> joining PC: why the host gave the join up
 *   WELCOME  host -> joining: the session's number, the cabinet's settings,
 *            the input delay, whether a backup follows, which player the
 *            host is, its player's card
 *   CHUNK    host -> joining: 1,024 bytes of the backup, sent again until
 *            all are acknowledged
 *   CHUNKS   joining -> host: which chunks have come (a bit each)
 *   INPUT    both ways, every vertical blank and every 10 ms: the inputs
 *            the other PC has not acknowledged (oldest first, up to 64),
 *            the newest of the other's this PC has with none missing, the
 *            menu's pause, and this PC's newest fingerprints
 *   BYE      leaving
 * and for a PC joining a game going on (section 49):
 *   LATE     host -> joining, in place of WELCOME: the session's start as
 *            it was (the cabinet's settings, the input delay, whether the
 *            backup follows, the host's player, both players' cards then)
 *            and the host's reference (the vertical blank at the end of the
 *            start-up, the machine's size and fingerprint there); the
 *            backup follows in CHUNKs as after a WELCOME
 *   JOINING  every 100 ms while the joining PC starts its copy: its
 *            vertical blank (from it), or 1 while the host waits for the
 *            game on the joining player's side to end (from the host)
 *   READY    joining -> host, every 100 ms: its copy is at the reference
 *            (its vertical blank, size and fingerprint there)
 *   PATCH    host -> joining: 1,176 bytes of the difference from the
 *            reference to the host's machine (snap_patch.h), with the
 *            vertical blank it was taken at, its whole size, the chunk's
 *            number and whether the joining player's card went in; sent
 *            again until acknowledged
 *   PATCHACK joining -> host, every 16 chunks and every 10 ms: the chunks
 *            that have come (all below a number, then a bit each for the
 *            next 4,096)
 * Numbers are big-endian. */
#include "session.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "secure.h"
#include "udp.h"
#include "settings.h"

#ifndef VF4FT_VERSION
#define VF4FT_VERSION "development build"
#endif
#ifndef VF4FT_SOURCE
#define VF4FT_SOURCE "sources unknown"     /* the builder gives a fingerprint of the sources (build.py) */
#endif
#define BUILD_ID VF4FT_VERSION " (" VF4FT_SOURCE ")"
#define BUILD_MAX 96

#define PROTO 1
enum { P_HELLO = 1, P_REFUSE, P_WELCOME, P_CHUNK, P_CHUNKS, P_INPUT, P_BYE, P_LATE, P_JOINING, P_READY, P_PATCH,
       P_PATCHACK, P_CHALLENGE, P_PROOF };
#define SEALED 0x80                 /* in the kind: a sealed datagram */
#define HDR 10
#define CHUNK 1024
#define NCHUNKS ((GAME_BACKUP_SIZE + CHUNK - 1) / CHUNK)
#define RING 512                    /* vertical blanks of inputs kept */
#define HRING 64                    /* fingerprints kept */
#define SEND_MAX 64                 /* inputs a datagram */
#define HASH_SEND 4                 /* fingerprints a datagram */
#define QUIET_MS 15000              /* nothing heard for this long: the other PC is gone */
#define HELLO_MS 60000              /* the joining PC asks this long */
#define WAIT_SAY_MS 300             /* waiting longer than this is said on the screen */
#define PCHUNK 1176                 /* bytes of the difference a datagram: sealed, 1,223 bytes (IPv6's least MTU
                                       leaves 1,232) */
#define PWINDOW 512                 /* its chunks sent and not acknowledged, at most */
#define PBURST 32                   /* chunks a turn of the network thread, at most */
#define PRESEND_MS 50               /* a chunk not acknowledged this long goes again */
#define ACK_BITS 4096               /* chunks a PATCHACK has a bit for */
#define PATCH_MAX (512u << 20)

/* a PC joining a game going on: where it stands */
enum {
    PH_PLAY,            /* the session runs (or has ended, or never began) */
    PH_OPEN,            /* host: it plays alone, and a PC can join */
    PH_BOOT,            /* a PC is joining: it starts its copy of the session's start */
    PH_READY,           /* its copy is at the reference (host: until the machine's thread takes it up) */
    PH_PICKED,          /* host: the machine's thread makes the difference */
    PH_SEND,            /* the difference goes over; the session runs again from its vertical blank */
};

static int role, cfg_port;
static char cfg_addr[256];
static udp_sock sock = UDP_NONE;
static udp_addr peer;
static Uint32 sid;
static int side, delay;
static unsigned long long (*hash_fn)(unsigned frame);

static SDL_Mutex *lock;
static SDL_Condition *cond;
static SDL_Thread *thread;
static SDL_AtomicInt quit_thread, cancelled, paused_here;

/* under lock */
static char status[320];
static int ended;                   /* the session is over (or never began) */
static int left;                    /* the other PC said it leaves: its inputs that came are played first */
static struct { unsigned frame; Uint16 v; } mine[RING], theirs[RING];
static unsigned mine_first, mine_newest;    /* this PC's inputs: from the first stamped (0 none yet) */
static int started;                 /* the start-up step has ended: inputs from then + delay (mine_first) */
static unsigned theirs_full;        /* the other's inputs are all here up to this vertical blank */
static unsigned their_ack;          /* and they have all of this PC's up to this one */
static int their_pause;
static Uint64 heard_ms;
static struct { unsigned frame; Uint64 h; } my_hash[HRING], their_hash[HRING];
static unsigned verified, agreed;     /* the newest check both agreed on; how many did */
/* the host's backup going over (host: what is acknowledged; joining: what has come) */
static unsigned char chunk_have[(NCHUNKS + 7) / 8];
static const unsigned char *backup_src;
static int backup_going, guest_running;
static unsigned char hello_card_pkt[512];   /* the host's WELCOME (or LATE), sent again for a repeated HELLO */
static int welcome_len;
static int thread_gone;             /* the network thread has stopped (the network failed) */

/* a PC joining a game going on (under lock) */
static int phase;                   /* PH_* */
static int late;                    /* joining PC: it joins a game going on, until it has the host's machine */
static session_start start_kept;    /* host: what both machines started from, for a PC that joins later */
static unsigned ref_frame;          /* the host's reference (joining PC: as the host said) */
static Uint32 ref_size;
static Uint64 ref_hash;
static unsigned their_frame;        /* the joining PC's copy at the reference (host: from READY; joining: its own) */
static Uint32 their_size;
static Uint64 their_hash_ref;
static session_player late_player;  /* host: the joining player (from its HELLO) */
static unsigned boot_frame;         /* the joining PC's vertical blank as it starts its copy */
static int card_wait;               /* the host waits for the game on the joining player's side to end */
static char peer_text[96];
static char open_where[96];         /* host, open: this PC's address to join */
/* the difference (host: going, *_bits what is acknowledged; joining: coming, what has come) */
static unsigned char *patch_buf, *patch_bits;
static Uint32 *patch_sent;          /* host: when each chunk went last (SDL_GetTicks | 1) */
static size_t patch_n;
static unsigned patch_count, patch_cum, patch_next, patch_frame, patch_new, patch_resent;
static int patch_card, patch_done;
static Uint64 patch_t0;

/* the join code and the keys (section 50) */
#define CODE_TRIES 3                /* wrong codes typed, then the code no longer works */
static const char channel[] = "VF4FT two PCs: " BUILD_ID;   /* CPace's CI: both PCs run the same build */
static const char ad_joining[] = "joining PC", ad_host[] = "host";
static SDL_Mutex *tx_lock;
static secure_out tx;               /* to the other PC (under tx_lock) */
static secure_in rx;                /* from it (where datagrams are taken: the thread, or the steps before it) */
static int keyed;                   /* tx and rx are the session's (under tx_lock) */
/* host (under lock) */
static char code[SECURE_CODE_DIGITS + 1];   /* the join code now ("" none) */
static int code_tries;              /* wrong codes typed for it */
static int code_spent;              /* why there is none: 1 a PC joined with it, 2 three wrong codes */
static int waiting;                 /* host_wait runs: its note says the code */
static int stranger_lines;          /* lines logged about PCs outside the session while this code stands */
#define STRANGER_LINES 20
static char wait_where[96];         /* this PC's address as that note says it */
static char wrong_from[96];         /* the PC a wrong code last came from */
static struct {                     /* a PC trying the code: its HELLO's numbers, this PC's point, the keys */
    int on;
    udp_addr from;
    unsigned char sid[SECURE_SID], Ya[SECURE_POINT], Yb[SECURE_POINT];
    secure_out out;
    secure_in in;
} try_;
/* joining PC: the code typed */
static char code_typed[SECURE_CODE_DIGITS + 1];

/* ---- bytes ---- */
static void put32(unsigned char *p, Uint32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (unsigned char)v; }
static Uint32 get32(const unsigned char *p) { return (Uint32)p[0] << 24 | (Uint32)p[1] << 16 | (Uint32)p[2] << 8 | p[3]; }
static void put16(unsigned char *p, Uint16 v) { p[0] = v >> 8; p[1] = (unsigned char)v; }
static Uint16 get16(const unsigned char *p) { return (Uint16)(p[0] << 8 | p[1]); }
static void put64(unsigned char *p, Uint64 v) { put32(p, (Uint32)(v >> 32)); put32(p + 4, (Uint32)v); }
static Uint64 get64(const unsigned char *p) { return (Uint64)get32(p) << 32 | get32(p + 4); }
static int bit(const unsigned char *b, unsigned i) { return b[i / 8] >> (i % 8) & 1; }
static void bit_set(unsigned char *b, unsigned i) { b[i / 8] |= (unsigned char)(1u << (i % 8)); }

static int header(unsigned char *p, int kind)
{
    memcpy(p, "VF4N", 4);
    p[4] = PROTO;
    p[5] = (unsigned char)kind;
    put32(p + 6, sid);
    return HDR;
}

/* a datagram to `to`, sealed with the session's keys (its kind marked SEALED) */
static void send_sealed(const udp_addr *to, const unsigned char *b, int n)
{
    unsigned char s[HDR + 2048];
    if (n < HDR || n + SECURE_OVER > (int)sizeof s)
        return;
    memcpy(s, b, (size_t)n);
    s[5] |= SEALED;
    SDL_LockMutex(tx_lock);
    size_t m = keyed ? secure_seal(&tx, s, HDR, (size_t)n) : 0;
    SDL_UnlockMutex(tx_lock);
    if (m)
        udp_send(sock, to, s, m);
}

/* a sealed datagram opened in place with the keys `in`: its plain length,
 * its kind without SEALED; -1 when it is not ours or not sealed, or its seal
 * is not right, or it came before */
static int unseal(unsigned char *b, int n, secure_in *in)
{
    if (n < HDR || memcmp(b, "VF4N", 4) || b[4] != PROTO || !(b[5] & SEALED))
        return -1;
    size_t m = secure_open(in, b, HDR, (size_t)n);
    if (!m)
        return -1;
    b[5] &= (unsigned char)~SEALED;
    return (int)m;
}

/* the session's keys gone (under lock): none, or the next try's */
static void keys_forget(void)
{
    SDL_LockMutex(tx_lock);
    secure_forget(&tx, &rx);
    keyed = 0;
    SDL_UnlockMutex(tx_lock);
}

/* a player: its card's state, the card, its profile's name */
static int put_player(unsigned char *p, const session_player *pl)
{
    p[0] = (unsigned char)pl->card;
    memcpy(p + 1, pl->bytes, GAME_CARD_SIZE);
    memcpy(p + 1 + GAME_CARD_SIZE, pl->name, CARD_NAME_MAX);
    p[GAME_CARD_SIZE + CARD_NAME_MAX] = 0;
    return 1 + GAME_CARD_SIZE + CARD_NAME_MAX;
}
#define PLAYER_BYTES (1 + GAME_CARD_SIZE + CARD_NAME_MAX)
#define LATE_BYTES (7 + 16 + 2 * PLAYER_BYTES)
/* a HELLO's size, before section 50 and since: the host answers a shorter
 * one with nothing, so what it sends a PC outside the session (a REFUSE of
 * HDR + 199 bytes at most, a CHALLENGE of HDR + 32) is never more than came
 * in (no use for a forged sender's address) */
#define HELLO_BYTES (HDR + BUILD_MAX + PLAYER_BYTES)

static void get_player(const unsigned char *p, session_player *pl)
{
    pl->card = p[0] <= SESSION_CARD_BYTES ? p[0] : SESSION_CARD_NONE;
    memcpy(pl->bytes, p + 1, GAME_CARD_SIZE);
    memcpy(pl->name, p + 1 + GAME_CARD_SIZE, CARD_NAME_MAX);
    pl->name[CARD_NAME_MAX - 1] = 0;
    if (pl->card != SESSION_CARD_BYTES)
        memset(pl->bytes, 0, GAME_CARD_SIZE);
}

static Uint32 new_sid(void)
{
    Uint32 v;
    if (secure_random(&v, sizeof v))
        v = (Uint32)(SDL_GetPerformanceCounter() ^ (SDL_GetTicksNS() << 7));
    return v | 1;
}

static void (*log_fn)(const char *line);

void session_logger(void (*fn)(const char *line))
{
    log_fn = fn;
}

static void vsay(const char *fmt, va_list ap)
{
    char line[400];
    SDL_strlcpy(line, "session: ", sizeof line);
    SDL_vsnprintf(line + 9, sizeof line - 9, fmt, ap);
    if (log_fn)
        log_fn(line);
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

/* host, under lock: a line about a PC outside the session (refused, or
 * trying the code). STRANGER_LINES of them while one code stands, then one
 * saying the rest are not logged: datagrams anyone can send do not fill the
 * log. (Wrong codes, three at most for each code, are said with say.) */
static void say_stranger(const char *fmt, ...)
{
    if (stranger_lines > STRANGER_LINES)
        return;
    if (stranger_lines++ == STRANGER_LINES) {
        say("more datagrams from PCs outside the session: not logged until the join code changes");
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

/* bytes from another PC as text for the log or a note: printable ASCII
 * (others as '?'), up to a 0 byte or n bytes */
static void printable(char *out, size_t size, const unsigned char *b, size_t n)
{
    size_t at = 0;
    for (size_t i = 0; i < n && b[i] && at + 1 < size; i++)
        out[at++] = b[i] >= 0x20 && b[i] < 0x7f ? (char)b[i] : '?';
    out[at] = 0;
}

/* under lock: the session is over, for this reason */
static void end_locked(const char *why)
{
    if (ended)
        return;
    ended = 1;
    SDL_strlcpy(status, why, sizeof status);
    say("ended: %s", why);
    SDL_BroadcastCondition(cond);
}

void session_config(int r, int port, const char *addr)
{
    role = r;
    cfg_port = port > 0 ? port : UDP_PORT;
    SDL_strlcpy(cfg_addr, addr ? addr : "", sizeof cfg_addr);
}

static int host_side;               /* the host's player (session_host_side) */

void session_host_side(int s)
{
    host_side = s == 1;
}

int session_host_side_get(void)
{
    return host_side;
}

int session_role(void)
{
    return role;
}

/* ---- inputs and fingerprints (under lock) ---- */

/* a session's inputs and fingerprints from nothing (again) */
static void play_reset(void)
{
    for (int i = 0; i < RING; i++)
        mine[i].frame = theirs[i].frame = ~0u;
    for (int i = 0; i < HRING; i++)
        my_hash[i].frame = their_hash[i].frame = ~0u;
    mine_first = mine_newest = 0;
    started = 0;
    theirs_full = their_ack = 0;
    their_pause = 0;
    verified = agreed = 0;
    ended = left = 0;
}

static void advance_full(void)
{
    if (!started)
        return;
    while (theirs[(theirs_full + 1) % RING].frame == theirs_full + 1)
        theirs_full++;
    SDL_BroadcastCondition(cond);
}

static void compare(unsigned frame)
{
    int i = (int)(frame / SESSION_CHECK % HRING);
    if (my_hash[i].frame != frame || their_hash[i].frame != frame)
        return;
    if (my_hash[i].h != their_hash[i].h) {
        char why[200];
        SDL_snprintf(why, sizeof why, "The two games went apart at vertical blank %u. "
                     "(This PC %016llx, the other %016llx.)", frame, (unsigned long long)my_hash[i].h,
                     (unsigned long long)their_hash[i].h);
        end_locked(why);
        return;
    }
    if (frame > verified || (!agreed && frame == 0)) {
        verified = frame;
        agreed++;
    }
}

/* an INPUT datagram's body */
static void take_input(const unsigned char *p, int n)
{
    if (n < 5)
        return;
    unsigned first = get32(p);
    int count = p[4];
    p += 5, n -= 5;
    if (n < 2 * count + 6)
        return;
    for (int i = 0; i < count; i++) {
        unsigned f = first + (unsigned)i;
        if (started && f <= theirs_full)
            continue;
        theirs[f % RING].frame = f;
        theirs[f % RING].v = get16(p + 2 * i);
    }
    p += 2 * count, n -= 2 * count;
    unsigned ack = get32(p);
    if (ack > their_ack)
        their_ack = ack;
    their_pause = p[4] & 1;
    int nh = p[5];
    p += 6, n -= 6;
    for (int i = 0; i < nh && n >= 12; i++, p += 12, n -= 12) {
        unsigned f = get32(p);
        int k = (int)(f / SESSION_CHECK % HRING);
        their_hash[k].frame = f;
        their_hash[k].h = get64(p + 4);
        compare(f);
    }
    advance_full();
}

/* an INPUT datagram (under lock) */
static int make_input(unsigned char *b)
{
    int n = header(b, P_INPUT);
    unsigned first = their_ack + 1, count = 0;
    if (mine_first && first < mine_first)
        first = mine_first;
    if (mine_first && mine_newest >= first) {
        count = mine_newest - first + 1;
        if (count > SEND_MAX)
            count = SEND_MAX;
    }
    put32(b + n, first);
    b[n + 4] = (unsigned char)count;
    n += 5;
    for (unsigned i = 0; i < count; i++, n += 2)
        put16(b + n, mine[(first + i) % RING].v);
    put32(b + n, started ? theirs_full : 0);
    b[n + 4] = (unsigned char)(SDL_GetAtomicInt(&paused_here) ? 1 : 0);
    unsigned char *nh = b + n + 5;
    n += 6;
    *nh = 0;
    /* the newest fingerprints, newest first */
    unsigned newest = 0;
    for (int i = 0; i < HRING; i++)
        if (my_hash[i].frame != ~0u && my_hash[i].frame > newest)
            newest = my_hash[i].frame;
    for (int k = 0; k < HASH_SEND && my_hash[newest / SESSION_CHECK % HRING].frame == newest; k++) {
        const int i = (int)(newest / SESSION_CHECK % HRING);
        put32(b + n, newest);
        put64(b + n + 4, my_hash[i].h);
        n += 12;
        (*nh)++;
        if (newest < SESSION_CHECK)
            break;
        newest -= SESSION_CHECK;
    }
    return n;
}

/* this PC's inputs to the other, while the session runs (a joining PC's
 * once it has the host's game: the host takes its first INPUT as saying so) */
static void send_input(void)
{
    unsigned char b[HDR + 5 + 2 * SEND_MAX + 6 + 12 * HASH_SEND];
    SDL_LockMutex(lock);
    if ((phase == PH_PLAY || phase == PH_SEND) && !late) {
        int n = make_input(b);
        send_sealed(&peer, b, n);
    }
    SDL_UnlockMutex(lock);
}

static void send_bye(void)
{
    unsigned char b[HDR];
    header(b, P_BYE);
    for (int i = 0; i < 3; i++)
        send_sealed(&peer, b, sizeof b);
}

/* why the host says no: in the clear to a PC outside the session (peer 0),
 * sealed to the session's PC (peer 1) */
static void send_refuse_to(const udp_addr *to, const char *why, int to_peer)
{
    unsigned char b[HDR + 200];
    int n = header(b, P_REFUSE);
    if (!to_peer)
        put32(b + 6, 0);
    size_t l = SDL_strlen(why);
    if (l > 199)
        l = 199;
    memcpy(b + n, why, l);
    if (to_peer)
        send_sealed(to, b, n + (int)l);
    else
        udp_send(sock, to, b, (size_t)n + l);
}

static void send_refuse(const udp_addr *to, const char *why)
{
    send_refuse_to(to, why, 0);
}

/* the host: the backup's chunks not acknowledged yet */
static void send_chunks(void)
{
    unsigned char b[HDR + 2 + CHUNK];
    for (int i = 0; i < NCHUNKS; i++) {
        if (chunk_have[i / 8] >> (i % 8) & 1)
            continue;
        int n = header(b, P_CHUNK);
        put16(b + n, (Uint16)i);
        int len = i == NCHUNKS - 1 ? GAME_BACKUP_SIZE - i * CHUNK : CHUNK;
        memcpy(b + n + 2, backup_src + i * CHUNK, (size_t)len);
        send_sealed(&peer, b, n + 2 + len);
    }
}

static int all_chunks(void)
{
    for (int i = 0; i < NCHUNKS; i++)
        if (!(chunk_have[i / 8] >> (i % 8) & 1))
            return 0;
    return 1;
}

static void send_chunk_ack(const udp_addr *to)
{
    unsigned char b[HDR + sizeof chunk_have];
    int n = header(b, P_CHUNKS);
    memcpy(b + n, chunk_have, sizeof chunk_have);
    send_sealed(to, b, n + (int)sizeof chunk_have);
}

/* a clear datagram's kind, or -1 when it is not ours */
static int clear_kind(const unsigned char *b, int n)
{
    if (n < HDR || memcmp(b, "VF4N", 4) || b[4] != PROTO)
        return -1;
    return b[5];
}

/* ---- the join code (section 50; under lock) ---- */

static void try_drop(void)
{
    try_.on = 0;
    secure_forget(&try_.out, &try_.in);
}

/* the host's note while it waits for the first player */
static void wait_status(void)
{
    int h = host_side;
    char wrong[160] = "";
    if (code_tries && code[0])
        SDL_snprintf(wrong, sizeof wrong, " A wrong code was typed on %s.", wrong_from);
    if (code[0])
        SDL_snprintf(status, sizeof status, "Hosting as player %d: waiting for player %d.%s On the other PC, join %s "
                     "with the code %.3s %s.", h + 1, 2 - h, wrong, wait_where, code, code + 3);
    else
        SDL_snprintf(status, sizeof status, "Hosting as player %d: waiting for player %d. Three wrong codes were "
                     "typed (the last on %s), so the code no longer works: get a new one in the game's menu, Join "
                     "code.", h + 1, 2 - h, wrong_from);
}

/* host: a new code; a PC trying the one before has to start again */
static void code_new(void)
{
    try_drop();
    code_tries = 0;
    code_spent = 0;
    stranger_lines = 0;
    if (secure_new_code(code)) {
        code[0] = 0;
        say("no random numbers for a join code: no other PC can join");
    } else {
        say("the join code is now %s", code);
    }
    if (waiting)
        wait_status();
}

/* host: a HELLO from a PC outside the session, while one can join. The same
 * build, with a code to join: this PC's point (CHALLENGE), and the keys the
 * PC's PROOF will open with if it typed the code. The same HELLO again (our
 * CHALLENGE was lost): the same CHALLENGE */
static void try_hello(const unsigned char *b, int n, const udp_addr *from)
{
    if (n < HELLO_BYTES)
        return;
    char build[BUILD_MAX + 1], text[96];
    memcpy(build, b + HDR, BUILD_MAX);
    build[BUILD_MAX] = 0;
    udp_text(from, text, sizeof text);
    if (strcmp(build, BUILD_ID)) {
        char why[240], shown[BUILD_MAX + 1];
        SDL_snprintf(why, sizeof why, "The host runs another build of the game: %s. Both PCs need the same.", BUILD_ID);
        printable(shown, sizeof shown, b + HDR, BUILD_MAX);
        say_stranger("refused %s: it runs %s", text, shown);
        send_refuse(from, why);
        return;
    }
    if (!code[0]) {
        send_refuse(from, code_spent == 2 ? "Three wrong codes were typed, so the host's code no longer works. Its "
                                            "player can get a new one in the game's menu (Join code)."
                                          : "The host has no join code now.");
        return;
    }
    const unsigned char *s = b + HDR + BUILD_MAX, *Ya = s + SECURE_SID;
    unsigned char out[HDR + SECURE_POINT];
    Uint32 keep = sid;
    sid = 0;
    int m = header(out, P_CHALLENGE);
    sid = keep;
    if (try_.on && udp_same(from, &try_.from) && !memcmp(try_.sid, s, SECURE_SID) && !memcmp(try_.Ya, Ya, SECURE_POINT)) {
        memcpy(out + m, try_.Yb, SECURE_POINT);
        udp_send(sock, from, out, sizeof out);
        return;
    }
    unsigned char g[32], y[32], isk[SECURE_ISK];
    if (secure_random(y, sizeof y)) {
        send_refuse(from, "The host has no random numbers for the code's exchange.");
        return;
    }
    try_drop();
    cpace_generator(g, code, SECURE_CODE_DIGITS, channel, sizeof channel - 1, s, SECURE_SID);
    cpace_point(try_.Yb, y, g);
    int bad = cpace_key(isk, y, Ya, s, SECURE_SID, Ya, ad_joining, sizeof ad_joining - 1, try_.Yb, ad_host,
                        sizeof ad_host - 1);
    secure_wipe(g, sizeof g);
    if (!bad) {
        try_.on = 1;
        try_.from = *from;
        memcpy(try_.sid, s, SECURE_SID);
        memcpy(try_.Ya, Ya, SECURE_POINT);
        secure_keys(isk, 1, &try_.out, &try_.in);
        memcpy(out + m, try_.Yb, SECURE_POINT);
        udp_send(sock, from, out, sizeof out);
        say_stranger("%s tries to join: the code's exchange", text);
    } else {
        say_stranger("%s tries to join with a point that is not right: not answered", text);
    }
    secure_wipe(y, sizeof y);
    secure_wipe(isk, sizeof isk);
}

/* host: a sealed PROOF from the PC trying the code. It opens: the PC typed
 * the code. Returns 1 then, with its player in *pl and its keys the
 * session's; the code is used up. It does not open: a wrong code (the third
 * and the code no longer works), and the PC is told */
static int try_proof(unsigned char *b, int n, const udp_addr *from, session_player *pl)
{
    if (!try_.on || !udp_same(from, &try_.from))
        return 0;
    char text[96];
    udp_text(from, text, sizeof text);
    int m = unseal(b, n, &try_.in);
    if (m < 0) {
        try_drop();
        code_tries++;
        SDL_strlcpy(wrong_from, text, sizeof wrong_from);
        if (code_tries >= CODE_TRIES) {
            code[0] = 0;
            code_spent = 2;
            say("%s typed a wrong code, the third: the code no longer works", text);
            send_refuse(from, "The code is not right. That was the third wrong one, so the host's code no longer "
                              "works: its player can get a new one in the game's menu (Join code).");
        } else {
            say("%s typed a wrong code (%d of %d)", text, code_tries, CODE_TRIES);
            send_refuse(from, "The code is not right.");
        }
        if (waiting)
            wait_status();
        else if (code_tries >= CODE_TRIES)
            SDL_snprintf(status, sizeof status, "Three wrong join codes were typed (the last on %s), so the code no "
                         "longer works.", text);
        else
            SDL_snprintf(status, sizeof status, "A wrong join code was typed on %s.", text);
        return 0;
    }
    if (b[5] != P_PROOF || m < HDR + PLAYER_BYTES)
        return 0;
    get_player(b + HDR, pl);
    SDL_LockMutex(tx_lock);
    tx = try_.out;
    rx = try_.in;
    keyed = 1;
    SDL_UnlockMutex(tx_lock);
    try_drop();
    code[0] = 0;
    code_tries = 0;
    code_spent = 1;
    say("%s typed the code: the session's keys are made", text);
    return 1;
}

/* ---- a PC joining a game going on (under lock) ---- */

static void patch_free(void)
{
    free(patch_buf);
    free(patch_bits);
    free(patch_sent);
    patch_buf = patch_bits = NULL;
    patch_sent = NULL;
}

/* what the note says while a PC joins */
static void late_status(void)
{
    unsigned pct = 0;
    if (phase == PH_BOOT && ref_frame)
        pct = boot_frame >= ref_frame ? 99 : (unsigned)((unsigned long long)boot_frame * 100 / ref_frame);
    else if (phase == PH_SEND && patch_count)
        pct = (unsigned)((unsigned long long)patch_cum * 100 / patch_count);
    if (role == SESSION_HOST) {
        int p = 2 - side;           /* the joining player */
        if (phase == PH_BOOT)
            SDL_snprintf(status, sizeof status, "Player %d is joining from %s: starting the game there (%u%%).", p,
                         peer_text, pct);
        else if ((phase == PH_READY || phase == PH_PICKED) && card_wait)
            SDL_snprintf(status, sizeof status, "Player %d is joining from %s, once the game on player %d's side is "
                         "over (the card there is in use).", p, peer_text, p);
        else if (phase == PH_READY || phase == PH_PICKED)
            SDL_snprintf(status, sizeof status, "Player %d is joining from %s: copying this game to it.", p, peer_text);
        else if (phase == PH_SEND)
            SDL_snprintf(status, sizeof status, "Player %d is joining from %s: copying this game to it (%u%%).", p,
                         peer_text, pct);
    } else {
        if (phase == PH_BOOT)
            SDL_snprintf(status, sizeof status, "Joining the game going on at %s: starting the game here (%u%%).",
                         peer_text, pct);
        else if (phase == PH_READY && card_wait)
            SDL_snprintf(status, sizeof status, "Joining the game going on at %s, once the game on player %d's side "
                         "is over.", peer_text, side + 1);
        else if (phase == PH_READY)
            SDL_snprintf(status, sizeof status, "Joining the game going on at %s: copying it here.", peer_text);
        else if (phase == PH_SEND)
            SDL_snprintf(status, sizeof status, "Joining the game going on at %s: copying it here (%u%%).", peer_text,
                         pct);
    }
}

/* the host: the joining PC is gone (or the join was given up); open again,
 * with a new code */
static void late_drop(const char *why)
{
    say("%s", why);
    SDL_strlcpy(status, why, sizeof status);
    phase = PH_OPEN;
    backup_going = 0;
    welcome_len = 0;
    card_wait = 0;
    memset(&peer, 0, sizeof peer);
    sid = 0;
    patch_free();
    keys_forget();
    code_new();
}

/* the host, open: a PC typed the code (try_proof). It joins the game going
 * on: LATE, then the backup as the session began with it */
static void late_accept(const session_player *pl, const udp_addr *from, Uint64 now)
{
    char text[96];
    udp_text(from, text, sizeof text);
    late_player = *pl;
    peer = *from;
    SDL_strlcpy(peer_text, text, sizeof peer_text);
    sid = new_sid();
    const session_start *s = &start_kept;
    unsigned char *w = hello_card_pkt;
    int m = header(w, P_LATE);
    w[m++] = (unsigned char)s->coins;
    w[m++] = (unsigned char)s->region;
    w[m++] = (unsigned char)s->network;
    w[m++] = (unsigned char)s->cards;
    w[m++] = (unsigned char)s->delay;
    w[m++] = (unsigned char)s->backup;
    w[m++] = (unsigned char)side;       /* the host's player: the joining PC is the other */
    put32(w + m, ref_frame);
    put32(w + m + 4, ref_size);
    put64(w + m + 8, ref_hash);
    m += 16;
    m += put_player(w + m, &s->player[0]);
    m += put_player(w + m, &s->player[1]);
    welcome_len = m;
    send_sealed(&peer, w, m);
    heard_ms = now;
    boot_frame = 0;
    card_wait = 0;
    backup_src = s->backup_bytes;
    memset(chunk_have, 0, sizeof chunk_have);
    backup_going = s->backup;
    guest_running = 0;
    phase = PH_BOOT;
    say("player %d is joining the game going on, from %s (card %s)", 2 - side, text,
        late_player.card == SESSION_CARD_NONE ? "none" : late_player.name);
    late_status();
}

/* the host: the difference's chunk i */
static void patch_one(unsigned char *b, unsigned i)
{
    int n = header(b, P_PATCH);
    put32(b + n, patch_frame);
    put32(b + n + 4, (Uint32)patch_n);
    put32(b + n + 8, i);
    b[n + 12] = (unsigned char)patch_card;
    n += 13;
    size_t at = (size_t)i * PCHUNK, len = patch_n - at < PCHUNK ? patch_n - at : PCHUNK;
    memcpy(b + n, patch_buf + at, len);
    send_sealed(&peer, b, n + (int)len);
}

/* the host, every turn of the thread: chunks not acknowledged in time
 * again, then new ones while fewer than PWINDOW are on their way */
static void send_patch(Uint64 now)
{
    unsigned char b[HDR + 13 + PCHUNK];
    Uint32 t = (Uint32)now | 1;
    int sent = 0;
    SDL_LockMutex(lock);
    if (phase == PH_SEND && role == SESSION_HOST) {
        for (unsigned i = patch_cum; i < patch_next && sent < PBURST; i++)
            if (!bit(patch_bits, i) && t - patch_sent[i] >= PRESEND_MS) {
                patch_one(b, i);
                patch_sent[i] = t;
                patch_resent++;
                sent++;
            }
        while (sent < PBURST && patch_next < patch_count && patch_next - patch_cum < PWINDOW) {
            patch_one(b, patch_next);
            patch_sent[patch_next++] = t;
            sent++;
        }
    }
    SDL_UnlockMutex(lock);
}

/* the host: the joining PC has the whole difference */
static void patch_over(void)
{
    if (phase != PH_SEND)
        return;
    phase = PH_PLAY;
    say("player %d's PC has this game: %zu bytes in %u datagrams, %llu ms (%u sent again)", 2 - side, patch_n,
        patch_count, (unsigned long long)(SDL_GetTicks() - patch_t0), patch_resent);
    patch_free();
    status[0] = 0;
}

/* the host: a PATCHACK's body */
static void patch_acked(const unsigned char *p, int m)
{
    if (m < 4 + ACK_BITS / 8 || !patch_bits)
        return;
    unsigned cum = get32(p);
    if (cum > patch_count)
        cum = patch_count;
    for (unsigned i = patch_cum; i < cum; i++)
        bit_set(patch_bits, i);
    for (unsigned k = 0; k < ACK_BITS && cum + k < patch_count; k++)
        if (bit(p + 4, k))
            bit_set(patch_bits, cum + k);
    while (patch_cum < patch_count && bit(patch_bits, patch_cum))
        patch_cum++;
    if (patch_cum == patch_count)
        patch_over();
}

/* the joining PC: what has come of the difference */
static void send_patch_ack(void)
{
    unsigned char b[HDR + 4 + ACK_BITS / 8];
    int n = header(b, P_PATCHACK);
    put32(b + n, patch_cum);
    memset(b + n + 4, 0, ACK_BITS / 8);
    for (unsigned k = 0; k < ACK_BITS && patch_cum + k < patch_count; k++)
        if (patch_bits && bit(patch_bits, patch_cum + k))
            bit_set(b + n + 4, k);
    send_sealed(&peer, b, (int)sizeof b);
}

/* the joining PC: a PATCH's body. The first one says where the session
 * runs again from */
static void patch_take(const unsigned char *p, int m)
{
    if (m < 13)
        return;
    unsigned f = get32(p), total = get32(p + 4), i = get32(p + 8);
    int card = p[12] & 1;
    p += 13, m -= 13;
    if (late && phase == PH_READY) {
        if (!total || total > PATCH_MAX)
            return;
        patch_free();
        patch_n = total;
        patch_count = (unsigned)((total + PCHUNK - 1) / PCHUNK);
        patch_buf = malloc(total);
        patch_bits = calloc((patch_count + 7) / 8, 1);
        if (!patch_buf || !patch_bits) {
            patch_free();
            end_locked("This PC has too little memory for the host's game.");
            return;
        }
        patch_frame = f;
        patch_card = card;
        patch_cum = patch_new = 0;
        patch_done = 0;
        patch_t0 = SDL_GetTicks();
        /* the session runs again from the vertical blank after the host's machine was taken */
        started = 1;
        mine_first = f + 1 + (unsigned)delay;
        theirs_full = mine_first - 1;
        phase = PH_SEND;
        say("the host's game is coming: its machine at the end of vertical blank %u, a difference of %u bytes; "
            "this player's card %s", f, total, card ? "is in its unit" : "stays out");
    }
    if (phase == PH_SEND && f == patch_frame && total == patch_n && i < patch_count) {
        size_t at = (size_t)i * PCHUNK, len = patch_n - at < PCHUNK ? patch_n - at : PCHUNK;
        if ((size_t)m == len && !bit(patch_bits, i)) {
            memcpy(patch_buf + at, p, len);
            bit_set(patch_bits, i);
            patch_new++;
            while (patch_cum < patch_count && bit(patch_bits, patch_cum))
                patch_cum++;
            if (patch_cum == patch_count) {
                patch_done = 1;
                phase = PH_PLAY;
                status[0] = 0;
                say("the host's game has come: %zu bytes in %llu ms", patch_n,
                    (unsigned long long)(SDL_GetTicks() - patch_t0));
                SDL_BroadcastCondition(cond);
            }
        }
        if (patch_new >= 16 || patch_done) {
            send_patch_ack();
            patch_new = 0;
        }
    } else if (patch_done) {
        send_patch_ack();               /* all had come: the host did not hear so */
    }
}

/* every 100 ms while a PC joins: the other is told this one is there (and
 * how far it is), and the note says how far it is */
static void late_tick(void)
{
    unsigned char b[HDR + 16];
    int n = 0;
    if (role == SESSION_HOST && (phase == PH_BOOT || phase == PH_READY || phase == PH_PICKED)) {
        n = header(b, P_JOINING);
        put32(b + n, 0);
        b[n + 4] = (unsigned char)card_wait;
        n += 5;
    } else if (role == SESSION_GUEST && late && phase == PH_BOOT) {
        n = header(b, P_JOINING);
        put32(b + n, boot_frame);
        b[n + 4] = 0;
        n += 5;
    } else if (role == SESSION_GUEST && late && phase == PH_READY) {
        n = header(b, P_READY);
        put32(b + n, their_frame);
        put32(b + n + 4, their_size);
        put64(b + n + 8, their_hash_ref);
        n += 16;
    }
    if (n)
        send_sealed(&peer, b, n);
    if (phase != PH_PLAY && phase != PH_OPEN && !ended)
        late_status();
}

/* ---- the running session's own thread: receives, and sends every 10 ms ---- */

/* a REFUSE's words, as "The host said no: ..." (printable ASCII only: they
 * come from the other PC) */
static void refuse_text(char *why, size_t size, const unsigned char *b, int n)
{
    SDL_strlcpy(why, "The host said no: ", size);
    size_t at = SDL_strlen(why);
    if (n > HDR && at < size)
        printable(why + at, size - at, b + HDR, (size_t)(n - HDR < 200 ? n - HDR : 200));
}

/* a datagram (under lock) */
static void take_datagram(unsigned char *b, int n, const udp_addr *from, Uint64 now)
{
    int k = clear_kind(b, n);
    if (k < 0)
        return;
    int from_peer = phase != PH_OPEN && udp_same(from, &peer);
    if (role == SESSION_HOST && (k == P_HELLO || k == (P_PROOF | SEALED))) {
        session_player pl;
        if (from_peer) {
            if (k == (P_PROOF | SEALED) && welcome_len)
                send_sealed(&peer, hello_card_pkt, welcome_len);    /* our WELCOME (or LATE) was lost */
        } else if (phase == PH_OPEN) {
            if (k == P_HELLO)
                try_hello(b, n, from);
            else if (try_proof(b, n, from, &pl))
                late_accept(&pl, from, now);
        } else if (k == P_HELLO && n >= HELLO_BYTES) {
            send_refuse(from, phase == PH_PLAY || phase == PH_SEND ? "A game is going on there already."
                                                                   : "Another player is joining there now.");
        }
        return;
    }
    if (!from_peer)
        return;
    n = unseal(b, n, &rx);          /* from here on, sealed with the session's keys only */
    if (n < 0)
        return;
    k = b[5];
    if (k == P_REFUSE && role == SESSION_GUEST && late) {       /* the host gave the join up */
        char why[240];
        refuse_text(why, sizeof why, b, n);
        end_locked(why);
        return;
    }
    if (get32(b + 6) != sid)
        return;
    heard_ms = now;
    const unsigned char *p = b + HDR;
    int m = n - HDR;
    switch (k) {
    case P_INPUT:
        if (role == SESSION_HOST && phase == PH_SEND)
            patch_over();               /* its inputs come: it has the game */
        if (phase != PH_PLAY && phase != PH_SEND)
            break;
        take_input(p, m);
        if (role == SESSION_HOST && !guest_running) {
            guest_running = 1;
            say("player %d's machine has started", 2 - side);
        }
        break;
    case P_BYE:
        if (role == SESSION_HOST && (phase == PH_BOOT || phase == PH_READY || phase == PH_PICKED)) {
            char why[120];
            SDL_snprintf(why, sizeof why, "Player %d stopped joining.", 2 - side);
            late_drop(why);
            break;
        }
        if (!left)
            say("player %d's PC says it leaves", 2 - side);
        left = 1;
        SDL_BroadcastCondition(cond);
        break;
    case P_CHUNKS:
        if (role == SESSION_HOST && m >= (int)sizeof chunk_have)
            for (size_t i = 0; i < sizeof chunk_have; i++)
                chunk_have[i] |= p[i];
        break;
    case P_CHUNK:
        if (role == SESSION_GUEST)
            send_chunk_ack(&peer);      /* all came before the machine started: our last answer was lost */
        break;
    case P_JOINING:
        if (m < 5)
            break;
        if (role == SESSION_HOST && phase == PH_BOOT)
            boot_frame = get32(p);
        else if (role == SESSION_GUEST && late)
            card_wait = p[4] & 1;
        break;
    case P_READY:
        if (role == SESSION_HOST && phase == PH_BOOT && m >= 16) {
            their_frame = get32(p);
            their_size = get32(p + 4);
            their_hash_ref = get64(p + 8);
            phase = PH_READY;
            say("player %d's PC has its copy at the end of vertical blank %u: %s", 2 - side, their_frame,
                their_frame == ref_frame && their_size == ref_size && their_hash_ref == ref_hash
                    ? "the same as this PC's reference"
                    : "NOT the same as this PC's reference (the whole machine goes over)");
            late_status();
        }
        break;
    case P_PATCH:
        if (role == SESSION_GUEST)
            patch_take(p, m);
        break;
    case P_PATCHACK:
        if (role == SESSION_HOST && phase == PH_SEND)
            patch_acked(p, m);
        break;
    }
}

/* under lock: has the other PC been quiet too long? */
static void quiet_check(Uint64 now)
{
    if (ended || now - heard_ms <= QUIET_MS || (role == SESSION_HOST && phase == PH_OPEN))
        return;
    if (role == SESSION_HOST && (phase == PH_BOOT || phase == PH_READY || phase == PH_PICKED)) {
        char why[120];
        SDL_snprintf(why, sizeof why, "Player %d's PC stopped answering as it joined.", 2 - side);
        late_drop(why);
        return;
    }
    end_locked("The other PC stopped answering.");
}

static int SDLCALL session_thread(void *arg)
{
    (void)arg;
    unsigned char b[2048];
    Uint64 next_send = 0, next_chunks = 0, next_late = 0, next_ack = 0;
    while (!SDL_GetAtomicInt(&quit_thread)) {
        udp_addr from;
        int n = udp_recv(sock, b, sizeof b, &from, 4);
        Uint64 now = SDL_GetTicks();
        if (n < 0) {
            SDL_LockMutex(lock);
            char why[300];
            SDL_snprintf(why, sizeof why, "The network failed: %s", udp_error());
            thread_gone = 1;
            if (role == SESSION_HOST && phase != PH_PLAY && phase != PH_SEND) {
                say("%s", why);         /* the host plays alone: no other PC can join now */
                SDL_strlcpy(status, why, sizeof status);
                phase = PH_PLAY;
            } else {
                end_locked(why);
            }
            SDL_UnlockMutex(lock);
            break;
        }
        SDL_LockMutex(lock);
        if (n > 0)
            take_datagram(b, n, &from, now);
        int ph = phase;
        SDL_UnlockMutex(lock);
        if ((ph == PH_PLAY || ph == PH_SEND) && now >= next_send) {
            next_send = now + 10;
            send_input();
        }
        if (role == SESSION_HOST && backup_going && now >= next_chunks) {
            next_chunks = now + 30;
            SDL_LockMutex(lock);
            int going = !guest_running && !all_chunks() && ph != PH_OPEN;
            SDL_UnlockMutex(lock);
            if (going)
                send_chunks();
            else
                backup_going = 0;
        }
        if (ph != PH_PLAY && ph != PH_OPEN && now >= next_late) {
            next_late = now + 100;
            SDL_LockMutex(lock);
            late_tick();
            SDL_UnlockMutex(lock);
        }
        if (ph == PH_SEND && role == SESSION_HOST)
            send_patch(now);
        if (ph == PH_SEND && role == SESSION_GUEST && now >= next_ack) {
            next_ack = now + 10;
            SDL_LockMutex(lock);
            if (phase == PH_SEND)
                send_patch_ack();
            SDL_UnlockMutex(lock);
        }
        SDL_LockMutex(lock);
        quiet_check(now);
        SDL_UnlockMutex(lock);
    }
    return 0;
}

/* ---- the connection ---- */

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LockMutex(lock);
    SDL_vsnprintf(status, sizeof status, fmt, ap);
    SDL_UnlockMutex(lock);
    va_end(ap);
}

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char why[320];
    SDL_vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    SDL_LockMutex(lock);
    end_locked(why);
    SDL_UnlockMutex(lock);
    return -1;
}

/* this PC's address as a joining PC types it (with the port when not the usual) */
static void where_text(char *out, size_t n, const char *none)
{
    char ip[64];
    udp_local_ip(ip, sizeof ip);
    if (!ip[0])
        SDL_strlcpy(out, none, n);
    else if (cfg_port != UDP_PORT)
        SDL_snprintf(out, n, "%s:%d", ip, cfg_port);
    else
        SDL_strlcpy(out, ip, n);
}

static int host_wait(const session_start *mine_s, session_start *out)
{
    char text[96];
    int h = host_side;
    SDL_LockMutex(lock);
    where_text(wait_where, sizeof wait_where, "this PC's address");
    waiting = 1;
    code_new();                     /* and the note says it */
    SDL_UnlockMutex(lock);
    say("hosting on port %d as player %d; this PC's address %s; build %s", cfg_port, h + 1, wait_where, BUILD_ID);
    unsigned char b[2048];
    while (!SDL_GetAtomicInt(&cancelled)) {
        udp_addr from;
        int n = udp_recv(sock, b, sizeof b, &from, 50);
        if (n < 0) {
            SDL_LockMutex(lock);
            waiting = 0;
            SDL_UnlockMutex(lock);
            return fail("The network failed: %s", udp_error());
        }
        int k = n > 0 ? clear_kind(b, n) : -1;
        if (k != P_HELLO && k != (P_PROOF | SEALED))
            continue;
        session_player pl;
        int joined = 0;
        SDL_LockMutex(lock);
        if (k == P_HELLO)
            try_hello(b, n, &from);
        else
            joined = try_proof(b, n, &from, &pl);
        if (joined)
            waiting = 0;
        SDL_UnlockMutex(lock);
        if (!joined)
            continue;
        udp_text(&from, text, sizeof text);
        *out = *mine_s;
        out->player[h] = mine_s->own;
        out->player[!h] = pl;
        out->side = h;
        peer = from;
        SDL_strlcpy(peer_text, text, sizeof peer_text);
        sid = new_sid();
        /* WELCOME */
        unsigned char *w = hello_card_pkt;
        int m = header(w, P_WELCOME);
        w[m++] = (unsigned char)out->coins;
        w[m++] = (unsigned char)out->region;
        w[m++] = (unsigned char)out->network;
        w[m++] = (unsigned char)out->cards;
        w[m++] = (unsigned char)out->delay;
        w[m++] = (unsigned char)out->backup;
        w[m++] = (unsigned char)h;          /* the host's player: the joining PC is the other */
        m += put_player(w + m, &out->player[h]);
        welcome_len = m;
        send_sealed(&peer, w, m);
        say("player %d joined from %s (card %s)", 2 - h, text,
            out->player[!h].card == SESSION_CARD_NONE ? "none" : out->player[!h].name);
        start_kept = *out;                  /* for a PC that joins later (section 49) */
        backup_src = start_kept.backup_bytes;
        memset(chunk_have, 0, sizeof chunk_have);
        backup_going = out->backup;
        if (backup_going)
            send_chunks();
        set_status("Player %d joined from %s.", 2 - h, text);
        return 0;
    }
    SDL_LockMutex(lock);
    waiting = 0;
    SDL_UnlockMutex(lock);
    return fail("Hosting was cancelled.");
}

static int guest_join(const session_start *mine_s, session_start *out)
{
    char text[96];
    udp_text(&peer, text, sizeof text);
    SDL_strlcpy(peer_text, text, sizeof peer_text);
    set_status("Joining %s: waiting for its answer.", text);
    say("joining %s; build %s", text, BUILD_ID);
    if (!code_typed[0])
        return fail("No join code: type the code the host's game shows (the launcher's Join page asks for it).");
    /* the code's exchange (section 50): this try's number and point; the host's point gives the keys */
    unsigned char y[32], g[32], Ya[SECURE_POINT], try_sid[SECURE_SID];
    if (secure_random(y, sizeof y) || secure_random(try_sid, sizeof try_sid))
        return fail("This PC has no random numbers for the code's exchange.");
    cpace_generator(g, code_typed, SECURE_CODE_DIGITS, channel, sizeof channel - 1, try_sid, SECURE_SID);
    cpace_point(Ya, y, g);
    secure_wipe(g, sizeof g);
    unsigned char hello[HDR + BUILD_MAX + PLAYER_BYTES], proof[HDR + PLAYER_BYTES + SECURE_OVER], b[2048];
    sid = 0;
    int n = header(hello, P_HELLO), proof_n = 0;
    memset(hello + n, 0, sizeof hello - (size_t)n);
    SDL_strlcpy((char *)hello + n, BUILD_ID, BUILD_MAX);
    memcpy(hello + n + BUILD_MAX, try_sid, SECURE_SID);
    memcpy(hello + n + BUILD_MAX + SECURE_SID, Ya, SECURE_POINT);
    n = (int)sizeof hello;          /* zeros after the point: a build before section 50 reads another build's HELLO */
    Uint64 t0 = SDL_GetTicks(), next = 0;
    int welcomed = 0, backup = 0;
    while (!SDL_GetAtomicInt(&cancelled)) {
        Uint64 now = SDL_GetTicks();
        if (!welcomed && now >= next) {
            next = now + 250;
            if (proof_n)
                udp_send(sock, &peer, proof, (size_t)proof_n);     /* the same PROOF until the host answers */
            else
                udp_send(sock, &peer, hello, (size_t)n);
        }
        if (!welcomed && now - t0 > HELLO_MS) {
            secure_wipe(y, sizeof y);
            if (proof_n)
                return fail("%s answered, but did not take this PC's code or say why.", text);
            return fail("No answer from %s. Is the game hosting there, and can this PC reach it (the same network, "
                        "a firewall that lets the game in)?", text);
        }
        udp_addr from;
        int r = udp_recv(sock, b, sizeof b, &from, 20);
        if (r < 0)
            return fail("The network failed: %s", udp_error());
        if (r <= 0 || !udp_same(&from, &peer))
            continue;
        int k = clear_kind(b, r);
        if (k == P_REFUSE && !welcomed) {   /* in the clear: before the keys (another build, the code not right) */
            char why[240];
            refuse_text(why, sizeof why, b, r);
            secure_wipe(y, sizeof y);
            return fail("%s", why);
        }
        if (k == P_CHALLENGE && !proof_n && r >= HDR + SECURE_POINT) {
            unsigned char isk[SECURE_ISK];
            const unsigned char *Yb = b + HDR;
            int bad = cpace_key(isk, y, Yb, try_sid, SECURE_SID, Ya, ad_joining, sizeof ad_joining - 1, Yb, ad_host,
                                sizeof ad_host - 1);
            secure_wipe(y, sizeof y);
            if (bad)
                return fail("%s answered with a point that is not right.", text);
            SDL_LockMutex(tx_lock);
            secure_keys(isk, 0, &tx, &rx);
            keyed = 1;
            SDL_UnlockMutex(tx_lock);
            secure_wipe(isk, sizeof isk);
            /* PROOF: this player's card, sealed; the host opens it only with the same code */
            int m = header(proof, P_PROOF | SEALED);
            m += put_player(proof + m, &mine_s->own);
            SDL_LockMutex(tx_lock);
            proof_n = (int)secure_seal(&tx, proof, HDR, (size_t)m);
            SDL_UnlockMutex(tx_lock);
            udp_send(sock, &peer, proof, (size_t)proof_n);
            next = now + 250;
            say("the host answered: the code's exchange; this PC's code goes to it as a proof");
            continue;
        }
        if (!proof_n)
            continue;
        r = unseal(b, r, &rx);              /* from here on, sealed with the keys only */
        if (r < 0)
            continue;
        k = b[5];
        if (k == P_REFUSE && !welcomed) {
            char why[240];
            refuse_text(why, sizeof why, b, r);
            return fail("%s", why);
        }
        if (k == P_WELCOME && !welcomed && r >= HDR + 7 + PLAYER_BYTES) {
            const unsigned char *p = b + HDR;
            sid = get32(b + 6);
            *out = *mine_s;
            out->coins = p[0];
            out->region = p[1];
            out->network = p[2];
            out->cards = p[3];
            out->delay = p[4] ? p[4] : SESSION_DELAY;
            out->backup = p[5];
            int h = p[6] == 1;              /* the host's player */
            get_player(p + 7, &out->player[h]);
            out->player[!h] = mine_s->own;
            out->side = !h;
            welcomed = 1;
            backup = out->backup;
            memset(chunk_have, 0, sizeof chunk_have);
            say("welcomed by %s as player %d: coins %d, region %d, network %d, cards %d, delay %d, %s", text, 2 - h,
                out->coins, out->region, out->network, out->cards, out->delay,
                backup ? "its backup follows" : "a blank cabinet");
            set_status("Joined %s: getting its cabinet.", text);
            if (!backup)
                return 0;
            continue;
        }
        /* a game going on: its start as it was, which this PC replays to the host's reference */
        if (k == P_LATE && !welcomed && r >= HDR + LATE_BYTES) {
            const unsigned char *p = b + HDR;
            sid = get32(b + 6);
            *out = *mine_s;
            out->coins = p[0];
            out->region = p[1];
            out->network = p[2];
            out->cards = p[3];
            out->delay = p[4] ? p[4] : SESSION_DELAY;
            out->backup = p[5];
            int h = p[6] == 1;
            SDL_LockMutex(lock);
            ref_frame = get32(p + 7);
            ref_size = get32(p + 11);
            ref_hash = get64(p + 15);
            late = 1;
            SDL_UnlockMutex(lock);
            get_player(p + 23, &out->player[0]);
            get_player(p + 23 + PLAYER_BYTES, &out->player[1]);
            out->side = !h;
            welcomed = 1;
            backup = out->backup;
            memset(chunk_have, 0, sizeof chunk_have);
            say("joining the game going on at %s as player %d: its start was coins %d, region %d, network %d, "
                "cards %d, delay %d, %s; its reference is the end of vertical blank %u (%u bytes)", text, 2 - h,
                out->coins, out->region, out->network, out->cards, out->delay,
                backup ? "a backup (it follows)" : "a blank cabinet", ref_frame, ref_size);
            set_status("Joined %s, a game going on: getting its cabinet.", text);
            if (!backup)
                return 0;
            continue;
        }
        if (k == P_CHUNK && welcomed && get32(b + 6) == sid && r >= HDR + 2) {
            int i = get16(b + HDR);
            int len = i == NCHUNKS - 1 ? GAME_BACKUP_SIZE - i * CHUNK : CHUNK;
            if (i < NCHUNKS && r - HDR - 2 == len) {
                memcpy(out->backup_bytes + i * CHUNK, b + HDR + 2, (size_t)len);
                chunk_have[i / 8] |= (unsigned char)(1u << (i % 8));
            }
            send_chunk_ack(&peer);
            if (all_chunks()) {
                say("the host's backup has come (%d bytes)", GAME_BACKUP_SIZE);
                return 0;
            }
        }
    }
    secure_wipe(y, sizeof y);
    return fail("Joining was cancelled.");
}

int session_connect(const session_start *mine_s, session_start *out, unsigned long long (*hash)(unsigned frame))
{
    if (!lock) {
        lock = SDL_CreateMutex();
        cond = SDL_CreateCondition();
        tx_lock = SDL_CreateMutex();
    }
    hash_fn = hash;
    SDL_LockMutex(lock);
    play_reset();
    phase = PH_PLAY;
    late = 0;
    keys_forget();
    try_drop();
    SDL_UnlockMutex(lock);
    if (udp_init())
        return fail("The network cannot start: %s", udp_error());
    int r;
    if (role == SESSION_HOST) {
        sock = udp_open(cfg_port, NULL);
        if (sock == UDP_NONE)
            return fail("Cannot host: %s. Another program may be using the port.", udp_error());
        r = host_wait(mine_s, out);
    } else {
        if (udp_resolve(cfg_addr, UDP_PORT, &peer))
            return fail("Cannot join \"%s\": %s", cfg_addr, udp_error());
        sock = udp_open(0, &peer);
        if (sock == UDP_NONE)
            return fail("Cannot open the network: %s", udp_error());
        r = guest_join(mine_s, out);
    }
    if (r)
        return r;
    side = out->side;
    delay = out->delay;
    SDL_LockMutex(lock);
    heard_ms = SDL_GetTicks();
    status[0] = 0;
    if (late) {
        phase = PH_BOOT;
        late_status();
    }
    SDL_UnlockMutex(lock);
    SDL_SetAtomicInt(&quit_thread, 0);
    thread = SDL_CreateThread(session_thread, "session", NULL);
    if (!thread)
        return fail("Cannot start the network thread: %s", SDL_GetError());
    say("playing as player %d, input delay %d vertical blanks; receive buffer %d bytes%s", side + 1, delay,
        udp_room(sock), late ? "; first this PC starts the game as the session began, then it takes the host's game"
                             : "");
    return 0;
}

int session_frame(unsigned frame, int booting, unsigned bits, int coin, game_input *in)
{
    memset(in, 0, sizeof *in);
    SDL_LockMutex(lock);
    if (late) {                     /* the joining PC's copy of the session's start: no inputs, no fingerprints */
        boot_frame = frame;
        if (left && !ended) {
            char why[120];
            SDL_snprintf(why, sizeof why, "Player %d left the game.", 2 - side);
            end_locked(why);
        }
        int e = ended || SDL_GetAtomicInt(&cancelled);
        SDL_UnlockMutex(lock);
        return e ? -1 : 0;
    }
    SDL_UnlockMutex(lock);
    if (frame % SESSION_CHECK == 0) {
        Uint64 h = hash_fn(frame);
        SDL_LockMutex(lock);
        int i = (int)(frame / SESSION_CHECK % HRING);
        my_hash[i].frame = frame;
        my_hash[i].h = h;
        compare(frame);
        SDL_UnlockMutex(lock);
    }
    SDL_LockMutex(lock);
    if (ended) {
        SDL_UnlockMutex(lock);
        return -1;
    }
    if (booting && !started) {
        SDL_UnlockMutex(lock);
        return 0;                   /* the start-up: no inputs on either PC */
    }
    if (!started) {
        started = 1;
        mine_first = frame + (unsigned)delay;
        theirs_full = mine_first - 1;
        say("vertical blank %u: the game's start-up has ended; inputs from vertical blank %u", frame, mine_first);
        advance_full();
    }
    unsigned at = frame + (unsigned)delay;
    mine[at % RING].frame = at;
    mine[at % RING].v = (Uint16)((bits & 0x1ff) | (coin ? 0x200 : 0));
    mine_newest = at;
    SDL_UnlockMutex(lock);
    send_input();
    SDL_LockMutex(lock);
    if (frame < mine_first) {
        SDL_UnlockMutex(lock);
        return 0;                   /* the first `delay` vertical blanks: none pressed yet */
    }
    Uint64 t0 = SDL_GetTicks();
    int said = 0;
    while (theirs_full < frame && !ended && !left && !SDL_GetAtomicInt(&cancelled)) {
        SDL_WaitConditionTimeout(cond, lock, 5);
        Uint64 now = SDL_GetTicks();
        if (now - t0 > WAIT_SAY_MS && !ended) {
            if (phase == PH_SEND)
                late_status();      /* the host: the joining PC is still getting this game */
            else if (their_pause)
                SDL_snprintf(status, sizeof status, "Player %d has the game's menu open.", 2 - side);
            else
                SDL_snprintf(status, sizeof status, "Waiting for player %d's PC.", 2 - side);
            if (!said++)
                say("vertical blank %u: waiting for player %d (%s)", frame, 2 - side,
                    phase == PH_SEND ? "it is getting this game" : their_pause ? "their menu is open"
                                                                                : "nothing has come yet");
        }
    }
    if (!ended && left && theirs_full < frame) {
        char why[120];
        SDL_snprintf(why, sizeof why, "Player %d left the game.", 2 - side);
        end_locked(why);
    }
    if (said && !ended) {
        status[0] = 0;
        say("vertical blank %u: player %d's inputs came after %llu ms", frame, 2 - side,
            (unsigned long long)(SDL_GetTicks() - t0));
    }
    if (ended || SDL_GetAtomicInt(&cancelled)) {
        SDL_UnlockMutex(lock);
        return -1;
    }
    Uint16 a = mine[frame % RING].v, o = theirs[frame % RING].v;
    in->player[side] = a & 0x1ff;
    in->coin[side] = a >> 9 & 1;
    in->player[!side] = o & 0x1ff;
    in->coin[!side] = o >> 9 & 1;
    SDL_UnlockMutex(lock);
    return 0;
}

unsigned session_verified(void)
{
    SDL_LockMutex(lock);
    unsigned v = verified;
    SDL_UnlockMutex(lock);
    return v;
}

void session_paused(int on)
{
    SDL_SetAtomicInt(&paused_here, on);
}

void session_cancel(void)
{
    SDL_SetAtomicInt(&cancelled, 1);
    if (lock) {
        SDL_LockMutex(lock);
        SDL_BroadcastCondition(cond);
        SDL_UnlockMutex(lock);
    }
}

int session_status(char *buf, size_t n)
{
    if (!lock) {
        buf[0] = 0;
        return 1;
    }
    SDL_LockMutex(lock);
    SDL_strlcpy(buf, status, n);
    int e = ended;
    SDL_UnlockMutex(lock);
    return e;
}

void session_end(void)
{
    if (thread) {
        SDL_SetAtomicInt(&quit_thread, 1);
        SDL_WaitThread(thread, NULL);
        thread = NULL;
        say("%u fingerprints agreed with the other PC's, the newest at vertical blank %u", agreed, verified);
    }
    if (sock != UDP_NONE) {
        if (sid)
            send_bye();
        udp_close(sock);
        sock = UDP_NONE;
        udp_quit();
    }
    if (lock) {
        SDL_LockMutex(lock);
        patch_free();
        phase = PH_PLAY;
        keys_forget();
        try_drop();
        code[0] = 0;
        SDL_UnlockMutex(lock);
    }
}

/* ---- a PC joining a game going on ---- */

int session_open(unsigned r_frame, size_t r_size, unsigned long long r_hash)
{
    if (role != SESSION_HOST || !thread || sock == UDP_NONE || r_size >= 0xffffffffu)
        return -1;
    SDL_LockMutex(lock);
    if (thread_gone) {
        SDL_UnlockMutex(lock);
        return -1;
    }
    send_bye();                     /* the other PC (gone, or its game stopped) */
    say("%u fingerprints agreed with the other PC's, the newest at vertical blank %u", agreed, verified);
    keys_forget();
    where_text(open_where, sizeof open_where, "");
    ref_frame = r_frame;
    ref_size = (Uint32)r_size;
    ref_hash = r_hash;
    play_reset();
    phase = PH_OPEN;
    memset(&peer, 0, sizeof peer);
    sid = 0;
    welcome_len = 0;
    backup_going = 0;
    card_wait = 0;
    patch_free();
    say("open: another PC can join this game going on, as player %d", 2 - side);
    code_new();                     /* a fresh code after each player leaves */
    SDL_UnlockMutex(lock);
    return 0;
}

int session_late_ready(session_player *p, int *same)
{
    SDL_LockMutex(lock);
    int r = role == SESSION_HOST && phase == PH_READY;
    if (r) {
        *p = late_player;
        *same = their_frame == ref_frame && their_size == ref_size && their_hash_ref == ref_hash;
        phase = PH_PICKED;
    }
    SDL_UnlockMutex(lock);
    return r;
}

int session_late_still(int wait)
{
    SDL_LockMutex(lock);
    int r = phase == PH_PICKED;
    if (r && card_wait != (wait != 0)) {
        card_wait = wait != 0;
        say(card_wait ? "player %d's card unit holds the card of the player who left, in use: the join waits for "
                        "the game on that side to end"
                      : "the game on player %d's side is over: the join goes on", 2 - side);
        late_status();
    }
    SDL_UnlockMutex(lock);
    return r;
}

int session_late_send(unsigned char *patch, size_t n, unsigned frame, int card_in)
{
    SDL_LockMutex(lock);
    if (phase != PH_PICKED || !n || n > PATCH_MAX) {
        SDL_UnlockMutex(lock);
        free(patch);
        return -1;
    }
    patch_free();
    patch_buf = patch;
    patch_n = n;
    patch_count = (unsigned)((n + PCHUNK - 1) / PCHUNK);
    patch_bits = calloc((patch_count + 7) / 8, 1);
    patch_sent = calloc(patch_count, sizeof *patch_sent);
    if (!patch_bits || !patch_sent) {
        send_refuse_to(&peer, "The host has too little memory to send its game.", 1);
        late_drop("This PC has too little memory to send its game.");
        SDL_UnlockMutex(lock);
        return -1;
    }
    patch_cum = patch_next = patch_resent = 0;
    patch_frame = frame;
    patch_card = card_in;
    patch_t0 = SDL_GetTicks();
    /* the session runs again from the next vertical blank */
    play_reset();
    started = 1;
    mine_first = frame + 1 + (unsigned)delay;
    theirs_full = mine_first - 1;
    backup_going = 0;
    card_wait = 0;
    phase = PH_SEND;
    late_status();
    say("this game at the end of vertical blank %u goes to player %d's PC: a difference of %zu bytes; the session "
        "runs again from vertical blank %u, inputs from %u", frame, 2 - side, n, frame + 1, mine_first);
    SDL_UnlockMutex(lock);
    return 0;
}

void session_late_cancel(const char *why)
{
    SDL_LockMutex(lock);
    if (phase == PH_READY || phase == PH_PICKED || phase == PH_BOOT) {
        send_refuse_to(&peer, why, 1);
        late_drop(why);
    }
    SDL_UnlockMutex(lock);
}

int session_late(unsigned *rf)
{
    if (!lock)
        return 0;
    SDL_LockMutex(lock);
    int r = late;
    if (rf)
        *rf = ref_frame;
    SDL_UnlockMutex(lock);
    return r;
}

int session_late_get(unsigned r_frame, size_t r_size, unsigned long long r_hash, unsigned char **patch, size_t *n,
                     unsigned *frame, int *card_in)
{
    SDL_LockMutex(lock);
    their_frame = r_frame;
    their_size = (Uint32)r_size;
    their_hash_ref = r_hash;
    if (phase == PH_BOOT) {
        phase = PH_READY;
        say("this PC's copy is at the end of vertical blank %u: %s", r_frame,
            r_frame == ref_frame && r_size == ref_size && r_hash == ref_hash ? "the same as the host's reference"
                                                                            : "NOT the same as the host's reference");
        late_status();
    }
    while (!patch_done && !ended && !left && !SDL_GetAtomicInt(&cancelled))
        SDL_WaitConditionTimeout(cond, lock, 20);
    if (patch_done && !ended) {
        *patch = patch_buf;
        *n = patch_n;
        *frame = patch_frame;
        *card_in = patch_card;
        patch_buf = NULL;
        late = 0;
        SDL_UnlockMutex(lock);
        return 0;
    }
    if (!ended) {
        char why[120];
        if (left)
            SDL_snprintf(why, sizeof why, "Player %d left the game.", 2 - side);
        else
            SDL_strlcpy(why, "Joining was cancelled.", sizeof why);
        end_locked(why);
    }
    SDL_UnlockMutex(lock);
    return -1;
}

void session_late_failed(const char *why)
{
    SDL_LockMutex(lock);
    late = 0;
    end_locked(why);
    if (sock != UDP_NONE && sid)
        send_bye();
    SDL_UnlockMutex(lock);
}

int session_joining(void)
{
    if (!lock)
        return 0;
    SDL_LockMutex(lock);
    int r = !ended && (phase == PH_BOOT || phase == PH_READY || phase == PH_PICKED || phase == PH_SEND || late);
    SDL_UnlockMutex(lock);
    return r;
}

int session_can_join(char *where, size_t n)
{
    if (n)
        where[0] = 0;
    if (!lock)
        return 0;
    SDL_LockMutex(lock);
    int r = role == SESSION_HOST && phase == PH_OPEN && !thread_gone;
    if (r && n)
        SDL_strlcpy(where, open_where, n);
    SDL_UnlockMutex(lock);
    return r;
}

/* ---- the join code (section 50) ---- */

int session_code_typed(const char *typed)
{
    return secure_code_from(typed ? typed : "", code_typed);
}

int session_code(char *buf, size_t n)
{
    if (n)
        buf[0] = 0;
    if (!lock || role != SESSION_HOST)
        return SESSION_CODE_NONE;
    SDL_LockMutex(lock);
    int open = waiting || (phase == PH_OPEN && thread && !thread_gone);
    int r = !open ? SESSION_CODE_NONE : code[0] ? SESSION_CODE_SHOWN : SESSION_CODE_GONE;
    if (r == SESSION_CODE_SHOWN && n)
        SDL_snprintf(buf, n, "%.3s %s", code, code + 3);
    SDL_UnlockMutex(lock);
    return r;
}

void session_code_new(void)
{
    if (!lock || role != SESSION_HOST)
        return;
    SDL_LockMutex(lock);
    if (waiting || (phase == PH_OPEN && thread && !thread_gone)) {
        say("a new join code, asked for in the menu");
        code_new();
        if (!waiting)
            SDL_strlcpy(status, "A new join code.", sizeof status);
    }
    SDL_UnlockMutex(lock);
}
