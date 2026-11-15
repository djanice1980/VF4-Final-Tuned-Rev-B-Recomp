/* tests/secure: the two-PC game's security (runtime/host/secure.c,
 * docs/design.md section 50).
 *
 * 1. CPace against the test vectors of draft-irtf-cfrg-cpace-21, appendix
 *    B.1 (G_X25519 with SHA-512): the generator, both parties' points, the
 *    key (ISK) from either side, and the low-order points of B.1.10 that
 *    must give the neutral element (the key is not made).
 * 2. The generator and the Elligator map against reference.py's values (an
 *    independent reading of the draft, with Python's integers).
 * 3. The join code's digits.
 * 4. The sealed datagrams: open what was sealed; nothing changed is taken;
 *    nothing taken twice; another order within the window; too old; the
 *    other direction's key.
 *
 * usage: make -C tests/secure test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monocypher.h"
#include "reference.h"
#include "secure.h"

static int failed, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAILED: %s\n", what);
    }
}

static void hex(unsigned char *out, const char *h)
{
    for (size_t i = 0; h[2 * i]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static int same_hex(const unsigned char *b, const char *h)
{
    unsigned char t[128];
    hex(t, h);
    return !memcmp(b, t, strlen(h) / 2);
}

/* ---- 1. the draft's vectors (B.1) ---- */
static void draft_vectors(void)
{
    const char *prs = "Password";
    unsigned char ci[24], sid[16], ya[32], yb[32], g[32], Ya[32], Yb[32], isk_a[64], isk_b[64];
    hex(ci, "0b415f696e69746961746f720b425f726573706f6e646572");
    hex(sid, "7e4b4791d6a8ef019b936c79fb7f2c57");
    hex(ya, "21b4f4bd9e64ed355c3eb676a28ebedaf6d8f17bdc365995b319097153044080");
    hex(yb, "848b0779ff415f0af4ea14df9dd1d3c29ac41d836c7808896c4eba19c51ac40a");
    cpace_generator(g, prs, strlen(prs), ci, sizeof ci, sid, sizeof sid);
    check(same_hex(g, "d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f"), "B.1.1 generator g");
    cpace_point(Ya, ya, g);
    cpace_point(Yb, yb, g);
    check(same_hex(Ya, "1d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc989ce0c9f32"), "B.1.2 Ya");
    check(same_hex(Yb, "248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623"), "B.1.3 Yb");
    const char *isk = "6e19b875f7a561d6b3ca3dbb9ef42ac55de3e717881018204b8922b4d5e53bb2"
                      "aa82c300bea7b65d2b671da71922ddf6472301b79bc270adfa8bf413285f2263";
    check(!cpace_key(isk_a, ya, Yb, sid, sizeof sid, Ya, "ADa", 3, Yb, "ADb", 3), "B.1.5 A makes the key");
    check(!cpace_key(isk_b, yb, Ya, sid, sizeof sid, Ya, "ADa", 3, Yb, "ADb", 3), "B.1.5 B makes the key");
    check(same_hex(isk_a, isk), "B.1.5 ISK (initiator-responder), A's");
    check(same_hex(isk_b, isk), "B.1.5 ISK (initiator-responder), B's");

    /* B.1.10: low-order points (and with bit 255 set) */
    static const char *u[12] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "daffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "dbffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "d9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "cdeb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
        "4c9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
    };
    static const char *q[12] = {
        NULL, NULL, NULL, NULL, NULL, NULL,
        "d8e2c776bbacd510d09fd9278b7edcd25fc5ae9adfba3b6e040e8d3b71b21806",
        NULL,
        "c85c655ebe8be44ba9c0ffde69f2fe10194458d137f09bbff725ce58803cdb38",
        "db64dafa9b8fdd136914e61461935fe92aa372cb056314e1231bc4ec12417456",
        "e062dcd5376d58297be2618c7498f55baa07d7e03184e8aada20bca28888bf7a",
        "993c6ad11c4c29da9a56f7691fd0ff8d732e49de6250b6c2e80003ff4629a175",
    };
    unsigned char s[32], ux[32], qx[32], k[64];
    hex(s, "af46e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449aff");
    for (int i = 0; i < 12; i++) {
        char what[80];
        hex(ux, u[i]);
        cpace_point(qx, s, ux);
        int refused = cpace_key(k, s, ux, sid, sizeof sid, ux, "", 0, ux, "", 0) != 0;
        snprintf(what, sizeof what, "B.1.10 u%x: %s", i, q[i] ? "q as given" : "the neutral element");
        check(q[i] ? same_hex(qx, q[i]) : same_hex(qx, u[0]), what);
        snprintf(what, sizeof what, "B.1.10 u%x: the key is %s", i, q[i] ? "made" : "not made");
        check(refused == !q[i], what);
    }
}

/* ---- 2. reference.py's values ---- */
static void reference_values(void)
{
    int bad = 0;
    for (size_t i = 0; i < sizeof map_cases / sizeof map_cases[0]; i++) {
        unsigned char g[32];
        cpace_map(g, map_cases[i].u);
        if (memcmp(g, map_cases[i].g, 32)) {
            printf("  map case %zu differs\n", i);
            bad++;
        }
    }
    check(!bad, "the Elligator map at the field's edges and random points, as reference.py");
    bad = 0;
    /* gen_bytes holds four rows a case (code, CI, sid, g), each padded to 300 bytes */
    for (size_t i = 0; i < sizeof gen_lens / sizeof gen_lens[0]; i++) {
        unsigned char g[32];
        cpace_generator(g, gen_bytes[4 * i], gen_lens[i].prs_n, gen_bytes[4 * i + 1], gen_lens[i].ci_n,
                        gen_bytes[4 * i + 2], gen_lens[i].sid_n);
        if (memcmp(g, gen_bytes[4 * i + 3], 32)) {
            printf("  generator case %zu (code %u bytes, CI %u, sid %u) differs\n", i, gen_lens[i].prs_n,
                   gen_lens[i].ci_n, gen_lens[i].sid_n);
            bad++;
        }
    }
    check(!bad, "the generator for codes, CIs and sids of many lengths, as reference.py");
}

/* ---- 3. the join code ---- */
static void codes(void)
{
    int counts[10] = {0}, bad = 0;
    for (int i = 0; i < 20000; i++) {
        char c[8];
        if (secure_new_code(c) || strlen(c) != 6)
            bad++;
        for (int k = 0; k < 6; k++)
            if (c[k] < '0' || c[k] > '9')
                bad++;
            else
                counts[c[k] - '0']++;
    }
    check(!bad, "new codes: six digits");
    int even = 1;
    for (int d = 0; d < 10; d++)
        even &= counts[d] > 11000 && counts[d] < 13000;      /* 12,000 each expected */
    check(even, "new codes: each digit about as often");
    char c[8];
    check(!secure_code_from("482 913", c) && !strcmp(c, "482913"), "typed with a space");
    check(!secure_code_from("482-913", c) && !strcmp(c, "482913"), "typed with a dash");
    check(secure_code_from("48291", c) != 0, "five digits are not a code");
    check(secure_code_from("4829131", c) != 0, "seven digits are not a code");
    check(secure_code_from("48a913", c) != 0, "a letter is not a code");
}

/* ---- 4. the sealed datagrams ---- */
static size_t seal(secure_out *o, unsigned char *b, int i)
{
    memcpy(b, "VF4N\x01\x86\x00\x00\x00\x2a", 10);
    for (int k = 0; k < 40; k++)
        b[10 + k] = (unsigned char)(i * 7 + k);
    return secure_seal(o, b, 10, 50);
}

static int plain_ok(const unsigned char *b, size_t n, int i)
{
    if (n != 50 || memcmp(b, "VF4N\x01\x86\x00\x00\x00\x2a", 10))
        return 0;
    for (int k = 0; k < 40; k++)
        if (b[10 + k] != (unsigned char)(i * 7 + k))
            return 0;
    return 1;
}

static void sealed(void)
{
    unsigned char isk[64], other[64];
    secure_random(isk, sizeof isk);
    secure_random(other, sizeof other);
    secure_out h_out, j_out, x_out;
    secure_in h_in, j_in, x_in;
    secure_keys(isk, 1, &h_out, &h_in);
    secure_keys(isk, 0, &j_out, &j_in);
    secure_keys(other, 0, &x_out, &x_in);
    unsigned char b[128], c[128];

    size_t n = seal(&j_out, b, 0);
    check(n == 50 + SECURE_OVER, "a sealed datagram is 24 bytes longer");
    memcpy(c, b, n);
    check(plain_ok(b, secure_open(&h_in, b, 10, n), 0), "the host opens the joining PC's datagram");
    check(secure_open(&h_in, c, 10, n) == 0, "the same datagram again is not taken");

    n = seal(&h_out, b, 1);
    check(plain_ok(b, secure_open(&j_in, b, 10, n), 1), "the joining PC opens the host's datagram");

    /* each byte changed: not taken (and the window does not move) */
    int bad = 0;
    for (size_t at = 0; at < 50 + SECURE_OVER; at++) {
        n = seal(&j_out, b, 2);
        b[at] ^= 0x10;
        if (secure_open(&h_in, b, 10, n))
            bad++;
    }
    check(!bad, "any byte changed (header, counter, body, seal): not taken");
    n = seal(&j_out, b, 3);
    check(secure_open(&h_in, b, 10, n - 1) == 0, "cut short: not taken");
    n = seal(&j_out, b, 4);
    memcpy(c, b, n);
    check(secure_open(&j_in, c, 10, n) == 0, "with the other direction's key: not taken");
    check(secure_open(&x_in, c, 10, n) == 0, "with another try's key (another code): not taken");
    check(plain_ok(b, secure_open(&h_in, b, 10, n), 4), "and the right one still opens it");

    /* another order: 20 datagrams, opened last first */
    static unsigned char many[5000][128];
    size_t len[5000];
    for (int i = 0; i < 20; i++)
        len[i] = seal(&j_out, many[i], 100 + i);
    bad = 0;
    for (int i = 19; i >= 0; i--) {
        memcpy(c, many[i], len[i]);
        if (!plain_ok(c, secure_open(&h_in, c, 10, len[i]), 100 + i))
            bad++;
    }
    check(!bad, "within the window, in another order: all taken");
    bad = 0;
    for (int i = 0; i < 20; i++) {
        memcpy(c, many[i], len[i]);
        if (secure_open(&h_in, c, 10, len[i]))
            bad++;
    }
    check(!bad, "and none of them twice");

    /* the window: 5,000 sealed; the newest opened first; older than the window are not taken */
    for (int i = 0; i < 5000; i++)
        len[i] = seal(&j_out, many[i], i);
    memcpy(c, many[4999], len[4999]);
    check(plain_ok(c, secure_open(&h_in, c, 10, len[4999]), 4999), "a jump ahead of 5,000 counters");
    memcpy(c, many[4999 - SECURE_WINDOW], len[4999 - SECURE_WINDOW]);
    check(secure_open(&h_in, c, 10, len[4999 - SECURE_WINDOW]) == 0, "a counter the window has passed");
    memcpy(c, many[4999 - SECURE_WINDOW + 1], len[4999 - SECURE_WINDOW + 1]);
    check(plain_ok(c, secure_open(&h_in, c, 10, len[4999 - SECURE_WINDOW + 1]), 4999 - SECURE_WINDOW + 1),
          "the oldest counter still in the window");
    bad = 0;
    for (int i = 4999 - SECURE_WINDOW + 2; i < 4999; i++) {
        memcpy(c, many[i], len[i]);
        if (!plain_ok(c, secure_open(&h_in, c, 10, len[i]), i))
            bad++;
    }
    check(!bad, "all others in the window, after the newest");

    secure_forget(&h_out, &h_in);
    n = seal(&j_out, b, 9);
    check(secure_open(&h_in, b, 10, n) == 0, "forgotten keys open nothing");
    crypto_wipe(isk, sizeof isk);
}

int main(void)
{
    draft_vectors();
    reference_values();
    codes();
    sealed();
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
