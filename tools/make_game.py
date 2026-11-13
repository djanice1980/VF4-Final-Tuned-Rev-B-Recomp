#!/usr/bin/env python3
"""Builds the game from your own MAME set and installs it (docs/design.md,
section 8). Python 3.12 or later, nothing else: the compiler and SDL3 are
downloaded on first use (checked by SHA-256) into the builder's cache.

  python3 -I tools/make_game.py --set <folder> --out <install folder>
      <folder> holds gds-0036f.chd and vf4tuned.zip side by side, or is
      MAME's ROM folder (vf4tuned.zip, vf4tuned/gds-0036f.chd)

  python3 -I tools/make_game.py --update <install folder>
      builds again with this release, from the install's own data; the
      set is not needed. user/ (saves, settings) is kept.

Options:
  --target windows   the Windows exe (default; also the one for Steam/Proton on Linux)
  --target linux     a native Linux program (needs the system's clang or gcc and SDL3)
  --jobs N           compile N files at a time (default: one per processor)
  --keep             keep the build folder (translated code, objects) afterwards

The install folder holds only what your own set produced: never share it.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vf4build import VERSION, build, naomi, toolchain  # noqa: E402
import make_tables  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NOTICE = """Virtua Fighter 4 Final Tuned (Ver. B), built on this computer from your own
MAME set by the VF4 Final Tuned recomp builder %s.

Everything in this folder was made from your copy of the game: do not share
it. To rebuild after a new release of the builder, run its Update (or
tools/make_game.py --update <this folder>); your saves and settings in user/
are kept.

Run: %s
"""


class Log:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")
        self.t0 = time.monotonic()

    def __call__(self, msg):
        line = "[%6.1f] %s" % (time.monotonic() - self.t0, msg)
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()


def run(args):
    out = os.path.abspath(args.update or args.out)
    os.makedirs(out, exist_ok=True)
    log = Log(os.path.join(out, "build.log"))
    log("VF4 Final Tuned recomp builder %s; Python %s on %s" % (VERSION, sys.version.split()[0], sys.platform))
    info_path = os.path.join(out, "build.json")
    old = {}
    if os.path.isfile(info_path):
        with open(info_path) as f:
            old = json.load(f)
    target = args.target or old.get("target") or "windows"
    work = os.path.join(out, "build")
    data = os.path.join(out, "data")
    os.makedirs(os.path.join(out, "user"), exist_ok=True)

    # 1. the set: the disc's file and key into data/, the programs out of it
    if args.update:
        if not os.path.isfile(os.path.join(data, "VF41_S.BIN")):
            raise naomi.SetError("%s has no data/VF41_S.BIN: build it with --set first" % out)
        log("update: the programs from the installed image")
        with open(os.path.join(data, "dimm.key")) as f:
            key = bytes.fromhex(f.read().strip())
        if naomi.sha1_file(os.path.join(data, "VF41_S.BIN")) != naomi.IMAGE_SHA1:
            raise naomi.SetError("data/VF41_S.BIN is damaged (wrong SHA-1): build again with --set")
        progs = {}
        with open(os.path.join(data, "VF41_S.BIN"), "rb") as f:
            off, size, sha1 = naomi.PROGRAMS["main"]
            f.seek(off)
            progs["main"] = naomi.decrypt(f.read(size), key)
        import hashlib
        if hashlib.sha1(progs["main"]).hexdigest() != sha1:
            raise naomi.SetError("data/dimm.key does not decrypt the image: build again with --set")
    else:
        progs = naomi.extract(args.set, data, log)

    # 2. the compiler (downloaded the first time)
    tc = toolchain.Toolchain(target, log)
    log("compiler: %s" % tc.version)

    # 3. translate the main program
    os.makedirs(work, exist_ok=True)
    img = os.path.join(work, "main.bin")
    with open(img, "wb") as f:
        f.write(progs["main"])
    gen = os.path.join(work, "gen", "main")
    shutil.rmtree(gen, ignore_errors=True)
    log("translating main.bin (about a minute)")
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "translator", "translate.py"),
                        "--program", "main", "--image", img, "--out", gen], capture_output=True, text=True)
    with open(os.path.join(work, "translate.log"), "w") as f:
        f.write(r.stdout + r.stderr)
    if r.returncode != 0:
        raise build.BuildError("the translator failed:\n%s" % (r.stderr or r.stdout)[-4000:])
    tables = os.path.join(work, "tables")
    make_tables.write(tables)

    # 4. compile and link into a staging folder, then install over the old one
    jobs = args.jobs or os.cpu_count() or 4
    units = build.units(gen, os.path.join(tables, "tables.c"), tc, VERSION)
    log("compiling %d files, %d at a time" % (len(units), jobs))
    objs, warnings = build.compile_all(units, os.path.join(work, "obj"), tc, jobs, log)
    if warnings:
        with open(os.path.join(work, "warnings.log"), "w") as f:
            for src, w in warnings:
                f.write("== %s\n%s\n" % (src, w))
        log("%d files had compiler warnings (build/warnings.log)" % len(warnings))
    stage = os.path.join(work, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    build.link(objs, os.path.join(stage, tc.program), tc, log)
    for f in tc.extra_files:
        shutil.copy2(f, stage)
    for name in os.listdir(stage):
        os.replace(os.path.join(stage, name), os.path.join(out, name))
    with open(os.path.join(out, "READ ME.txt"), "w") as f:
        f.write(NOTICE % (VERSION, tc.program))
    info = {"builder": VERSION, "target": target, "program": tc.program,
            "built": datetime.datetime.now().isoformat(timespec="seconds"),
            "compiler": tc.version, "set": os.path.abspath(args.set) if args.set else old.get("set")}
    with open(info_path, "w") as f:
        json.dump(info, f, indent=2)
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    log("done: %s" % os.path.join(out, tc.program))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--set", help="the folder with your MAME set")
    g.add_argument("--update", metavar="FOLDER", help="rebuild an install with this release")
    ap.add_argument("--out", help="the install folder (with --set)")
    ap.add_argument("--target", choices=["windows", "linux"])
    ap.add_argument("--jobs", type=int)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if args.set and not args.out:
        ap.error("--set needs --out <install folder>")
    try:
        return run(args)
    except (naomi.SetError, toolchain.ToolchainError, build.BuildError, OSError) as e:
        print("\nerror: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
