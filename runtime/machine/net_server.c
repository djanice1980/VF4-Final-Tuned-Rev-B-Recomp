/* What is behind the network board: a name table, a host table and a
 * scripted stand-in for the operator's servers. All of it lives in this
 * process; nothing is looked up or connected to anywhere.
 *
 * The game makes three kinds of HTTP exchange, told apart here by the path
 * it posts to:
 *
 *   "poweron"   sdk/alab: POST .../PowerOn, the authentication. The body
 *               is a form text, packed (zlib, then base64) when the request
 *               says "Pragma: DFI" (naominet_poweron_body_build,
 *               naominet_poweron_request_build). The reply is checked by
 *               naominet_poweron_reply_check and read by
 *               naominet_poweron_reply_parse.
 *   "score.N"   sdk/alab: POST /game.php, the play reports (N is the
 *               request's zk: 1 after authentication, 2 a player starts,
 *               3 continues, 4 ends). The body is "SDF=" and a raw deflate
 *               stream in base64 with the game's own alphabet
 *               (naominet_score_body_build); the reply is checked by
 *               naominet_score_reply_check.
 *   the game's own requests (vf4lib/libnetwork through libnl2http): POST
 *               <uri>VF4NetUpload with a packed form text that begins
 *               cmd=NAME; the key is that name: "power_on", "start",
 *               "end", "message", "select". The reply's header x-rd2-cmd
 *               says which parser gets the body (net_reply_dispatch).
 *
 * For each key there is a reply written for this harness (reply_defaults
 * below; none of it is captured traffic), and the script can change any
 * part of it, make it fail, or make it slow: "net reply KEY ...".
 * docs/hle.md lists the exchanges, the default replies and what in the
 * game's code each part of them answers to.
 */
#include "net.h"
#include "hw.h"
#include "mem.h"
#include "machine.h"
#include "tables.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define NAMES 12
#define HOSTS 12
#define RULES 24
#define BODY_MAX 3000
#define SEEN 16

enum { PACK_DEFAULT = 0, PACK_NONE, PACK_STORED, PACK_FIXED, PACK_DYNAMIC, PACK_BAD };
enum { FRAME_DEFAULT = 0, FRAME_LENGTH, FRAME_CHUNKED, FRAME_CLOSE };
enum { FAIL_NONE = 0, FAIL_DROP, FAIL_HANG, FAIL_RESET, FAIL_GARBAGE, FAIL_CUT };
enum { KEEP_DEFAULT = 0, KEEP_CLOSE, KEEP_OPEN };
enum { KIND_POWERON = 0, KIND_SCORE, KIND_UPLOAD, KIND_OTHER };

typedef struct rule {
    u8 used;
    u8 has_status, has_body, has_cmd, has_http;
    u8 pack, framing, fail, keep;
    s32 times;                  /* -1: stands until changed; else: for this many requests more */
    u32 status;
    u32 delay_ms, piece, gap_ms, chunk;
    s32 wrap;                   /* base64 line length; 0: the default (76), -1: one line */
    char key[24];
    char reason[32];
    char http[8];
    char cmd[24];
    char headers[200];
    char body[BODY_MAX];
} rule;

static struct server_state {
    struct {
        u8 used, mode;          /* 0: answers; 1: no such name; 2: the lookup fails */
        u32 ip, delay_ms;
        char name[64];
    } names[NAMES];
    struct {
        u8 used, how;
        u32 ip, delay_ms;
    } hosts[HOSTS];
    rule rules[RULES];
    s32 clock_offset;           /* the servers' clock against the board's, in seconds */
    u32 keepalive_ms;           /* how long a server keeps an idle connection; 0: for ever */
    u32 upload_ip;
    struct {
        char key[24];
        u32 count;
    } seen[SEEN];
    u32 lookups, connects, requests, undecoded, crc_bad;
} sv;

/* ---- the replies written for this harness ----
 *
 * {req:NAME} is the value NAME has in the request; {pair:NAME} is
 * "NAME=value&" when the request has NAME; {clock} is the servers' date
 * and time in the six fields the authentication reply carries; {upload}
 * is the address the game's own requests go to. */
static const struct {
    const char *key;
    const char *body;
} reply_defaults[] = {
    /* naominet_poweron_reply_check wants "stat=" not followed by '-';
     * naominet_poweron_reply_parse takes uri, host, place_id (four hex
     * digits), name, nickname (a '#' splits off a tag), the date, setting,
     * region0 and four region names, and undoes %XX in the names */
    { "poweron", "stat=1&uri={upload}&host=vf4.harness.invalid&place_id=1A2B&name=HARNESS%20ARCADE&"
                 "nickname=VF4DEBUG%23ONE&{clock}&setting=1&region0=1&region_name0=NORTH&region_name1=EAST&"
                 "region_name2=SOUTH&region_name3=WEST" },
    /* naominet_score_reply_check: st=OK, then bn must be the board's
     * serial and ti its address text, as the request gave them */
    { "score", "st=OK&bn={req:bn}&ti={req:ti}" },
    /* net_power_on_reply_parse: the game's settings from the server. All
     * neutral here, and db_stat=1, without which the game does not take
     * them and never leaves "requesting game initialize" */
    { "power_on", "rare=0,0,0,0&syuchu=0,0,0&kumite=&nushi=0&masa=0&ryuko=0&"
                  "struggle_revise=0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0&nickname_revise=0,0,0,0,0,0,0&ticket_get=0&"
                  "db_stat=1" },
    /* net_start_reply_parse and net_end_reply_parse: the id must be the
     * play id the request carried */
    { "start", "id={req:id}&db_stat=1" },
    { "end", "id={req:id}&db_stat=1" },
    /* net_message_reply_take: the body is the text shown in the attract sequence */
    { "message", "THE NETWORK IS A STAND-IN INSIDE VF4DEBUG" },
    /* net_select_reply_store: the access code must come back; the rest is optional */
    { "select", "{pair:access_code_1p}{pair:access_code_2p}db_stat=1" },
    { "upload", "db_stat=1" },
    { "other", "not here" },
};

/* ---- addresses ---- */

u32 net_text_ip(const char *text, int *ok)
{
    unsigned a, b, c, d;
    char tail;
    *ok = sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
    /* as the game keeps an address: the first number in the lowest byte (ip_text_parse) */
    return *ok ? a | (b << 8) | (c << 16) | (d << 24) : 0;
}

const char *net_ip_text(u32 ip)
{
    static char ring[4][20];
    static int at;
    char *b = ring[at++ & 3];
    snprintf(b, sizeof ring[0], "%u.%u.%u.%u", ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24);
    return b;
}

static void name_set(const char *name, u32 ip, int mode, u32 delay_ms)
{
    int slot = -1;
    for (int i = NAMES - 1; i >= 0; i--)
        if (!sv.names[i].used)
            slot = i;
    for (int i = 0; i < NAMES; i++)
        if (sv.names[i].used && !strcmp(sv.names[i].name, name))
            slot = i;
    if (slot < 0)
        return;
    sv.names[slot].used = 1;
    sv.names[slot].mode = (u8)mode;
    sv.names[slot].ip = ip;
    sv.names[slot].delay_ms = delay_ms;
    snprintf(sv.names[slot].name, sizeof sv.names[slot].name, "%s", name);
}

static void host_set(u32 ip, int how, u32 delay_ms)
{
    int slot = -1;
    for (int i = HOSTS - 1; i >= 0; i--)
        if (!sv.hosts[i].used)
            slot = i;
    for (int i = 0; i < HOSTS; i++)
        if (sv.hosts[i].used && sv.hosts[i].ip == ip)
            slot = i;
    if (slot < 0)
        return;
    sv.hosts[slot].used = 1;
    sv.hosts[slot].how = (u8)how;
    sv.hosts[slot].ip = ip;
    sv.hosts[slot].delay_ms = delay_ms;
}

u32 net_server_resolve(const char *name, u32 *delay_ms, int *fail)
{
    sv.lookups++;
    *delay_ms = 1;
    *fail = 0;
    for (int i = 0; i < NAMES; i++)
        if (sv.names[i].used && !strcmp(sv.names[i].name, name)) {
            *delay_ms = sv.names[i].delay_ms;
            *fail = sv.names[i].mode == 2;
            return sv.names[i].mode ? 0 : sv.names[i].ip;
        }
    return 0;
}

int net_server_connect(u32 ip, u16 port, u32 *delay_ms)
{
    sv.connects++;
    *delay_ms = 1;
    for (int i = 0; i < HOSTS; i++)
        if (sv.hosts[i].used && sv.hosts[i].ip == ip) {
            *delay_ms = sv.hosts[i].delay_ms;
            if (sv.hosts[i].how == NET_HOST_ACCEPT && port != 80)
                return NET_HOST_REFUSE;         /* the servers listen on the HTTP port only */
            return sv.hosts[i].how;
        }
    return NET_HOST_UNREACHABLE;
}

/* ---- the servers' clock ---- */

static void clock_fields(s32 offset, int f[6])
{
    /* the board's clock counts seconds from the start of 1950 (hw.c) */
    u64 t = (u64)hw.rtc_base + net_now() / CPU_HZ + (u64)(s64)offset;
    u32 days = (u32)(t / 86400), rest = (u32)(t % 86400);
    u32 year = 1950;
    for (;;) {
        u32 len = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
        if (days < len)
            break;
        days -= len;
        year++;
    }
    static const u8 month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    u32 month = 0;
    for (;;) {
        u32 len = month_days[month] + (month == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (days < len)
            break;
        days -= len;
        month++;
    }
    f[0] = (int)year;
    f[1] = (int)month + 1;
    f[2] = (int)days + 1;
    f[3] = (int)(rest / 3600);
    f[4] = (int)(rest / 60 % 60);
    f[5] = (int)(rest % 60);
}

static s64 date_seconds(int y, int mo, int d, int h, int mi, int s)
{
    static const u16 before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    s64 days = 0;
    for (int year = 1950; year < y; year++)
        days += (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
    days += before[mo - 1] + (mo > 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) + (d - 1);
    return ((days * 24 + h) * 60 + mi) * 60 + s;
}

/* ---- reading a request ---- */

typedef struct request {
    char method[12], path[160], version[8];
    const char *head;           /* the header lines */
    u32 head_len;
    const u8 *body;
    u32 body_len;
    int kind;
    char key[24], generic[24];
    char text[3000];            /* the body as the game wrote it before packing */
    int decoded;
} request;

static const char *header_find(const request *q, const char *name, u32 *len)
{
    size_t nl = strlen(name);
    const char *p = q->head, *end = q->head + q->head_len;
    while (p < end) {
        const char *eol = memchr(p, '\r', (size_t)(end - p));
        if (!eol)
            eol = end;
        if ((size_t)(eol - p) > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            const char *v = p + nl + 1;
            while (v < eol && *v == ' ')
                v++;
            *len = (u32)(eol - v);
            return v;
        }
        p = eol + 2;
    }
    return NULL;
}

static int header_has(const request *q, const char *name, const char *word)
{
    u32 len = 0;
    const char *v = header_find(q, name, &len);
    size_t wl = strlen(word);
    for (u32 i = 0; v && i + wl <= len; i++)
        if (!strncasecmp(v + i, word, wl))
            return 1;
    return 0;
}

/* the value of a field of a form text ("a=1&b=2"), as it stands there */
static int form_value(const char *text, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = text; *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > nl && p[nl] == '=' && !strncmp(p, name, nl)) {
            size_t vl = len - nl - 1;
            while (vl && (p[nl + vl] == '\r' || p[nl + vl] == '\n'))
                vl--;
            if (vl >= cap)
                vl = cap - 1;
            memcpy(out, p + nl + 1, vl);
            out[vl] = 0;
            return 1;
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    out[0] = 0;
    return 0;
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

/* the game's own base64 alphabet for the play reports, from the image */
static int score_alphabet(char *out)
{
    u32 a = global_addr("b64_score_alphabet");
    if (!a || g_read(a, out, 64))
        return -1;
    out[64] = 0;
    for (int i = 0; i < 64; i++)
        if (!out[i] || memchr(out, out[i], (size_t)i))
            return -1;
    return 0;
}

static void request_decode(request *q)
{
    static u8 packed[4200];
    char zk[8];
    int n = -1, len = -1;
    q->decoded = 0;
    q->text[0] = 0;
    if (q->kind == KIND_SCORE) {
        /* "SDF=" and base64 in the game's alphabet with + / = written as
         * %2B %2F %3D; inside, a deflate stream without the zlib wrapper */
        static char b64[2200];
        char alphabet[65];
        u32 k = 0;
        if (q->body_len > 4 && !memcmp(q->body, "SDF=", 4) && !score_alphabet(alphabet)) {
            for (u32 i = 4; i < q->body_len && k + 1 < sizeof b64; i++) {
                if (q->body[i] == '%' && i + 2 < q->body_len) {
                    char hex[3] = { (char)q->body[i + 1], (char)q->body[i + 2], 0 };
                    b64[k++] = (char)strtoul(hex, NULL, 16);
                    i += 2;
                } else {
                    b64[k++] = (char)q->body[i];
                }
            }
            n = nz_b64_decode(b64, k, packed, sizeof packed, alphabet);
            if (n > 0)
                len = nz_inflate(packed, (u32)n, (u8 *)q->text, sizeof q->text - 1);
        }
    } else if (header_has(q, "Pragma", "DFI")) {
        n = nz_b64_decode((const char *)q->body, q->body_len, packed, sizeof packed, nz_b64_standard);
        if (n > 0)
            len = nz_zlib_unpack(packed, (u32)n, (u8 *)q->text, sizeof q->text - 1);
    } else {
        len = (int)(q->body_len < sizeof q->text - 1 ? q->body_len : sizeof q->text - 1);
        memcpy(q->text, q->body, (size_t)len);
    }
    if (len < 0) {
        sv.undecoded++;
        q->text[0] = 0;
    } else {
        q->text[len] = 0;
        q->decoded = 1;
    }
    if (q->kind == KIND_UPLOAD && q->decoded) {
        /* the game sends the CRC-32 of the body before packing: a check of this reader */
        u32 hl = 0;
        const char *h = header_find(q, "x-rd2-crc", &hl);
        if (h && strtoul(h, NULL, 16) != nz_crc32((const u8 *)q->text, (u32)len))
            sv.crc_bad++;
    }
    switch (q->kind) {
    case KIND_POWERON:
        snprintf(q->key, sizeof q->key, "poweron");
        snprintf(q->generic, sizeof q->generic, "poweron");
        break;
    case KIND_SCORE:
        form_value(q->text, "zk", zk, sizeof zk);
        snprintf(q->key, sizeof q->key, "score.%.4s", zk[0] ? zk : "0");
        snprintf(q->generic, sizeof q->generic, "score");
        break;
    case KIND_UPLOAD:
        form_value(q->text, "cmd", q->key, sizeof q->key);
        if (!q->key[0])
            snprintf(q->key, sizeof q->key, "upload");
        snprintf(q->generic, sizeof q->generic, "upload");
        break;
    default:
        snprintf(q->key, sizeof q->key, "other");
        snprintf(q->generic, sizeof q->generic, "other");
        break;
    }
}

/* ---- writing a reply ---- */

static const rule *rule_for(const request *q, int *index, int final_only)
{
    *index = -1;
    for (int i = 0; i < RULES; i++) {
        if (final_only && sv.rules[i].has_status && sv.rules[i].status == 100)
            continue;
        if (sv.rules[i].used && sv.rules[i].times > 0 &&
            (!strcmp(sv.rules[i].key, q->key) || !strcmp(sv.rules[i].key, q->generic))) {
            *index = i;
            return &sv.rules[i];
        }
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < RULES; i++) {
            if (final_only && sv.rules[i].has_status && sv.rules[i].status == 100)
                continue;
            if (sv.rules[i].used && sv.rules[i].times < 0 && !strcmp(sv.rules[i].key, pass ? q->generic : q->key))
                return &sv.rules[i];
        }
    return NULL;
}

static const char *default_body(const request *q)
{
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < sizeof reply_defaults / sizeof reply_defaults[0]; i++)
            if (!strcmp(reply_defaults[i].key, pass ? q->generic : q->key))
                return reply_defaults[i].body;
    return "";
}

static void expand(const char *tpl, const request *q, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    while (*tpl && n + 1 < cap) {
        const char *close = *tpl == '{' ? strchr(tpl, '}') : NULL;
        char word[64], value[400];
        if (!close || (size_t)(close - tpl) >= sizeof word) {
            out[n++] = *tpl++;
            continue;
        }
        memcpy(word, tpl + 1, (size_t)(close - tpl - 1));
        word[close - tpl - 1] = 0;
        value[0] = 0;
        if (!strncmp(word, "req:", 4)) {
            form_value(q->text, word + 4, value, sizeof value);
        } else if (!strncmp(word, "pair:", 5)) {
            char v[200];
            if (form_value(q->text, word + 5, v, sizeof v))
                snprintf(value, sizeof value, "%s=%s&", word + 5, v);
        } else if (!strcmp(word, "clock")) {
            int f[6];
            clock_fields(sv.clock_offset, f);
            snprintf(value, sizeof value, "year=%d&month=%d&day=%d&hour=%d&minute=%d&second=%d", f[0], f[1], f[2],
                     f[3], f[4], f[5]);
        } else if (!strcmp(word, "upload")) {
            snprintf(value, sizeof value, "http://%s/vf4/", net_ip_text(sv.upload_ip));
        } else if (!strcmp(word, "cmd")) {
            snprintf(value, sizeof value, "%s", q->key);
        } else {
            out[n++] = *tpl++;          /* not one of ours: a brace of the text */
            continue;
        }
        size_t vl = strlen(value);
        if (vl > cap - 1 - n)
            vl = cap - 1 - n;
        memcpy(out + n, value, vl);
        n += vl;
        tpl = close + 1;
    }
    out[n] = 0;
}

static void seen_count(const char *key)
{
    for (int i = 0; i < SEEN; i++) {
        if (!sv.seen[i].key[0])
            snprintf(sv.seen[i].key, sizeof sv.seen[i].key, "%s", key);
        if (!strcmp(sv.seen[i].key, key)) {
            sv.seen[i].count++;
            return;
        }
    }
}

static void reply_build(net_conn *conn, request *q, int final_only)
{
    static char body[BODY_MAX + 1200], packed_text[7000], head[1200];
    static u8 packed[5000];
    static const rule plain_rule;
    int index;
    const rule *r = rule_for(q, &index, final_only);
    u64 now = net_now();
    if (!r)
        r = &plain_rule;
    if (r->has_status && r->status == 100) {
        /* an interim response: "go on" by itself, and the reply proper (the
         * next rule's, or the built-in one) in a receive of its own, which is
         * how the client reads it (http_client_recv_step empties its buffer
         * after a 100) */
        char interim[80];
        u32 n = (u32)snprintf(interim, sizeof interim, "HTTP/1.1 100 %s\r\n\r\n", r->reason);
        if (index >= 0 && --sv.rules[index].times == 0)
            sv.rules[index].used = 0;
        reply_build(conn, q, 1);
        if (conn->rx_len + n <= NET_RX_MAX) {
            memmove(conn->rx + n, conn->rx, conn->rx_len);
            memcpy(conn->rx, interim, n);
            conn->rx_len += n;
            conn->rx_break = n;
        }
        return;
    }
    u32 delay = r->delay_ms ? r->delay_ms : 2;
    int http10 = !strcmp(q->version, "1.0");
    int will_close = r->keep == KEEP_CLOSE ||
                     (r->keep != KEEP_OPEN && (http10 || header_has(q, "Connection", "close")));

    seen_count(q->key);
    conn->exchanges++;
    conn->rx_len = conn->rx_pos = 0;
    conn->rx_break = 0;
    conn->rx_piece = r->piece;
    conn->rx_gap = net_ms(r->gap_ms ? r->gap_ms : 1);
    conn->rx_at = now + net_ms(delay);
    conn->close_at = 0;

    if (r->fail == FAIL_HANG) {
        net_say("reply", "%s: the server says nothing", q->key);
        goto used;
    }
    if (r->fail == FAIL_DROP) {
        conn->close_at = now + net_ms(delay);
        net_say("reply", "%s: the server closes without a word", q->key);
        goto used;
    }
    if (r->fail == FAIL_RESET) {
        conn->reset = 1;
        net_say("reply", "%s: the connection is reset", q->key);
        goto used;
    }
    if (r->fail == FAIL_GARBAGE) {
        static const char junk[] = "220 this is not the protocol you are looking for\r\n\r\n";
        memcpy(conn->rx, junk, sizeof junk - 1);
        conn->rx_len = sizeof junk - 1;
        conn->close_at = conn->rx_at;
        net_say("reply", "%s: the server answers with something that is not HTTP", q->key);
        goto used;
    }

    /* the body, then its packing */
    expand(r->has_body ? r->body : default_body(q), q, body, BODY_MAX + 1000);
    u32 body_len = (u32)strlen(body);
    int pack = r->pack;
    if (pack == PACK_DEFAULT)
        pack = q->kind == KIND_OTHER ? PACK_NONE : PACK_FIXED;
    const char *sent = body;
    u32 sent_len = body_len;
    if (pack != PACK_NONE) {
        int kind = pack == PACK_STORED ? NZ_STORED : pack == PACK_DYNAMIC ? NZ_DYNAMIC : NZ_FIXED;
        int n = nz_zlib_pack((const u8 *)body, body_len, packed, sizeof packed, kind), t = -1;
        if (n > 0 && pack == PACK_BAD) {
            /* a stream the game's inflate must refuse: a block type that does not exist */
            packed[2] |= 0x06;
        }
        if (n > 0 && q->kind == KIND_SCORE) {
            char alphabet[65];
            if (!score_alphabet(alphabet))
                t = nz_b64_encode(packed, (u32)n, packed_text, sizeof packed_text, alphabet, 0, "");
        } else if (n > 0) {
            /* the authentication reply is read line by line, each line a
             * whole number of base64 groups (naominet_poweron_reply_check);
             * the game's own requests ignore the line ends (base64_decode) */
            int wrap = r->wrap < 0 ? 0 : r->wrap > 0 && r->wrap % 4 == 0 ? r->wrap : 76;
            t = nz_b64_encode(packed, (u32)n, packed_text, sizeof packed_text - 4, nz_b64_standard, wrap, "\r\n");
            if (t >= 0) {
                memcpy(packed_text + t, "\r\n", 3);
                t += 2;
            }
        }
        if (t < 0) {
            net_say("reply", "%s: the reply could not be packed (too long, or the game's alphabet was not found)",
                    q->key);
            t = 0;
        }
        sent = packed_text;
        sent_len = (u32)t;
    }

    int framing = r->framing ? r->framing : FRAME_LENGTH;
    u32 status = r->has_status ? r->status : q->kind == KIND_OTHER ? 404u : 200u;
    const char *reason = r->has_status ? r->reason : q->kind == KIND_OTHER ? "Not Found" : "OK";
    u32 hn = 0;
#define HEAD(...) \
    do { \
        if (hn < sizeof head - 1) \
            hn += (u32)snprintf(head + hn, sizeof head - hn, __VA_ARGS__); \
    } while (0)
    HEAD("HTTP/%s %u %s\r\n", r->has_http ? r->http : "1.1", status, reason);
    if (q->kind == KIND_UPLOAD && !(r->has_cmd && !strcmp(r->cmd, "-")))
        HEAD("x-rd2-cmd: %s\r\n", r->has_cmd ? r->cmd : q->key);
    if (pack != PACK_NONE && q->kind != KIND_SCORE)
        HEAD("Pragma: DFI\r\n");
    HEAD("Content-Type: text/plain\r\n");
    if (framing == FRAME_LENGTH)
        HEAD("Content-Length: %u\r\n", sent_len);
    else if (framing == FRAME_CHUNKED)
        HEAD("Transfer-Encoding: chunked\r\n");
    if (will_close || framing == FRAME_CLOSE)
        HEAD("Connection: close\r\n");
    for (const char *h = r->headers; *h;) {
        /* the script's own header lines, kept as "Name: value" joined by a newline */
        const char *e = strchr(h, '\n');
        size_t l = e ? (size_t)(e - h) : strlen(h);
        HEAD("%.*s\r\n", (int)l, h);
        h += l + (e ? 1 : 0);
    }
    HEAD("\r\n");
#undef HEAD
    if (hn >= sizeof head)
        hn = sizeof head - 1;

    u32 at = 0;
    memcpy(conn->rx, head, hn);
    at = hn;
    if (framing == FRAME_CHUNKED) {
        /* one chunk unless the script asks for smaller ones: the client's
         * chunk reader adds every chunk's length to the one before
         * (http_client_chunk_size_parse), so that only the first is read right */
        u32 size = r->chunk ? r->chunk : sent_len ? sent_len : 1;
        for (u32 i = 0; i < sent_len && at + size + 16 < NET_RX_MAX; i += size) {
            u32 piece = sent_len - i < size ? sent_len - i : size;
            at += (u32)snprintf((char *)conn->rx + at, NET_RX_MAX - at, "%x\r\n", piece);
            memcpy(conn->rx + at, sent + i, piece);
            at += piece;
            memcpy(conn->rx + at, "\r\n", 2);
            at += 2;
        }
        memcpy(conn->rx + at, "0\r\n\r\n", 5);
        at += 5;
    } else {
        if (sent_len > NET_RX_MAX - at)
            sent_len = NET_RX_MAX - at;
        memcpy(conn->rx + at, sent, sent_len);
        at += sent_len;
    }
    if (r->fail == FAIL_CUT)
        at = hn + (at - hn) / 2;                /* the connection breaks half way through the body */
    conn->rx_len = at;
    u64 last = conn->rx_at;
    if (conn->rx_piece)
        last += (u64)(at / conn->rx_piece) * conn->rx_gap;
    if (will_close || framing == FRAME_CLOSE || r->fail == FAIL_CUT)
        conn->close_at = last;
    else if (sv.keepalive_ms)
        conn->close_at = last + net_ms(sv.keepalive_ms);
    net_say("reply", "%s: %u %s, %u bytes (%s%s%s)%s: %.900s", q->key, status, reason, at,
            pack == PACK_NONE ? "plain" : pack == PACK_STORED ? "stored" : pack == PACK_DYNAMIC ? "dynamic codes" :
            pack == PACK_BAD ? "a broken stream" : "fixed codes",
            framing == FRAME_CHUNKED ? ", chunked" : framing == FRAME_CLOSE ? ", ended by closing" : "",
            r->fail == FAIL_CUT ? ", cut short" : "", at > 4096 && q->kind == KIND_UPLOAD ?
            " (more than the 4096 bytes the game's buffer holds)" : "", body);
used:
    if (index >= 0 && --sv.rules[index].times == 0)
        sv.rules[index].used = 0;
}

void net_server_sent(net_conn *conn)
{
    /* is a whole request there? the header's end, then Content-Length bytes */
    for (;;) {
        request q;
        u8 *tx = conn->tx;
        u32 n = conn->tx_len, he = 0;
        for (u32 i = 0; i + 3 < n; i++)
            if (!memcmp(tx + i, "\r\n\r\n", 4)) {
                he = i + 4;
                break;
            }
        if (!he)
            return;
        memset(&q, 0, offsetof(request, text));
        const char *line_end = memchr(tx, '\r', he);
        char line[256];
        snprintf(line, sizeof line, "%.*s", (int)(line_end - (const char *)tx), (const char *)tx);
        char proto[24] = "";
        sscanf(line, "%11s %159s %23s", q.method, q.path, proto);
        snprintf(q.version, sizeof q.version, "%.7s", !strncmp(proto, "HTTP/", 5) ? proto + 5 : "");
        q.head = line_end + 2;
        q.head_len = (u32)((const char *)tx + he - 2 - q.head);
        u32 cl = 0, cll = 0;
        const char *clv = header_find(&q, "Content-Length", &cll);
        if (clv)
            cl = (u32)strtoul(clv, NULL, 10);
        if (n < he + cl)
            return;                             /* the body is still coming */
        q.body = tx + he;
        q.body_len = cl;
        q.kind = ends_with(q.path, "PowerOn") ? KIND_POWERON : ends_with(q.path, "game.php") ? KIND_SCORE :
                 ends_with(q.path, "VF4NetUpload") ? KIND_UPLOAD : KIND_OTHER;
        request_decode(&q);
        sv.requests++;
        net_say("http", "%s %s HTTP/%s (%u header bytes, %u body bytes) -> %s%s: %.1200s", q.method, q.path,
                q.version, he, cl, q.key, q.decoded ? "" : " (the body could not be unpacked)", q.text);
        reply_build(conn, &q, 0);
        /* what follows the request stays for the next turn */
        memmove(tx, tx + he + cl, n - he - cl);
        conn->tx_len = n - he - cl;
        if (!conn->tx_len)
            return;
    }
}

/* ---- the script ---- */

static rule *rule_new(const char *key, s32 times)
{
    rule *r = NULL;
    for (int i = 0; i < RULES; i++) {
        if (times < 0 && sv.rules[i].used && sv.rules[i].times < 0 && !strcmp(sv.rules[i].key, key)) {
            r = &sv.rules[i];           /* the standing rule of this key is written anew */
            break;
        }
        if (!r && !sv.rules[i].used)
            r = &sv.rules[i];
    }
    if (!r)
        return NULL;
    memset(r, 0, sizeof *r);
    r->used = 1;
    r->times = times;
    snprintf(r->key, sizeof r->key, "%s", key);
    return r;
}

static void rules_drop(const char *key)
{
    for (int i = 0; i < RULES; i++)
        if (sv.rules[i].used && (!key || !strcmp(sv.rules[i].key, key)))
            sv.rules[i].used = 0;
}

static int word_of(const char *t, const char *const *words, int n)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t, words[i]))
            return i;
    return -1;
}

static int reply_command(char **tok, int n, char *msg, size_t msg_len)
{
    static const char *const packs[] = { "", "none", "stored", "fixed", "dynamic", "bad" };
    static const char *const frames[] = { "", "length", "chunked", "close" };
    static const char *const fails[] = { "", "drop", "hang", "reset", "garbage", "cut" };
    const char *key = tok[1];
    s32 times = -1;
    int i = 2;
    if (n >= 3 && !strcmp(tok[2], "default")) {
        rules_drop(key);
        return 0;
    }
    if (i < n && !strcmp(tok[i], "once")) {
        times = 1;
        i++;
    } else if (i + 1 < n && !strcmp(tok[i], "times")) {
        times = (s32)strtol(tok[i + 1], NULL, 0);
        i += 2;
        if (times < 1)
            times = 1;
    }
    rule *r = rule_new(key, times);
    if (!r) {
        snprintf(msg, msg_len, "net reply: no room for another rule (%d at most)", RULES);
        return -1;
    }
    for (; i < n; i++) {
        const char *o = tok[i], *v = i + 1 < n ? tok[i + 1] : NULL;
        int k;
        if (!strcmp(o, "status") && i + 2 < n) {
            r->has_status = 1;
            r->status = (u32)strtoul(tok[i + 1], NULL, 0);
            snprintf(r->reason, sizeof r->reason, "%s", tok[i + 2]);
            for (char *p = r->reason; *p; p++)
                if (*p == '_')
                    *p = ' ';
            i += 2;
        } else if (!strcmp(o, "http") && v) {
            r->has_http = 1;
            snprintf(r->http, sizeof r->http, "%s", v);
            i++;
        } else if (!strcmp(o, "cmd") && v) {
            r->has_cmd = 1;
            snprintf(r->cmd, sizeof r->cmd, "%s", v);
            i++;
        } else if (!strcmp(o, "header") && v) {
            /* Name:value, with _ for a space in the value */
            size_t at = strlen(r->headers);
            const char *colon = strchr(v, ':');
            if (!colon) {
                snprintf(msg, msg_len, "net reply: header wants Name:value, not %s", v);
                return -1;
            }
            snprintf(r->headers + at, sizeof r->headers - at, "%s%.*s: %s", at ? "\n" : "", (int)(colon - v), v,
                     colon + 1);
            for (char *p = r->headers + at; *p; p++)
                if (*p == '_')
                    *p = ' ';
            i++;
        } else if (!strcmp(o, "pack") && v && (k = word_of(v, packs, 6)) > 0) {
            r->pack = (u8)k;
            i++;
        } else if (!strcmp(o, "frame") && v && (k = word_of(v, frames, 4)) > 0) {
            r->framing = (u8)k;
            i++;
        } else if (!strcmp(o, "fail") && v && (k = word_of(v, fails, 6)) > 0) {
            r->fail = (u8)k;
            i++;
        } else if (!strcmp(o, "delay") && v) {
            r->delay_ms = (u32)strtoul(v, NULL, 0);
            i++;
        } else if (!strcmp(o, "piece") && v) {
            r->piece = (u32)strtoul(v, NULL, 0);
            i++;
        } else if (!strcmp(o, "chunk") && v) {
            r->chunk = (u32)strtoul(v, NULL, 0);
            r->framing = FRAME_CHUNKED;
            i++;
        } else if (!strcmp(o, "gap") && v) {
            r->gap_ms = (u32)strtoul(v, NULL, 0);
            i++;
        } else if (!strcmp(o, "wrap") && v) {
            r->wrap = !strcmp(v, "none") ? -1 : (s32)strtol(v, NULL, 0);
            i++;
        } else if (!strcmp(o, "close")) {
            r->keep = KEEP_CLOSE;
        } else if (!strcmp(o, "keep")) {
            r->keep = KEEP_OPEN;
        } else if (!strcmp(o, "body")) {
            /* the rest of the line is the body; the words are joined with one space */
            size_t at = 0;
            r->has_body = 1;
            for (int j = i + 1; j < n; j++)
                at += (size_t)snprintf(r->body + at, at < sizeof r->body ? sizeof r->body - at : 0, "%s%s",
                                       j > i + 1 ? " " : "", tok[j]);
            if (at >= sizeof r->body) {
                snprintf(msg, msg_len, "net reply: the body is longer than %d characters", BODY_MAX - 1);
                r->used = 0;
                return -1;
            }
            break;
        } else {
            snprintf(msg, msg_len, "net reply: what is \"%s\"? (once, times N, status CODE REASON, http VERSION, "
                     "cmd NAME, header Name:value, pack none|stored|fixed|dynamic|bad, frame length|chunked|close, "
                     "chunk BYTES, fail drop|hang|reset|garbage|cut, delay MS, piece BYTES, gap MS, wrap N|none, "
                     "close, keep, body TEXT)", o);
            r->used = 0;
            return -1;
        }
    }
    return 0;
}

static int load_file(const char *path, char *msg, size_t msg_len);

int net_server_command(char **tok, int n, char *msg, size_t msg_len)
{
    const char *w = tok[0];
    int ok;
    if (!strcmp(w, "dns") && n >= 3) {
        /* net dns NAME A.B.C.D | none | fail  [delay MS] */
        u32 delay = n >= 5 && !strcmp(tok[3], "delay") ? (u32)strtoul(tok[4], NULL, 0) : 1;
        if (!strcmp(tok[2], "none")) {
            name_set(tok[1], 0, 1, delay);
        } else if (!strcmp(tok[2], "fail")) {
            name_set(tok[1], 0, 2, delay);
        } else {
            u32 ip = net_text_ip(tok[2], &ok);
            if (!ok) {
                snprintf(msg, msg_len, "net dns: %s is not an address, \"none\" or \"fail\"", tok[2]);
                return -1;
            }
            name_set(tok[1], ip, 0, delay);
        }
        return 0;
    }
    if (!strcmp(w, "host") && n >= 3) {
        /* net host A.B.C.D accept | refuse | unreachable | silent  [delay MS] */
        static const char *const hows[] = { "accept", "refuse", "unreachable", "silent" };
        u32 ip = net_text_ip(tok[1], &ok);
        int how = word_of(tok[2], hows, 4);
        if (!ok || how < 0) {
            snprintf(msg, msg_len, "net host: wants an address and accept, refuse, unreachable or silent");
            return -1;
        }
        host_set(ip, how, n >= 5 && !strcmp(tok[3], "delay") ? (u32)strtoul(tok[4], NULL, 0) : 1);
        return 0;
    }
    if (!strcmp(w, "upload") && n >= 2) {
        /* where the authentication reply sends the game's own requests */
        u32 ip = net_text_ip(tok[1], &ok);
        if (!ok) {
            snprintf(msg, msg_len, "net upload: %s is not an address", tok[1]);
            return -1;
        }
        sv.upload_ip = ip;
        host_set(ip, NET_HOST_ACCEPT, 1);
        return 0;
    }
    if (!strcmp(w, "reply") && n >= 2)
        return reply_command(tok, n, msg, msg_len);
    if (!strcmp(w, "clock") && n >= 2) {
        /* +SECONDS, -SECONDS, or a date and time the servers' clock shows from now on */
        if (n >= 3) {
            int y, mo, d, h, mi, s;
            if (sscanf(tok[1], "%d-%d-%d", &y, &mo, &d) != 3 || sscanf(tok[2], "%d:%d:%d", &h, &mi, &s) != 3 ||
                y < 1950 || mo < 1 || mo > 12) {
                snprintf(msg, msg_len, "net clock: wants +SECONDS, -SECONDS or YYYY-MM-DD hh:mm:ss");
                return -1;
            }
            sv.clock_offset = (s32)(date_seconds(y, mo, d, h, mi, s) - (s64)((u64)hw.rtc_base + net_now() / CPU_HZ));
        } else {
            sv.clock_offset = (s32)strtol(tok[1], NULL, 0);
        }
        return 0;
    }
    if (!strcmp(w, "keepalive") && n >= 2) {
        sv.keepalive_ms = !strcmp(tok[1], "forever") ? 0 : (u32)strtoul(tok[1], NULL, 0);
        return 0;
    }
    if (!strcmp(w, "defaults")) {
        net_server_defaults();
        return 0;
    }
    if (!strcmp(w, "load") && n >= 2)
        return load_file(tok[1], msg, msg_len);
    if (!strcmp(w, "pack") && n >= 3) {
        /* a text packed as a reply would be, in hex: for checking this
         * program's deflate against another one's inflate */
        static const char *const kinds[] = { "stored", "fixed", "dynamic" };
        static u8 packed[4400];
        static char line[1024];
        int kind = word_of(tok[1], kinds, 3);
        size_t at = 0;
        for (int j = 2; j < n; j++)
            at += (size_t)snprintf(line + at, at < sizeof line ? sizeof line - at : 0, "%s%s", j > 2 ? " " : "",
                                   tok[j]);
        int len = kind < 0 ? -1 : nz_zlib_pack((const u8 *)line, (u32)strlen(line), packed, sizeof packed, kind);
        printf("{\"ev\":\"net\",\"what\":\"pack\",\"kind\":\"%s\",\"hex\":\"", tok[1]);
        for (int j = 0; j < len; j++)
            printf("%02x", packed[j]);
        printf("\"}\n");
        fflush(stdout);
        return 0;
    }
    if (!strcmp(w, "unpack") && n >= 2) {
        /* the other way: a zlib stream in hex, unpacked by this program's inflate */
        static u8 packed[4400], text[8200];
        u32 len = 0;
        for (const char *p = tok[1]; p[0] && p[1] && len < sizeof packed; p += 2) {
            char hex[3] = { p[0], p[1], 0 };
            packed[len++] = (u8)strtoul(hex, NULL, 16);
        }
        int back = nz_zlib_unpack(packed, len, text, sizeof text / 2);
        printf("{\"ev\":\"net\",\"what\":\"unpack\",\"length\":%d,\"hex\":\"", back);
        for (int j = 0; j < back; j++)
            printf("%02x", text[j]);
        printf("\"}\n");
        fflush(stdout);
        return 0;
    }
    snprintf(msg, msg_len, "net %s: not a command of the network board (on, off, status, events, ip, nameserver, "
             "board, comms, command, latency, sendmax, socket fail, request, dns, host, upload, reply, clock, "
             "keepalive, defaults, load; see README.md)", w);
    return -1;
}

/* a file of the same commands, without the word "net"; a line may be long */
static int load_file(const char *path, char *msg, size_t msg_len)
{
    static char line[BODY_MAX + 600];
    static int depth;
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(msg, msg_len, "net load: cannot open %s", path);
        return -1;
    }
    int no = 0, bad = 0;
    depth++;
    while (!bad && depth < 4 && fgets(line, sizeof line, f)) {
        char *tok[64];
        int n = 0;
        no++;
        if (line[0] == '#')
            continue;
        for (char *t = strtok(line, " \t\r\n"); t && n < 64; t = strtok(NULL, " \t\r\n"))
            tok[n++] = t;
        if (!n)
            continue;
        char inner[300];
        if (hw_net_command(&cpu, tok, n, inner, sizeof inner)) {
            snprintf(msg, msg_len, "%s line %d: %.200s", path, no, inner);
            bad = 1;
        }
    }
    depth--;
    fclose(f);
    return bad ? -1 : 0;
}

void net_server_defaults(void)
{
    int ok;
    memset(&sv, 0, sizeof sv);
    /* every address is one of the three ranges set aside for examples
     * (192.0.2.0/24): none of them exists anywhere */
    name_set("naominet.jp", net_text_ip("192.0.2.10", &ok), 0, 1);
    name_set("sc.naominet.jp", net_text_ip("192.0.2.11", &ok), 0, 1);
    name_set("tenporouter.loc", net_text_ip("192.0.2.254", &ok), 0, 1);
    host_set(net_text_ip("192.0.2.10", &ok), NET_HOST_ACCEPT, 1);
    host_set(net_text_ip("192.0.2.11", &ok), NET_HOST_ACCEPT, 1);
    sv.upload_ip = net_text_ip("192.0.2.20", &ok);
    host_set(sv.upload_ip, NET_HOST_ACCEPT, 1);
    sv.keepalive_ms = 5000;
}

void net_server_init(void)
{
    net_server_defaults();
    snapshot_register("net-server", &sv, sizeof sv);
}

void net_server_stats(FILE *f)
{
    fprintf(f, "    servers: %u names looked up, %u connections asked for, %u requests answered", sv.lookups,
            sv.connects, sv.requests);
    if (sv.undecoded)
        fprintf(f, ", %u bodies not understood", sv.undecoded);
    if (sv.crc_bad)
        fprintf(f, ", %u bodies whose checksum header did not fit", sv.crc_bad);
    fprintf(f, "\n");
    if (sv.seen[0].key[0]) {
        fprintf(f, "    exchanges:");
        for (int i = 0; i < SEEN && sv.seen[i].key[0]; i++)
            fprintf(f, " %s %u,", sv.seen[i].key, sv.seen[i].count);
        fprintf(f, "\n");
    }
}
