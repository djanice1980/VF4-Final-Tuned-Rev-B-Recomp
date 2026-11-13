#!/usr/bin/env python3
"""Refresh meta/ from a vf4ft-decomp checkout.

usage: python3 -I tools/sync_meta.py <vf4ft-decomp checkout>

meta/ is what the translator knows about the game's two programs, taken
from the matching decompilation: where each routine starts, its name and
source file, the FPU mode of the instructions that do not run in the
default mode, and the tables of routine pointers. Addresses and names only:
no code and no data of the game.

Writes:
  meta/main/routines.tsv     address, name, file, size (ghidra/source_map.tsv)
  meta/main/globals.tsv      address, name             (src/include/vf4ft_globals.h: the named globals,
                                                        which the runtime's BIOS stand-in and devices look up)
  meta/main/fpu_context.tsv  address, field, detail    (ghidra/fpu_context.tsv)
  meta/main/code_tables.tsv  address, count, stride, name (ghidra/code_tables.tsv)
  meta/test/routines.tsv     (ghidra/source_map_test.tsv)
  meta/test/globals.tsv      (src_test/include/vf4ft_globals.h: the same for the test program, which the
                              runtime runs in test mode)
  meta/SOURCE                the decompilation commit these came from
"""
import csv
import os
import re
import subprocess
import sys


def read_tsv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def write_tsv(path, header, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="") as f:
        f.write("\t".join(header) + "\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")


def routines(src):
    rows = read_tsv(src)
    out, seen_a, seen_n = [], set(), set()
    for r in rows:
        a = int(r["address"], 16)
        if a in seen_a or r["name"] in seen_n:
            sys.exit(f"{src}: duplicate {r['address']} {r['name']}")
        seen_a.add(a)
        seen_n.add(r["name"])
        out.append((a, r["name"], r["file"], int(r["size"])))
    out.sort()
    return [(f"{a:08x}", n, f, z) for a, n, f, z in out]


def globals_(header):
    """(address, name) of each `extern <declaration>; /* 0x... */` line, by
    address; the name is the declaration's last word without array sizes,
    pointer stars or the parentheses of a function pointer"""
    out = []
    with open(header) as f:
        for line in f:
            m = re.match(r"^extern\s+(.*?)\s*;\s*/\*\s*0x([0-9a-fA-F]{8})\s*\*/", line)
            if not m:
                continue
            name = re.sub(r"(\[[^\]]*\])+$", "", m.group(1)).split()[-1].lstrip("*")
            name = re.sub(r"[()]", "", name)
            if re.match(r"^[A-Za-z_]\w*$", name):
                out.append((int(m.group(2), 16), name))
    out.sort()
    return [(f"{a:08x}", n) for a, n in out]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    decomp = sys.argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    meta = os.path.join(root, "meta")
    g = os.path.join(decomp, "ghidra")

    main_r = routines(os.path.join(g, "source_map.tsv"))
    test_r = routines(os.path.join(g, "source_map_test.tsv"))
    write_tsv(os.path.join(meta, "main", "routines.tsv"), ["address", "name", "file", "size"], main_r)
    write_tsv(os.path.join(meta, "test", "routines.tsv"), ["address", "name", "file", "size"], test_r)
    globs = globals_(os.path.join(decomp, "src", "include", "vf4ft_globals.h"))
    write_tsv(os.path.join(meta, "main", "globals.tsv"), ["address", "name"], globs)
    test_globs = globals_(os.path.join(decomp, "src_test", "include", "vf4ft_globals.h"))
    write_tsv(os.path.join(meta, "test", "globals.tsv"), ["address", "name"], test_globs)

    fpu = [(r["address"], r["field"], r["detail"]) for r in read_tsv(os.path.join(g, "fpu_context.tsv"))]
    bad = [r for r in read_tsv(os.path.join(g, "fpu_context.tsv")) if r["kind"] != "redecoded"]
    if bad:
        sys.exit(f"fpu_context.tsv has problem rows the translator does not handle: {bad[:3]}")
    write_tsv(os.path.join(meta, "main", "fpu_context.tsv"), ["address", "field", "detail"], fpu)

    tables = [(r["address"], r["count"], r["stride"], r["name"])
              for r in read_tsv(os.path.join(g, "code_tables.tsv"))]
    write_tsv(os.path.join(meta, "main", "code_tables.tsv"), ["address", "count", "stride", "name"], tables)

    commit = subprocess.run(["git", "-C", decomp, "log", "-1", "--format=%H %cs"],
                            capture_output=True, text=True, check=True).stdout.strip()
    with open(os.path.join(meta, "SOURCE"), "w") as f:
        f.write("vf4ft-decomp " + commit + "\n")
    print(f"main {len(main_r)} routines, {len(globs)} globals, test {len(test_r)} routines, "
          f"{len(test_globs)} globals, "
          f"{len(fpu)} FPU mode rows, {len(tables)} code tables; decomp {commit}")


if __name__ == "__main__":
    main()
