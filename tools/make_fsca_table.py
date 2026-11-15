#!/usr/bin/env python3
"""Writes runtime/cpu/fsca_table.inc: FSCA's sine for a quarter turn, the
table runtime/cpu/fsca.h gives every angle's sine and cosine from
(docs/design.md section 45).

FSCA takes a 16-bit angle (65,536 to a turn) and gives its sine and cosine
as floats. The runtime used to compute them with the host's sin() and cos()
in double precision, then round to float. A table makes the bits the same
on every PC whatever its math library. Its values are those same results:
this script computes them the way op_fsca did, with Python's math.sin and
math.cos, which are the C library's. Run it on Linux (glibc), where they
were checked to match the 32-bit and 64-bit builds and Wine's and Proton's
ucrtbase for all 65,536 angles, so translated runs keep their hashes.

The quarter table (angles 0 to 16,384) gives every sine and cosine by the
usual symmetries except three: the double-precision angle for a half turn
is not exactly pi, nor a quarter or three quarters exactly pi/2 or 3pi/2,
so sin(32768), cos(16384) and cos(49152) are tiny numbers rather than 0.
fsca.h keeps those three as constants. The script checks all 131,072 values
against the table before writing it.

usage: python3 -I tools/make_fsca_table.py [OUT]   (default runtime/cpu/fsca_table.inc)
"""
import math
import os
import struct
import sys

STEP = 6.283185307179586476925 / 65536.0    # op_fsca's: 2 pi / 65536 in double


def f32(x):
    """x rounded to float, as its bits"""
    return struct.unpack("<I", struct.pack("<f", x))[0]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "runtime", "cpu", "fsca_table.inc")
    sin_bits = [f32(math.sin(k * STEP)) for k in range(65536)]
    cos_bits = [f32(math.cos(k * STEP)) for k in range(65536)]
    quarter = sin_bits[:16385]
    special = {"sin_half": sin_bits[32768], "cos_quarter": cos_bits[16384], "cos_3quarter": cos_bits[49152]}

    # fsca.h's rule, checked against every value
    def sin_of(k):
        k &= 0xffff
        r = k & 0x3fff
        q = k >> 14
        if q == 0:
            return quarter[r]
        if q == 1:
            return quarter[0x4000 - r]
        if q == 2:
            return quarter[r] ^ 0x80000000 if r else special["sin_half"]
        return quarter[0x4000 - r] ^ 0x80000000

    def cos_of(k):
        k &= 0xffff
        if k == 0x4000:
            return special["cos_quarter"]
        if k == 0xc000:
            return special["cos_3quarter"]
        return sin_of(k + 0x4000)

    bad = [k for k in range(65536) if sin_of(k) != sin_bits[k] or cos_of(k) != cos_bits[k]]
    if bad:
        sys.exit("the table's rule misses %d angles (first %d): this C library's sin/cos differ from "
                 "glibc's; run this on Linux" % (len(bad), bad[0]))
    expect = {"sin_half": 0x250d3132, "cos_quarter": 0x248d3132, "cos_3quarter": 0xa553c9ca}
    if special != expect:
        sys.exit("the three special values differ from fsca.h's: %s" % special)

    lines = ["/* runtime/cpu/fsca_table.inc: written by tools/make_fsca_table.py, do not edit. FSCA's",
             " * sine for angles 0 to 16384 (a quarter turn), as float bits (docs/design.md section 45). */"]
    for i in range(0, 16385, 8):
        lines.append(" ".join("0x%08xu," % b for b in quarter[i:i + 8]))
    with open(out + ".tmp", "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(out + ".tmp", out)
    print("%s: 16385 values; all 65536 sines and cosines checked" % out)


if __name__ == "__main__":
    main()
