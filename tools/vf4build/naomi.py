"""The player's MAME set: finding it, checking it, and taking out what the
build needs.

  vf4tuned.zip    the cartridge's PIC (317-0387-com.pic): the DES key of
                  the DIMM image and the name of the boot file
  gds-0036f.chd   the GD-ROM: BHX.BIN names the DIMM image, VF41_S.BIN,
                  which the DIMM board loads into its memory, decrypted

The DIMM image is kept as it is on the disc (encrypted) with its key; the
game decrypts it into memory when it starts (runtime/host/dimm_key.c). The
builder decrypts only the two programs it translates.
"""
import hashlib
import os
import zipfile

from . import chd, des, gdrom

CHD_SHA1 = "68edece4239e5adfef9df143defb711ff2b4db72"     # MAME's vf4tuned: gds-0036f
IMAGE_SHA1 = "bc14725891838940221cc39b8bb653ebc2b02b21"   # VF41_S.BIN as on the disc
IMAGE_SIZE = 251658240
PIC_NAME = "317-0387-com.pic"
# the Naomi header's load tables (0x360 main, 0x3c0 test): image offset, size
PROGRAMS = {
    "main": (0x020000, 0x340000, "e1288dda29d526c086086042b760fdc10be4662a"),
    "test": (0x360000, 0x080000, "29ab3e757ec3b734f7a9ec7d260cebfad9caa0e1"),
}


class SetError(Exception):
    pass


def find_set(folder):
    """(chd path, zip path) in a folder: both side by side, or MAME's ROM
    folder layout (vf4tuned.zip, vf4tuned/gds-0036f.chd)."""
    folder = os.path.abspath(folder)
    cands_chd = [os.path.join(folder, "gds-0036f.chd"), os.path.join(folder, "vf4tuned", "gds-0036f.chd")]
    cands_zip = [os.path.join(folder, "vf4tuned.zip"), os.path.join(os.path.dirname(folder), "vf4tuned.zip")]
    c = next((p for p in cands_chd if os.path.isfile(p)), None)
    z = next((p for p in cands_zip if os.path.isfile(p)), None)
    missing = []
    if not c:
        missing.append("gds-0036f.chd (in the folder or in its vf4tuned/ folder)")
    if not z:
        missing.append("vf4tuned.zip")
    if missing:
        raise SetError("not found in %s: %s" % (folder, "; ".join(missing)))
    return c, z


def read_pic(zip_path):
    try:
        with zipfile.ZipFile(zip_path) as z:
            names = {n.lower(): n for n in z.namelist()}
            if PIC_NAME not in names:
                raise SetError("%s has no %s: is it the vf4tuned set?" % (zip_path, PIC_NAME))
            pic = z.read(names[PIC_NAME])
    except zipfile.BadZipFile:
        raise SetError("%s is not a zip file" % zip_path)
    if len(pic) != 0x4000:
        raise SetError("%s in %s is %d bytes, not 16,384" % (PIC_NAME, zip_path, len(pic)))
    return pic


def pic_key(pic):
    """The DES key (8 bytes, as DES takes them) and the boot file's name."""
    k = bytes(pic[0x780 + 2 * i] for i in range(7)) + bytes([pic[0x7a0]])
    name = bytes(pic[0x7c0 + 2 * i] for i in range(7)) + bytes(pic[0x7e0 + 2 * i] for i in range(7))
    return k[::-1], name.split(b"\0")[0].decode("ascii", "replace")


def _swap8(b):
    """The image is DES on little-endian 64-bit words: each block reversed."""
    out = bytearray(len(b))
    for i in range(8):
        out[i::8] = b[7 - i::8]
    return bytes(out)


def decrypt(data, key):
    return _swap8(des.decrypt(_swap8(data), des.subkeys(key)))


def open_set(folder, log=print):
    """Checks the set; returns (Disc, key, image name)."""
    chd_path, zip_path = find_set(folder)
    log("set: %s and %s" % (chd_path, zip_path))
    try:
        c = chd.Chd(chd_path)
    except (chd.ChdError, OSError) as e:
        raise SetError("%s: %s" % (chd_path, e))
    if c.sha1 != CHD_SHA1:
        raise SetError("%s is not MAME's gds-0036f (SHA-1 %s, not %s): VF4 Final Tuned Ver. B is needed"
                       % (chd_path, c.sha1, CHD_SHA1))
    key, boot = pic_key(read_pic(zip_path))
    disc = gdrom.Disc(c)
    files = disc.files()
    if boot not in files:
        raise SetError("the PIC names %s, which the disc does not have" % boot)
    name = disc.read_file(boot)[0xc0:0xd0].split(b"\0")[0].decode("ascii", "replace")
    if name not in files or files[name][1] != IMAGE_SIZE:
        raise SetError("%s names %s, which is not the expected image" % (boot, name))
    return disc, key, name


def extract(folder, data_dir, log=print):
    """Writes data_dir/VF41_S.BIN and data_dir/dimm.key, and returns
    {program: decrypted bytes} for main and test, all checked by SHA-1."""
    disc, key, name = open_set(folder, log)
    os.makedirs(data_dir, exist_ok=True)
    img = os.path.join(data_dir, "VF41_S.BIN")
    have = os.path.isfile(img) and os.path.getsize(img) == IMAGE_SIZE and sha1_file(img) == IMAGE_SHA1
    if have:
        log("DIMM image already extracted and checked")
    else:
        log("extracting %s (240 MiB) from the GD-ROM" % name)
        h = hashlib.sha1()
        last = [-1]

        class Hashing:
            def __init__(self, f):
                self.f = f

            def write(self, b):
                h.update(b)
                self.f.write(b)

        def progress(done, size):
            pct = done * 100 // size
            if pct // 10 != last[0]:
                last[0] = pct // 10
                log("  %d%%" % pct)

        with open(img + ".part", "wb") as f:
            disc.read_file(name, Hashing(f), progress)
        if h.hexdigest() != IMAGE_SHA1:
            os.remove(img + ".part")
            raise SetError("the extracted image's SHA-1 is %s, not %s" % (h.hexdigest(), IMAGE_SHA1))
        os.replace(img + ".part", img)
    with open(os.path.join(data_dir, "dimm.key"), "w") as f:
        f.write(key.hex() + "\n")
    progs = {}
    with open(img, "rb") as f:
        for p, (off, size, sha1) in PROGRAMS.items():
            log("decrypting %s.bin" % p)
            f.seek(off)
            b = decrypt(f.read(size), key)
            if hashlib.sha1(b).hexdigest() != sha1:
                raise SetError("%s.bin decrypted to SHA-1 %s, not %s" % (p, hashlib.sha1(b).hexdigest(), sha1))
            progs[p] = b
    return progs


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                return h.hexdigest()
            h.update(b)
