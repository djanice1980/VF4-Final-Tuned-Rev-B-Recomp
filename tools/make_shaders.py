#!/usr/bin/env python3
"""Compiles the game's shaders (runtime/host/shaders) and writes them as C
arrays of bytes:

  *.vert, *.frag, *.comp   to SPIR-V, into runtime/host/gpu_shaders.h: the
                           GPU renderer's, and the Vulkan ray tracing's
                           compute shaders;
  *.hlsl                   to DXIL, into runtime/host/dx_shaders.h: the
                           DirectX 12 ray tracing's (compute shaders).

The headers are committed, so building the game needs no shader compiler;
run this after changing a shader.

It needs glslc for the first kind (Arch: sudo pacman -S shaderc; other
systems: the Vulkan SDK from https://vulkan.lunarg.com). For the second
kind it needs Microsoft's dxc: set DXC to its path, or have a dxc in PATH.
Use Microsoft's own build (github.com/microsoft/DirectXShaderCompiler,
releases: the linux_dxc archive; unpack it anywhere and set
DXC=.../bin/dxc): it signs what it makes, as Windows wants, and the build
a Linux distribution packages may not make DXIL at all (CachyOS's of
1.10.2605 failed on every shader). Without a dxc the DXIL header is left
as it is, and the tool says so.

usage: [DXC=/path/to/bin/dxc] python3 tools/make_shaders.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "runtime", "host", "shaders")
OUT = os.path.join(ROOT, "runtime", "host", "gpu_shaders.h")
OUT_DX = os.path.join(ROOT, "runtime", "host", "dx_shaders.h")


def array(ident, data):
    lines = ["static const unsigned char %s[%d] = {" % (ident, len(data))]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    return lines + ["};", ""]


def spirv():
    glslc = shutil.which("glslc")
    if not glslc:
        sys.exit("glslc not found (Arch: sudo pacman -S shaderc)")
    names = sorted(n for n in os.listdir(SRC) if n.endswith((".vert", ".frag", ".comp")))
    out = ["/* The GPU renderer's shaders as SPIR-V: written by tools/make_shaders.py from",
           " * runtime/host/shaders/. Do not edit; change the shader and run the tool. */",
           "#ifndef VF4FT_GPU_SHADERS_H", "#define VF4FT_GPU_SHADERS_H", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            spv = os.path.join(tmp, n + ".spv")
            # a compute shader is the ray tracing own (ray queries: Vulkan 1.2); the others are SDL_GPU's
            env = "vulkan1.2" if n.endswith(".comp") else "vulkan1.0"
            subprocess.run([glslc, "-O", "--target-env=" + env, "-o", spv, os.path.join(SRC, n)], check=True)
            out += array("spv_" + n.replace(".", "_"), open(spv, "rb").read())
    out.append("#endif")
    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print("%d shaders -> %s" % (len(names), os.path.relpath(OUT, ROOT)))


def dxil():
    names = sorted(n for n in os.listdir(SRC) if n.endswith(".hlsl"))
    if not names:
        return
    dxc = os.environ.get("DXC") or shutil.which("dxc")
    if not dxc or not os.path.exists(dxc):
        print("no dxc (set DXC=/path/to/bin/dxc): %s is left as it is" % os.path.relpath(OUT_DX, ROOT))
        return
    env = dict(os.environ)
    lib = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(dxc))), "lib")
    if os.path.isdir(lib):          # Microsoft's archive: its libraries are beside bin/
        env["LD_LIBRARY_PATH"] = lib + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    out = ["/* The DirectX 12 ray tracing's shaders as DXIL: written by tools/make_shaders.py from",
           " * the .hlsl files of runtime/host/shaders. Do not edit; change the shader and run the tool. */",
           "#ifndef VF4FT_DX_SHADERS_H", "#define VF4FT_DX_SHADERS_H", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            blob = os.path.join(tmp, n + ".dxil")
            # compute shaders with inline ray queries: shader model 6.5
            r = subprocess.run([dxc, "-T", "cs_6_5", "-E", "main", "-O3", "-Fo", blob, os.path.join(SRC, n)], env=env,
                               capture_output=True, text=True)
            if r.returncode != 0 or not os.path.exists(blob):
                sys.exit("dxc failed on %s:\n%s" % (n, (r.stderr or r.stdout)[-3000:]))
            data = open(blob, "rb").read()
            if data[4:20] == bytes(16):
                sys.exit("%s: this dxc left the shader unsigned (Windows would refuse it): use Microsoft's own build" % n)
            out += array("dxil_" + n.replace(".", "_"), data)
    out.append("#endif")
    with open(OUT_DX, "w") as f:
        f.write("\n".join(out) + "\n")
    print("%d shaders -> %s" % (len(names), os.path.relpath(OUT_DX, ROOT)))


def main():
    spirv()
    dxil()


if __name__ == "__main__":
    main()
