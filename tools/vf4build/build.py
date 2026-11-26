"""Compiling and linking the game: the same compiler flags as the CMake
build (CMakeLists.txt), run directly so that the player needs no CMake."""
import concurrent.futures
import glob
import hashlib
import os

from . import proc

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
R = lambda *p: os.path.join(ROOT, "runtime", *p)          # noqa: E731

COMMON = ["-O3", "-DNDEBUG", "-std=gnu11", "-Wall", "-Wextra",
          "-msse2", "-ffp-contract=off", "-fno-strict-aliasing"]        # floating point as the interpreter does it
MACHINE_INC = ["-I" + R("machine"), "-I" + R("native"), "-I" + R("sound"), "-I" + R("cpu")]


class BuildError(Exception):
    pass


def source_id():
    """A fingerprint of the sources a game is made from: SHA-256 over the
    builder's .c, .h, .inc and .py files (runtime/, translator/, tools/; line
    ends as \\n), its first 12 digits. Two PCs playing each other compare
    it with the version (runtime/host/session.c): the same set gives the
    same game only from the same sources."""
    h = hashlib.sha256()
    files = []
    for top in ("runtime", "translator", "tools"):
        for d, dirs, names in os.walk(os.path.join(ROOT, top)):
            dirs[:] = sorted(x for x in dirs if x != "__pycache__")
            files += [os.path.join(d, n) for n in names if n.endswith((".c", ".h", ".inc", ".py"))]
    for f in sorted(files, key=lambda p: os.path.relpath(p, ROOT).replace(os.sep, "/")):
        rel = os.path.relpath(f, ROOT).replace(os.sep, "/")
        with open(f, "rb") as fh:
            data = fh.read().replace(b"\r\n", b"\n")
        h.update(rel.encode() + b"\0" + str(len(data)).encode() + b"\0" + data)
    return h.hexdigest()[:12]


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
    # runtime/vulkan first: Khronos's Vulkan headers for the ray tracing (rt_vk.c), whatever the system has
    hflags = COMMON + ["-I" + R("vulkan")] + tc.sdl_cflags + MACHINE_INC + ["-I" + R("host"), "-I" + R("ui"), "-I" + R("crypto"),
                                                     '-DVF4FT_VERSION="%s"' % version,
                                                     '-DVF4FT_SOURCE="sources %s"' % source_id()]
    for s in ("main.c", "game_machine.c", "dimm_image.c", "settings.c", "gpu_render.c", "menu.c", "updates.c",
              "art.c", "cards.c", "udp.c", "session.c", "snap_patch.c", "secure.c", "devices.c", "compositor.c",
              "vsync.c", "rt_scene.c", "rt_vk.c", "sysidle.c", "perf.c"):
        out.append((R("host", s), hflags))
    if tc.target == "windows":      # the ray tracing through DirectX 12 (toolchain.py links Windows' d3d12 and dxgi)
        out.append((R("host", "rt_dx12.c"), hflags))
    for s in ("monocypher.c", "monocypher-ed25519.c"):    # the two-PC game's cryptography (runtime/crypto, ORIGIN.md)
        out.append((R("crypto", s), COMMON + ["-I" + R("crypto")]))
    for s in ("ui.c", "fonts.c", "options.c", "kofi_badge.c", "card_editor.c", "controls.c"):    # the menu's drawing kit (runtime/ui)
        out.append((R("ui", s), hflags))
    return out


def compile_all(units_, obj_dir, tc, jobs, log):
    os.makedirs(obj_dir, exist_ok=True)
    warnings = []

    def one(i, src, flags):
        obj = os.path.join(obj_dir, "%03d_%s.o" % (i, os.path.splitext(os.path.basename(src))[0]))
        r = proc.run(tc.cc + flags + ["-c", src, "-o", obj])
        return src, obj, r

    done, by_index = 0, {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(one, i, s, f): i for i, (s, f) in enumerate(units_)}
        for fu in concurrent.futures.as_completed(futs):      # as they end, for the progress
            src, obj, r = fu.result()
            done += 1
            if r.returncode != 0:
                for f in futs:
                    f.cancel()
                raise BuildError("compiling %s failed:\n%s" % (os.path.relpath(src, ROOT), (r.stderr or r.stdout)[-4000:]))
            if r.stderr.strip():
                warnings.append((src, r.stderr))
            by_index[futs[fu]] = obj
            if hasattr(log, "pct"):
                log.pct(100 * done / len(futs))
            if done % 10 == 0 or done == len(futs):
                log("  compiled %d of %d" % (done, len(futs)))
    objs = [by_index[i] for i in range(len(units_))]      # the link's order: the units'
    return objs, warnings


def link(objs, out_path, tc, log):
    log("linking %s" % os.path.basename(out_path))
    r = proc.run(tc.cc + ["-O3"] + objs + ["-o", out_path] + tc.link)
    if r.returncode != 0:
        raise BuildError("linking failed:\n%s" % (r.stderr or r.stdout)[-4000:])
