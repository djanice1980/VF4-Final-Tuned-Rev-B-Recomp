#!/usr/bin/env python3
"""Compiles the GPU renderer's shaders (runtime/host/shaders/*.vert, *.frag)
to SPIR-V and writes runtime/host/gpu_shaders.h, the bytes as C arrays.

The header is committed, so building the game needs no shader compiler;
run this after changing a shader. It needs glslc (Arch: sudo pacman -S
shaderc; other systems: the Vulkan SDK from https://vulkan.lunarg.com).

usage: python3 tools/make_shaders.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "runtime", "host", "shaders")
OUT = os.path.join(ROOT, "runtime", "host", "gpu_shaders.h")


def main():
    glslc = shutil.which("glslc")
    if not glslc:
        sys.exit("glslc not found (Arch: sudo pacman -S shaderc)")
    names = sorted(n for n in os.listdir(SRC) if n.endswith((".vert", ".frag")))
    out = ["/* The GPU renderer's shaders as SPIR-V: written by tools/make_shaders.py from",
           " * runtime/host/shaders/. Do not edit; change the shader and run the tool. */",
           "#ifndef VF4FT_GPU_SHADERS_H", "#define VF4FT_GPU_SHADERS_H", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            spv = os.path.join(tmp, n + ".spv")
            subprocess.run([glslc, "-O", "--target-env=vulkan1.0", "-o", spv, os.path.join(SRC, n)], check=True)
            data = open(spv, "rb").read()
            ident = "spv_" + n.replace(".", "_")
            out.append("static const unsigned char %s[%d] = {" % (ident, len(data)))
            for i in range(0, len(data), 16):
                out.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
            out.append("};")
            out.append("")
    out.append("#endif")
    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print("%d shaders -> %s" % (len(names), os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
