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
  --launcher         the launcher's way (runtime/launcher): progress lines for it
                     ("@stage N OF TEXT", "@pct P") on standard output, and standard
                     input closing cancels the build (exit code 3)

The install folder holds only what your own set produced: never share it.
"""
import argparse
import datetime
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vf4build import VERSION, build, naomi, proc, toolchain  # noqa: E402
import make_tables  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NOTICE = """Virtua Fighter 4 Final Tuned (Ver. B), built on this computer from your own
MAME set by the VF4 Final Tuned recomp builder %s.

Everything in this folder was made from your copy of the game: do not share
it. To rebuild after a new release of the builder, run its Update (or
tools/make_game.py --update <this folder>); your saves and settings in user/
are kept.

Run: %s

The menu's fonts are Noto Sans and Noto Sans Mono, under the SIL Open Font
License 1.1: Font license.txt.
"""


STAGES = 6     # the stages the launcher shows: Log.stage


class Log:
    def __init__(self, path, launcher=False):
        self.f = open(path, "a", encoding="utf-8")
        self.t0 = time.monotonic()
        self.launcher = launcher
        self.last_pct = None

    def __call__(self, msg):
        line = "[%6.1f] %s" % (time.monotonic() - self.t0, msg)
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()

    def stage(self, n, text):
        """--launcher: stage n of STAGES begins (its bar moves until pct)"""
        if self.launcher:
            print("@stage %d %d %s" % (n, STAGES, text), flush=True)
            self.last_pct = None

    def pct(self, value):
        """--launcher: how far the stage is, 0..100"""
        v = int(value)
        if self.launcher and v != self.last_pct:
            print("@pct %d" % v, flush=True)
            self.last_pct = v


# what an install folder holds: the builder's files, and what the game and
# Wine or Proton add beside the game (the launcher's install_names, which
# lists the same). A first build refuses a folder with anything else in it,
# since the build writes and deletes these names there.
INSTALL_NAMES = {"build", "data", "user", "build.log", "build.json", "build.json.part", "READ ME.txt",
                 "Font license.txt", "vf4ft.exe", "vf4ft", "SDL3.dll", "vf4ft.log", "vkd3d-proton.cache",
                 "vkd3d-proton.cache.write", "vf4ft.dxvk-cache"}


def check_install_folder(out):
    if not os.path.isdir(out):
        return
    known = {n.lower() for n in INSTALL_NAMES} if os.name == "nt" else INSTALL_NAMES
    others = sorted(n for n in os.listdir(out) if (n.lower() if os.name == "nt" else n) not in known)
    if others:
        raise naomi.SetError("the install folder %s has other files in it (%s): choose an empty folder"
                             % (out, ", ".join(others[:5]) + (", ..." if len(others) > 5 else "")))


def release_folder():
    """the release folder this builder is in (its release.json beside
    builder/), as the game reads it from build.json; None in a repository"""
    rel = os.path.dirname(os.path.abspath(ROOT))
    if os.path.isfile(os.path.join(rel, "release.json")):
        return rel.replace("\\", "/")         # no escapes in build.json for the game's reader
    return None


def run(args):
    out = os.path.abspath(args.update or args.out)
    if not args.update:
        check_install_folder(out)
    os.makedirs(out, exist_ok=True)
    log = Log(os.path.join(out, "build.log"), args.launcher)
    if args.launcher:
        proc.watch_stdin(log)
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
    log.stage(1, "Checking your game files" if args.update else "Reading your game files")
    if args.update:
        if not os.path.isfile(os.path.join(data, "VF41_S.BIN")):
            raise naomi.SetError("%s has no data/VF41_S.BIN: build it with --set first" % out)
        log("update: the programs from the installed image")
        with open(os.path.join(data, "dimm.key")) as f:
            key = bytes.fromhex(f.read().strip())
        if naomi.sha1_file(os.path.join(data, "VF41_S.BIN")) != naomi.IMAGE_SHA1:
            raise naomi.SetError("data/VF41_S.BIN is damaged (wrong SHA-1): build again with --set")
        import hashlib
        progs = {}
        with open(os.path.join(data, "VF41_S.BIN"), "rb") as f:
            for p, (off, size, sha1) in naomi.PROGRAMS.items():     # the game and its test program
                f.seek(off)
                progs[p] = naomi.decrypt(f.read(size), key)
                if hashlib.sha1(progs[p]).hexdigest() != sha1:
                    raise naomi.SetError("data/dimm.key does not decrypt the image: build again with --set")
    else:
        progs = naomi.extract(args.set, data, log)

    # 2. the compiler (downloaded the first time)
    log.stage(2, "Getting the compiler")
    tc = toolchain.Toolchain(target, log)
    log("compiler: %s" % tc.version)

    # 3. translate the game and its test program (the operator's test mode)
    log.stage(3, "Translating the game's code")
    os.makedirs(work, exist_ok=True)
    gens = []
    with open(os.path.join(work, "translate.log"), "w") as tlog:
        for p, how_long in (("main", " (about a minute)"), ("test", "")):
            img = os.path.join(work, "%s.bin" % p)
            with open(img, "wb") as f:
                f.write(progs[p])
            gen = os.path.join(work, "gen", p)
            shutil.rmtree(gen, ignore_errors=True)
            log("translating %s.bin%s" % (p, how_long))
            r = proc.run([sys.executable, "-I", os.path.join(ROOT, "translator", "translate.py"),
                          "--program", p, "--image", img, "--out", gen])
            tlog.write("== %s.bin\n%s%s" % (p, r.stdout, r.stderr))
            if r.returncode != 0:
                raise build.BuildError("the translator failed on %s.bin:\n%s" % (p, (r.stderr or r.stdout)[-4000:]))
            gens.append(gen)
    tables = os.path.join(work, "tables")
    make_tables.write(tables)

    # 4. compile and link into a staging folder, then install over the old one
    jobs = args.jobs or os.cpu_count() or 4
    units = build.units(gens, [os.path.join(tables, "tables.c"), os.path.join(tables, "tables_test.c")], tc, VERSION)
    log.stage(4, "Compiling")
    log("compiling %d files, %d at a time" % (len(units), jobs))
    objs, warnings = build.compile_all(units, os.path.join(work, "obj"), tc, jobs, log)
    if warnings:
        with open(os.path.join(work, "warnings.log"), "w") as f:
            for src, w in warnings:
                f.write("== %s\n%s\n" % (src, w))
        log("%d files had compiler warnings (build/warnings.log)" % len(warnings))
    log.stage(5, "Linking")
    stage = os.path.join(work, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    build.link(objs, os.path.join(stage, tc.program), tc, log)
    for f in tc.extra_files:
        shutil.copy2(f, stage)
    log.stage(6, "Putting the game folder together")
    with proc.installing():     # a cancel waits for this, and after it the build is done
        for name in os.listdir(stage):
            os.replace(os.path.join(stage, name), os.path.join(out, name))
        with open(os.path.join(out, "READ ME.txt"), "w") as f:
            f.write(NOTICE % (VERSION, tc.program))
        shutil.copy2(os.path.join(ROOT, "runtime", "ui", "OFL.txt"), os.path.join(out, "Font license.txt"))
        info = {"builder": VERSION, "target": target, "program": tc.program,
                "built": datetime.datetime.now().isoformat(timespec="seconds"),
                "compiler": tc.version, "set": os.path.abspath(args.set) if args.set else old.get("set")}
        rel = release_folder()
        if rel:
            info["release"] = rel
        with open(info_path + ".part", "w") as f:
            json.dump(info, f, indent=2)
        os.replace(info_path + ".part", info_path)
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
    ap.add_argument("--launcher", action="store_true")
    args = ap.parse_args()
    if args.set and not args.out:
        ap.error("--set needs --out <install folder>")
    try:
        return run(args)
    except proc.Cancelled:
        return proc.CANCELLED       # the cancel's own thread ends the program
    except (naomi.SetError, toolchain.ToolchainError, build.BuildError, OSError) as e:
        print("\nerror: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
