"""A FLAC frame decoder for CHD's CD FLAC codec (cdfl), written from the
FLAC format specification (RFC 9639). A cdfl hunk is bare FLAC frames
(no stream header) of 16-bit stereo at 44.1 kHz, then the Deflate-packed
subcode, which the builder does not need. The samples come out big-endian,
as CHD stores CD sectors.
"""
import array
import sys


class FlacError(Exception):
    pass


class Bits:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def read(self, n):
        if n == 0:
            return 0
        if n > 56:
            hi = self.read(n - 32)
            return (hi << 32) | self.read(32)
        byte, sh = self.pos >> 3, self.pos & 7
        if byte >= len(self.data):
            raise FlacError("FLAC data ended early")
        w = int.from_bytes(self.data[byte:byte + 8].ljust(8, b"\0"), "big")
        self.pos += n
        return (w >> (64 - sh - n)) & ((1 << n) - 1)

    def signed(self, n):
        v = self.read(n)
        return v - (1 << n) if n and v >> (n - 1) else v

    def unary(self):
        """Zeros before the next one bit."""
        n = 0
        while True:
            byte, sh = self.pos >> 3, self.pos & 7
            if byte >= len(self.data):
                raise FlacError("FLAC data ended early")
            b = (self.data[byte] << sh) & 0xff
            if b:
                z = 8 - b.bit_length()
                self.pos += z + 1
                return n + z
            n += 8 - sh
            self.pos += 8 - sh

    def align(self):
        self.pos = (self.pos + 7) & ~7


BLOCK = {1: 192, 2: 576, 3: 1152, 4: 2304, 5: 4608}
DEPTH = {1: 8, 2: 12, 4: 16, 5: 20, 6: 24, 7: 32}


def _residual(b, block, order, out):
    method = b.read(2)
    if method > 1:
        raise FlacError("reserved residual coding method")
    pbits, escape = (4, 15) if method == 0 else (5, 31)
    porder = b.read(4)
    parts = 1 << porder
    for p in range(parts):
        n = (block >> porder) - (order if p == 0 else 0)
        k = b.read(pbits)
        if k == escape:
            w = b.read(5)
            for _ in range(n):
                out.append(b.signed(w))
            continue
        for _ in range(n):
            q = b.unary()
            v = (q << k) | b.read(k)
            out.append((v >> 1) ^ -(v & 1))


def _subframe(b, block, bps):
    if b.read(1):
        raise FlacError("bad subframe padding")
    t = b.read(6)
    wasted = 0
    if b.read(1):
        wasted = b.unary() + 1
        bps -= wasted
    if t == 0:
        s = [b.signed(bps)] * block
    elif t == 1:
        s = [b.signed(bps) for _ in range(block)]
    elif 8 <= t <= 12:
        order = t - 8
        s = [b.signed(bps) for _ in range(order)]
        res = []
        _residual(b, block, order, res)
        for r in res:
            if order == 0:
                p = 0
            elif order == 1:
                p = s[-1]
            elif order == 2:
                p = 2 * s[-1] - s[-2]
            elif order == 3:
                p = 3 * s[-1] - 3 * s[-2] + s[-3]
            else:
                p = 4 * s[-1] - 6 * s[-2] + 4 * s[-3] - s[-4]
            s.append(p + r)
    elif t >= 32:
        order = t - 31
        s = [b.signed(bps) for _ in range(order)]
        prec = b.read(4) + 1
        if prec == 16:
            raise FlacError("bad LPC precision")
        shift = b.signed(5)
        coef = [b.signed(prec) for _ in range(order)]
        res = []
        _residual(b, block, order, res)
        rc = coef[::-1]
        for r in res:
            acc = 0
            base = len(s) - order
            for j in range(order):
                acc += rc[j] * s[base + j]
            s.append((acc >> shift) + r)
    else:
        raise FlacError("reserved subframe type %d" % t)
    if wasted:
        s = [v << wasted for v in s]
    return s


def decode_cd(data, samples):
    """`samples` stereo 16-bit samples from bare FLAC frames, as big-endian bytes."""
    b = Bits(data)
    left, right = [], []
    while len(left) < samples:
        if b.read(15) != 0x7ffc:
            raise FlacError("lost FLAC frame sync")
        b.read(1)                                    # blocking strategy
        bs, sr = b.read(4), b.read(4)
        chan, depth = b.read(4), b.read(3)
        b.read(1)
        first = b.read(8)                            # the frame or sample number, UTF-8 style
        extra = 0
        while first & (0x80 >> extra) and extra < 7:
            extra += 1
        for _ in range(max(0, extra - 1)):
            b.read(8)
        if bs == 6:
            block = b.read(8) + 1
        elif bs == 7:
            block = b.read(16) + 1
        elif bs >= 8:
            block = 256 << (bs - 8)
        elif bs in BLOCK:
            block = BLOCK[bs]
        else:
            raise FlacError("reserved block size")
        if sr == 12:
            b.read(8)
        elif sr in (13, 14):
            b.read(16)
        b.read(8)                                    # header CRC-8
        bps = 16 if depth == 0 else DEPTH.get(depth)
        if bps is None or chan > 10:
            raise FlacError("unsupported FLAC frame")
        if chan == 1:
            a = _subframe(b, block, bps)
            c = _subframe(b, block, bps)
        elif chan == 8:                              # left, side
            a = _subframe(b, block, bps)
            sd = _subframe(b, block, bps + 1)
            c = [x - y for x, y in zip(a, sd)]
        elif chan == 9:                              # side, right
            sd = _subframe(b, block, bps + 1)
            c = _subframe(b, block, bps)
            a = [x + y for x, y in zip(sd, c)]
        elif chan == 10:                             # mid, side
            m = _subframe(b, block, bps)
            sd = _subframe(b, block, bps + 1)
            a, c = [], []
            for x, y in zip(m, sd):
                x = (x << 1) | (y & 1)
                a.append((x + y) >> 1)
                c.append((x - y) >> 1)
        else:
            raise FlacError("CD FLAC data is not stereo")
        b.align()
        b.read(16)                                   # frame CRC-16
        left += a
        right += c
    pcm = array.array("h", [0]) * (2 * samples)
    pcm[0::2] = array.array("h", left[:samples])
    pcm[1::2] = array.array("h", right[:samples])
    if sys.byteorder == "little":
        pcm.byteswap()
    return pcm.tobytes()
