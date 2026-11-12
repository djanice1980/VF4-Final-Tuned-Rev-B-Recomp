/* The Maple bus host controller, the I/O bridge on its port 0 (the game's
 * library calls it the MIE) and one JVS I/O board behind the bridge.
 *
 * Why a model and not a replacement: every routine of sdk/libam/maple.c
 * builds its own frame and starts the transfer by hand, and everything the
 * game knows about switches, coins and the settings EEPROM comes through
 * those frames. With this model all of sdk/libam (maple.c, jamma.c,
 * eeprom.c) and sdk/nlajamma (jvs.c, jvs_replies.c, credit.c) run as the
 * game's own bytes.
 *
 * Sources: the frame layout is what the decompiled maple.c builds (first
 * word: last-frame bit, port, pattern, length; second: reply address;
 * third: command, recipient, sender, data words); the bridge's subcommands
 * and reply layouts are what jamma.c and eeprom.c send and read back; the
 * JVS commands are those of the public JAMMA Video Standard (the same ones
 * jvs_replies.c parses). The identification text the bridge answers with is
 * read from the game image at run time (the game compares against its own
 * copy).
 */
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "control.h"
#include "tables.h"
#include "observe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { JVS_FAULT_NONE, JVS_FAULT_ACK, JVS_FAULT_STATUS, JVS_FAULT_SILENT };

typedef struct maple_state {
    u32 frame_list, trigger, enable, timing, hard_trigger;
    /* a fault of the JVS board a script asked for (the controller's "jvs fault"): which, its value, and
     * the virtual frame it ends at (0: none; 0xffffffff: until "jvs fault off") */
    u32 fault_kind, fault_value, fault_until;
    u8 eeprom[0x80];
    /* the bridge */
    u8 frame_cmd[32][32];       /* per node: the command it repeats every frame */
    u8 frame_cmd_len[32];
    u8 reply[8][64];            /* per request id: the last JVS reply packet (reports only) */
    u8 reply_len[8];
    u8 reply_node[8];
    u8 reply_valid[8];
    u8 legacy_out[4];
    /* the JVS board */
    u8 node_addr;               /* 0 until addressed */
    u16 coin_count[2];
    u32 coins_seen[2];
    u32 transfers, jvs_packets;
} maple_state;

static maple_state ms;

#define JVS_ID "VF4DEBUG VIRTUAL I/O BOARD;JVS;Ver1.00"

/* the JVS board: answers one command packet; returns the number of report
 * bytes written to out (0: no answer) */
static int jvs_node(const u8 *cmd, int len, int dest, u8 *out)
{
    int n = 0;
    int i = 0;
    if (dest != 0xff && dest != ms.node_addr)
        return 0;
    while (i < len) {
        u8 c = cmd[i++];
        switch (c) {
        case 0xf0:                              /* bus reset */
            ms.node_addr = 0;
            return 0;
        case 0xf1:                              /* set address */
            if (ms.node_addr != 0)
                return 0;                       /* already addressed: the next board would answer */
            ms.node_addr = cmd[i++];
            out[n++] = 1;
            break;
        case 0x10:                              /* identification */
            out[n++] = 1;
            memcpy(out + n, JVS_ID, sizeof JVS_ID);
            n += (int)sizeof JVS_ID;
            break;
        case 0x11: out[n++] = 1; out[n++] = 0x13; break;    /* command format revision 1.3 */
        case 0x12: out[n++] = 1; out[n++] = 0x20; break;    /* JVS revision 2.0 */
        case 0x13: out[n++] = 1; out[n++] = 0x10; break;    /* communication version 1.0 */
        case 0x14:                              /* features */
            out[n++] = 1;
            out[n++] = 1; out[n++] = 2; out[n++] = 13; out[n++] = 0;    /* 2 players, 13 switches each */
            out[n++] = 2; out[n++] = 2; out[n++] = 0; out[n++] = 0;     /* 2 coin slots */
            out[n++] = 0x12; out[n++] = 6; out[n++] = 0; out[n++] = 0;  /* 6 output lines */
            out[n++] = 0;
            break;
        case 0x15:                              /* main board identification: a string */
            while (i < len && cmd[i])
                i++;
            i++;
            out[n++] = 1;
            break;
        case 0x20: {                            /* switches */
            int players = cmd[i++], bytes = cmd[i++];
            out[n++] = 1;
            out[n++] = (inputs.sys & SYS_TEST) ? 0x80 : 0;
            for (int p = 0; p < players; p++) {
                u32 b = p < 2 ? inputs.player[p] : 0;
                u8 b0 = 0, b1 = 0;
                if (b & BTN_START) b0 |= 0x80;
                if (p == 0 && (inputs.sys & SYS_SERVICE)) b0 |= 0x40;
                if (b & BTN_UP) b0 |= 0x20;
                if (b & BTN_DOWN) b0 |= 0x10;
                if (b & BTN_LEFT) b0 |= 0x08;
                if (b & BTN_RIGHT) b0 |= 0x04;
                if (b & BTN_P) b0 |= 0x02;      /* push 1: punch with the game's factory button setting */
                if (b & BTN_K) b0 |= 0x01;      /* push 2: kick */
                if (b & BTN_G) b1 |= 0x80;      /* push 3: guard */
                if (b & BTN_E) b1 |= 0x40;      /* push 4: the fourth button */
                for (int k = 0; k < bytes; k++)
                    out[n++] = k == 0 ? b0 : k == 1 ? b1 : 0;
            }
            break;
        }
        case 0x21: {                            /* coin counters */
            int slots = cmd[i++];
            out[n++] = 1;
            for (int s = 0; s < slots; s++) {
                if (s < 2) {
                    ms.coin_count[s] = (u16)((ms.coin_count[s] + (inputs.coins[s] - ms.coins_seen[s])) & 0x3fff);
                    ms.coins_seen[s] = inputs.coins[s];
                }
                u16 v = s < 2 ? ms.coin_count[s] : 0;
                out[n++] = (u8)(v >> 8);
                out[n++] = (u8)v;
            }
            break;
        }
        case 0x22: {                            /* analog inputs: none fitted, zeros */
            int ch = cmd[i++];
            out[n++] = 1;
            for (int k = 0; k < ch; k++) {
                out[n++] = 0;
                out[n++] = 0;
            }
            break;
        }
        case 0x30: {                            /* take coins off a counter */
            int slot = cmd[i++];
            u16 v = (u16)((cmd[i] << 8) | cmd[i + 1]);
            i += 2;
            if (slot >= 1 && slot <= 2)
                ms.coin_count[slot - 1] = (u16)((ms.coin_count[slot - 1] - v) & 0x3fff);
            out[n++] = 1;
            break;
        }
        case 0x31:                              /* add to a payout counter */
            i += 3;
            out[n++] = 1;
            break;
        case 0x32: {                            /* output lines */
            int bytes = cmd[i++];
            i += bytes;
            out[n++] = 1;
            break;
        }
        default:
            out[n++] = 2;                       /* unknown command: the parameter-error report */
            return n;
        }
    }
    return n;
}

/* one exchange on the JVS bus, kept for request id until it is asked for */
static void bridge_exchange(int id, int node, const u8 *cmd, int len)
{
    id &= 7;
    ms.jvs_packets++;
    ms.reply_valid[id] = 1;
    ms.reply_node[id] = (u8)node;
    ms.reply_len[id] = (u8)jvs_node(cmd, len, node, ms.reply[id]);
}

static int bridge_86(const u8 *d, int nbytes, u8 *reply)
{
    u8 sub = d[0];
    int out_len = 8;
    u32 head = 0x00002086;                      /* any word that is neither -1 nor a resend request */
    memcpy(reply, &head, 4);
    reply[4] = sub;
    reply[5] = 0;                               /* status: fine */
    switch (sub) {
    case 0x01:                                  /* EEPROM: read request */
        break;
    case 0x03:                                  /* EEPROM: the image */
        memcpy(reply + 4, ms.eeprom, 0x80);
        out_len = 4 + 0x80;
        break;
    case 0x0b: {                                /* EEPROM: write a block */
        int off = d[1], len = d[2];
        if (off + len <= 0x80 && 4 + len <= nbytes)
            memcpy(ms.eeprom + off, d + 4, (size_t)len);
        break;
    }
    case 0x13: {                                /* a node's every-frame command */
        int node = d[1] & 31, len = d[2];
        if (len > 32) len = 32;
        ms.frame_cmd_len[node] = (u8)len;
        memcpy(ms.frame_cmd[node], d + 3, (size_t)len);
        break;
    }
    case 0x17:                                  /* send to a node now */
    case 0x21: {                                /* send to a node, its every-frame command after it */
        u8 cmd[64];
        int id = d[5], node = d[6], len = d[7];
        memcpy(ms.legacy_out, d + 1, 4);
        if (len > 12) len = 12;
        memcpy(cmd, d + 8, (size_t)len);
        if (sub == 0x21 && node < 32) {
            memcpy(cmd + len, ms.frame_cmd[node], ms.frame_cmd_len[node]);
            len += ms.frame_cmd_len[node];
        }
        bridge_exchange(id, node, cmd, len);
        break;
    }
    case 0x31:                                  /* the old I/O port: 16 input bytes back */
        memcpy(ms.legacy_out, d + 1, 4);
        memset(reply + 5, 0xff, 16);            /* nothing closed; the board's DIP switches all off */
        out_len = 0x18;
        break;
    case 0x15: {                                /* fetch a node's reply */
        int id = d[1] & 7;
        memset(reply + 5, 0xff, 16);
        u8 *p = reply + 0x15;
        int n = ms.reply_valid[id] ? ms.reply_len[id] : 0;
        u32 fault = ms.fault_until && machine_frame < ms.fault_until ? ms.fault_kind : 0;
        if (fault == JVS_FAULT_SILENT)
            n = 0;                              /* the board sent nothing back */
        p[0] = (u8)id;                          /* request id */
        p[1] = ms.node_addr ? 0x8e : 0x8f;      /* sense line: every board has its address */
        p[2] = ms.reply_node[id] == 0xff ? ms.node_addr : ms.reply_node[id];
        p[3] = fault == JVS_FAULT_ACK ? (u8)ms.fault_value : 0;         /* acknowledge status */
        p[4] = (u8)n;                           /* report bytes */
        p[5] = 0xe0;                            /* sync */
        p[6] = 0;                               /* to the host */
        p[7] = (u8)(n + 2);                     /* length: status, reports, sum */
        p[8] = fault == JVS_FAULT_STATUS ? (u8)ms.fault_value : 1;      /* status: normal */
        memcpy(p + 9, ms.reply[id], (size_t)n);
        ms.reply_valid[id] = 0;
        out_len = (0x15 + 9 + n + 3) & ~3;
        break;
    }
    default:
        break;
    }
    return out_len;
}

static void maple_frame(sh4 *c, u32 w0, u32 reply_addr, const u32 *frame, int data_words)
{
    (void)c;
    u32 port = (w0 >> 16) & 3, pattern = (w0 >> 8) & 7;
    u32 cmd = frame[0] & 0xff, to = (frame[0] >> 8) & 0xff;
    u8 reply[0x100];
    memset(reply, 0, sizeof reply);
    ms.transfers++;
    const u8 *d = (const u8 *)(frame + 1);
    int nbytes = data_words * 4;
    int reply_bytes = 4;
    /* ports 1 and 2: the card units (hw_card.c), when the cabinet has them */
    if (port == 1 || port == 2) {
        reply_bytes = hw_card_frame((int)port - 1, pattern, cmd, to, (frame[0] >> 16) & 0xff, d, nbytes, reply);
        if (reply_bytes > 0)
            g_write(reply_addr, reply, (u32)reply_bytes);
        return;
    }
    /* else only the bridge on port 0 answers; a frame nothing answers leaves
     * the all-ones word the game put in the reply buffer */
    if (port != 0 || pattern != 0 || to != 0x20)
        return;
    switch (cmd) {
    case 0x01:                                  /* device request */
    case 0x03: {                                /* reset */
        u32 head = 0x00002007;
        memcpy(reply, &head, 4);
        reply_bytes = 4;
        break;
    }
    case 0x82: {                                /* identification: the text the game expects */
        u32 a = global_addr("mie_ident_expected");
        if (a)
            g_read(a, reply, 0x38);
        reply_bytes = 0x48;
        break;
    }
    case 0x80: {                                /* program upload: the sum of the frame's bytes back */
        u32 head = 0x00002007, sum = 0;
        for (int i = 0; i < nbytes; i++)
            sum += d[i];
        memcpy(reply, &head, 4);
        reply[4] = (u8)sum;
        reply_bytes = 8;
        break;
    }
    case 0x86:
        reply_bytes = bridge_86(d, nbytes, reply);
        break;
    default: {
        u32 head = 0x00002007;
        memcpy(reply, &head, 4);
        reply_bytes = 4;
        break;
    }
    }
    g_write(reply_addr, reply, (u32)reply_bytes);
}

static void maple_dma(sh4 *c)
{
    u32 a = ms.frame_list;
    for (int guard = 0; guard < 64; guard++) {
        u32 w0 = g_r32(a), reply_addr = g_r32(a + 4);
        int data_words = (int)(w0 & 0xff);
        u32 frame[1 + 255];
        g_read(a + 8, frame, 4u * (u32)(1 + data_words));
        maple_frame(c, w0, reply_addr, frame, data_words);
        a += 8 + 4 * (u32)(1 + data_words);
        if (w0 & 0x80000000u)
            break;
    }
    hw_raise(c, IRQ_MAPLE_DMA_END);
}

int hw_maple_read(sh4 *c, u32 p, u32 *out)
{
    (void)c;
    switch (p) {
    case 0x005f6c04: *out = ms.frame_list; return 1;
    case 0x005f6c10: *out = ms.trigger; return 1;
    case 0x005f6c14: *out = ms.enable; return 1;
    case 0x005f6c18: *out = 0; return 1;        /* a transfer is over as soon as it is started */
    case 0x005f6c80: *out = ms.timing; return 1;
    case 0x005f6c8c: *out = ms.hard_trigger; return 1;
    }
    return 0;
}

int hw_maple_write(sh4 *c, u32 p, u32 v)
{
    switch (p) {
    case 0x005f6c04: ms.frame_list = v; return 1;
    case 0x005f6c10: ms.trigger = v; return 1;
    case 0x005f6c14: ms.enable = v; return 1;
    case 0x005f6c18:
        if ((v & 1) && (ms.enable & 1))
            maple_dma(c);
        return 1;
    case 0x005f6c80: ms.timing = v; return 1;
    case 0x005f6c8c: ms.hard_trigger = v; return 1;
    }
    return 0;
}

/* The controller's "jvs": faults of the I/O board a script can switch on.
 *
 *   jvs fault ack VALUE [FRAMES]     the acknowledge status byte of every reply the game fetches is VALUE
 *                                    (the game's jvs_node_error_check counts a frame with it set as bad)
 *   jvs fault status VALUE [FRAMES]  the reply's status byte is VALUE instead of 1 ("normal")
 *   jvs fault silent [FRAMES]        the fetch brings no report bytes: the board did not answer
 *   jvs fault off
 *
 * Where each byte stands in the reply the bridge hands back is in bridge_86 (sub-command 0x15); what
 * the game makes of it is its own code's business (vf4lib/libbase/src/input.c, sdk/nlajamma). */
int hw_maple_command(char **tok, int n, char *msg, size_t len)
{
    if (n >= 2 && !strcmp(tok[0], "fault")) {
        u32 frames = 0xffffffffu;
        if (!strcmp(tok[1], "off")) {
            ms.fault_kind = ms.fault_until = 0;
            return 0;
        }
        if (!strcmp(tok[1], "silent")) {
            ms.fault_kind = JVS_FAULT_SILENT;
            if (n > 2)
                frames = (u32)strtoul(tok[2], NULL, 0);
        } else if (n >= 3 && (!strcmp(tok[1], "ack") || !strcmp(tok[1], "status"))) {
            ms.fault_kind = tok[1][0] == 'a' ? JVS_FAULT_ACK : JVS_FAULT_STATUS;
            ms.fault_value = (u32)strtoul(tok[2], NULL, 0);
            if (n > 3)
                frames = (u32)strtoul(tok[3], NULL, 0);
        } else {
            snprintf(msg, len, "jvs fault ack VALUE [FRAMES] | status VALUE [FRAMES] | silent [FRAMES] | off");
            return -1;
        }
        ms.fault_until = frames == 0xffffffffu ? frames : machine_frame + frames;
        char note[100];
        snprintf(note, sizeof note, "the I/O board model: fault %s %u until frame %u", tok[1], ms.fault_value, ms.fault_until);
        obs_note(note);
        return 0;
    }
    snprintf(msg, len, "jvs fault ack VALUE [FRAMES] | status VALUE [FRAMES] | silent [FRAMES] | off");
    return -1;
}

/* the settings EEPROM's 128 bytes (for the controller's "backup") */
u8 *hw_maple_eeprom(void)
{
    return ms.eeprom;
}

void hw_maple_init(void)
{
    memset(&ms, 0, sizeof ms);
    memset(ms.eeprom, 0xff, sizeof ms.eeprom);  /* a blank part: the game writes its defaults */
    snapshot_register("maple", &ms, sizeof ms);
    hw_card_init();
}
