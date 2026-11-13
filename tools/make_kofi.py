#!/usr/bin/env python3
"""Writes runtime/ui/kofi_badge.c: runtime/ui/kofi_badge.png as a C array,
for the game's menu and its first-start notice (docs/design.md section 26).

The badge is Ko-fi's own "Support me on Ko-fi" button, which Ko-fi gives
creators to link to their page; it links to the author's Ko-fi page here, as
in the author's pcbview (assets/kofi_badge.png there). The program draws it
with SDL_LoadPNG_IO (SDL 3.4 and later), so it stays a PNG: a C array, not
#embed, so that any C compiler builds it (the native Linux target uses the
player's own compiler). Run again only when the PNG changes:

  python3 tools/make_kofi.py
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "runtime", "ui", "kofi_badge.png")
OUT = os.path.join(ROOT, "runtime", "ui", "kofi_badge.c")


def main():
    data = open(SRC, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s is not a PNG" % SRC)
    lines = ["/* Made by tools/make_kofi.py: do not edit. runtime/ui/kofi_badge.png, Ko-fi's",
             " * \"Support me on Ko-fi\" button, as PNG data (drawn with SDL_LoadPNG_IO). */",
             "#include <stddef.h>", "",
             "const unsigned char ui_kofi_badge_png[%d] = {" % len(data)]
    for i in range(0, len(data), 20):
        lines.append("    " + ", ".join(str(b) for b in data[i:i + 20]) + ",")
    lines += ["};", "const size_t ui_kofi_badge_png_size = sizeof ui_kofi_badge_png;", ""]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("%s: %d bytes of PNG" % (OUT, len(data)))


if __name__ == "__main__":
    main()
