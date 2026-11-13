#!/usr/bin/env python3
"""Puts a release together (docs/design.md, section 8): the launcher, what
it runs, and this repository's builder files. Nothing of the game: the
builder files are taken from git (tracked files only), so work/, build/
and anything made from a set cannot get in.

usage: python3 -I tools/package_release.py windows LAUNCHER.exe OUTDIR [--version V]
       python3 -I tools/package_release.py linux LAUNCHER OUTDIR [--version V]

  windows: vf4ft-launcher.exe, SDL3.dll (SDL's official build), python/
           (the embeddable Python from python.org), builder/
  linux:   vf4ft-launcher, builder/ (Python and SDL3 are the system's)

Both also get release.json: the version (the builder's VERSION), the
platform and the GitHub repository, which the launcher's updater reads
(tools/update.py, docs/design.md section 24). A folder without it is a
development build and never updates itself. --version V packages it as V
(release.json and the packaged builder's VERSION), for testing updates.
A release's zip is named vf4ft-VERSION-PLATFORM.zip: the updater looks for
that name among a release's files.

OUTDIR is made fresh, then zipped beside itself as OUTDIR.zip. When LAUNCHER
is in a CMake build folder it is rebuilt there first, so a launcher older
than its sources cannot get in (M5's first release had one built before
--do and --exit existed).
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vf4build import toolchain  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# python.org, over HTTPS, 2026-11-12 (the release page gives a sigstore bundle, no hash)
PYTHON_EMBED = ("https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip",
                "90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159")
BUILDER_PATHS = ["LICENSE", "README.md", "docs", "meta", "runtime", "tools", "translator"]
REPO = "djanice1980/VF4-Final-Tuned-Rev-B-Recomp"      # where the updater looks for releases


def tracked(paths):
    r = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--"] + paths, capture_output=True, check=True)
    return [p for p in r.stdout.decode().split("\0") if p]


def fresh_launcher(launcher):
    """rebuilds the launcher in its CMake build folder, if it is in one"""
    d = os.path.dirname(os.path.abspath(launcher))
    if not os.path.isfile(os.path.join(d, "CMakeCache.txt")):
        print("%s: not in a CMake build folder; packaged as it is" % launcher)
        return
    print("rebuilding the launcher in", d, flush=True)
    r = subprocess.run(["cmake", "--build", d, "--target", "vf4ft-launcher"])
    if r.returncode != 0:
        sys.exit("the launcher did not build")


def uncommitted(paths):
    r = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--"] + paths, capture_output=True, check=True)
    return [line[3:] for line in r.stdout.decode().splitlines()]


def python_embed(dest):
    url, sha = PYTHON_EMBED
    cache = os.path.join(toolchain.cache_dir(), os.path.basename(url))
    if not os.path.isfile(cache) or hashlib.sha256(open(cache, "rb").read()).hexdigest() != sha:
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        print("downloading", url)
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "vf4ft-builder"})) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != sha:
            sys.exit("%s: SHA-256 is not the pinned one" % url)
        with open(cache, "wb") as f:
            f.write(data)
    with zipfile.ZipFile(cache) as z:
        z.extractall(dest)


def main():
    a = sys.argv[1:]
    version = None
    if len(a) == 5 and a[3] == "--version":
        version = a.pop(4)
        a.pop(3)
    if len(a) != 3 or a[0] not in ("windows", "linux") or (version is not None and not re.fullmatch(
            r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", version)):
        sys.exit(__doc__)
    kind, launcher, out = a[0], a[1], os.path.abspath(a[2])
    fresh_launcher(launcher)
    for p in uncommitted(BUILDER_PATHS):
        print("warning: not committed, packaged as it is now:", p)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    files = tracked(BUILDER_PATHS)
    for p in files:
        d = os.path.join(out, "builder", p)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, p), d)
    init = os.path.join(out, "builder", "tools", "vf4build", "__init__.py")
    src = open(init, encoding="utf-8").read()
    m = re.search(r'^VERSION = "([^"]+)"$', src, re.M)
    if not m:
        sys.exit("%s has no VERSION line" % init)
    if version and version != m[1]:
        print("packaged as %s (the builder says %s)" % (version, m[1]))
        with open(init, "w", encoding="utf-8") as f:
            f.write(src[:m.start(1)] + version + src[m.end(1):])
    with open(os.path.join(out, "release.json"), "w", encoding="utf-8") as f:
        json.dump({"version": version or m[1], "platform": kind, "repo": REPO}, f, indent=2)
        f.write("\n")
    shutil.copy2(launcher, os.path.join(out, os.path.basename(launcher)))
    if kind == "windows":
        sdl = toolchain.fetch("sdl3-mingw")
        shutil.copy2(os.path.join(sdl, "x86_64-w64-mingw32", "bin", "SDL3.dll"), out)
        python_embed(os.path.join(out, "python"))
    with open(os.path.join(out, "READ ME.txt"), "w") as f:
        f.write("VF4 Final Tuned recomp: the builder and launcher.\n\n"
                "Run vf4ft-launcher, choose the folder with your MAME vf4tuned set\n"
                "(gds-0036f.chd and vf4tuned.zip), press Build, then Play.\n"
                "The game is built into game/ beside the launcher, from your own set;\n"
                "this download contains none of the game. Details: builder/README.md\n\n"
                "The launcher's fonts are Noto Sans and Noto Sans Mono, under the SIL\n"
                "Open Font License 1.1: builder/runtime/ui/OFL.txt.\n")
    zpath = out.rstrip(os.sep) + ".zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for dirpath, _, names in os.walk(out):
            for n in names:
                full = os.path.join(dirpath, n)
                z.write(full, os.path.relpath(full, os.path.dirname(out)))
    print("%d builder files; %s (%d MB)" % (len(files), zpath, os.path.getsize(zpath) // 1000000))


if __name__ == "__main__":
    main()
