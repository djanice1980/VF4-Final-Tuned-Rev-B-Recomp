/* The two-PC game's security: see secure.h and docs/design.md section 50. */
#include "secure.h"

#include <string.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <sys/random.h>
#endif

int secure_random(void *buf, size_t n)
{
#ifdef _WIN32
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
#else
    unsigned char *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
#endif
}

int secure_new_code(char code[SECURE_CODE_DIGITS + 1])
{
    /* 32 random bits, taken only below 4,294,000,000 (4,294 times 10^6), so
     * each of the 10^6 codes is as likely */
    for (;;) {
        unsigned char b[4];
        if (secure_random(b, sizeof b))
            return -1;
        uint32_t v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        crypto_wipe(b, sizeof b);
        if (v >= 4294000000u)
            continue;
        v %= 1000000u;
        for (int i = SECURE_CODE_DIGITS - 1; i >= 0; i--, v /= 10)
            code[i] = (char)('0' + v % 10);
        code[SECURE_CODE_DIGITS] = 0;
        return 0;
    }
}

int secure_code_from(const char *typed, char code[SECURE_CODE_DIGITS + 1])
{
    int n = 0;
    for (const char *s = typed; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            if (n == SECURE_CODE_DIGITS)
                return -1;
            code[n++] = *s;
        } else if (*s != ' ' && *s != '-' && *s != '.') {
            return -1;
        }
    }
    code[n] = 0;
    return n == SECURE_CODE_DIGITS ? 0 : -1;
}

/* ---- CPace (draft-irtf-cfrg-cpace-21: sections 6 to 8.2, appendix A) ---- */

#define DSI "CPace255"
#define DSI_ISK "CPace255_ISK"
#define SHA512_BLOCK 128

/* LEB128's length then the bytes (prepend_len), into a hash */
static void lv(crypto_sha512_ctx *h, const void *p, size_t n)
{
    uint8_t len[10];
    size_t k = 0;
    size_t v = n;
    do {
        len[k++] = (uint8_t)((v & 0x7f) | (v >= 0x80 ? 0x80 : 0));
        v >>= 7;
    } while (v);
    crypto_sha512_update(h, len, k);
    if (n)
        crypto_sha512_update(h, p, n);
}

static size_t lv_len(size_t n)
{
    size_t k = 1;
    for (size_t v = n >> 7; v; v >>= 7)
        k++;
    return k + n;
}

/* p = 2^255 - 19, little-endian */
static const uint8_t field_p[32] = {
    0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
};

/* d = a - b (little-endian, 32 bytes); the borrow out (1 when a < b) */
static unsigned sub32(uint8_t d[32], const uint8_t a[32], const uint8_t b[32])
{
    unsigned borrow = 0;
    for (int i = 0; i < 32; i++) {
        unsigned v = (unsigned)a[i] - b[i] - borrow;
        d[i] = (uint8_t)v;
        borrow = v >> 8 & 1;
    }
    return borrow;
}

/* r < 2^255 as the field element below 2^254 with the same square.
 * Monocypher's Elligator map reads 254 bits (its representatives use the top
 * two for padding), while CPace's decodeUCoordinate keeps 255. The map
 * depends only on r^2, so r mod p, or p - r when that is 2^254 or more,
 * gives the same point. Constant time: the code is in r */
static void field_254(uint8_t r[32])
{
    uint8_t t[32];
    unsigned below = sub32(t, r, field_p);          /* r - p: taken when r >= p */
    uint8_t keep = (uint8_t)(0 - below);            /* 0xff: r stays */
    for (int i = 0; i < 32; i++)
        r[i] = (uint8_t)((r[i] & keep) | (t[i] & ~keep));
    sub32(t, field_p, r);                           /* p - r */
    uint8_t high = (uint8_t)(0 - (r[31] >> 6 & 1)); /* 0xff: r >= 2^254 */
    for (int i = 0; i < 32; i++)
        r[i] = (uint8_t)((t[i] & high) | (r[i] & ~high));
    crypto_wipe(t, sizeof t);
}

void cpace_generator(uint8_t g[32], const void *prs, size_t prs_n, const void *ci, size_t ci_n, const void *sid,
                     size_t sid_n)
{
    /* generator_string: lv_cat(DSI, PRS, zero_bytes(len_zpad), CI, sid) */
    static const uint8_t zero[SHA512_BLOCK];
    size_t used = 1 + lv_len(prs_n) + lv_len(sizeof DSI - 1);
    size_t zpad = used < SHA512_BLOCK ? SHA512_BLOCK - used : 0;
    crypto_sha512_ctx h;
    crypto_sha512_init(&h);
    lv(&h, DSI, sizeof DSI - 1);
    lv(&h, prs, prs_n);
    lv(&h, zero, zpad);
    lv(&h, ci, ci_n);
    lv(&h, sid, sid_n);
    uint8_t hash[64];
    crypto_sha512_final(&h, hash);
    cpace_map(g, hash);
    crypto_wipe(hash, sizeof hash);
    crypto_wipe(&h, sizeof h);
}

void cpace_map(uint8_t g[32], const uint8_t u[32])
{
    uint8_t r[32];
    memcpy(r, u, 32);
    r[31] &= 0x7f;                  /* decodeUCoordinate: bit 255 dropped */
    field_254(r);
    crypto_elligator_map(g, r);
    crypto_wipe(r, sizeof r);
}

void cpace_point(uint8_t Y[32], const uint8_t y[32], const uint8_t g[32])
{
    crypto_x25519(Y, y, g);
}

int cpace_key(uint8_t isk[SECURE_ISK], const uint8_t y[32], const uint8_t other[32], const void *sid, size_t sid_n,
              const uint8_t Ya[32], const void *ada, size_t ada_n, const uint8_t Yb[32], const void *adb,
              size_t adb_n)
{
    static const uint8_t neutral[32];
    uint8_t K[32];
    crypto_x25519(K, y, other);
    if (!crypto_verify32(K, neutral)) {
        crypto_wipe(K, sizeof K);
        return -1;
    }
    /* H.hash(lv_cat(DSI_ISK, sid, K) || transcript_ir(Ya, ADa, Yb, ADb)) */
    crypto_sha512_ctx h;
    crypto_sha512_init(&h);
    lv(&h, DSI_ISK, sizeof DSI_ISK - 1);
    lv(&h, sid, sid_n);
    lv(&h, K, sizeof K);
    lv(&h, Ya, 32);
    lv(&h, ada, ada_n);
    lv(&h, Yb, 32);
    lv(&h, adb, adb_n);
    crypto_sha512_final(&h, isk);
    crypto_wipe(K, sizeof K);
    crypto_wipe(&h, sizeof h);
    return 0;
}

/* ---- the sealed datagrams ---- */

static const char to_joining[] = "VF4N host to joining PC";
static const char to_host[] = "VF4N joining PC to host";

void secure_keys(const uint8_t isk[SECURE_ISK], int host, secure_out *out, secure_in *in)
{
    memset(out, 0, sizeof *out);
    memset(in, 0, sizeof *in);
    const char *o = host ? to_joining : to_host, *i = host ? to_host : to_joining;
    crypto_blake2b_keyed(out->key, 32, isk, SECURE_ISK, (const uint8_t *)o, strlen(o));
    crypto_blake2b_keyed(in->key, 32, isk, SECURE_ISK, (const uint8_t *)i, strlen(i));
}

void secure_forget(secure_out *out, secure_in *in)
{
    crypto_wipe(out, sizeof *out);
    crypto_wipe(in, sizeof *in);
}

void secure_wipe(void *p, size_t n)
{
    crypto_wipe(p, n);
}

static void nonce_of(uint8_t nonce[24], uint64_t c)
{
    memset(nonce, 0, 24);
    for (int i = 0; i < 8; i++)
        nonce[i] = (uint8_t)(c >> 8 * i);
}

size_t secure_seal(secure_out *out, uint8_t *dg, size_t clear, size_t n)
{
    size_t body = n - clear;
    uint64_t c = out->next++;
    memmove(dg + clear + 8, dg + clear, body);
    for (int i = 0; i < 8; i++)
        dg[clear + i] = (uint8_t)(c >> (56 - 8 * i));      /* big-endian, as the protocol's numbers */
    uint8_t nonce[24];
    nonce_of(nonce, c);
    crypto_aead_lock(dg + clear + 8, dg + clear + 8 + body, out->key, nonce, dg, clear + 8, dg + clear + 8, body);
    return n + SECURE_OVER;
}

size_t secure_open(secure_in *in, uint8_t *dg, size_t clear, size_t n)
{
    if (n < clear + SECURE_OVER)
        return 0;
    uint64_t c = 0;
    for (int i = 0; i < 8; i++)
        c = c << 8 | dg[clear + i];
    if (in->any && c <= in->top &&
        (in->top - c >= SECURE_WINDOW || in->seen[c % SECURE_WINDOW / 64] >> (c % 64) & 1))
        return 0;                   /* came before, or too old to tell */
    size_t body = n - clear - SECURE_OVER;
    uint8_t nonce[24];
    nonce_of(nonce, c);
    if (crypto_aead_unlock(dg + clear + 8, dg + n - 16, in->key, nonce, dg, clear + 8, dg + clear + 8, body))
        return 0;
    if (!in->any || c > in->top) {
        /* the window moves up to c: the counters it passes over have not come */
        uint64_t from = in->any ? in->top + 1 : 0;
        if (c - from >= SECURE_WINDOW)
            memset(in->seen, 0, sizeof in->seen);
        else
            for (uint64_t k = from; k <= c; k++)
                in->seen[k % SECURE_WINDOW / 64] &= ~(1ull << (k % 64));
        in->top = c;
        in->any = 1;
    }
    in->seen[c % SECURE_WINDOW / 64] |= 1ull << (c % 64);
    memmove(dg + clear, dg + clear + 8, body);
    return clear + body;
}
