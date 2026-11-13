"""Compiling and linking the game: the same compiler flags as the CMake
build (CMakeLists.txt), run directly so that the player needs no CMake."""
import concurrent.futures
import glob
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
R = lambda *p: os.path.join(ROOT, "runtime", *p)          # noqa: E731

COMMON = ["-O3", "-DNDEBUG", "-std=gnu11", "-Wall", "-Wextra",
          "-msse2", "-ffp-contract=off", "-fno-strict-aliasing"]        # floating point as the interpreter does it
MACHINE_INC = ["-I" + R("machine"), "-I" + R("native"), "-I" + R("sound"), "-I" + R("cpu")]


class BuildError(Exception):
    pass


def units(gen_dirs, tables_cs, tc, version):
    """[(source, flags)] of every C file of the game: gen_dirs are the
    translated programs (main.bin, test.bin), tables_cs their tables."""
    out = []
    for s in ("rt_core.c", "sh4rt_fpu.c"):
        out.append((R("cpu", s), COMMON + ["-I" + R("cpu")]))
    for gen_dir in gen_dirs:
        gen = sorted(glob.glob(os.path.join(gen_dir, "*.c")))
        if not gen:
            raise BuildError("no translated code in %s" % gen_dir)
        for s in gen:
            out.append((s, COMMON + ["-I" + gen_dir, "-I" + R("cpu")]))
    mflags = COMMON + MACHINE_INC + ["-Wno-unused-parameter"]
    for s in [R("native", "rt_native.c")] + sorted(glob.glob(R("machine", "*.c"))) + \
            [R("sound", "arm7.c"), R("sound", "aica.c")]:
        out.append((s, mflags))
    for tables_c in tables_cs:
        out.append((tables_c, mflags + ["-O0", "-g0"]))
    hflags = COMMON + tc.sdl_cflags + MACHINE_INC + ['-DVF4FT_VERSION="%s"' % version]
    for s in ("main.c", "game_machine.c", "dimm_image.c", "settings.c", "gpu_render.c"):
        out.append((R("host", s), hflags))
    return out


def compile_all(units_, obj_dir, tc, jobs, log):
    os.makedirs(obj_dir, exist_ok=True)
    objs, warnings = [], []

    def one(i, src, flags):
        obj = os.path.join(obj_dir, "%03d_%s.o" % (i, os.path.splitext(os.path.basename(src))[0]))
        r = subprocess.run(tc.cc + flags + ["-c", src, "-o", obj], capture_output=True, text=True)
        return src, obj, r

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(one, i, s, f) for i, (s, f) in enumerate(units_)]
        for fu in futs:
            src, obj, r = fu.result()
            done += 1
            if r.returncode != 0:
                for f in futs:
                    f.cancel()
                raise BuildError("compiling %s failed:\n%s" % (os.path.relpath(src, ROOT), (r.stderr or r.stdout)[-4000:]))
            if r.stderr.strip():
                warnings.append((src, r.stderr))
            objs.append(obj)
            if done % 10 == 0 or done == len(futs):
                log("  compiled %d of %d" % (done, len(futs)))
    return objs, warnings


def link(objs, out_path, tc, log):
    log("linking %s" % os.path.basename(out_path))
    r = subprocess.run(tc.cc + ["-O3"] + objs + ["-o", out_path] + tc.link, capture_output=True, text=True)
    if r.returncode != 0:
        raise BuildError("linking failed:\n%s" % (r.stderr or r.stdout)[-4000:])
