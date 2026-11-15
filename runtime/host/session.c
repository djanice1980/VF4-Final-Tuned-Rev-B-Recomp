/* Two players on two PCs: see session.h and docs/design.md section 46.
 *
 * Every datagram starts with "VF4N", the protocol's version, its kind and
 * the session's number (0 in a HELLO):
 *   HELLO    joining -> host, every 250 ms until answered: the build, the
 *            joining player's card
 *   REFUSE   host -> joining: why (another build, a game going on)
 *   WELCOME  host -> joining: the session's number, the cabinet's settings,
 *            the input delay, the host player's card, whether a backup
 *            follows
 *   CHUNK    host -> joining: 1,024 bytes of the backup, sent again until
 *            all are acknowledged
 *   CHUNKS   joining -> host: which chunks have come (a bit each)
 *   INPUT    both ways, every vertical blank and every 10 ms: the inputs
 *            the other PC has not acknowledged (oldest first, up to 64),
 *            the newest of the other's this PC has with none missing, the
 *            menu's pause, and this PC's newest fingerprints
 *   BYE      leaving
 * Numbers are big-endian. */
#include "session.h"

#include <SDL3/SDL.h>
#include <string.h>

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
enum { P_HELLO = 1, P_REFUSE, P_WELCOME, P_CHUNK, P_CHUNKS, P_INPUT, P_BYE };
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
static unsigned char hello_card_pkt[512];   /* the host's WELCOME, sent again for a repeated HELLO */
static int welcome_len;

/* ---- bytes ---- */
static void put32(unsigned char *p, Uint32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (unsigned char)v; }
static Uint32 get32(const unsigned char *p) { return (Uint32)p[0] << 24 | (Uint32)p[1] << 16 | (Uint32)p[2] << 8 | p[3]; }
static void put16(unsigned char *p, Uint16 v) { p[0] = v >> 8; p[1] = (unsigned char)v; }
static Uint16 get16(const unsigned char *p) { return (Uint16)(p[0] << 8 | p[1]); }

static int header(unsigned char *p, int kind)
{
    memcpy(p, "VF4N", 4);
    p[4] = PROTO;
    p[5] = (unsigned char)kind;
    put32(p + 6, sid);
    return HDR;
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

static void get_player(const unsigned char *p, session_player *pl)
{
    pl->card = p[0] <= SESSION_CARD_BYTES ? p[0] : SESSION_CARD_NONE;
    memcpy(pl->bytes, p + 1, GAME_CARD_SIZE);
    memcpy(pl->name, p + 1 + GAME_CARD_SIZE, CARD_NAME_MAX);
    pl->name[CARD_NAME_MAX - 1] = 0;
    if (pl->card != SESSION_CARD_BYTES)
        memset(pl->bytes, 0, GAME_CARD_SIZE);
}

static void (*log_fn)(const char *line);

void session_logger(void (*fn)(const char *line))
{
    log_fn = fn;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char line[400];
    SDL_strlcpy(line, "session: ", sizeof line);
    SDL_vsnprintf(line + 9, sizeof line - 9, fmt, ap);
    va_end(ap);
    if (log_fn)
        log_fn(line);
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

int session_role(void)
{
    return role;
}

/* ---- inputs and fingerprints (under lock) ---- */

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
        SDL_snprintf(why, sizeof why, "The two games went apart at vertical blank %u, so the game stopped. "
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
        their_hash[k].h = (Uint64)get32(p + 4) << 32 | get32(p + 8);
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
        put32(b + n + 4, (Uint32)(my_hash[i].h >> 32));
        put32(b + n + 8, (Uint32)my_hash[i].h);
        n += 12;
        (*nh)++;
        if (newest < SESSION_CHECK)
            break;
        newest -= SESSION_CHECK;
    }
    return n;
}

static void send_input(void)
{
    unsigned char b[HDR + 5 + 2 * SEND_MAX + 6 + 12 * HASH_SEND];
    SDL_LockMutex(lock);
    int n = make_input(b);
    SDL_UnlockMutex(lock);
    udp_send(sock, &peer, b, (size_t)n);
}

static void send_bye(void)
{
    unsigned char b[HDR];
    header(b, P_BYE);
    for (int i = 0; i < 3; i++)
        udp_send(sock, &peer, b, sizeof b);
}

static void send_refuse(const udp_addr *to, const char *why)
{
    unsigned char b[HDR + 200];
    int n = header(b, P_REFUSE);
    put32(b + 6, 0);                /* to a PC outside the session */
    size_t l = SDL_strlen(why);
    if (l > 199)
        l = 199;
    memcpy(b + n, why, l);
    udp_send(sock, to, b, (size_t)n + l);
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
        udp_send(sock, &peer, b, (size_t)(n + 2 + len));
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
    udp_send(sock, to, b, (size_t)n + sizeof chunk_have);
}

/* a datagram's header: its kind, or -1 when it is not ours or not this session's */
static int kind_of(const unsigned char *b, int n, int any_session)
{
    if (n < HDR || memcmp(b, "VF4N", 4) || b[4] != PROTO)
        return -1;
    if (!any_session && get32(b + 6) != sid)
        return -1;
    return b[5];
}

/* ---- the running session's own thread: receives, and sends every 10 ms ---- */

static int SDLCALL session_thread(void *arg)
{
    (void)arg;
    unsigned char b[2048];
    Uint64 next_send = 0, next_chunks = 0;
    while (!SDL_GetAtomicInt(&quit_thread)) {
        udp_addr from;
        int n = udp_recv(sock, b, sizeof b, &from, 4);
        Uint64 now = SDL_GetTicks();
        if (n > 0 && !udp_same(&from, &peer) && role == SESSION_HOST && kind_of(b, n, 1) == P_HELLO) {
            send_refuse(&from, "A game is going on there already.");
        } else if (n > 0 && udp_same(&from, &peer)) {
            int k = kind_of(b, n, 1);
            if (k == P_HELLO && role == SESSION_HOST && welcome_len) {
                udp_send(sock, &peer, hello_card_pkt, (size_t)welcome_len);     /* our WELCOME was lost */
            } else if (kind_of(b, n, 0) == k && k > 0) {
                SDL_LockMutex(lock);
                heard_ms = now;
                if (k == P_INPUT) {
                    take_input(b + HDR, n - HDR);
                    if (role == SESSION_HOST && !guest_running) {
                        guest_running = 1;
                        say("player 2's machine has started");
                    }
                } else if (k == P_BYE) {
                    if (!left)
                        say("player %d's PC says it leaves", 2 - side);
                    left = 1;
                    SDL_BroadcastCondition(cond);
                } else if (k == P_CHUNKS && role == SESSION_HOST && n >= HDR + (int)sizeof chunk_have) {
                    for (size_t i = 0; i < sizeof chunk_have; i++)
                        chunk_have[i] |= b[HDR + i];
                } else if (k == P_CHUNK && role == SESSION_GUEST) {
                    send_chunk_ack(&peer);      /* all came before the machine started: our last answer was lost */
                }
                SDL_UnlockMutex(lock);
            }
        } else if (n < 0) {
            SDL_LockMutex(lock);
            char why[300];
            SDL_snprintf(why, sizeof why, "The network failed: %s", udp_error());
            end_locked(why);
            SDL_UnlockMutex(lock);
            break;
        }
        if (now >= next_send) {
            next_send = now + 10;
            send_input();
        }
        if (role == SESSION_HOST && backup_going && now >= next_chunks) {
            next_chunks = now + 30;
            SDL_LockMutex(lock);
            int going = !guest_running && !all_chunks();
            SDL_UnlockMutex(lock);
            if (going)
                send_chunks();
            else
                backup_going = 0;
        }
        SDL_LockMutex(lock);
        if (!ended && now - heard_ms > QUIET_MS)
            end_locked("The other PC stopped answering, so the game stopped.");
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

static int host_wait(const session_start *mine_s, session_start *out)
{
    char ip[64], text[96];
    udp_local_ip(ip, sizeof ip);
    set_status("Hosting: waiting for player 2. On the other PC, join %s (port %d).",
               ip[0] ? ip : "this PC's address", cfg_port);
    say("hosting on port %d; this PC's address %s; build %s", cfg_port, ip[0] ? ip : "(no network)", BUILD_ID);
    unsigned char b[2048];
    while (!SDL_GetAtomicInt(&cancelled)) {
        udp_addr from;
        int n = udp_recv(sock, b, sizeof b, &from, 50);
        if (n < 0)
            return fail("The network failed: %s", udp_error());
        if (n <= 0 || kind_of(b, n, 1) != P_HELLO || n < HDR + BUILD_MAX + PLAYER_BYTES)
            continue;
        char build[BUILD_MAX + 1];
        memcpy(build, b + HDR, BUILD_MAX);
        build[BUILD_MAX] = 0;
        udp_text(&from, text, sizeof text);
        if (strcmp(build, BUILD_ID)) {
            char why[240];
            SDL_snprintf(why, sizeof why, "The host runs another build of the game: %s. Both PCs need the same.",
                         BUILD_ID);
            say("refused %s: it runs %s", text, build);
            send_refuse(&from, why);
            continue;
        }
        *out = *mine_s;
        get_player(b + HDR + BUILD_MAX, &out->player[1]);
        out->side = 0;
        peer = from;
        sid = (Uint32)(SDL_GetPerformanceCounter() ^ (SDL_GetTicksNS() << 7)) | 1;
        /* WELCOME */
        unsigned char *w = hello_card_pkt;
        int m = header(w, P_WELCOME);
        w[m++] = (unsigned char)out->coins;
        w[m++] = (unsigned char)out->region;
        w[m++] = (unsigned char)out->network;
        w[m++] = (unsigned char)out->cards;
        w[m++] = (unsigned char)out->delay;
        w[m++] = (unsigned char)out->backup;
        m += put_player(w + m, &out->player[0]);
        welcome_len = m;
        udp_send(sock, &peer, w, (size_t)m);
        say("player 2 joined from %s (card %s)", text,
            out->player[1].card == SESSION_CARD_NONE ? "none" : out->player[1].name);
        backup_src = out->backup_bytes;
        memset(chunk_have, 0, sizeof chunk_have);
        backup_going = out->backup;
        if (backup_going)
            send_chunks();
        set_status("Player 2 joined from %s.", text);
        return 0;
    }
    return fail("Hosting was cancelled.");
}

static int guest_join(const session_start *mine_s, session_start *out)
{
    char text[96];
    udp_text(&peer, text, sizeof text);
    set_status("Joining %s: waiting for its answer.", text);
    say("joining %s; build %s", text, BUILD_ID);
    unsigned char hello[HDR + BUILD_MAX + PLAYER_BYTES], b[2048];
    sid = 0;
    int n = header(hello, P_HELLO);
    memset(hello + n, 0, BUILD_MAX);
    SDL_strlcpy((char *)hello + n, BUILD_ID, BUILD_MAX);
    n += BUILD_MAX;
    n += put_player(hello + n, &mine_s->player[1]);
    Uint64 t0 = SDL_GetTicks(), next = 0;
    int welcomed = 0, backup = 0;
    while (!SDL_GetAtomicInt(&cancelled)) {
        Uint64 now = SDL_GetTicks();
        if (!welcomed && now >= next) {
            next = now + 250;
            udp_send(sock, &peer, hello, (size_t)n);
        }
        if (!welcomed && now - t0 > HELLO_MS)
            return fail("No answer from %s. Is the game hosting there, and can this PC reach it (the same network, "
                        "a firewall that lets the game in)?", text);
        udp_addr from;
        int r = udp_recv(sock, b, sizeof b, &from, 20);
        if (r < 0)
            return fail("The network failed: %s", udp_error());
        if (r <= 0 || !udp_same(&from, &peer))
            continue;
        int k = kind_of(b, r, 1);
        if (k == P_REFUSE) {
            char why[201];
            int l = r - HDR > 200 ? 200 : r - HDR;
            memcpy(why, b + HDR, (size_t)l);
            why[l] = 0;
            return fail("The host said no: %s", why);
        }
        if (k == P_WELCOME && !welcomed && r >= HDR + 6 + PLAYER_BYTES) {
            const unsigned char *p = b + HDR;
            sid = get32(b + 6);
            *out = *mine_s;
            out->coins = p[0];
            out->region = p[1];
            out->network = p[2];
            out->cards = p[3];
            out->delay = p[4] ? p[4] : SESSION_DELAY;
            out->backup = p[5];
            get_player(p + 6, &out->player[0]);
            out->side = 1;
            welcomed = 1;
            backup = out->backup;
            memset(chunk_have, 0, sizeof chunk_have);
            say("welcomed by %s: coins %d, region %d, network %d, cards %d, delay %d, %s", text, out->coins,
                out->region, out->network, out->cards, out->delay, backup ? "its backup follows" : "a blank cabinet");
            set_status("Joined %s: getting its cabinet.", text);
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
    return fail("Joining was cancelled.");
}

int session_connect(const session_start *mine_s, session_start *out, unsigned long long (*hash)(unsigned frame))
{
    if (!lock) {
        lock = SDL_CreateMutex();
        cond = SDL_CreateCondition();
    }
    hash_fn = hash;
    for (int i = 0; i < RING; i++)
        mine[i].frame = theirs[i].frame = ~0u;
    for (int i = 0; i < HRING; i++)
        my_hash[i].frame = their_hash[i].frame = ~0u;
    ended = 0;
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
    SDL_UnlockMutex(lock);
    SDL_SetAtomicInt(&quit_thread, 0);
    thread = SDL_CreateThread(session_thread, "session", NULL);
    if (!thread)
        return fail("Cannot start the network thread: %s", SDL_GetError());
    say("playing as player %d, input delay %d vertical blanks", side + 1, delay);
    return 0;
}

int session_frame(unsigned frame, int booting, unsigned bits, int coin, game_input *in)
{
    memset(in, 0, sizeof *in);
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
            if (their_pause)
                SDL_snprintf(status, sizeof status, "Player %d has the game's menu open.", 2 - side);
            else
                SDL_snprintf(status, sizeof status, "Waiting for player %d's PC.", 2 - side);
            if (!said++)
                say("vertical blank %u: waiting for player %d (%s)", frame, 2 - side,
                    their_pause ? "their menu is open" : "nothing has come yet");
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
}
