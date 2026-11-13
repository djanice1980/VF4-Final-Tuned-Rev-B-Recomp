"""A reader for MAME's CHD v5 disc images (the format chdman writes), with
the CD codecs: LZMA (cdlz), Deflate (cdzl), FLAC (cdfl) and, where the
Python has it, Zstandard (cdzs). Written from the public description of
the format; no code from MAME or libchdr.

Only what the builder needs: the header, the hunk map, hunks, the CD track
metadata, and the data of a CD frame. Subcode and the regenerated sync
and ECC of CD sectors are not rebuilt (only the 2,048 user bytes of a
sector are used), so hunk CRCs are not checked; the map's CRC is, and the
files taken from the disc are checked by SHA-1 afterwards.
"""
import binascii
import lzma
import struct
import zlib

from . import flac

FRAME = 2448                  # a CD frame in a CHD: 2,352 bytes of sector, 96 of subcode
SECTOR = 2352
SUBCODE = 96

# map entry types
TYPE_0, TYPE_1, TYPE_2, TYPE_3 = 0, 1, 2, 3
NONE, SELF, PARENT = 4, 5, 6
RLE_SMALL, RLE_LARGE = 7, 8
SELF_0, SELF_1 = 9, 10
PARENT_SELF, PARENT_0, PARENT_1 = 11, 12, 13


class ChdError(Exception):
    pass


class Bits:
    """Reads a bitstream most significant bit first."""

    def __init__(self, data):
        self.data = data
        self.pos = 0                       # in bits

    def read(self, n):
        """n bits (at most 56) as an unsigned number; zeros past the end."""
        if n == 0:
            return 0
        byte, sh = self.pos >> 3, self.pos & 7
        w = int.from_bytes(self.data[byte:byte + 8].ljust(8, b"\0"), "big")
        self.pos += n
        return (w >> (64 - sh - n)) & ((1 << n) - 1)

    def peek(self, n):
        p = self.pos
        v = self.read(n)
        self.pos = p
        return v


class Huffman:
    """The map's Huffman code: 16 symbols, codes of at most 8 bits, the code
    lengths sent run-length coded, codes assigned canonically with the
    longest codes taking the lowest values."""

    def __init__(self, bits, numcodes=16, maxbits=8):
        nb = 5 if maxbits >= 16 else 4 if maxbits >= 8 else 3
        lengths = []
        while len(lengths) < numcodes:
            v = bits.read(nb)
            if v != 1:
                lengths.append(v)
                continue
            v = bits.read(nb)
            if v == 1:
                lengths.append(1)
            else:
                lengths.extend([v] * (bits.read(nb) + 3))
        if len(lengths) != numcodes:
            raise ChdError("bad Huffman table in the map")
        hist = [0] * 33
        for n in lengths:
            if n > maxbits:
                raise ChdError("bad Huffman code length in the map")
            hist[n] += 1
        start = 0
        for n in range(32, 0, -1):
            nxt = (start + hist[n]) >> 1
            if n != 1 and nxt * 2 != start + hist[n]:
                raise ChdError("inconsistent Huffman table in the map")
            hist[n] = start
            start = nxt
        self.maxbits = maxbits
        self.table = [None] * (1 << maxbits)
        for sym, n in enumerate(lengths):
            if n == 0:
                continue
            code = hist[n]
            hist[n] += 1
            shift = maxbits - n
            for k in range(code << shift, (code + 1) << shift):
                self.table[k] = (sym, n)

    def decode(self, bits):
        e = self.table[bits.peek(self.maxbits)]
        if e is None:
            raise ChdError("bad Huffman code in the map")
        bits.pos += e[1]
        return e[0]


def _lzma(data, size):
    d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[
        {"id": lzma.FILTER_LZMA1, "dict_size": 1 << 20, "lc": 3, "lp": 0, "pb": 2}])
    out = d.decompress(data, max_length=size)
    if len(out) != size:
        raise ChdError("LZMA data ended early")
    return out


def _deflate(data, size):
    d = zlib.decompressobj(-15)
    out = d.decompress(data, size)
    if len(out) != size:
        raise ChdError("Deflate data ended early")
    return out


def _zstd(data, size):
    try:
        from compression import zstd
    except ImportError:
        raise ChdError("this CHD uses Zstandard, which needs Python 3.14 or later")
    out = zstd.ZstdDecompressor().decompress(data, max_length=size)
    if len(out) != size:
        raise ChdError("Zstandard data ended early")
    return out


class Chd:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(124)
        if len(h) < 124 or h[:8] != b"MComprHD":
            raise ChdError("not a CHD file")
        length, version = struct.unpack(">II", h[8:16])
        if version != 5:
            raise ChdError("CHD version %d; only version 5 (MAME 0.146 and later) is read" % version)
        self.codecs = [h[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logical, self.mapoffset, self.metaoffset = struct.unpack(">QQQ", h[32:56])
        self.hunkbytes, self.unitbytes = struct.unpack(">II", h[56:64])
        self.rawsha1 = h[64:84].hex()
        self.sha1 = h[84:104].hex()
        self.parentsha1 = h[104:124].hex()
        if any(h[104:124]):
            raise ChdError("this CHD needs a parent CHD")
        self.hunks = (self.logical + self.hunkbytes - 1) // self.hunkbytes
        self._read_map()
        self.cache = {}

    def _read_map(self):
        if self.codecs[0] == b"\0\0\0\0":
            raise ChdError("uncompressed CHDs are not read")
        self.f.seek(self.mapoffset)
        mh = self.f.read(16)
        mapbytes = struct.unpack(">I", mh[0:4])[0]
        firstoffs = int.from_bytes(mh[4:10], "big")
        mapcrc = struct.unpack(">H", mh[10:12])[0]
        lengthbits, selfbits, parentbits = mh[12], mh[13], mh[14]
        bits = Bits(self.f.read(mapbytes))
        huff = Huffman(bits)
        types = bytearray(self.hunks)
        last, rep = 0, 0
        for i in range(self.hunks):
            if rep:
                types[i] = last
                rep -= 1
                continue
            v = huff.decode(bits)
            if v == RLE_SMALL:
                types[i] = last
                rep = 2 + huff.decode(bits)
            elif v == RLE_LARGE:
                types[i] = last
                rep = 2 + 16 + (huff.decode(bits) << 4)
                rep += huff.decode(bits)
            else:
                types[i] = last = v
        raw = bytearray()
        self.map = []
        cur, last_self, last_parent = firstoffs, 0, 0
        units = self.hunkbytes // self.unitbytes
        for i in range(self.hunks):
            t = types[i]
            off, n, crc = cur, 0, 0
            if t <= TYPE_3:
                n = bits.read(lengthbits)
                crc = bits.read(16)
                cur += n
            elif t == NONE:
                n = self.hunkbytes
                crc = bits.read(16)
                cur += n
            elif t == SELF:
                off = last_self = bits.read(selfbits)
            elif t == PARENT:
                off = last_parent = bits.read(parentbits)
            elif t in (SELF_0, SELF_1):
                if t == SELF_1:
                    last_self += 1
                t, off = SELF, last_self
            elif t == PARENT_SELF:
                t = PARENT
                off = last_parent = i * units
            elif t in (PARENT_0, PARENT_1):
                if t == PARENT_1:
                    last_parent += units
                t, off = PARENT, last_parent
            else:
                raise ChdError("bad map entry type %d" % t)
            raw += bytes([t]) + n.to_bytes(3, "big") + off.to_bytes(6, "big") + crc.to_bytes(2, "big")
            self.map.append((t, n, off))
        if binascii.crc_hqx(bytes(raw), 0xffff) != mapcrc:
            raise ChdError("the hunk map's CRC is wrong (a damaged file)")

    def codec_counts(self):
        names = {SELF: "copy", PARENT: "parent", NONE: "none"}
        out = {}
        for t, _, _ in self.map:
            k = names.get(t) or self.codecs[t].decode("latin-1")
            out[k] = out.get(k, 0) + 1
        return out

    def hunk(self, i):
        """The sector data of a hunk's frames, subcode left out (the CD
        codecs keep it apart): frames * 2,352 bytes. Only for CD CHDs."""
        if i in self.cache:
            return self.cache[i]
        t, n, off = self.map[i]
        frames = self.hunkbytes // FRAME
        size = frames * SECTOR
        if t == SELF:
            return self.hunk(off)
        if t == PARENT:
            raise ChdError("this CHD needs a parent CHD")
        self.f.seek(off)
        src = self.f.read(n)
        if t == NONE:
            out = b"".join(src[k * FRAME:k * FRAME + SECTOR] for k in range(frames))
        else:
            codec = self.codecs[t]
            if codec == b"cdfl":
                out = flac.decode_cd(src, frames * SECTOR // 4)
            elif codec in (b"cdlz", b"cdzl", b"cdzs"):
                ecc = (frames + 7) // 8
                lb = 2 if self.hunkbytes < 65536 else 3
                base = int.from_bytes(src[ecc:ecc + lb], "big")
                data = src[ecc + lb:ecc + lb + base]
                out = {b"cdlz": _lzma, b"cdzl": _deflate, b"cdzs": _zstd}[codec](data, size)
            else:
                raise ChdError("unsupported CHD codec %r" % codec)
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[i] = out
        return out

    def metadata(self):
        """(tag, data) of every metadata entry."""
        out, off = [], self.metaoffset
        while off:
            self.f.seek(off)
            tag, fl, nxt = struct.unpack(">4sIQ", self.f.read(16))
            n = fl & 0xffffff
            out.append((tag, self.f.read(n)))
            off = nxt
        return out

    def tracks(self):
        """[(number, type, first CHD frame, frames)] from the GD-ROM (CHGD)
        or CD (CHT2) metadata; each track is padded to 4 frames in the CHD."""
        out, frame = [], 0
        for tag, data in self.metadata():
            if tag not in (b"CHGD", b"CHT2", b"CHTR"):
                continue
            f = dict(kv.split(":", 1) for kv in data.rstrip(b"\0").decode("ascii").split())
            n = int(f["FRAMES"])
            out.append((int(f["TRACK"]), f["TYPE"], frame, n))
            frame += (n + 3) // 4 * 4
        return out

    def frame_data(self, frame):
        """The 2,352 bytes of sector data of a CHD frame."""
        per = self.hunkbytes // FRAME
        h = self.hunk(frame // per)
        k = frame % per
        return h[k * SECTOR:(k + 1) * SECTOR]
