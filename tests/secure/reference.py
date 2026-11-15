#!/usr/bin/env python3
"""Reference values for tests/secure: CPace's calculate_generator for
G_X25519 with SHA-512 (draft-irtf-cfrg-cpace-21, sections 8.1 and 8.2 and
appendix A), written from the draft's definitions with Python's integers.
It shares nothing with runtime/host/secure.c (which does the arithmetic
with Monocypher), so the two agreeing checks the C side's steps: the
generator string's lengths and padding, decodeUCoordinate's 255 bits, and
the reduction secure.c makes for Monocypher's 254-bit Elligator map.

usage: python3 -I tests/secure/reference.py <out.h>

Writes a C header: map_cases (a u coordinate, the point Elligator 2 gives)
and gen_cases (a code, CI, sid, the generator), from a fixed seed.
"""
import hashlib
import random
import sys

P = 2 ** 255 - 19
A = 486662


def leb128(n):
    out = b""
    while True:
        if n < 128:
            out += bytes([n])
        else:
            out += bytes([(n & 0x7f) | 0x80])
        n >>= 7
        if n == 0:
            return out


def prepend_len(b):
    return leb128(len(b)) + b


def lv_cat(*args):
    return b"".join(prepend_len(a) for a in args)


def generator_string(dsi, prs, ci, sid, s_in_bytes=128):
    zpad = max(0, s_in_bytes - 1 - len(prepend_len(prs)) - len(prepend_len(dsi)))
    return lv_cat(dsi, prs, bytes(zpad), ci, sid)


def decode_u(b):
    return int.from_bytes(b[:32], "little") & ((1 << 255) - 1)


def elligator2(r):
    """The draft's A.5: Z = 2 for Curve25519; the u coordinate only."""
    r %= P
    v = -A * pow(1 + 2 * r * r, P - 2, P) % P
    e = pow((v * v * v + A * v * v + v) % P, (P - 1) // 2, P)
    assert e in (1, P - 1)      # never 0 on Curve25519 (A^2 - 4 is not a square)
    x = v if e == 1 else (-v - A) % P
    return x.to_bytes(32, "little")


def generator(prs, ci, sid):
    h = hashlib.sha512(generator_string(b"CPace255", prs, ci, sid)).digest()[:32]
    return elligator2(decode_u(h))


def c_bytes(b):
    return "{" + ",".join("0x%02x" % x for x in b) + "}" if b else "{0}"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    # the draft's own vector (B.1.1) first: this file reads the draft as secure.c does
    g = generator(b"Password", bytes.fromhex("0b415f696e69746961746f720b425f726573706f6e646572"),
                  bytes.fromhex("7e4b4791d6a8ef019b936c79fb7f2c57"))
    assert g.hex() == "d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f", g.hex()

    rnd = random.Random(20261115)
    # u coordinates around the edges: p and above (reduced), 2^254 and above (bit 254 set), bit 255 set
    us = [0, 1, 2, 18, 19, P - 1, P, P + 1, P + 5, P + 18, 2 ** 254 - 1, 2 ** 254, 2 ** 254 + 7, 2 ** 255 - 1,
          2 ** 255, 2 ** 256 - 1, 2 ** 255 + 2 ** 254 + 3]
    us += [rnd.getrandbits(256) for _ in range(40)]
    maps = [(u.to_bytes(32, "little"), elligator2(decode_u(u.to_bytes(32, "little")))) for u in us]

    gens = []
    for i in range(40):
        prs = bytes(rnd.choice(b"0123456789") for _ in range(6)) if i < 20 else rnd.randbytes(rnd.choice(
            [0, 1, 6, 100, 108, 109, 110, 127, 128, 200, 300]))
        ci = rnd.randbytes(rnd.choice([0, 5, 40, 127, 128, 129, 150]))
        sid = rnd.randbytes(rnd.choice([0, 16, 32]))
        gens.append((prs, ci, sid, generator(prs, ci, sid)))

    out = ["/* written by tests/secure/reference.py: do not edit */",
           "static const struct { unsigned char u[32], g[32]; } map_cases[] = {"]
    out += ["    {%s, %s}," % (c_bytes(u), c_bytes(g)) for u, g in maps]
    out += ["};", "static const unsigned char gen_bytes[][300] = {"]
    for prs, ci, sid, g in gens:
        out += ["    %s, %s, %s, %s," % (c_bytes(prs), c_bytes(ci), c_bytes(sid), c_bytes(g))]
    out += ["};", "static const struct { unsigned prs_n, ci_n, sid_n; } gen_lens[] = {"]
    out += ["    {%d, %d, %d}," % (len(prs), len(ci), len(sid)) for prs, ci, sid, _ in gens]
    out += ["};", ""]
    with open(sys.argv[1], "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
