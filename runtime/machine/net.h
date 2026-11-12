/* The network side of the DIMM board, without a network.
 *
 *   hw_dimm_net.c   the board: its command registers, the window of board
 *                   memory the game exchanges requests and data through,
 *                   the socket operations, the board's own requests to the
 *                   game, the two BIOS services the game's link code calls
 *   net_server.c    what stands behind the board: a name table, a host
 *                   table and a scripted stand-in for the operator's
 *                   servers that answers the game's HTTP requests with
 *                   canned replies
 *   net_zip.c       deflate, inflate, base64, the two checksums: what the
 *                   stand-in needs to read the game's packed request
 *                   bodies and to pack its replies
 *
 * Nothing here opens a socket, resolves a name or leaves the process in
 * any way: every "address" is a number in a table of this model.
 *
 * Off by default: with the model off the board answers nothing, as before,
 * and a run is the same run. --net on the command line or "net on" as the
 * first command of a script turns it on (docs/hle.md, "The DIMM board's
 * network side").
 */
#ifndef VF4_NET_H
#define VF4_NET_H

#include "sh4.h"
#include <stddef.h>
#include <stdio.h>

/* ---- the board (hw_dimm_net.c) ---- */

extern int hw_net_option;       /* --net: the model is on from the first instruction */

void hw_net_init(void);
/* after the BIOS stand-in is in place: applies --net */
void hw_net_boot(sh4 *c);
int hw_net_is_on(void);

/* the board's five registers at 0x005f703c-0x005f704c; 0 when the model is off */
int hw_net_reg_read(sh4 *c, u32 p, u32 *out);
int hw_net_reg_write(sh4 *c, u32 p, u32 v);
/* the window of board memory behind the cartridge port (offsets
 * 0x0f000000-0x0fffffff); 0 when the model is off or the offset is outside */
int hw_net_pio_read(u32 offset, u32 *out);
int hw_net_pio_write(u32 offset, u32 v);
int hw_net_block_read(u32 offset, u8 *dst, u32 n);
int hw_net_block_write(u32 offset, const u8 *src, u32 n);
/* a call that left the image: the two BIOS services of the link (0x58, 0x60) */
int hw_net_outside(sh4 *c, u32 target, int kind);
/* the controller's "net ..." command; 0 when done, else msg holds what is wrong */
int hw_net_command(sh4 *c, char **tok, int n, char *msg, size_t msg_len);
void hw_net_stats(FILE *f);

/* window memory for the server side and the socket operations */
void net_win_read(u32 addr, void *dst, u32 n);      /* addr: a DIMM address (0x0f......) */
void net_win_write(u32 addr, const void *src, u32 n);
/* virtual time */
u64 net_now(void);
u64 net_ms(u32 ms);                                 /* milliseconds as cycles */
/* one line about what happened: an event on standard output (when asked
 * for) and a note in the call log */
void net_say(const char *what, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- the stand-in for the servers (net_server.c) ---- */

enum { NET_HOST_ACCEPT = 0, NET_HOST_REFUSE, NET_HOST_UNREACHABLE, NET_HOST_SILENT };

/* errors, in the numbering the game's own time-out code (0x3c) belongs to */
enum { NET_ECONNRESET = 54, NET_ENOTCONN = 57, NET_ETIMEDOUT = 60, NET_ECONNREFUSED = 61, NET_EHOSTUNREACH = 65,
       NET_EBADF = 9, NET_EMFILE = 24, NET_EINVAL = 22, NET_EOPNOTSUPP = 45 };

#define NET_RX_MAX 0x2800
#define NET_TX_MAX 0x2000

typedef struct net_conn {
    u8 rx[NET_RX_MAX];          /* what the server has sent and the game has not taken yet */
    u32 rx_len, rx_pos;
    u8 tx[NET_TX_MAX];          /* what the game has sent and the server has not answered yet */
    u32 tx_len;
    u64 rx_at;                  /* the data is there from this cycle on */
    u32 rx_piece;               /* at most this many bytes per receive; 0: all there is */
    u32 rx_break;               /* a receive ends at this offset of rx (an interim response); 0: no such place */
    u64 rx_gap;                 /* cycles between two pieces */
    u64 close_at;               /* the server closes at this cycle; 0: it does not */
    u8 closed;                  /* the server has closed */
    u8 reset;                   /* ... by a reset: the next operation fails */
    u8 exchanges;               /* requests answered on this connection */
    u32 ip;
    u16 port;
} net_conn;

void net_server_init(void);
void net_server_defaults(void);
/* a name looked up: the address as the game takes it (first number of the
 * dotted form in the lowest byte), 0 when there is none; *delay_ms says how
 * long the answer takes */
u32 net_server_resolve(const char *name, u32 *delay_ms, int *fail);
/* a connection asked for: NET_HOST_*, and how long the answer takes */
int net_server_connect(u32 ip, u16 port, u32 *delay_ms);
/* the game has sent n more bytes (already appended to conn->tx): when a
 * whole request is there, the reply is put into conn->rx */
void net_server_sent(net_conn *conn);
/* the script: "dns", "host", "reply", "fail", "clock", "keepalive", "show" ... (tok[0] is the word after "net") */
int net_server_command(char **tok, int n, char *msg, size_t msg_len);
void net_server_stats(FILE *f);
u32 net_text_ip(const char *text, int *ok);
const char *net_ip_text(u32 ip);

/* ---- packing (net_zip.c) ---- */

enum { NZ_STORED = 0, NZ_FIXED = 1, NZ_DYNAMIC = 2 };

u32 nz_crc32(const u8 *p, u32 n);
u32 nz_adler32(const u8 *p, u32 n);
/* raw deflate streams; the length written, or -1 */
int nz_inflate(const u8 *in, u32 n, u8 *out, u32 cap);
int nz_deflate(const u8 *in, u32 n, u8 *out, u32 cap, int kind);
/* the same in a zlib wrapper (two header bytes, the Adler-32 at the end) */
int nz_zlib_unpack(const u8 *in, u32 n, u8 *out, u32 cap);
int nz_zlib_pack(const u8 *in, u32 n, u8 *out, u32 cap, int kind);
/* base64 with any 64-character alphabet; wrap > 0 breaks the text into
 * lines of that many characters, each ended with eol */
int nz_b64_encode(const u8 *in, u32 n, char *out, u32 cap, const char *alphabet, int wrap, const char *eol);
int nz_b64_decode(const char *in, u32 n, u8 *out, u32 cap, const char *alphabet);
extern const char nz_b64_standard[65];
/* round trips of every block kind and of base64; 0 when all hold */
int nz_selftest(char *msg, size_t msg_len);

#endif
