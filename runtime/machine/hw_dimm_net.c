/* The network side of the DIMM board: a model of its command interface.
 *
 * The board the game is loaded from also carries its network link. The
 * game talks to it three ways, all of them through the cartridge port:
 *
 *   1. five 16-bit registers at 0x005f703c (command, offset, parameter low
 *      and high, status), an interrupt line (bit 3 of the system chip's
 *      external status, on the level-2 line) and two BIOS services. The
 *      game posts a command with dimm_cmd_post (sdk/nlasock/dimm_link.c);
 *      the board asks things of the game by raising its interrupt, which
 *      ends in dimm_board_request_serve (sdk/nladimm, hand-written).
 *   2. the top 16 MiB of the board's memory, read and written with the
 *      cartridge port's address and data registers (dimm_pio_read,
 *      dimm_pio_write): one 4 KiB request block per socket slot, a send
 *      area, a receive area, a control block. sock_slots_prepare gives the
 *      layout.
 *   3. words the board keeps in the game's low RAM (0x0c01fc00 and up),
 *      and a second, slower command path through them (dimm_command_post
 *      and dimm_command_poll of sdk/nladimm/nladimm.c).
 *
 * This file answers all three. Every layout in it comes from the game's
 * own code as the decompilation gives it; docs/hle.md lists them one by
 * one with the routine each was read from. What is behind the board (the
 * name table, the hosts, the servers' replies) is net_server.c.
 *
 * Nothing here makes a network connection of any kind. An "address" is a
 * number in the stand-in's tables and data never leaves this process.
 *
 * The model is off unless asked for (--net, or "net on" as the first
 * command of a script). Off, it answers nothing and writes nothing: the
 * registers and the port behave as they did before it existed.
 */
#include "net.h"
#include "hle.h"
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "dispatch.h"
#include "observe.h"
#include "tables.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int hw_net_option;

/* ---- where things are ---- */

/* the window: DIMM addresses 0x0f000000-0x0fffffff, the top 16 MiB of a
 * board with one 256 MiB module (dimm_board_info_fetch: module base =
 * 0xff000000 + the modules' bytes). Offsets inside it, from
 * sock_slots_prepare, ctl_channel_prepare, peer_area_reset,
 * dimm_command_post: */
#define WIN_BASE        0x0f000000u
#define WIN_SIZE        0x01000000u
#define WIN_SLOTS       0x00800000u     /* 64 request blocks of 0x1000 */
#define WIN_CONTROL     0x00c00000u     /* the control channel's request */
#define WIN_COMMAND_ARG 0x00ff8000u     /* arguments of the low-RAM command path */

#define WIN_PAGE        4096u
#define WIN_PAGES       (WIN_SIZE / WIN_PAGE)
#define POOL_PAGES      384u            /* 1.5 MiB of the 16 are ever touched; kept sparse for the snapshots */

/* the registers */
#define REG_COMMAND     0x005f703cu
#define REG_OFFSET      0x005f7040u
#define REG_PARAM_LO    0x005f7044u
#define REG_PARAM_HI    0x005f7048u
#define REG_STATUS      0x005f704cu
#define EXT_IRQ_DIMM    0x00000008u     /* bit 3 of the external interrupt status and of its level-2 mask */

/* the BIOS's part: two service entries the game's link code calls, the
 * interrupt handler slot of event 0x3a0 and a few instructions of our own
 * that hand the interrupt to the routine the game left in its vector word */
#define ROM_SERVICES    0xa0060000u
#define RAM_SERVICES    0x0c018000u
#define STUB_58         (RAM_SERVICES + 0x300u)
#define STUB_60         (RAM_SERVICES + 0x308u)
#define VECTOR_BASE     0x8c000000u
#define IRQ_STUB        (VECTOR_BASE + 0x40u)
#define IRQ_SLOT        (VECTOR_BASE + 0x200u + (0x3a0u - 0x200u) / 8u)

#define SOCKS 8                         /* socket numbers 1 to 8; 0 is the slot the game makes sockets on */
#define QUEUE 32
#define TIMED 24

enum { S_NEW = 0, S_CONNECTING, S_OPEN, S_FAILED };

typedef struct board_sock {
    u8 used, state;
    u8 connect_cmd;             /* the reply command the connect waits to send */
    u8 recv_wait, recv_cmd;
    s32 connect_result, connect_error;
    s32 error;                  /* what operation 0x11 answers */
    u32 limit[3];               /* operation 0x10: the three time limits, in ms */
    u64 connect_at;
    u32 recv_addr, recv_len;
    u64 recv_deadline;
    net_conn conn;
} board_sock;

typedef struct out_cmd {
    u8 cmd, tag;
    u16 low;
    u16 offset;
    u32 value;
} out_cmd;

enum { TAG_NONE = 0, TAG_SCRIPT, TAG_INTERNAL };

static struct net_state {
    int on;
    int events;                 /* say what happens on standard output */
    u32 in[4];                  /* what the game wrote into the registers */
    u32 out[4];                 /* what the game reads from them */
    u32 status;
    /* the window, sparse */
    u16 page_of[WIN_PAGES];
    u32 pages_used, pages_refused;
    /* board to game: a queue, and the one request that is on the line */
    out_cmd queue[QUEUE];
    int q_head, q_n;
    int flying;
    out_cmd flight;
    u64 flight_since, deliver_at;
    int flight_warned;
    /* answers that are due at a time */
    struct {
        u8 used, cmd, slot;
        s32 result;
        u64 at;
    } timed[TIMED];
    board_sock sock[SOCKS + 1];
    struct {
        u8 wait, cmd;
        u32 count, rd, wr, er;
        u64 deadline;
    } sel;
    u64 wake[8];                /* the turns that are scheduled */
    /* settings */
    u32 latency;                /* cycles from a post to the board's answer */
    u32 send_max;               /* a send takes at most this many bytes; 0: all */
    u32 create_fail;            /* the next N socket creations fail */
    u32 id_word, own_ip, name_server[2];
    u32 comms_mode;
    u32 base_set;               /* the board has told the game its base address (request 3) */
    u32 command_mode;           /* low-RAM commands: 0 answer, 1 fail, 2 silent */
    u32 command_busy;
    u32 switch_value;
    /* counters */
    u32 posts, replies, requests, irq_raised, unknown_ops, unknown_posts;
    u32 ops[0x18];
    u32 service_58, service_60;
    u32 low_commands;
    u8 pool[POOL_PAGES][WIN_PAGE];
} ns;

static u32 g_hook_vector, g_pending, g_reply, g_shared, g_command_table, g_comms, g_id_word, g_extra;

static u32 board_word(void)
{
    return ns.id_word ? ns.id_word : hle_board_id_word(1);
}

/* ---- small things ---- */

u64 net_now(void)
{
    return cpu.cycles;
}

u64 net_ms(u32 ms)
{
    return (u64)ms * (CPU_HZ / 1000);
}

int hw_net_is_on(void)
{
    return ns.on;
}

static void json_text(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch == '\r')
            fputs("\\r", f);
        else if (ch == '\n')
            fputs("\\n", f);
        else if (ch < 0x20 || ch >= 0x7f)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

void net_say(const char *what, const char *fmt, ...)
{
    char text[2400], note[2500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (ns.events) {
        printf("{\"ev\":\"net\",\"what\":\"%s\",\"frame\":%u,\"text\":", what, machine_frame);
        json_text(stdout, text);
        printf("}\n");
        fflush(stdout);
    }
    snprintf(note, sizeof note, "net %s: %s", what, text);
    obs_note(note);
    if (cfg.verbose)
        fprintf(stderr, "[net] vb %u %s: %s\n", machine_frame, what, text);
}

/* ---- the window ---- */

static u8 *win_page(u32 offset, int make)
{
    u32 p = (offset & (WIN_SIZE - 1)) / WIN_PAGE;
    if (!ns.page_of[p]) {
        if (!make)
            return NULL;
        if (ns.pages_used >= POOL_PAGES) {
            ns.pages_refused++;
            return NULL;
        }
        memset(ns.pool[ns.pages_used], 0, WIN_PAGE);
        ns.page_of[p] = (u16)(++ns.pages_used);
    }
    return ns.pool[ns.page_of[p] - 1];
}

void net_win_read(u32 addr, void *dst, u32 n)
{
    u8 *d = dst;
    for (u32 i = 0; i < n; i++) {
        u8 *p = win_page(addr + i, 0);
        d[i] = p ? p[(addr + i) & (WIN_PAGE - 1)] : 0;
    }
}

void net_win_write(u32 addr, const void *src, u32 n)
{
    const u8 *s = src;
    for (u32 i = 0; i < n; i++) {
        u8 *p = win_page(addr + i, 1);
        if (p)
            p[(addr + i) & (WIN_PAGE - 1)] = s[i];
    }
}

static int in_window(u32 offset)
{
    return ns.on && offset >= WIN_BASE && offset - WIN_BASE < WIN_SIZE;
}

int hw_net_pio_read(u32 offset, u32 *out)
{
    u8 b[2];
    if (!in_window(offset))
        return 0;
    net_win_read(offset, b, 2);
    *out = (u32)(b[0] | (b[1] << 8));
    return 1;
}

int hw_net_pio_write(u32 offset, u32 v)
{
    u8 b[2] = { (u8)v, (u8)(v >> 8) };
    if (!in_window(offset))
        return 0;
    net_win_write(offset, b, 2);
    return 1;
}

int hw_net_block_read(u32 offset, u8 *dst, u32 n)
{
    if (!in_window(offset))
        return 0;
    net_win_read(offset, dst, n);
    return 1;
}

int hw_net_block_write(u32 offset, const u8 *src, u32 n)
{
    if (!in_window(offset))
        return 0;
    net_win_write(offset, src, n);
    return 1;
}

static u32 win_w32(u32 addr)
{
    u32 v;
    net_win_read(addr, &v, 4);
    return v;
}

/* ---- the board's turn comes by an event ---- */

static void board_event(sh4 *c, u32 arg);

static void wake_at(u64 at)
{
    u64 now = net_now();
    int room = -1;
    if (at <= now)
        at = now + 1;
    for (int i = 0; i < 8; i++) {
        if (ns.wake[i] > now && ns.wake[i] <= at)
            return;                     /* an earlier turn is already coming, and it will look ahead again */
        if (ns.wake[i] <= now)
            room = i;
    }
    if (room < 0)
        return;                         /* (eight turns pending, all later: cannot happen with the rule above) */
    ns.wake[room] = at;
    event_add(at, board_event, 0, "DIMM board");
}

/* ---- board to game ---- */

static void out_push(u8 cmd, u16 low, u16 offset, u32 value, u8 tag)
{
    if (ns.q_n >= QUEUE) {
        fprintf(stderr, "[net] the board's queue of requests to the game is full; command 0x%02x dropped\n", cmd);
        return;
    }
    out_cmd *o = &ns.queue[(ns.q_head + ns.q_n++) % QUEUE];
    o->cmd = cmd;
    o->low = low;
    o->offset = offset;
    o->value = value;
    o->tag = tag;
}

/* the answer to a socket request: the game's sock_reply_take wants low
 * byte 4, the slot in the offset register and the result in the two
 * parameter halves; ctl_reply_take the same with low byte 5 */
static void answer(u8 cmd, u32 slot, s32 result)
{
    out_push(cmd, cmd == 0x22 ? 5 : 4, (u16)slot, (u32)result, TAG_NONE);
    ns.replies++;
}

static void answer_at(u8 cmd, u32 slot, s32 result, u64 at)
{
    for (int i = 0; i < TIMED; i++)
        if (!ns.timed[i].used) {
            ns.timed[i].used = 1;
            ns.timed[i].cmd = cmd;
            ns.timed[i].slot = (u8)slot;
            ns.timed[i].result = result;
            ns.timed[i].at = at;
            wake_at(at);
            return;
        }
    answer(cmd, slot, result);          /* no room to wait: now */
    wake_at(net_now() + 1);
}

/* puts the next request on the line: the registers and the interrupt */
static void deliver(sh4 *c)
{
    u64 now = net_now();
    if (ns.flying || !ns.q_n)
        return;
    if (now < ns.deliver_at) {
        wake_at(ns.deliver_at);
        return;
    }
    if (!g_r32(g_hook_vector)) {
        /* the game has not said yet where its service routine is */
        wake_at(now + FRAME_CYCLES);
        return;
    }
    ns.flight = ns.queue[ns.q_head];
    ns.q_head = (ns.q_head + 1) % QUEUE;
    ns.q_n--;
    ns.flying = 1;
    ns.flight_since = now;
    ns.flight_warned = 0;
    /* requests the game serves itself (0-15) carry bit 15; the answers to
     * its own posts (16 and up) do not: with bit 29 of the board word set
     * the game takes every call as a request */
    ns.out[0] = (ns.flight.cmd < 16 ? 0x8000u : 0) | ((u32)ns.flight.cmd << 9) | (ns.flight.low & 0x1ffu);
    if (ns.flight.tag == TAG_SCRIPT && ns.flight.cmd >= 0x40)
        ns.out[0] = ns.flight.value >> 16;      /* a raw command word from the script */
    ns.out[1] = ns.flight.offset;
    ns.out[2] = ns.flight.value & 0xffff;
    ns.out[3] = ns.flight.cmd >= 0x40 ? 0 : ns.flight.value >> 16;
    if (g_r32(IRQ_SLOT) != IRQ_STUB)
        low_ram_w32(IRQ_SLOT, IRQ_STUB);        /* (the game cleared the BIOS's slot) */
    ns.irq_raised++;
    hw_raise_ext(c, EXT_IRQ_DIMM);
}

/* the game has set status bit 8: it is done with the request on the line */
static void flight_done(sh4 *c)
{
    if (!ns.flying)
        return;
    ns.flying = 0;
    hw.irq_external &= ~EXT_IRQ_DIMM;
    hw_irq_update(c);
    if (ns.flight.cmd < 16 || ns.flight.cmd >= 0x40)
        ns.requests++;
    if (ns.flight.tag == TAG_SCRIPT) {
        /* what the game wrote back: its answer byte beside the command
         * number, the offset and the two parameter halves */
        u32 word = ns.in[0] & 0xffff;
        if (((ns.out[0] >> 9) & 0x3f) >= 16)
            net_say("request", "command word %04x: a command number of 16 or more, handed to the game's table of "
                    "handlers (nothing is written back)", ns.out[0] & 0xffff);
        else
            net_say("request", "command word %04x answered %04x: answer byte %02x, offset %04x, parameter %04x%04x",
                    ns.out[0] & 0xffff, word, word & 0xff, ns.in[1] & 0xffff, ns.in[3] & 0xffff, ns.in[2] & 0xffff);
    }
    ns.deliver_at = net_now() + 400;
    if (ns.q_n)
        wake_at(ns.deliver_at);
}

/* ---- sockets ---- */

static board_sock *sock_of(u32 n)
{
    return n >= 1 && n <= SOCKS && ns.sock[n].used ? &ns.sock[n] : NULL;
}

static void recv_try(board_sock *s, u32 number)
{
    u64 now = net_now();
    net_conn *k = &s->conn;
    u32 avail = k->rx_len - k->rx_pos;
    if (avail && k->rx_at <= now) {
        u32 n = avail < s->recv_len ? avail : s->recv_len;
        if (k->rx_piece && n > k->rx_piece)
            n = k->rx_piece;
        if (k->rx_break > k->rx_pos && n > k->rx_break - k->rx_pos)
            n = k->rx_break - k->rx_pos;
        net_win_write(s->recv_addr, k->rx + k->rx_pos, n);
        k->rx_pos += n;
        if (k->rx_pos == k->rx_len)
            k->rx_pos = k->rx_len = 0;
        else if (k->rx_piece)
            k->rx_at = now + k->rx_gap;
        s->recv_wait = 0;
        answer(s->recv_cmd, number, (s32)n);
    } else if (!avail && k->reset) {
        s->recv_wait = 0;
        s->error = NET_ECONNRESET;
        answer(s->recv_cmd, number, -1);
    } else if (!avail && k->closed) {
        s->recv_wait = 0;
        answer(s->recv_cmd, number, 0);         /* the other side has closed: nothing more comes */
    } else if (s->recv_deadline && now >= s->recv_deadline) {
        s->recv_wait = 0;
        s->error = NET_ETIMEDOUT;
        answer(s->recv_cmd, number, -1);
    }
}

static int sock_readable(board_sock *s)
{
    net_conn *k = &s->conn;
    return s->state == S_OPEN && ((k->rx_len > k->rx_pos && k->rx_at <= net_now()) || k->closed || k->reset);
}

/* operation 0xc: which of the sockets named in the three sets are ready.
 * The sets are 32 bytes each in the slot's data area, one bit a socket
 * (naominet_conn_idle_poll sets bit sock & 31 of word sock >> 5). */
static int select_try(int final)
{
    u32 rd[8], wr[8], count = 0;
    memset(rd, 0, sizeof rd);
    memset(wr, 0, sizeof wr);
    for (u32 n = 1; n <= SOCKS && n < ns.sel.count; n++) {
        board_sock *s = sock_of(n);
        u32 word = n >> 5, bit = 1u << (n & 31);
        if (!s)
            continue;
        if (ns.sel.rd && (win_w32(ns.sel.rd + 4 * word) & bit) && sock_readable(s)) {
            rd[word] |= bit;
            count++;
        }
        if (ns.sel.wr && (win_w32(ns.sel.wr + 4 * word) & bit) && s->state == S_OPEN && !s->conn.closed) {
            wr[word] |= bit;
            count++;
        }
    }
    if (!count && !final)
        return 0;
    if (ns.sel.rd)
        net_win_write(ns.sel.rd, rd, 32);
    if (ns.sel.wr)
        net_win_write(ns.sel.wr, wr, 32);
    if (ns.sel.er) {
        memset(rd, 0, sizeof rd);
        net_win_write(ns.sel.er, rd, 32);
    }
    ns.sel.wait = 0;
    answer(ns.sel.cmd, 0, (s32)count);
    return 1;
}

static const char *op_name(u32 op)
{
    static const char *const names[0x18] = {
        "?", "accept", "bind", "close", "connect", "resolve", "lookup", "ioctl", "listen", "receive", "send", "create",
        "select", "?", "set option", "get option", "time limits", "error", "two texts (0x12)", "two texts (0x13)",
        "switch", "text (0x15)", "receive from", "send to"
    };
    return op < 0x18 ? names[op] : "?";
}

/* A socket request: the game has written a block into slot `slot` of the
 * window and posted command 0x11 (it will wait) or 0x12 (it will ask
 * later); the answer goes back as command 0x13 or 0x14. The block's first
 * word is the operation; the rest by operation, as the game's *_pack_args
 * and *_request_write routines lay it out. */
static void socket_request(sh4 *c, u32 post, u32 slot)
{
    u32 block = WIN_BASE + WIN_SLOTS + (slot & 63) * 0x1000u, w[10];
    u8 reply = (u8)(post + 2);
    u64 now = net_now(), soon = now + ns.latency;
    char text[80];
    net_win_read(block, w, sizeof w);
    u32 op = w[0];
    if (op < 0x18)
        ns.ops[op]++;
    switch (op) {
    case 0x0b: {                /* create: domain, type, protocol (sock_create_pack_args) */
        u32 n = 0;
        if (ns.create_fail) {
            ns.create_fail--;
        } else {
            for (n = 1; n <= SOCKS && ns.sock[n].used; n++)
                ;
        }
        if (n < 1 || n > SOCKS) {
            net_say("socket", "create (%u, %u, %u): refused", w[1], w[2], w[3]);
            answer_at(reply, slot, -1, soon);
            break;
        }
        board_sock *s = &ns.sock[n];
        memset(s, 0, sizeof *s);
        s->used = 1;
        net_say("socket", "create (%u, %u, %u): socket %u", w[1], w[2], w[3], n);
        answer_at(reply, slot, (s32)n, soon);
        break;
    }
    case 0x04: {                /* connect: socket, the address's place, its length; the address at +0x20 */
        board_sock *s = sock_of(w[1]);
        u8 a[16];
        net_win_read(block + 0x20, a, 16);
        u32 ip = (u32)(a[4] | (a[5] << 8) | (a[6] << 16) | ((u32)a[7] << 24)), delay = 0;
        u16 port = (u16)((a[2] << 8) | a[3]);
        if (!s || s->state == S_CONNECTING) {
            answer_at(reply, slot, -1, soon);
            break;
        }
        int how = net_server_connect(ip, port, &delay);
        s->conn.ip = ip;
        s->conn.port = port;
        s->state = S_CONNECTING;
        s->connect_cmd = reply;
        s->connect_at = soon + net_ms(delay);
        s->connect_result = how == NET_HOST_ACCEPT ? 0 : -1;
        s->connect_error = how == NET_HOST_REFUSE ? NET_ECONNREFUSED : how == NET_HOST_UNREACHABLE ? NET_EHOSTUNREACH
                           : how == NET_HOST_SILENT ? NET_ETIMEDOUT : 0;
        if (how == NET_HOST_SILENT)             /* nobody answers: the board gives up when its first limit has passed */
            s->connect_at = s->limit[0] ? soon + net_ms(s->limit[0]) : ~0ull;
        net_say("socket", "connect %u to %s port %u: %s", w[1], net_ip_text(ip), port,
                how == NET_HOST_ACCEPT ? "accepted" : how == NET_HOST_REFUSE ? "refused" : how == NET_HOST_UNREACHABLE
                ? "no route" : "no answer");
        if (s->connect_at != ~0ull)
            wake_at(s->connect_at);
        break;
    }
    case 0x03: {                /* close: socket */
        board_sock *s = sock_of(w[1]);
        if (s) {
            net_say("socket", "close %u", w[1]);
            s->used = 0;
        }
        answer_at(reply, slot, s ? 0 : -1, soon);
        break;
    }
    case 0x05:                  /* resolve: the name's place, its length, two name servers, the slot;
                                 * the name at +0x20 */
    case 0x06: {                /* lookup: the same without the servers */
        u32 delay = 0;
        int fail = 0;
        net_win_read(block + 0x20, text, 64);
        text[64] = 0;
        u32 ip = net_server_resolve(text, &delay, &fail);
        net_say("resolve", "%s: %s", text, fail ? "the lookup fails" : ip ? net_ip_text(ip) : "no such name");
        answer_at(reply, slot, fail ? -1 : (s32)ip, soon + net_ms(delay));
        break;
    }
    case 0x0a: {                /* send: socket, where the bytes are, how many, flags (sock_send_pack_args) */
        board_sock *s = sock_of(w[1]);
        u32 n = w[3];
        if (!s || s->state != S_OPEN) {
            if (s)
                s->error = NET_ENOTCONN;
            answer_at(reply, slot, -1, soon);
            break;
        }
        if (s->conn.reset || s->conn.closed) {
            s->error = NET_ECONNRESET;
            answer_at(reply, slot, -1, soon);
            break;
        }
        if (ns.send_max && n > ns.send_max)
            n = ns.send_max;
        if (n > NET_TX_MAX - s->conn.tx_len)
            n = NET_TX_MAX - s->conn.tx_len;
        net_win_read(w[2], s->conn.tx + s->conn.tx_len, n);
        s->conn.tx_len += n;
        net_server_sent(&s->conn);
        answer_at(reply, slot, (s32)n, soon);
        break;
    }
    case 0x09: {                /* receive: socket, where to put the bytes, room, flags (sock_recv_pack_args) */
        board_sock *s = sock_of(w[1]);
        if (!s || s->state != S_OPEN || s->recv_wait) {
            if (s)
                s->error = NET_ENOTCONN;
            answer_at(reply, slot, -1, soon);
            break;
        }
        s->recv_wait = 1;
        s->recv_cmd = reply;
        s->recv_addr = w[2];
        s->recv_len = w[3];
        s->recv_deadline = s->limit[2] ? now + net_ms(s->limit[2]) : 0;
        wake_at(soon);
        break;
    }
    case 0x0c: {                /* select: count, the three sets' places, the time-out's place; the time-out at +0x20 */
        if (ns.sel.wait) {
            answer_at(reply, slot, -1, soon);
            break;
        }
        ns.sel.wait = 1;
        ns.sel.cmd = reply;
        ns.sel.count = w[1];
        ns.sel.rd = w[2];
        ns.sel.wr = w[3];
        ns.sel.er = w[4];
        ns.sel.deadline = w[5] ? soon + (u64)w[8] * CPU_HZ + (u64)w[9] * (CPU_HZ / 1000000) : ~0ull;
        wake_at(soon);
        break;
    }
    case 0x10: {                /* three values for a socket: the game hands over its three time limits */
        board_sock *s = sock_of(w[1]);
        if (s) {
            s->limit[0] = w[2];
            s->limit[1] = w[3];
            s->limit[2] = w[4];
        }
        answer_at(reply, slot, s ? 0 : -1, soon);
        break;
    }
    case 0x11: {                /* what went wrong on a socket */
        board_sock *s = sock_of(w[1]);
        answer_at(reply, slot, s ? s->error : NET_EBADF, soon);
        break;
    }
    case 0x14:                  /* a switch of the board, 0 or 1 (board_switch_request_write) */
        ns.switch_value = w[1];
        net_say("board", "switch request, value %u", w[1]);
        answer_at(reply, slot, 0, soon);
        break;
    /* The rest nothing in the image calls. They are answered so that a
     * caller comes back: done for what needs no other side, an error for
     * what would wait for one. */
    case 0x02: case 0x07: case 0x08: case 0x0e: case 0x12: case 0x13: case 0x15:
        answer_at(reply, slot, 0, soon);
        break;
    case 0x0f: {                /* get option: the value's and the length's places in words 4 and 5 */
        u32 zero = 0, four = 4;
        if (w[4])
            net_win_write(w[4], &zero, 4);
        if (w[5])
            net_win_write(w[5], &four, 4);
        answer_at(reply, slot, 0, soon);
        break;
    }
    case 0x17:                  /* send to: nobody is there, the bytes are taken */
        answer_at(reply, slot, (s32)w[3], soon);
        break;
    case 0x01: case 0x16: {     /* accept, receive from: nobody ever comes */
        board_sock *s = sock_of(w[1]);
        if (s)
            s->error = NET_EOPNOTSUPP;
        answer_at(reply, slot, -1, soon);
        break;
    }
    default:
        ns.unknown_ops++;
        net_say("socket", "operation 0x%x on slot %u has no model: answered -1", op, slot);
        answer_at(reply, slot, -1, soon);
        break;
    }
    if (op < 0x18 && cfg.verbose > 1)
        fprintf(stderr, "[net] post 0x%02x slot %u: %s (%08x %08x %08x %08x)\n", post, slot, op_name(op), w[1], w[2],
                w[3], w[4]);
    (void)c;
}

/* BIOS service 0x60, called by dimm_cmd_post with 1 once the command and
 * its three operands stand in the registers: the board looks at them */
static void post_taken(sh4 *c)
{
    u32 word = ns.in[0] & 0xffff, id = (word >> 9) & 0x3f, low = word & 0xff;
    ns.posts++;
    switch (id) {
    case 0x11:
    case 0x12:
        socket_request(c, id, low);
        break;
    case 0x21: {                /* the control channel: a block at its own place, answered by command 0x22 */
        u32 op = win_w32(WIN_BASE + WIN_CONTROL);
        net_say("board", "control request %u", op);
        answer_at(0x22, 0, 0, net_now() + ns.latency);
        break;
    }
    case 0x23:                  /* two values or a bare operation for the board; the game waits for nothing */
        net_say("board", "command 0x23, operation %u, values %04x %04x", ns.in[1] & 0xffff, ns.in[2] & 0xffff,
                ns.in[3] & 0xffff);
        break;
    case 0x31: case 0x33: case 0x34: case 0x35:
        /* the second machine's link (sdk/nlasock/peer.c): nothing in the
         * image calls it; answered with command 0x30 and "done" so that a
         * caller comes back */
        out_push(0x30, 0, 0, 0, TAG_NONE);
        ns.deliver_at = net_now() + ns.latency;
        wake_at(ns.deliver_at);
        break;
    default:
        ns.unknown_posts++;
        net_say("board", "command 0x%02x posted (word %04x): no model, no answer", id, word);
        break;
    }
}

/* ---- the slower command path through the words in low RAM ---- */

/* The game sets bit n of the pending word and clears the reply word
 * (dimm_command_post); the board acts, shows the command's argument in the
 * shared words and puts its state into bits 8-9 of the reply word: 1 under
 * way, 2 done, 3 failed (dimm_command_poll). The board has one way into
 * the game's RAM: the requests the game's service routine answers. So the
 * model writes those words as the board must: by request 3 (here is my
 * base address) and request 10 (store this word). */
static void low_write32(u32 offset, u32 value)
{
    if (!ns.base_set) {
        out_push(3, 0, 0, g_shared, TAG_INTERNAL);
        ns.base_set = 1;
    }
    out_push(10, 0, (u16)offset, value, TAG_INTERNAL);
}

static void low_command_poll(void)
{
    u32 pending = g_r32(g_pending);
    if (!pending)
        ns.command_busy = 0;
    if (!pending || ns.command_busy == pending || ns.command_mode == 2)
        return;
    u32 n = 0;
    while (n < 31 && !(pending & (1u << n)))
        n++;
    ns.command_busy = pending;
    ns.low_commands++;
    /* the game's own table says where a command's argument is and how long */
    s32 size = (s32)g_r32(g_command_table + 16 * n), dimm_off = (s32)g_r32(g_command_table + 16 * n + 8),
        shared_off = (s32)g_r32(g_command_table + 16 * n + 12);
    net_say("board", "low-RAM command %u (argument: %d bytes)%s", n, size > 0 ? size : 0,
            ns.command_mode == 1 ? ": refused" : "");
    if (ns.command_mode == 1) {
        low_write32(g_reply - g_shared, 0x00000300u);
        low_write32(g_pending - g_shared, 0);
        return;
    }
    low_write32(g_reply - g_shared, 0x00000100u);
    if (size > 0 && n != 0x10) {
        u8 a[32];
        memset(a, 0, sizeof a);
        net_win_read(WIN_BASE + WIN_COMMAND_ARG + (u32)dimm_off, a, (u32)size > 32 ? 32 : (u32)size);
        for (s32 i = 0; i < size; i += 4) {
            u32 v;
            memcpy(&v, a + i, 4);
            if (i + 4 > size) {         /* an argument that does not end on a word: keep what follows it */
                u32 old = g_r32(g_shared + (u32)shared_off + (u32)i), mask = (1u << (8 * (size - i))) - 1;
                v = (v & mask) | (old & ~mask);
            }
            low_write32((u32)shared_off + (u32)i, v);
        }
    }
    low_write32(g_pending - g_shared, 0);
    /* done; the top two bytes are the firmware's version, which is what
     * dimm_firmware_version_get posts a command for */
    low_write32(g_reply - g_shared, 0x03170200u);
}

/* ---- the board's turn ---- */

static void board_poll(sh4 *c)
{
    u64 now = net_now(), next = ~0ull;
    for (int i = 0; i < TIMED; i++)
        if (ns.timed[i].used) {
            if (ns.timed[i].at <= now) {
                ns.timed[i].used = 0;
                answer(ns.timed[i].cmd, ns.timed[i].slot, ns.timed[i].result);
            } else if (ns.timed[i].at < next) {
                next = ns.timed[i].at;
            }
        }
    for (u32 n = 1; n <= SOCKS; n++) {
        board_sock *s = &ns.sock[n];
        if (!s->used)
            continue;
        if (s->state == S_CONNECTING) {
            if (s->connect_at <= now) {
                s->state = s->connect_result ? S_FAILED : S_OPEN;
                s->error = s->connect_error;
                answer(s->connect_cmd, n, s->connect_result);
            } else if (s->connect_at < next) {
                next = s->connect_at;
            }
        }
        net_conn *k = &s->conn;
        if (k->close_at && !k->closed) {
            if (k->close_at <= now) {
                k->closed = 1;
                net_say("socket", "the server closes socket %u", n);
            } else if (k->close_at < next) {
                next = k->close_at;
            }
        }
        if (s->recv_wait) {
            recv_try(s, n);
            if (s->recv_wait) {
                if (k->rx_len > k->rx_pos && k->rx_at < next)
                    next = k->rx_at;
                if (s->recv_deadline && s->recv_deadline < next)
                    next = s->recv_deadline;
            }
        }
    }
    if (ns.sel.wait && !select_try(now >= ns.sel.deadline)) {
        if (ns.sel.deadline < next)
            next = ns.sel.deadline;
        for (u32 n = 1; n <= SOCKS; n++) {
            net_conn *k = &ns.sock[n].conn;
            if (ns.sock[n].used && k->rx_len > k->rx_pos && k->rx_at > now && k->rx_at < next)
                next = k->rx_at;
        }
    }
    if (ns.comms_mode == 2) {
        low_command_poll();
        if (now + CPU_HZ / 1000 < next)
            next = now + CPU_HZ / 1000;         /* the board looks at the pending word a thousand times a second */
    }
    if (ns.flying) {
        /* a board without bit 29 is put off while a block read runs: the
         * game masks the line and sets a flag (dimm_board_request_serve);
         * the BIOS's part here is to open the line again once the read is
         * over */
        u32 busy = 0;
        hw_cart_read(c, 0x005f7418u, &busy);
        if (!(hw.mask2_external & EXT_IRQ_DIMM) && g_r32(g_comms + 8) == 1 && !busy) {
            hw.mask2_external |= EXT_IRQ_DIMM;
            hw_irq_update(c);
        }
        if (!ns.flight_warned && now - ns.flight_since > 2 * CPU_HZ) {
            ns.flight_warned = 1;
            fprintf(stderr, "[net] vb %u: the board's request (command 0x%02x) has waited two seconds for the game "
                    "(interrupts held off? at %s)\n", machine_frame, ns.flight.cmd, addr_text(c->pc));
        }
        if (now + FRAME_CYCLES < next)
            next = now + FRAME_CYCLES;
    }
    deliver(c);
    if (!ns.flying && ns.q_n && ns.deliver_at < next)
        next = ns.deliver_at;
    if (next != ~0ull)
        wake_at(next);
}

static void board_event(sh4 *c, u32 arg)
{
    (void)arg;
    if (ns.on)
        board_poll(c);
}

/* ---- the registers ---- */

int hw_net_reg_read(sh4 *c, u32 p, u32 *out)
{
    (void)c;
    if (!ns.on)
        return 0;
    switch (p) {
    case REG_COMMAND: *out = ns.out[0]; return 1;
    case REG_OFFSET: *out = ns.out[1]; return 1;
    case REG_PARAM_LO: *out = ns.out[2]; return 1;
    case REG_PARAM_HI: *out = ns.out[3]; return 1;
    case REG_STATUS: *out = ns.status | 1; return 1;    /* bit 0: the board is ready for the game's word */
    }
    return 0;
}

int hw_net_reg_write(sh4 *c, u32 p, u32 v)
{
    if (!ns.on)
        return 0;
    switch (p) {
    case REG_COMMAND: ns.in[0] = v; return 1;
    case REG_OFFSET: ns.in[1] = v; return 1;
    case REG_PARAM_LO: ns.in[2] = v; return 1;
    case REG_PARAM_HI: ns.in[3] = v; return 1;
    case REG_STATUS:
        ns.status = v & 0xffff;
        if (v & 0x100)
            flight_done(c);
        return 1;
    }
    return 0;
}

int hw_net_outside(sh4 *c, u32 target, int kind)
{
    u32 t = target & 0x1fffffffu;
    if (!ns.on || kind == DISP_RETURN)
        return 0;
    if (t == STUB_58) {
        /* dimm_g1_timing_apply hands over (-1, the request vector word's
         * value). What the BIOS does with them is not known; nothing
         * comes back. */
        ns.service_58++;
        c->pc = c->pr;
        return 1;
    }
    if (t == STUB_60) {
        ns.service_60++;
        if (c->r[4] == 1)
            post_taken(c);
        c->r[0] = 0;
        c->pc = c->pr;
        return 1;
    }
    return 0;
}

/* ---- on and off ---- */

static int names_find(char *msg, size_t msg_len)
{
    static const struct {
        const char *name;
        u32 *where;
    } names[] = {
        { "dimm_request_hook_vector", &g_hook_vector }, { "dimm_pending_command", &g_pending },
        { "dimm_reply_word", &g_reply }, { "dimm_shared_area", &g_shared },
        { "dimm_command_table", &g_command_table }, { "dimm_comms_mode", &g_comms },
        { "dimm_board_id_word", &g_id_word }, { "dimm_board_extra", &g_extra },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        *names[i].where = global_addr(names[i].name);
        if (!*names[i].where) {
            snprintf(msg, msg_len, "the decompilation has no global named %s (renamed? src/hw_dimm_net.c uses it)",
                     names[i].name);
            return -1;
        }
    }
    return 0;
}

static void low_words_write(void)
{
    low_ram_w32(g_id_word, board_word());
    /* the board's network settings: http_server_lookup_step and
     * naominet_init read its own address and the two name servers from
     * the board record, which copies them from here (+0x20, +0x2c, +0x30) */
    low_ram_w32(g_extra + 0x20, ns.own_ip);
    low_ram_w32(g_extra + 0x2c, ns.name_server[0]);
    low_ram_w32(g_extra + 0x30, ns.name_server[1]);
    low_ram_w32(g_comms, ns.comms_mode);
}

static int net_on(sh4 *c, char *msg, size_t msg_len)
{
    if (ns.on)
        return 0;
    if (names_find(msg, msg_len))
        return -1;
    ns.on = 1;
    ns.status = 0;
    /* the two service entries, in the ROM block the game copies and in the
     * copy when it is already made */
    g_w32(ROM_SERVICES + 0x58, STUB_58);
    g_w32(ROM_SERVICES + 0x60, STUB_60);
    for (u32 a = STUB_58; a <= STUB_60; a += 8) {
        g_w16(ROM_SERVICES + (a - RAM_SERVICES), 0x000b);       /* rts */
        g_w16(ROM_SERVICES + (a - RAM_SERVICES) + 2, 0x0009);   /* nop */
    }
    if (g_r32(RAM_SERVICES) != 0) {
        low_ram_w32(RAM_SERVICES + 0x58, STUB_58);
        low_ram_w32(RAM_SERVICES + 0x60, STUB_60);
    }
    /* The board's interrupt. On the real machine the BIOS takes it and
     * calls the routine whose address the game left in its vector word
     * (dimm_request_hook_install). Here the game's own interrupt entry
     * runs (as for every interrupt in this harness) and finds this in the
     * handler slot of event 0x3a0: load the vector word, jump there. */
    static const u16 stub[8] = { 0xd003 /* mov.l @(12,pc),r0 */, 0x6002 /* mov.l @r0,r0 */, 0x2008 /* tst r0,r0 */,
                                 0x8901 /* bt +1 */, 0x402b /* jmp @r0 */, 0x0009 /* nop */, 0x000b /* rts */,
                                 0x0009 /* nop */ };
    for (u32 i = 0; i < 8; i++)
        low_ram_w16(IRQ_STUB + 2 * i, stub[i]);
    low_ram_w32(IRQ_STUB + 16, g_hook_vector);
    low_ram_w32(IRQ_SLOT, IRQ_STUB);
    hw.mask2_external |= EXT_IRQ_DIMM;
    /* the words the game's service routine and link code keep beside the
     * comms mode word: counts and flags, zero at the start */
    for (u32 a = g_comms + 4; a < g_comms + 0x20; a += 4)
        low_ram_w32(a, 0);
    low_words_write();
    hw_irq_update(c);
    net_say("board", "on: board word %08x, address %s%s", board_word(), net_ip_text(ns.own_ip),
            machine_frame ? " (the game has been running: it read the board's words at its boot and may not take "
            "the board as one with a network side)" : "");
    return 0;
}

static void net_off(sh4 *c)
{
    if (!ns.on)
        return;
    /* the board goes quiet; what the game has in its memory stays */
    ns.on = 0;
    ns.flying = 0;
    ns.q_n = 0;
    hw.irq_external &= ~EXT_IRQ_DIMM;
    hw_irq_update(c);
    net_say("board", "off");
}

static void settings_default(void)
{
    ns.events = 1;
    ns.latency = 4000;                  /* 20 microseconds */
    /* 0: the word of hle_board_id_word, a board with one 256 MiB module (as
     * without the model) and bit 29, which is what the game's
     * net_board_present asks for; "net board WORD" sets another */
    ns.id_word = 0;
    int ok;
    ns.own_ip = net_text_ip("192.0.2.50", &ok);
    ns.name_server[0] = net_text_ip("192.0.2.1", &ok);
    ns.name_server[1] = net_text_ip("192.0.2.2", &ok);
}

void hw_net_init(void)
{
    char msg[200];
    memset(&ns, 0, sizeof ns);
    names_find(msg, sizeof msg);        /* (said when the model is turned on, if one is missing) */
    settings_default();
    net_server_init();
    snapshot_register("dimm-net", &ns, sizeof ns);
    event_register(board_event, "DIMM board");
}

void hw_net_boot(sh4 *c)
{
    char msg[200];
    if (hw_net_option && net_on(c, msg, sizeof msg)) {
        fprintf(stderr, "vf4debug: --net: %s\n", msg);
        exit(1);
    }
}

/* ---- the controller's commands ---- */

static u32 number(const char *t)
{
    return (u32)strtoul(t, NULL, 0);
}

int hw_net_command(sh4 *c, char **tok, int n, char *msg, size_t msg_len)
{
    msg[0] = 0;
    if (n < 1) {
        snprintf(msg, msg_len, "net: what? (on, off, status, ...; see README.md, \"The network board\")");
        return -1;
    }
    const char *w = tok[0];
    if (!strcmp(w, "on"))
        return net_on(c, msg, msg_len);
    if (!strcmp(w, "off")) {
        net_off(c);
        return 0;
    }
    if (!strcmp(w, "events") && n >= 2) {
        ns.events = !strcmp(tok[1], "on");
        return 0;
    }
    if (!strcmp(w, "ip") && n >= 2) {
        int ok;
        ns.own_ip = net_text_ip(tok[1], &ok);
        if (!ok)
            goto bad;
        if (ns.on)
            low_words_write();
        return 0;
    }
    if (!strcmp(w, "nameserver") && n >= 2) {
        int ok, ok2 = 1;
        ns.name_server[0] = net_text_ip(tok[1], &ok);
        if (n >= 3)
            ns.name_server[1] = net_text_ip(tok[2], &ok2);
        if (!ok || !ok2)
            goto bad;
        if (ns.on)
            low_words_write();
        return 0;
    }
    if (!strcmp(w, "board") && n >= 2) {
        /* the board word: bit 29 the network side, bit 27 the "switch" requests, bits 16-19 the module */
        ns.id_word = number(tok[1]);
        if (ns.on)
            low_words_write();
        return 0;
    }
    if (!strcmp(w, "comms") && n >= 2) {
        ns.comms_mode = number(tok[1]);
        if (ns.on) {
            low_words_write();
            wake_at(net_now() + 1);
        }
        return 0;
    }
    if (!strcmp(w, "command") && n >= 2) {
        ns.command_mode = !strcmp(tok[1], "fail") ? 1 : !strcmp(tok[1], "silent") ? 2 : 0;
        return 0;
    }
    if (!strcmp(w, "latency") && n >= 2) {
        ns.latency = number(tok[1]);
        return 0;
    }
    if (!strcmp(w, "sendmax") && n >= 2) {
        ns.send_max = number(tok[1]);
        return 0;
    }
    if (!strcmp(w, "socket") && n >= 2 && !strcmp(tok[1], "fail")) {
        ns.create_fail = n >= 3 ? number(tok[2]) : 1;
        return 0;
    }
    if (!strcmp(w, "request") && n >= 2) {
        /* the board asks something of the game: a command 0-15 with its
         * extra bits, offset and 32-bit parameter, or a raw command word */
        if (!ns.on) {
            snprintf(msg, msg_len, "net request: the model is off");
            return -1;
        }
        if (!strcmp(tok[1], "word") && n >= 3)
            out_push(0x40, 0, n >= 4 ? (u16)number(tok[3]) : 0,
                     (number(tok[2]) << 16) | (n >= 5 ? number(tok[4]) & 0xffff : 0), TAG_SCRIPT);
        else
            out_push((u8)(number(tok[1]) & 15), n >= 3 ? (u16)number(tok[2]) : 0, n >= 4 ? (u16)number(tok[3]) : 0,
                     n >= 5 ? number(tok[4]) : 0, TAG_SCRIPT);
        wake_at(net_now() + 1);
        return 0;
    }
    if (!strcmp(w, "hangup")) {
        /* every server closes its side of what is open, now */
        for (u32 i = 1; i <= SOCKS; i++)
            if (ns.sock[i].used && !ns.sock[i].conn.closed)
                ns.sock[i].conn.close_at = net_now() + 1;
        wake_at(net_now() + 1);
        return 0;
    }
    if (!strcmp(w, "status")) {
        int open = 0;
        for (u32 i = 1; i <= SOCKS; i++)
            open += ns.sock[i].used;
        printf("{\"ev\":\"net\",\"what\":\"status\",\"frame\":%u,\"on\":%d,\"posts\":%u,\"replies\":%u,\"requests\":%u,"
               "\"sockets\":%d,\"waiting\":%d,\"unknown\":%u}\n", machine_frame, ns.on, ns.posts, ns.replies,
               ns.requests, open, ns.q_n + ns.flying, ns.unknown_ops + ns.unknown_posts);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(w, "selftest")) {
        char text[200];
        int r = nz_selftest(text, sizeof text);
        printf("{\"ev\":\"net\",\"what\":\"selftest\",\"ok\":%s,\"text\":\"%s\"}\n", r ? "false" : "true", text);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(w, "defaults")) {
        /* the script's settings of the board as at the start; then the servers' (net_server.c) */
        ns.create_fail = 0;
        ns.send_max = 0;
        ns.latency = 4000;
    }
    return net_server_command(tok, n, msg, msg_len);
bad:
    snprintf(msg, msg_len, "net %s: not an address (four numbers with dots)", w);
    return -1;
}

void hw_net_stats(FILE *f)
{
    if (!ns.on && !ns.posts)
        return;
    fprintf(f, "  network board: %u commands posted by the game, %u answered, %u requests of the board served, "
            "%u interrupts", ns.posts, ns.replies, ns.requests, ns.irq_raised);
    if (ns.unknown_ops + ns.unknown_posts)
        fprintf(f, ", %u without a model", ns.unknown_ops + ns.unknown_posts);
    fprintf(f, "\n    operations:");
    for (u32 op = 0; op < 0x18; op++)
        if (ns.ops[op])
            fprintf(f, " %s %u,", op_name(op), ns.ops[op]);
    fprintf(f, " window pages in use %u%s\n", ns.pages_used, ns.pages_refused ? " (some writes had no room)" : "");
    net_server_stats(f);
}
