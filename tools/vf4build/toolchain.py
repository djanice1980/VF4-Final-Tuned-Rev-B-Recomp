"""The pinned compiler and SDL3, downloaded once into the builder's cache
and checked by SHA-256 (docs/design.md, section 3).

  llvm-mingw   clang, lld and the mingw-w64 headers and libraries: makes
               the Windows exe, on Windows or on Linux
  SDL3         the official prebuilt package for mingw (SDL3.dll and its
               import library)

Both are freely redistributable. For the native Linux build the system's
own C compiler and SDL3 are used instead (nothing is downloaded).
"""
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

LLVM_MINGW_VER = "20261104"
SDL3_VER = "3.4.20"
_GH = "https://github.com"
PINS = {
    "llvm-mingw-linux": (
        "%s/mstorsjo/llvm-mingw/releases/download/%s/llvm-mingw-%s-ucrt-ubuntu-22.04-x86_64.tar.xz"
        % (_GH, LLVM_MINGW_VER, LLVM_MINGW_VER),
        "919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926", 84104244),
    "llvm-mingw-windows": (
        "%s/mstorsjo/llvm-mingw/releases/download/%s/llvm-mingw-%s-ucrt-x86_64.zip"
        % (_GH, LLVM_MINGW_VER, LLVM_MINGW_VER),
        "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f", 191037046),
    "sdl3-mingw": (
        "%s/libsdl-org/SDL/releases/download/release-%s/SDL3-devel-%s-mingw.zip" % (_GH, SDL3_VER, SDL3_VER),
        "5d321d3d2d499cfabad3023921e4b56bbec03558060f4e195ce626d200126442", 5475969),
}


class ToolchainError(Exception):
    pass


def cache_dir():
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    else:
        base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "vf4ft-builder")


def _download(url, dest, sha256, size, log):
    h = hashlib.sha256()
    log("downloading %s (%d MB)" % (url.rsplit("/", 1)[1], size // 1000000))
    req = urllib.request.Request(url, headers={"User-Agent": "vf4ft-builder"})
    done, step = 0, 0
    with urllib.request.urlopen(req, timeout=60) as r, open(dest + ".part", "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            h.update(b)
            done += len(b)
            if size and hasattr(log, "pct"):
                log.pct(min(100, 100 * done / size))
            if size and done * 10 // size > step:
                step = done * 10 // size
                log("  %d%%" % min(100, step * 10))
    if h.hexdigest() != sha256:
        os.remove(dest + ".part")
        raise ToolchainError("%s: SHA-256 %s, not the pinned %s (download damaged or changed)"
                             % (url, h.hexdigest(), sha256))
    os.replace(dest + ".part", dest)


def fetch(name, log=print, cache=None):
    """The folder `name` is unpacked in (downloaded and checked the first time)."""
    url, sha256, size = PINS[name]
    cache = cache or cache_dir()
    target = os.path.join(cache, name)
    stamp = os.path.join(target, ".vf4ft-sha256")
    if os.path.isfile(stamp) and open(stamp).read().strip() == sha256:
        return target
    os.makedirs(cache, exist_ok=True)
    archive = os.path.join(cache, url.rsplit("/", 1)[1])
    if not (os.path.isfile(archive) and _sha256(archive) == sha256):
        try:
            _download(url, archive, sha256, size, log)
        except OSError as e:
            raise ToolchainError("cannot download %s: %s" % (url, e))
    log("unpacking %s" % os.path.basename(archive))
    tmp = target + ".part"
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.rmtree(target, ignore_errors=True)
    os.makedirs(tmp)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
    else:
        with tarfile.open(archive) as t:
            t.extractall(tmp, filter="data")
    # one top folder in each archive: lift its contents up
    tops = os.listdir(tmp)
    inner = os.path.join(tmp, tops[0]) if len(tops) == 1 and os.path.isdir(os.path.join(tmp, tops[0])) else tmp
    os.replace(inner, target) if inner != tmp else os.replace(tmp, target)
    shutil.rmtree(tmp, ignore_errors=True)
    with open(stamp, "w") as f:
        f.write(sha256 + "\n")
    os.remove(archive)
    return target


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


class Toolchain:
    """What a build needs: the compiler command, its flags for SDL3, the
    link flags, and the files to install beside the program."""

    def __init__(self, target, log=print, cache=None):
        self.target = target
        if target == "windows":
            host = "windows" if os.name == "nt" else "linux"
            if host == "linux" and platform.machine().lower() not in ("x86_64", "amd64"):
                raise ToolchainError("the Windows build needs an x86-64 Linux or Windows machine")
            llvm = fetch("llvm-mingw-" + host, log, cache)
            sdl = os.path.join(fetch("sdl3-mingw", log, cache), "x86_64-w64-mingw32")
            exe = ".exe" if os.name == "nt" else ""
            self.cc = [os.path.join(llvm, "bin", "x86_64-w64-mingw32-clang" + exe)]
            self.sdl_cflags = ["-I" + os.path.join(sdl, "include")]
            self.link = ["-mwindows", "-static", os.path.join(sdl, "lib", "libSDL3.dll.a"), "-lm", "-lpthread"]
            self.program = "vf4ft.exe"
            self.extra_files = [os.path.join(sdl, "bin", "SDL3.dll")]
        elif target == "linux":
            cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
            if not cc:
                raise ToolchainError("the native Linux build needs a C compiler: install clang "
                                     "(Arch: sudo pacman -S clang sdl3; Debian/Ubuntu: sudo apt install clang libsdl3-dev)")
            self.cc = [cc]
            pc = shutil.which("pkg-config") or shutil.which("pkgconf")
            flags = None
            if pc:
                r = subprocess.run([pc, "--cflags", "--libs", "sdl3"], capture_output=True, text=True)
                if r.returncode == 0:
                    flags = r.stdout.split()
            if flags is None:
                raise ToolchainError("the native Linux build needs SDL3 3.4 or later and its headers "
                                     "(Arch: sudo pacman -S sdl3 pkgconf; Debian/Ubuntu: sudo apt install libsdl3-dev pkg-config)")
            self.sdl_cflags = [f for f in flags if f.startswith(("-I", "-D"))]
            self.link = [f for f in flags if not f.startswith(("-I", "-D"))] + ["-lm", "-lpthread"]
            self.program = "vf4ft"
            self.extra_files = []
        else:
            raise ToolchainError("unknown target %s" % target)
        try:
            v = subprocess.run(self.cc + ["--version"], capture_output=True, text=True, timeout=60)
        except OSError as e:
            raise ToolchainError("cannot run the compiler %s: %s" % (self.cc[0], e))
        self.version = (v.stdout.splitlines() or ["?"])[0]


if __name__ == "__main__":
    t = Toolchain(sys.argv[1] if len(sys.argv) > 1 else "windows")
    print(t.cc, t.version)
