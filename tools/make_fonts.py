#!/usr/bin/env python3
"""Writes runtime/ui/fonts.c: the fonts the launcher and the game's menu
draw with (runtime/ui/ui.c), cut down to the characters they use and kept
in the program as C arrays, so that any C compiler builds them (no #embed:
the native Linux target uses the player's own compiler).

The fonts are Noto Sans and Noto Sans Mono (SIL Open Font License 1.1,
runtime/ui/OFL.txt; no Reserved Font Name, so a cut-down copy may keep the
name). Their copyright and license name records are kept in each font.
Made from Arch Linux's noto-fonts 2026.11.01 (Noto Sans 2.015, Noto Sans
Mono 2.014); run again only to change the fonts or the characters:

  python3 tools/make_fonts.py [NOTO_FONT_DIR]       (default /usr/share/fonts/noto)

Needs fontTools (Arch: sudo pacman -S python-fonttools).
"""
import io
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "runtime", "ui", "fonts.c")

# Basic Latin, Latin-1, dashes, quotes, bullet, ellipsis, per mille, angle
# quotes, euro, trade mark
TEXT = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + list(range(0x2010, 0x2028)) + \
    list(range(0x2030, 0x203B)) + [0x20AC, 0x2122]
FONTS = [  # (C name, file)
    ("ui_font_regular", "NotoSans-Regular.ttf"),
    ("ui_font_medium", "NotoSans-Medium.ttf"),
    ("ui_font_bold", "NotoSans-Bold.ttf"),
    ("ui_font_mono", "NotoSansMono-Regular.ttf"),
]


def cut(path):
    font = TTFont(path)
    o = subset.Options()
    o.hinting = False                       # stb_truetype does not hint
    o.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]  # names, copyright, the license and its URL
    o.name_languages = [0x409]
    o.layout_features = ["kern"]
    o.notdef_outline = True
    s = subset.Subsetter(o)
    s.populate(unicodes=TEXT)
    s.subset(font)
    b = io.BytesIO()
    font.save(b)
    return font, b.getvalue()


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/noto"
    out = ["/* Made by tools/make_fonts.py: do not edit. Noto Sans and Noto Sans Mono,",
           " * cut down to the characters in that script, as TrueType data.",
           " * Licensed under the SIL Open Font License 1.1: runtime/ui/OFL.txt. */",
           "#include <stddef.h>", ""]
    for name, file in FONTS:
        font, data = cut(os.path.join(src, file))
        n = font["name"]
        out.append("/* %s, %s" % (file, n.getDebugName(5)))
        out.append(" * %s" % n.getDebugName(0))
        out.append(" * %s */" % n.getDebugName(13))
        out.append("const unsigned char %s[%d] = {" % (name, len(data)))
        for i in range(0, len(data), 24):
            out.append("    " + ",".join(str(x) for x in data[i:i + 24]) + ",")
        out.append("};")
        out.append("const size_t %s_size = sizeof %s;" % (name, name))
        out.append("")
        print("%s: %d bytes" % (file, len(data)))
    with open(OUT, "w") as f:
        f.write("\n".join(out))
    print("wrote", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
