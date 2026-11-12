/* Packing for the server stand-in: the deflate format both ways, the zlib
 * wrapper, base64 with a given alphabet, CRC-32 and Adler-32.
 *
 * Why it is here: the game packs every request body (zlib's compress, then
 * base64) and expects its replies packed the same way, which it unpacks
 * with its own zlib. The stand-in has to read the one and write the other,
 * and the game's inflate has three kinds of block to run (stored, fixed
 * codes, dynamic codes), so the writer can make each of them on request.
 *
 * Written from the public description of the formats (RFC 1950, RFC 1951,
 * RFC 4648); no library's code was used. Sizes are small (a body is at most
 * a few KiB), so everything is done the plain way.
 */
#include "net.h"

#include <string.h>

const char nz_b64_standard[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ---- checksums ---- */

u32 nz_crc32(const u8 *p, u32 n)
{
    static u32 table[256];
    if (!table[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    u32 c = 0xffffffffu;
    for (u32 i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

u32 nz_adler32(const u8 *p, u32 n)
{
    u32 a = 1, b = 0;
    for (u32 i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* ---- the constants of the format ---- */

static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                  115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
                                  0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                   1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                   12, 13, 13 };
static const u8 clen_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static void fixed_lengths(u8 *lit, u8 *dist)
{
    for (int i = 0; i < 288; i++)
        lit[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < 30; i++)
        dist[i] = 5;
}

/* ---- inflate ---- */

typedef struct inflater {
    const u8 *in;
    u32 n, pos;
    u32 acc;
    int have;
    u8 *out;
    u32 cap, len;
    int bad;
} inflater;

static u32 take(inflater *s, int need)
{
    while (s->have < need) {
        if (s->pos >= s->n) {
            s->bad = 1;
            return 0;
        }
        s->acc |= (u32)s->in[s->pos++] << s->have;
        s->have += 8;
    }
    u32 v = s->acc & ((1u << need) - 1);
    s->acc >>= need;
    s->have -= need;
    return v;
}

/* a prefix code by its lengths: how many codes of each length, and the
 * symbols in the order of their codes */
typedef struct prefix_code {
    u16 count[16];
    u16 symbol[288];
} prefix_code;

static int code_build(prefix_code *h, const u8 *len, int n)
{
    u16 at[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = left * 2 - h->count[l];
        if (left < 0)
            return -1;                  /* more codes than the lengths allow */
    }
    at[1] = 0;
    for (int l = 1; l < 15; l++)
        at[l + 1] = (u16)(at[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (len[i])
            h->symbol[at[len[i]]++] = (u16)i;
    return left;                        /* 0: complete */
}

static int code_read(inflater *s, const prefix_code *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)take(s, 1);
        if (s->bad)
            return -1;
        int count = h->count[l];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_codes(inflater *s, const prefix_code *lit, const prefix_code *dist)
{
    for (;;) {
        int sym = code_read(s, lit);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (s->len >= s->cap)
                return -1;
            s->out[s->len++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            u32 len = len_base[sym] + take(s, len_extra[sym]);
            int d = code_read(s, dist);
            if (d < 0 || d >= 30)
                return -1;
            u32 back = dist_base[d] + take(s, dist_extra[d]);
            if (s->bad || back > s->len || s->len + len > s->cap)
                return -1;
            for (u32 i = 0; i < len; i++, s->len++)
                s->out[s->len] = s->out[s->len - back];
        }
    }
}

int nz_inflate(const u8 *in, u32 n, u8 *out, u32 cap)
{
    inflater s = { in, n, 0, 0, 0, out, cap, 0, 0 };
    for (;;) {
        u32 last = take(&s, 1), kind = take(&s, 2);
        if (s.bad)
            return -1;
        if (kind == 0) {
            s.acc = 0;
            s.have = 0;
            if (s.pos + 4 > n)
                return -1;
            u32 len = in[s.pos] | (in[s.pos + 1] << 8), inv = in[s.pos + 2] | (in[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ inv) != 0xffff || s.pos + len > n || s.len + len > cap)
                return -1;
            memcpy(out + s.len, in + s.pos, len);
            s.pos += len;
            s.len += len;
        } else if (kind == 1) {
            u8 ll[288], dl[30];
            prefix_code lit, dist;
            fixed_lengths(ll, dl);
            code_build(&lit, ll, 288);
            code_build(&dist, dl, 30);
            if (inflate_codes(&s, &lit, &dist))
                return -1;
        } else if (kind == 2) {
            u32 nlit = take(&s, 5) + 257, ndist = take(&s, 5) + 1, nclen = take(&s, 4) + 4;
            u8 lens[320], cl[19];
            prefix_code clen, lit, dist;
            if (s.bad || nlit > 286 || ndist > 30)
                return -1;
            memset(cl, 0, sizeof cl);
            for (u32 i = 0; i < nclen; i++)
                cl[clen_order[i]] = (u8)take(&s, 3);
            if (s.bad || code_build(&clen, cl, 19) < 0)
                return -1;
            u32 i = 0;
            while (i < nlit + ndist) {
                int sym = code_read(&s, &clen);
                if (sym < 0)
                    return -1;
                if (sym < 16) {
                    lens[i++] = (u8)sym;
                    continue;
                }
                u32 rep;
                u8 v = 0;
                if (sym == 16) {
                    if (!i)
                        return -1;
                    v = lens[i - 1];
                    rep = 3 + take(&s, 2);
                } else if (sym == 17) {
                    rep = 3 + take(&s, 3);
                } else {
                    rep = 11 + take(&s, 7);
                }
                if (s.bad || i + rep > nlit + ndist)
                    return -1;
                while (rep--)
                    lens[i++] = v;
            }
            if (!lens[256] || code_build(&lit, lens, (int)nlit) < 0 || code_build(&dist, lens + nlit, (int)ndist) < 0)
                return -1;
            if (inflate_codes(&s, &lit, &dist))
                return -1;
        } else {
            return -1;
        }
        if (last)
            return (int)s.len;
    }
}

int nz_zlib_unpack(const u8 *in, u32 n, u8 *out, u32 cap)
{
    if (n < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
        return -1;
    int len = nz_inflate(in + 2, n - 2, out, cap);
    return len;                         /* the check value at the end is the sender's business */
}

/* ---- deflate ---- */

typedef struct bit_writer {
    u8 *out;
    u32 cap, len;
    u32 acc;
    int have;
    int over;
} bit_writer;

static void put_byte(bit_writer *w, u32 b)
{
    if (w->len < w->cap)
        w->out[w->len++] = (u8)b;
    else
        w->over = 1;
}

static void put(bit_writer *w, u32 v, int n)
{
    w->acc |= v << w->have;
    w->have += n;
    while (w->have >= 8) {
        put_byte(w, w->acc & 0xff);
        w->acc >>= 8;
        w->have -= 8;
    }
}

static void put_flush(bit_writer *w)
{
    if (w->have)
        put_byte(w, w->acc & 0xff);
    w->acc = 0;
    w->have = 0;
}

/* a code goes out with its first bit first */
static void put_code(bit_writer *w, u32 code, int len)
{
    u32 r = 0;
    for (int i = 0; i < len; i++)
        r |= ((code >> i) & 1) << (len - 1 - i);
    put(w, r, len);
}

/* the codes that belong to a set of lengths */
static void codes_assign(const u8 *len, int n, u16 *code)
{
    u16 count[16] = { 0 }, next[16];
    for (int i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    u32 c = 0;
    for (int l = 1; l < 16; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = (u16)c;
    }
    for (int i = 0; i < n; i++)
        code[i] = len[i] ? next[len[i]]++ : 0;
}

/* Code lengths for a set of frequencies, none longer than max. The lengths
 * of the plain construction (join the two rarest, again and again) are
 * counted per length, the ones that are too long are cut to max, and the
 * count is put right by taking one code of the longest length away and
 * splitting a shorter one in two until the lengths add up to exactly one
 * whole code space; then the most frequent symbols get the shortest
 * lengths. At least two symbols get a length, so the code is complete. */
static void lengths_make(const u32 *freq_in, int n, int max, u8 *len)
{
    u32 freq[288];
    int order[288], parent[2 * 288], depth_count[40];
    u32 weight[2 * 288];
    u8 alive[2 * 288];
    int m = 0;
    memset(len, 0, (size_t)n);
    for (int i = 0; i < n; i++)
        freq[i] = freq_in[i];
    int used = 0;
    for (int i = 0; i < n; i++)
        used += freq[i] != 0;
    for (int i = 0; i < n && used < 2; i++)
        if (!freq[i]) {
            freq[i] = 1;                /* a second symbol, so that no code stands alone */
            used++;
        }
    for (int i = 0; i < n; i++)
        if (freq[i])
            order[m++] = i;
    int nodes = m, live = m;
    for (int i = 0; i < m; i++) {
        weight[i] = freq[order[i]];
        parent[i] = -1;
        alive[i] = 1;
    }
    while (live > 1) {
        int a = -1, b = -1;
        for (int i = 0; i < nodes; i++) {
            if (!alive[i])
                continue;
            if (a < 0 || weight[i] < weight[a]) {
                b = a;
                a = i;
            } else if (b < 0 || weight[i] < weight[b]) {
                b = i;
            }
        }
        weight[nodes] = weight[a] + weight[b];
        parent[nodes] = -1;
        alive[nodes] = 1;
        parent[a] = parent[b] = nodes;
        alive[a] = alive[b] = 0;
        nodes++;
        live--;
    }
    memset(depth_count, 0, sizeof depth_count);
    for (int i = 0; i < m; i++) {
        int d = 0;
        for (int p = parent[i]; p >= 0; p = parent[p])
            d++;
        depth_count[d > max ? max : d]++;
    }
    u32 total = 0;
    for (int l = max; l > 0; l--)
        total += (u32)depth_count[l] << (max - l);
    while (total > (1u << max)) {
        depth_count[max]--;
        for (int l = max - 1; l > 0; l--)
            if (depth_count[l]) {
                depth_count[l]--;
                depth_count[l + 1] += 2;
                break;
            }
        total--;
    }
    /* the symbols from the most frequent down (ties: the lower number first) */
    for (int i = 1; i < m; i++) {
        int s = order[i], k = i;
        while (k > 0 && freq[order[k - 1]] < freq[s]) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = s;
    }
    int at = 0;
    for (int l = 1; l <= max; l++)
        for (int k = 0; k < depth_count[l]; k++)
            len[order[at++]] = (u8)l;
}

typedef struct lz_token {
    u16 len;            /* 0: a literal, in dist */
    u16 dist;
} lz_token;

#define LZ_MAX 0x8000
#define LZ_HASH 4096

static int lz_tokens(const u8 *in, u32 n, lz_token *t)
{
    static int head[LZ_HASH], prev[LZ_MAX];
    int count = 0;
    for (int i = 0; i < LZ_HASH; i++)
        head[i] = -1;
    u32 i = 0;
    while (i < n) {
        u32 best = 0, best_dist = 0;
        if (i + 3 <= n) {
            u32 h = ((u32)in[i] * 506832829u + (u32)in[i + 1] * 2654435761u + in[i + 2] * 40503u) >> 20 & (LZ_HASH - 1);
            int tries = 0;
            for (int cand = head[h]; cand >= 0 && tries < 48; cand = prev[cand], tries++) {
                u32 l = 0, lim = n - i < 258 ? n - i : 258;
                while (l < lim && in[(u32)cand + l] == in[i + l])
                    l++;
                if (l > best) {
                    best = l;
                    best_dist = i - (u32)cand;
                }
            }
        }
        u32 step = best >= 3 ? best : 1;
        if (best >= 3) {
            t[count].len = (u16)best;
            t[count].dist = (u16)best_dist;
        } else {
            t[count].len = 0;
            t[count].dist = in[i];
        }
        count++;
        for (u32 k = i; k < i + step; k++)
            if (k + 3 <= n) {
                u32 h = ((u32)in[k] * 506832829u + (u32)in[k + 1] * 2654435761u + in[k + 2] * 40503u) >> 20 &
                        (LZ_HASH - 1);
                prev[k] = head[h];
                head[h] = (int)k;
            }
        i += step;
    }
    return count;
}

static int len_symbol(u32 len)
{
    int s = 28;
    while (s > 0 && len_base[s] > len)
        s--;
    return s;
}

static int dist_symbol(u32 dist)
{
    int s = 29;
    while (s > 0 && dist_base[s] > dist)
        s--;
    return s;
}

static void tokens_write(bit_writer *w, const lz_token *t, int count, const u8 *ll, const u16 *lc, const u8 *dl,
                         const u16 *dc)
{
    for (int i = 0; i < count; i++) {
        if (!t[i].len) {
            put_code(w, lc[t[i].dist], ll[t[i].dist]);
            continue;
        }
        int ls = len_symbol(t[i].len), ds = dist_symbol(t[i].dist);
        put_code(w, lc[257 + ls], ll[257 + ls]);
        put(w, t[i].len - len_base[ls], len_extra[ls]);
        put_code(w, dc[ds], dl[ds]);
        put(w, t[i].dist - dist_base[ds], dist_extra[ds]);
    }
    put_code(w, lc[256], ll[256]);
}

int nz_deflate(const u8 *in, u32 n, u8 *out, u32 cap, int kind)
{
    static lz_token tokens[LZ_MAX];
    bit_writer w = { out, cap, 0, 0, 0, 0 };
    if (n > LZ_MAX)
        return -1;
    if (kind == NZ_STORED) {
        put(&w, 1, 1);
        put(&w, 0, 2);
        put_flush(&w);
        put_byte(&w, n & 0xff);
        put_byte(&w, n >> 8);
        put_byte(&w, ~n & 0xff);
        put_byte(&w, (~n >> 8) & 0xff);
        for (u32 i = 0; i < n; i++)
            put_byte(&w, in[i]);
        return w.over ? -1 : (int)w.len;
    }
    int count = lz_tokens(in, n, tokens);
    u8 ll[288], dl[30];
    u16 lc[288], dc[30];
    if (kind == NZ_FIXED) {
        fixed_lengths(ll, dl);
        codes_assign(ll, 288, lc);
        codes_assign(dl, 30, dc);
        put(&w, 1, 1);
        put(&w, 1, 2);
        tokens_write(&w, tokens, count, ll, lc, dl, dc);
        put_flush(&w);
        return w.over ? -1 : (int)w.len;
    }
    /* dynamic codes: the frequencies of this body make the code */
    u32 lf[288] = { 0 }, df[30] = { 0 };
    for (int i = 0; i < count; i++) {
        if (!tokens[i].len) {
            lf[tokens[i].dist]++;
        } else {
            lf[257 + len_symbol(tokens[i].len)]++;
            df[dist_symbol(tokens[i].dist)]++;
        }
    }
    lf[256] = 1;
    lengths_make(lf, 286, 15, ll);
    lengths_make(df, 30, 15, dl);
    codes_assign(ll, 286, lc);
    codes_assign(dl, 30, dc);
    int nlit = 286, ndist = 30;
    while (nlit > 257 && !ll[nlit - 1])
        nlit--;
    while (ndist > 1 && !dl[ndist - 1])
        ndist--;
    /* the two sets of lengths as one run-length coded list */
    u8 all[320], sym[320], extra[320];
    int total = nlit + ndist, ns = 0;
    memcpy(all, ll, (size_t)nlit);
    memcpy(all + nlit, dl, (size_t)ndist);
    for (int i = 0; i < total;) {
        int v = all[i], run = 1;
        while (i + run < total && all[i + run] == v)
            run++;
        if (v == 0) {
            while (run >= 11) {
                int r = run > 138 ? 138 : run;
                sym[ns] = 18, extra[ns++] = (u8)(r - 11);
                run -= r, i += r;
            }
            if (run >= 3) {
                sym[ns] = 17, extra[ns++] = (u8)(run - 3);
                i += run, run = 0;
            }
            for (; run > 0; run--, i++)
                sym[ns] = 0, extra[ns++] = 0;
        } else {
            sym[ns] = (u8)v, extra[ns++] = 0;
            i++, run--;
            while (run >= 3) {
                int r = run > 6 ? 6 : run;
                sym[ns] = 16, extra[ns++] = (u8)(r - 3);
                run -= r, i += r;
            }
            for (; run > 0; run--, i++)
                sym[ns] = (u8)v, extra[ns++] = 0;
        }
    }
    u32 cf[19] = { 0 };
    u8 cl[19];
    u16 cc[19];
    for (int i = 0; i < ns; i++)
        cf[sym[i]]++;
    lengths_make(cf, 19, 7, cl);
    codes_assign(cl, 19, cc);
    int nclen = 19;
    while (nclen > 4 && !cl[clen_order[nclen - 1]])
        nclen--;
    put(&w, 1, 1);
    put(&w, 2, 2);
    put(&w, (u32)(nlit - 257), 5);
    put(&w, (u32)(ndist - 1), 5);
    put(&w, (u32)(nclen - 4), 4);
    for (int i = 0; i < nclen; i++)
        put(&w, cl[clen_order[i]], 3);
    for (int i = 0; i < ns; i++) {
        put_code(&w, cc[sym[i]], cl[sym[i]]);
        if (sym[i] == 16)
            put(&w, extra[i], 2);
        else if (sym[i] == 17)
            put(&w, extra[i], 3);
        else if (sym[i] == 18)
            put(&w, extra[i], 7);
    }
    tokens_write(&w, tokens, count, ll, lc, dl, dc);
    put_flush(&w);
    return w.over ? -1 : (int)w.len;
}

int nz_zlib_pack(const u8 *in, u32 n, u8 *out, u32 cap, int kind)
{
    if (cap < 6)
        return -1;
    out[0] = 0x78;
    out[1] = 0x9c;                      /* 0x789c: deflate, a 32 KiB window, a multiple of 31 */
    int len = nz_deflate(in, n, out + 2, cap - 6, kind);
    if (len < 0)
        return -1;
    u32 a = nz_adler32(in, n);
    out[2 + len] = (u8)(a >> 24);
    out[3 + len] = (u8)(a >> 16);
    out[4 + len] = (u8)(a >> 8);
    out[5 + len] = (u8)a;
    return len + 6;
}

/* ---- base64 ---- */

int nz_b64_encode(const u8 *in, u32 n, char *out, u32 cap, const char *alphabet, int wrap, const char *eol)
{
    u32 len = 0, col = 0, eol_len = eol ? (u32)strlen(eol) : 0;
    for (u32 i = 0; i < n; i += 3) {
        u32 left = n - i, v = (u32)in[i] << 16;
        if (left > 1)
            v |= (u32)in[i + 1] << 8;
        if (left > 2)
            v |= in[i + 2];
        if (len + 4 + eol_len + 1 > cap)
            return -1;
        out[len++] = alphabet[v >> 18];
        out[len++] = alphabet[(v >> 12) & 63];
        out[len++] = left > 1 ? alphabet[(v >> 6) & 63] : '=';
        out[len++] = left > 2 ? alphabet[v & 63] : '=';
        col += 4;
        if (wrap > 0 && col >= (u32)wrap && i + 3 < n) {
            memcpy(out + len, eol, eol_len);
            len += eol_len;
            col = 0;
        }
    }
    out[len] = 0;
    return (int)len;
}

int nz_b64_decode(const char *in, u32 n, u8 *out, u32 cap, const char *alphabet)
{
    u32 acc = 0, len = 0;
    int have = 0;
    for (u32 i = 0; i < n; i++) {
        if (in[i] == '=')
            break;
        const char *p = in[i] ? memchr(alphabet, in[i], 64) : NULL;
        if (!p)
            continue;                   /* line ends and the like */
        acc = (acc << 6) | (u32)(p - alphabet);
        have += 6;
        if (have >= 8) {
            have -= 8;
            if (len >= cap)
                return -1;
            out[len++] = (u8)(acc >> have);
        }
    }
    return (int)len;
}

/* ---- a check of itself ---- */

int nz_selftest(char *msg, size_t msg_len)
{
    static u8 src[6000], packed[9000], back[6000];
    static char text[13000];
    static const char *const kinds[3] = { "stored", "fixed", "dynamic" };
    for (int sample = 0; sample < 6; sample++) {
        u32 n = 0;
        if (sample == 1) {
            n = 1;
            src[0] = 'a';
        } else if (sample == 2) {
            for (n = 0; n < 5000; n++)
                src[n] = (u8)("id=0123456789&db_stat=1&"[n % 24]);
        } else if (sample == 3) {
            u32 x = 12345;
            for (n = 0; n < 4000; n++) {
                x = x * 1103515245u + 12345u;
                src[n] = (u8)(x >> 16);
            }
        } else if (sample == 4) {
            for (n = 0; n < 3000; n++)
                src[n] = 0;
        } else if (sample == 5) {
            for (n = 0; n < 2000; n++)
                src[n] = (u8)(n % 7 ? 'a' + n % 3 : n);
        }
        for (int kind = 0; kind < 3; kind++) {
            int p = nz_zlib_pack(src, n, packed, sizeof packed, kind);
            int b = p < 0 ? -1 : nz_zlib_unpack(packed, (u32)p, back, sizeof back);
            if (b != (int)n || memcmp(src, back, n)) {
                snprintf(msg, msg_len, "sample %d does not come back from a %s block (%d bytes packed, %d back)",
                         sample, kinds[kind], p, b);
                return -1;
            }
            int t = nz_b64_encode(packed, (u32)p, text, sizeof text, nz_b64_standard, 76, "\r\n");
            int d = t < 0 ? -1 : nz_b64_decode(text, (u32)t, back, sizeof back, nz_b64_standard);
            if (d != p || memcmp(packed, back, (size_t)p)) {
                snprintf(msg, msg_len, "sample %d (%s) does not come back from base64", sample, kinds[kind]);
                return -1;
            }
        }
    }
    if (nz_crc32((const u8 *)"123456789", 9) != 0xcbf43926u || nz_adler32((const u8 *)"Wikipedia", 9) != 0x11e60398u) {
        snprintf(msg, msg_len, "a checksum of a known text is wrong");
        return -1;
    }
    snprintf(msg, msg_len, "6 samples, 3 block kinds, base64 and both checksums hold");
    return 0;
}
