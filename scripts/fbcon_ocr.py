#!/usr/bin/env python3
"""
fbcon_ocr.py — read the text off a QEMU screendump.

The framebuffer console paints an 8x8 bitmap font, so the screen can be read
back exactly rather than approximately: every cell is either a glyph in the
kernel's own font table or it is not text at all.  That is what makes the
screen a gateable surface instead of something a human has to squint at.

The font is parsed out of the kernel source, so this cannot drift from what
the kernel actually paints -- a second copy of the table here is the one way
this check could pass while the screen is wrong.

Usage: fbcon_ocr.py <shot.ppm> [--rows N]
"""
import re
import sys
import os

FONT_C = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "kernel", "drivers", "fbcon", "fbcon.c")


def load_font():
    src = open(FONT_C).read()
    body = src.split("font8x8[", 1)[1].split("};", 1)[0]
    rows = re.findall(r"\{([^}]*)\}", body)
    font = {}
    for i, r in enumerate(rows):
        b = tuple(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", r))
        if len(b) != 8:
            continue
        font.setdefault(b, chr(0x20 + i))
    return font


def read_ppm(path):
    f = open(path, "rb")
    if f.readline().strip() != b"P6":
        raise SystemExit("not a P6 ppm: " + path)
    line = f.readline()
    while line.startswith(b"#"):
        line = f.readline()
    w, h = map(int, line.split())
    f.readline()
    return w, h, f.read()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    maxrows = None
    if "--rows" in sys.argv:
        maxrows = int(sys.argv[sys.argv.index("--rows") + 1])

    font = load_font()
    w, h, d = read_ppm(path)
    cols, rows = w // 8, h // 8
    if maxrows:
        rows = min(rows, maxrows)

    out = []
    for cy in range(rows):
        line = ""
        for cx in range(cols):
            cell = []
            for gy in range(8):
                bits = 0
                for gx in range(8):
                    x, y = cx * 8 + gx, cy * 8 + gy
                    i = (y * w + x) * 3
                    # White on black: any channel well above black is "lit".
                    if i + 2 < len(d) and (d[i] > 100 or d[i+1] > 100 or d[i+2] > 100):
                        bits |= 1 << gx
                cell.append(bits)
            line += font.get(tuple(cell), " " if not any(cell) else "?")
        out.append(line.rstrip())
    while out and not out[-1]:
        out.pop()
    print("\n".join(out))


if __name__ == "__main__":
    main()
