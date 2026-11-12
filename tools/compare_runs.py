#!/usr/bin/env python3
"""The translated game against VF4debug on one controller script.

usage: python3 -I tools/compare_runs.py SCRIPT [--headless PATH] [--vf4debug DIR] [--keep DIR]

Builds VF4debug's own binary from its checkout (into build/vf4debug-ref of
this repository: the checkout is only read), runs it and the translated
build (build/linux/vf4ft_headless) on the same script at the same time, and
compares every "hash" line the script asks for (docs/design.md, section 9).
Each run's events are kept in KEEP/vf4debug.jsonl and KEEP/translated.jsonl.
Prints "same" with the count, or the first frame where they differ, and
exits 0 or 1. The reference is always VF4debug's binary, never a saved file.

The script's own lines are VF4debug's controller commands, for example
"run 100" and "hash" repeated.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def hashes(text):
    return [ln for ln in text.splitlines() if '"ev":"hash"' in ln]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--headless", default=os.path.join(ROOT, "build", "linux", "vf4ft_headless"))
    ap.add_argument("--vf4debug", default=os.path.join(os.path.dirname(ROOT), "VF4debug"))
    ap.add_argument("--keep", default=os.path.join(ROOT, "build", "compare"),
                    help="where the two runs write their reports")
    a = ap.parse_args()

    # make cannot take a path with a space in it: give VF4debug's Makefile
    # the build directory relative to its checkout
    ref_dir = os.path.join(ROOT, "build", "vf4debug-ref")
    rel = os.path.relpath(ref_dir, a.vf4debug)
    if " " in rel:
        sys.exit(f"compare_runs: {rel} has a space in it; give --vf4debug a checkout that shares more of the path")
    r = subprocess.run(["make", "-C", a.vf4debug, f"BUILD={rel}", "-j16", "-s"], capture_output=True, text=True)
    if r.returncode:
        sys.exit("compare_runs: VF4debug did not build:\n" + r.stdout + r.stderr)
    if not os.path.exists(a.headless):
        sys.exit(f"compare_runs: no {a.headless}: build it first (README.md, M2)")

    os.makedirs(a.keep, exist_ok=True)
    script = os.path.abspath(a.script)
    runs = {}
    for name, exe in (("vf4debug", os.path.join(ref_dir, "vf4debug")), ("translated", a.headless)):
        out = os.path.join(a.keep, name)
        runs[name] = subprocess.Popen([exe, "--script", script, "--out", out, "-q"], stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL, text=True, cwd=a.keep)
    res = {}
    for name, p in runs.items():
        text = p.communicate()[0]
        with open(os.path.join(a.keep, name + ".jsonl"), "w") as f:     # every event the run printed
            f.write(text)
        res[name] = hashes(text)
    ref, got = res["vf4debug"], res["translated"]
    if not ref:
        sys.exit("compare_runs: the script printed no hash (add \"hash\" lines)")
    for i, (x, y) in enumerate(zip(ref, got)):
        if x != y:
            print(f"differ at hash {i + 1} of {len(ref)}:\n  vf4debug   {x}\n  translated {y}")
            return 1
    if len(ref) != len(got):
        print(f"differ: {len(ref)} hashes from VF4debug, {len(got)} from the translated build")
        return 1
    print(f"same: {len(ref)} hashes, the last {ref[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
