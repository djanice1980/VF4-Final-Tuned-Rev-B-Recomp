/* The two card reader/writer units, one per player, on Maple ports 1 and 2.
 *
 * Why a model: the game's card library (vf4lib/libcard) never touches the
 * hardware; it queues requests with sdk/libam/nlag1.c, and card_unit_step
 * sends them as Maple frames, one unit per vertical blank. With the frames
 * answered, the whole chain runs as the game's own bytes: card_units_start
 * finds the units, card_unit_step polls them, libcard reads the card, checks
 * it, plays with what it holds and writes the results back.
 *
 * Everything here comes from what the decompiled game does with the units
 * (docs/hle.md, "The card units", names the routine behind each line):
 *   - the start-up exchange is card_units_start's (sdk/libam/nlam.c) with
 *     maple_unit_read_ident and maple_unit_upload_firmware (maple.c);
 *   - the requests, their follow-up polls and what is read out of a reply
 *     are card_unit_step, card_request_payload, card_reply_bad,
 *     card_reply_status and card_reply_store (nlag1.c);
 *   - which requests make a read and a write, and which words of the card
 *     they touch, are card_read_start, card_write_start, card_lock_start
 *     and card_unlock_start (vf4lib/libcard/src/card_common.c);
 *   - what a card must hold to be taken (and what a card looks like before
 *     its first use) are card_header_check (card_common.c) and
 *     card_raw_is_blank (card_mode.c).
 * What a request does inside the unit is not in the game, so the model does
 * the least that makes the game's own sequences mean something: a card is
 * 32 words; request 0xa0 takes the card's words into the unit's buffer,
 * 0xa1 hands the buffer over, 0xb1 puts words on the card, 0xd9 and 0xda
 * set and clear the lock the game shows in status bit 6. The key the game
 * sends with 0xd4 is kept and shown, not checked.
 *
 * The card's contents live in a host buffer per unit; the controller's
 * "card" command (README.md) puts a card in (a file of 128 bytes, or a
 * card as it is before its first use), takes it out and writes the buffer
 * to a file. Nothing of a card is kept in this repository.
 */
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "observe.h"
#include "tables.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CARD_BYTES 0x80         /* 32 words: card_reply_store copies that many from a read */
#define DATA_FIRST 5            /* the first word the game writes (card_write_start: blocks 5 to 31) */

/* status byte of every reply (card_reply_status reads it at reply + 4) */
#define ST_NO_CARD 0x01         /* bit 0: the game counts replies without it before it believes in a card */
#define ST_LOCKED  0x40         /* bit 6: the game's "lock is on" byte */
/* bits 1 to 5 end a request with the game's result codes 4 to 8 */

/* reply codes: the game only refuses 0xfc and 0xfd (card_reply_bad); 0xfe is
 * what the decompilation's logged run shows a unit answering its idle poll with */
#define REPLY_STATUS 0xfe
#define REPLY_REFUSED 0xfc

/* polls the unit leaves unanswered after a request (the game takes "no
 * answer" to a follow-up poll as "still working", up to 400 times) */
enum { BUSY_NONE = 0, BUSY_SHORT = 1, BUSY_WRITE = 2 };

typedef struct card_unit {
    u8 present;                 /* a card is in the unit */
    u8 locked;                  /* the lock is on (0xd9) */
    u8 eject_wanted;            /* the script took the card while the lock was on: it leaves when the lock opens */
    u8 started;                 /* the start-up exchange got as far as the program upload */
    u8 card[CARD_BYTES];        /* the card */
    u8 buffer[CARD_BYTES];      /* the unit's read buffer: the card as request 0xa0 found it */
    u8 key[4];                  /* the last key the game sent (0xd4) */
    u32 busy;                   /* follow-up polls still to leave unanswered */
    int delay;                  /* -1: the defaults above; else polls every request takes */
    /* faults a script asked for */
    u32 fault_bits;             /* or-ed into the status byte (bits 1-5) */
    u32 fault_bits_left;        /* for this many replies; 0xffffffff: until switched off */
    u32 silent_left;            /* frames to leave without any answer */
    u32 refuse_left;            /* replies to give with the code the game counts as bad */
    u32 pull_after_writes;      /* the card leaves after this many more write requests; 0: off */
    /* what happened, for "card pN status" and the closing report */
    u32 n_frames, n_idle, n_requests, n_unanswered;
    u32 n_reads, n_fetches, n_writes, n_words_written, n_commits, n_keys, n_locks, n_unlocks, n_d0, n_unknown;
    u32 n_inserted, n_removed;
    u32 last_request;
} card_unit;

static struct card_state {
    u32 fitted;                 /* the cabinet has the units (--cards, or "card units on") */
    u32 probed;                 /* the game has looked for them */
    u32 serial_next;            /* serial number of the next fresh card */
    card_unit u[2];
} cs;

int hw_card_option;             /* set by --cards before the machine is made */

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    obs_note(text);             /* the call log says what the script did with the cards */
    if (cfg.verbose)
        fprintf(stderr, "[card] vb %-6u %s\n", machine_frame, text);
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 get_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* A card before its first use, as the game's own checks define it:
 * card_header_check wants the top four bits of word 0 to be 1 and the top
 * half of word 3 to be 0x046c (both read high byte first by
 * card_read_finish); card_raw_is_blank wants the 108 bytes from word 5 on to
 * be 107 zeros and one 0xff. Word 1 is the card's own number (the game calls
 * it the access key and derives the unit's key from it). */
static void card_fresh(u8 *card, u32 serial)
{
    memset(card, 0, CARD_BYTES);
    put_be32(card + 0, 0x10000000u);
    put_be32(card + 4, serial);
    put_be32(card + 12, 0x046c0000u);
    card[CARD_BYTES - 1] = 0xff;
}

static int card_is_fresh(const u8 *card)
{
    for (int i = 4 * DATA_FIRST; i < CARD_BYTES - 1; i++)
        if (card[i])
            return 0;
    return card[CARD_BYTES - 1] == 0xff;
}

static void card_remove(int n, const char *how)
{
    card_unit *u = &cs.u[n];
    if (!u->present)
        return;
    u->present = 0;
    u->eject_wanted = 0;
    u->pull_after_writes = 0;
    u->n_removed++;
    note("card unit %d: the card (number %08x) is out: %s", n + 1, get_be32(u->card + 4), how);
}

static u8 status_byte(card_unit *u)
{
    u8 s = 0;
    if (!u->present)
        s |= ST_NO_CARD;
    if (u->locked)
        s |= ST_LOCKED;
    if (u->fault_bits_left) {
        s |= (u8)(u->fault_bits & 0x3e);
        if (u->fault_bits_left != 0xffffffffu)
            u->fault_bits_left--;
    }
    return s;
}

static u32 busy_for(card_unit *u, u32 dflt)
{
    return u->delay >= 0 ? (u32)u->delay : dflt;
}

/* a reply: code, the address the frame was sent to, the port byte it came
 * from (card_reply_bad compares bytes 1 and 2 with card_unit_maple_ids), the
 * count of words that follow, then the status byte; the words of a read
 * start at byte 8 (card_reply_store) */
static int reply_status(card_unit *u, u8 *reply, u32 to, u32 from, int with_buffer)
{
    reply[0] = REPLY_STATUS;
    if (u->refuse_left) {
        u->refuse_left--;
        reply[0] = REPLY_REFUSED;
    }
    reply[1] = (u8)to;
    reply[2] = (u8)from;
    reply[3] = with_buffer ? 1 + CARD_BYTES / 4 : 1;
    reply[4] = status_byte(u);
    if (!with_buffer)
        return 8;
    memcpy(reply + 8, u->buffer, CARD_BYTES);
    return 8 + CARD_BYTES;
}

/* One frame for unit n (0, 1). d: the data words after the command word.
 * Returns the number of reply bytes; 0 when the unit does not answer (the
 * game then finds the all-ones word it put into the reply buffer). */
int hw_card_frame(int n, u32 pattern, u32 cmd, u32 to, u32 from, const u8 *d, int nbytes, u8 *reply)
{
    card_unit *u = &cs.u[n];
    cs.probed = 1;
    if (!cs.fitted)
        return 0;
    u->n_frames++;
    if (pattern != 0) {                         /* the reset pattern of maple_port_reset: nothing comes back */
        u->busy = 0;
        u->locked = 0;
        return 0;
    }
    if (u->silent_left) {
        if (u->silent_left != 0xffffffffu)
            u->silent_left--;
        u->n_unanswered++;
        return 0;
    }
    switch (cmd) {
    /* ---- the start-up exchange (card_units_start), the same four steps as the I/O bridge's ---- */
    case 0x01:
    case 0x03:
        reply[0] = 0x07;
        reply[1] = (u8)to;
        reply[2] = (u8)from;
        return 4;
    case 0x82: {                                /* identification: maple_unit_read_ident compares 0x38 bytes */
        u32 a = global_addr("mie_ident_expected");
        if (a)
            g_read(a, reply, 0x38);
        return 0x48;
    }
    case 0x80: {                                /* program upload: the sum of the frame's bytes back */
        u32 sum = 0;
        for (int i = 0; i < nbytes; i++)
            sum += d[i];
        reply[0] = 0x07;
        reply[1] = (u8)to;
        reply[2] = (u8)from;
        reply[3] = 1;
        reply[4] = (u8)sum;
        u->started = 1;
        return 8;
    }
    /* ---- polls (card_unit_step) ---- */
    case 0x90:                                  /* while no request is queued */
        u->n_idle++;
        return reply_status(u, reply, to, from, 0);
    case 0x91:                                  /* after a request, until it is done */
        if (u->busy) {
            u->busy--;
            u->n_unanswered++;
            return 0;
        }
        return reply_status(u, reply, to, from, 0);
    /* ---- requests ---- */
    case 0xd0:                                  /* first request of every read and write */
        u->n_d0++;
        break;
    case 0xa0:                                  /* take the card into the unit's buffer */
        u->n_reads++;
        if (u->present)
            memcpy(u->buffer, u->card, CARD_BYTES);
        else
            memset(u->buffer, 0, CARD_BYTES);
        u->busy = busy_for(u, BUSY_SHORT);
        break;
    case 0xa1:                                  /* hand the buffer over; sent as a request and as its own follow-up */
        if (nbytes == 0 && u->busy) {
            u->busy--;
            u->n_unanswered++;
            return 0;
        }
        u->n_fetches++;
        u->last_request = cmd;
        return reply_status(u, reply, to, from, 1);
    case 0xd4:                                  /* the key the game derived from the card's number */
        u->n_keys++;
        if (nbytes >= 4)
            memcpy(u->key, d, 4);
        break;
    case 0xb1: {                                /* write: first word {word number, count}, then the words */
        int first = nbytes >= 4 ? d[0] : 0, count = nbytes >= 4 ? d[1] : 0;
        u->n_writes++;
        if (count > (nbytes - 4) / 4)
            count = (nbytes - 4) / 4;
        if (u->present && first >= DATA_FIRST && first + count <= CARD_BYTES / 4) {
            memcpy(u->card + 4 * first, d + 4, 4u * (u32)count);
            u->n_words_written += (u32)count;
        }
        u->busy = busy_for(u, BUSY_WRITE);
        if (u->pull_after_writes && --u->pull_after_writes == 0)
            card_remove(n, "pulled in the middle of a write");
        break;
    }
    case 0xc1:                                  /* last request of every write */
        u->n_commits++;
        u->busy = busy_for(u, BUSY_WRITE);
        break;
    case 0xd9:                                  /* lock on */
        u->n_locks++;
        u->locked = 1;
        u->busy = busy_for(u, BUSY_SHORT);
        break;
    case 0xda:                                  /* lock off */
        u->n_unlocks++;
        u->locked = 0;
        u->busy = busy_for(u, BUSY_SHORT);
        if (u->eject_wanted)
            card_remove(n, "taken when the lock opened");
        break;
    default:                                    /* 0xd1 is in the library but nothing calls it */
        u->n_unknown++;
        break;
    }
    u->n_requests++;
    u->last_request = cmd;
    return reply_status(u, reply, to, from, 0);
}

/* ---- the controller's side ---- */

static void say_status(int n)
{
    card_unit *u = &cs.u[n];
    printf("{\"ev\":\"card\",\"unit\":%d,\"fitted\":%u,\"probed\":%u,\"started\":%u,\"present\":%u,\"locked\":%u,"
           "\"busy\":%u,\"number\":\"%08x\",\"key\":\"%02x%02x%02x%02x\",\"blank\":%d,"
           "\"frames\":%u,\"idle_polls\":%u,\"requests\":%u,\"unanswered\":%u,\"reads\":%u,\"fetches\":%u,"
           "\"writes\":%u,\"words_written\":%u,\"commits\":%u,\"keys\":%u,\"locks\":%u,\"unlocks\":%u,"
           "\"unknown\":%u,\"inserted\":%u,\"removed\":%u,\"last_request\":%u,\"frame\":%u}\n",
           n + 1, cs.fitted, cs.probed, u->started, u->present, u->locked, u->busy,
           u->present ? get_be32(u->card + 4) : 0, u->key[0], u->key[1], u->key[2], u->key[3],
           u->present && card_is_fresh(u->card),
           u->n_frames, u->n_idle, u->n_requests, u->n_unanswered, u->n_reads, u->n_fetches, u->n_writes,
           u->n_words_written, u->n_commits, u->n_keys, u->n_locks, u->n_unlocks, u->n_unknown, u->n_inserted,
           u->n_removed, u->last_request, machine_frame);
    fflush(stdout);
}

static int fail(char *msg, size_t len, const char *text)
{
    snprintf(msg, len, "%s", text);
    return -1;
}

static int need_units(char *msg, size_t len)
{
    if (cs.fitted)
        return 0;
    if (cs.probed)
        return fail(msg, len, "the cabinet has no card units and the game has already looked for them "
                    "(card_units_start runs once, early in the boot): start with --cards, or put "
                    "\"card units on\" at the top of the script");
    cs.fitted = 1;
    note("card units fitted");
    return 0;
}

/* "card ..." of the controller; tok is what follows the word "card".
 * 0 when done; else msg says what was wrong. */
int hw_card_command(char **tok, int n, char *msg, size_t len)
{
    if (n >= 2 && !strcmp(tok[0], "units")) {
        int on = !strcmp(tok[1], "on");
        if (cs.probed && (u32)on != cs.fitted)
            return fail(msg, len, "the game has already looked for the card units (card_units_start runs once, "
                        "early in the boot): \"card units\" has to come before the first \"run\" or \"until\"");
        cs.fitted = (u32)on;
        return 0;
    }
    int side = n >= 1 && !strcasecmp(tok[0], "p1") ? 0 : n >= 1 && !strcasecmp(tok[0], "p2") ? 1 : -1;
    if (side < 0 || n < 2)
        return fail(msg, len, "card: expected \"card units on|off\" or \"card p1|p2 insert FILE|blank [NUMBER] | "
                    "eject | pull [after N] | save FILE | status | delay N|default | fault ...\"");
    card_unit *u = &cs.u[side];
    const char *what = tok[1];
    if (!strcmp(what, "status")) {
        say_status(side);
        return 0;
    }
    if (need_units(msg, len))
        return -1;
    if (!strcmp(what, "insert") && n >= 3) {
        u8 card[CARD_BYTES];
        if (u->present)
            return fail(msg, len, "card: there is a card in that unit already (eject or pull it first)");
        if (!strcmp(tok[2], "blank")) {
            u32 serial = n >= 4 ? (u32)strtoul(tok[3], NULL, 0) : 0x20040000u + ++cs.serial_next;
            card_fresh(card, serial);
        } else {
            FILE *f = fopen(tok[2], "rb");
            if (!f || fread(card, 1, CARD_BYTES, f) != CARD_BYTES || fgetc(f) != EOF) {
                if (f)
                    fclose(f);
                snprintf(msg, len, "card: %s is not a card image (a file of %d bytes)", tok[2], CARD_BYTES);
                return -1;
            }
            fclose(f);
        }
        memcpy(u->card, card, CARD_BYTES);
        u->present = 1;
        u->eject_wanted = 0;
        u->n_inserted++;
        note("card unit %d: card in (%s, number %08x)", side + 1, tok[2], get_be32(card + 4));
        return 0;
    }
    if (!strcmp(what, "eject")) {               /* the player takes the card; a locked unit holds it till the lock opens */
        if (!u->present)
            return 0;
        if (u->locked) {
            u->eject_wanted = 1;
            note("card unit %d: the card is asked for; the lock is on", side + 1);
        } else {
            card_remove(side, "taken");
        }
        return 0;
    }
    if (!strcmp(what, "pull")) {                /* out now, lock or not; "after N": during the Nth write request from now */
        if (n >= 4 && !strcmp(tok[2], "after")) {
            u->pull_after_writes = (u32)strtoul(tok[3], NULL, 0);
            return 0;
        }
        card_remove(side, "pulled");
        return 0;
    }
    if (!strcmp(what, "save") && n >= 3) {
        if (!u->present)
            return fail(msg, len, "card: no card in that unit");
        FILE *f = fopen(tok[2], "wb");
        if (!f || fwrite(u->card, 1, CARD_BYTES, f) != CARD_BYTES) {
            if (f)
                fclose(f);
            snprintf(msg, len, "card: cannot write %s", tok[2]);
            return -1;
        }
        fclose(f);
        note("card unit %d: card image written to %s", side + 1, tok[2]);
        return 0;
    }
    if (!strcmp(what, "delay") && n >= 3) {     /* follow-up polls every request leaves unanswered */
        u->delay = !strcmp(tok[2], "default") ? -1 : atoi(tok[2]);
        return 0;
    }
    if (!strcmp(what, "fault") && n >= 3) {
        u32 count = n >= 5 ? (u32)strtoul(tok[4], NULL, 0) : 0xffffffffu;
        if (!strcmp(tok[2], "off")) {
            u->fault_bits = u->fault_bits_left = u->silent_left = u->refuse_left = 0;
            u->pull_after_writes = 0;
            u->delay = -1;
        } else if (!strcmp(tok[2], "error") && n >= 4) {
            /* the game's result codes 4 to 8 are status bits 1 to 5 (card_reply_status) */
            int code = atoi(tok[3]);
            if (code < 4 || code > 8)
                return fail(msg, len, "card: fault error takes the game's result code, 4 to 8");
            u->fault_bits = 1u << (code - 3);
            u->fault_bits_left = count;
        } else if (!strcmp(tok[2], "silent")) {
            u->silent_left = n >= 4 ? (u32)strtoul(tok[3], NULL, 0) : 0xffffffffu;
        } else if (!strcmp(tok[2], "refuse")) {
            u->refuse_left = n >= 4 ? (u32)strtoul(tok[3], NULL, 0) : 0xffffffffu;
        } else {
            return fail(msg, len, "card: fault takes off | error CODE [REPLIES] | silent [FRAMES] | refuse [REPLIES]");
        }
        note("card unit %d: fault %s %s", side + 1, tok[2], n >= 4 ? tok[3] : "");
        return 0;
    }
    return fail(msg, len, "card: unknown request (insert FILE|blank [NUMBER], eject, pull [after N], save FILE, "
                "status, delay N|default, fault ...)");
}

/* recomp (M6): the game window keeps the players' cards in files of its
 * own (runtime/host/game_machine.c): a card goes in from bytes (NULL: one
 * never used), and the card in a unit comes out as it is now. Same effect
 * as "card pN insert" and "card pN save". */
int hw_card_put(int n, const u8 *card)
{
    if (n < 0 || n > 1 || cs.u[n].present)
        return -1;
    card_unit *u = &cs.u[n];
    if (card)
        memcpy(u->card, card, CARD_BYTES);
    else
        card_fresh(u->card, 0x20040000u + ++cs.serial_next);
    u->present = 1;
    u->eject_wanted = 0;
    u->n_inserted++;
    note("card unit %d: card in (from the game window, number %08x)", n + 1, get_be32(u->card + 4));
    return 0;
}

/* recomp (card profiles): the player takes the card, as "card pN eject":
 * a unit whose lock is on (the game is using the card) holds it until the
 * game opens the lock. 0: the unit is empty now; 1: it leaves when the lock
 * opens (ask again); -1: no such unit */
int hw_card_take(int n)
{
    if (n < 0 || n > 1)
        return -1;
    card_unit *u = &cs.u[n];
    if (!u->present)
        return 0;
    if (u->locked) {
        u->eject_wanted = 1;
        return 1;
    }
    card_remove(n, "taken (the game window)");
    return 0;
}

int hw_card_get(int n, u8 *card)
{
    if (n < 0 || n > 1 || !cs.u[n].present)
        return -1;
    memcpy(card, cs.u[n].card, CARD_BYTES);
    return 0;
}

void hw_card_stats(FILE *f)
{
    if (!cs.fitted)
        return;
    for (int n = 0; n < 2; n++) {
        card_unit *u = &cs.u[n];
        fprintf(f, "  card unit %d: %s; %u frames (%u idle polls, %u requests, %u left unanswered), %u reads, "
                "%u write requests (%u words), %u cards in, %u out\n", n + 1,
                !u->started ? "never started" : u->present ? (u->locked ? "card in, lock on" : "card in") : "empty",
                u->n_frames, u->n_idle, u->n_requests, u->n_unanswered, u->n_reads, u->n_writes,
                u->n_words_written, u->n_inserted, u->n_removed);
    }
}

void hw_card_init(void)
{
    memset(&cs, 0, sizeof cs);
    cs.fitted = (u32)hw_card_option;
    cs.u[0].delay = cs.u[1].delay = -1;
    snapshot_register("card-units", &cs, sizeof cs);
}
