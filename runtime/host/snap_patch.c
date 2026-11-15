/* See snap_patch.h. The format, made for this program:
 *
 *   "VF4P", the version (1), the result's size (8 bytes), its fingerprint
 *   (8), ref's size (8) and ref's fingerprint (8), all little-endian; then
 *   commands until the result is whole, each a number (LEB128: 7 bits a
 *   byte, the low ones first, the top bit set while more follow) whose low
 *   two bits say what it is and the rest a length n:
 *     0  n new bytes follow
 *     1  n bytes from ref, at the position (a number) that follows
 *     2  n bytes from the result itself, the distance back (a number) that
 *        follows (it may be less than n: a run)
 *
 * Making it: ref is indexed by 16 bytes at every 16th position. At each
 * position of cur, the 16 bytes there are looked up (any match of 31 bytes
 * or more holds an indexed position, so it is found), and 8 bytes are
 * looked up among cur's own earlier positions; the longer match wins,
 * stretched back over the new bytes before it. Unchanged memory is long
 * copies from ref, the rest new bytes. */
#include "snap_patch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VERSION 1
#define HEAD 44
#define REF_BITS 22                 /* ref's index: 4 Mi slots (16 MiB) */
#define SELF_BITS 18                /* cur's own: 256 Ki slots */
#define REF_MIN 16
#define SELF_MIN 12

static uint64_t ld64(const unsigned char *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

unsigned long long snap_hash(const unsigned char *p, size_t n)
{
    /* four lanes of FNV-1a over 64-bit words, then the tail byte by byte */
    const uint64_t m = 0x100000001b3ull;
    uint64_t h[4] = { 0xcbf29ce484222325ull, 0x84222325cbf29ce4ull, 0x9e3779b97f4a7c15ull, 0x7f4a7c159e3779b9ull };
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int j = 0; j < 4; j++)
            h[j] = (h[j] ^ ld64(p + i + 8 * j)) * m;
    uint64_t r = h[0] ^ n;
    for (int j = 1; j < 4; j++)
        r = (r ^ h[j]) * m;
    for (; i < n; i++)
        r = (r ^ p[i]) * m;
    return r;
}

static uint32_t hash16(const unsigned char *p)
{
    uint64_t a = ld64(p) * 0x9e3779b97f4a7c15ull, b = ld64(p + 8) * 0xc2b2ae3d27d4eb4full;
    return (uint32_t)((a ^ (b >> 7) ^ b) >> (64 - REF_BITS));
}

static uint32_t hash8(const unsigned char *p)
{
    return (uint32_t)((ld64(p) * 0x9e3779b97f4a7c15ull) >> (64 - SELF_BITS));
}

/* how many bytes a and b have the same, up to n */
static size_t same_len(const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t k = 0;
    while (k + 8 <= n) {
        uint64_t x = ld64(a + k) ^ ld64(b + k);
        if (x)
            return k + (size_t)(__builtin_ctzll(x) >> 3);
        k += 8;
    }
    while (k < n && a[k] == b[k])
        k++;
    return k;
}

typedef struct out_buf {
    unsigned char *p;
    size_t n, cap;
    int bad;
} out_buf;

static void put_bytes(out_buf *o, const void *b, size_t n)
{
    if (o->bad)
        return;
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap : 1 << 20;
        while (cap < o->n + n)
            cap *= 2;
        unsigned char *q = realloc(o->p, cap);
        if (!q) {
            o->bad = 1;
            return;
        }
        o->p = q;
        o->cap = cap;
    }
    memcpy(o->p + o->n, b, n);
    o->n += n;
}

static void put_num(out_buf *o, uint64_t v)
{
    unsigned char b[10];
    int n = 0;
    do {
        b[n] = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v)
            b[n] |= 0x80;
        n++;
    } while (v);
    put_bytes(o, b, (size_t)n);
}

static void put_le(unsigned char *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> 8 * i);
}

static uint64_t get_le(const unsigned char *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint64_t)p[i] << 8 * i;
    return v;
}

static void put_new(out_buf *o, const unsigned char *b, size_t n)
{
    if (!n)
        return;
    put_num(o, (uint64_t)n << 2 | 0);
    put_bytes(o, b, n);
}

int snap_patch_make(const unsigned char *ref, size_t ref_n, const unsigned char *cur, size_t cur_n,
                    unsigned char **out, size_t *out_n)
{
    uint32_t *rt = calloc((size_t)1 << REF_BITS, sizeof *rt);       /* 1 + a position in ref, 0 none */
    uint32_t *st = calloc((size_t)1 << SELF_BITS, sizeof *st);      /* 1 + a position in cur */
    out_buf o = { 0 };
    if (!rt || !st || ref_n >= UINT32_MAX || cur_n >= UINT32_MAX) {
        free(rt);
        free(st);
        return -1;
    }
    for (size_t p = 0; p + REF_MIN <= ref_n; p += 16)
        rt[hash16(ref + p)] = (uint32_t)p + 1;
    unsigned char head[HEAD];
    memcpy(head, "VF4P", 4);
    put_le(head + 4, VERSION, 4);
    put_le(head + 8, cur_n, 8);
    put_le(head + 16, snap_hash(cur, cur_n), 8);
    put_le(head + 24, ref_n, 8);
    put_le(head + 32, snap_hash(ref, ref_n), 8);
    put_le(head + 40, 0, 4);
    put_bytes(&o, head, HEAD);

    size_t i = 0, lit = 0;          /* lit: where the new bytes not yet put began */
    while (i + REF_MIN <= cur_n) {
        size_t len = 0, from = 0, back = 0;
        int kind = 0;
        uint32_t r = rt[hash16(cur + i)];
        if (r) {
            size_t p = r - 1;
            size_t l = same_len(ref + p, cur + i, (ref_n - p < cur_n - i) ? ref_n - p : cur_n - i);
            if (l >= REF_MIN) {
                size_t b = 0;
                while (b < i - lit && b < p && ref[p - b - 1] == cur[i - b - 1])
                    b++;
                len = l;
                back = b;
                from = p;
                kind = 1;
            }
        }
        uint32_t h = hash8(cur + i), s = st[h];
        st[h] = (uint32_t)i + 1;
        if (s) {
            size_t q = s - 1;
            size_t l = same_len(cur + q, cur + i, cur_n - i);
            if (l >= SELF_MIN && l > len + back) {
                size_t b = 0;
                while (b < i - lit && b < q && cur[q - b - 1] == cur[i - b - 1])
                    b++;
                len = l;
                back = b;
                from = q;
                kind = 2;
            }
        }
        if (!kind) {
            i++;
            continue;
        }
        size_t at = i - back;
        put_new(&o, cur + lit, at - lit);
        put_num(&o, (uint64_t)(len + back) << 2 | (uint64_t)kind);
        put_num(&o, kind == 1 ? from - back : at - (from - back));
        i += len;
        lit = i;
        /* a few of the copied positions into cur's own index */
        if (i >= 8 && i + 8 <= cur_n)
            st[hash8(cur + i - 8)] = (uint32_t)(i - 8) + 1;
    }
    put_new(&o, cur + lit, cur_n - lit);
    free(rt);
    free(st);
    if (o.bad) {
        free(o.p);
        return -1;
    }
    *out = o.p;
    *out_n = o.n;
    return 0;
}

static int get_num(const unsigned char **p, const unsigned char *end, uint64_t *v)
{
    uint64_t r = 0;
    for (int s = 0; s < 64; s += 7) {
        if (*p >= end)
            return -1;
        unsigned char b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << s;
        if (!(b & 0x80)) {
            *v = r;
            return 0;
        }
    }
    return -1;
}

int snap_patch_apply(const unsigned char *ref, size_t ref_n, const unsigned char *patch, size_t patch_n,
                     unsigned char **out, size_t *out_n)
{
    if (patch_n < HEAD || memcmp(patch, "VF4P", 4) || get_le(patch + 4, 4) != VERSION)
        return -1;
    uint64_t n = get_le(patch + 8, 8), want = get_le(patch + 16, 8);
    if (!get_le(patch + 24, 8)) {   /* made from nothing */
        ref = NULL;
        ref_n = 0;
    }
    if (get_le(patch + 24, 8) != ref_n || get_le(patch + 32, 8) != snap_hash(ref, ref_n) || n >= UINT32_MAX)
        return -1;
    unsigned char *r = malloc(n ? (size_t)n : 1);
    if (!r)
        return -1;
    const unsigned char *p = patch + HEAD, *end = patch + patch_n;
    size_t at = 0;
    while (at < n) {
        uint64_t c, len, x;
        if (get_num(&p, end, &c))
            goto bad;
        len = c >> 2;
        if (!len || len > n - at)
            goto bad;
        switch (c & 3) {
        case 0:
            if ((uint64_t)(end - p) < len)
                goto bad;
            memcpy(r + at, p, (size_t)len);
            p += len;
            break;
        case 1:
            if (get_num(&p, end, &x) || x > ref_n || len > ref_n - x)
                goto bad;
            memcpy(r + at, ref + x, (size_t)len);
            break;
        case 2:
            if (get_num(&p, end, &x) || !x || x > at)
                goto bad;
            if (x >= len)
                memcpy(r + at, r + at - x, (size_t)len);
            else
                for (uint64_t k = 0; k < len; k++)     /* byte by byte: the copy overlaps itself (a run) */
                    r[at + k] = r[at - x + k];
            break;
        default:
            goto bad;
        }
        at += (size_t)len;
    }
    if (p != end || snap_hash(r, (size_t)n) != want)
        goto bad;
    *out = r;
    *out_n = (size_t)n;
    return 0;
bad:
    free(r);
    return -1;
}
