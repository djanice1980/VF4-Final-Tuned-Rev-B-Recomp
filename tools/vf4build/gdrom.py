"""The ISO 9660 file system in a GD-ROM's high-density area (ECMA-119).

The high-density area starts at LBA 45,000; its directory records hold
absolute LBAs. In MAME's CHD of a GD-ROM, track 3 is that area.
"""
from . import chd as chdmod

HD_START = 45000


class Disc:
    def __init__(self, c):
        self.c = c
        t3 = [t for t in c.tracks() if t[0] == 3]
        if not t3 or not t3[0][1].startswith("MODE1"):
            raise chdmod.ChdError("no high-density data track (track 3): not a GD-ROM")
        self.first = t3[0][2]
        self.frames = t3[0][3]

    def sector(self, lba):
        k = lba - HD_START
        if not 0 <= k < self.frames:
            raise chdmod.ChdError("sector %d is outside the high-density area" % lba)
        return self.c.frame_data(self.first + k)[16:16 + 2048]

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            out += self.sector(lba)
            lba += 1
        return bytes(out[:size])

    def files(self):
        """{path: (lba, size)} of every file."""
        pvd = self.sector(HD_START + 16)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise chdmod.ChdError("no ISO 9660 volume in the high-density area")
        root = pvd[156:156 + 34]
        out = {}
        self._dir(int.from_bytes(root[2:6], "little"), int.from_bytes(root[10:14], "little"), "", out, 0)
        return out

    def _dir(self, lba, size, prefix, out, depth):
        if depth > 16:
            return
        data = self.read(lba, size)
        pos = 0
        while pos < len(data):
            n = data[pos]
            if n == 0:                       # records never cross a sector
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + n]
            pos += n
            ident = rec[33:33 + rec[32]]
            if ident in (b"\0", b"\1"):
                continue
            name = ident.decode("ascii", "replace").split(";")[0]
            elba = int.from_bytes(rec[2:6], "little")
            esize = int.from_bytes(rec[10:14], "little")
            if rec[25] & 2:
                self._dir(elba, esize, prefix + name + "/", out, depth + 1)
            else:
                out[prefix + name] = (elba, esize)

    def read_file(self, path, out=None, progress=None):
        """A file's bytes, or written to `out` (a writable file) in pieces."""
        lba, size = self.files()[path]
        if out is None:
            return self.read(lba, size)
        done = 0
        while done < size:
            n = min(size - done, 2048 * 256)
            out.write(self.read(lba + done // 2048, n))
            done += n
            if progress:
                progress(done, size)
