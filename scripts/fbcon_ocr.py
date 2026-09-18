#!/usr/bin/env python3
"""
fbcon_ocr.py — read the text off a QEMU screendump.

The framebuffer console paints an 8x8 bitmap font, so the screen can be read
back exactly rather than approximately: every cell is either a glyph in the
kernel's own font table or it is not text at all.  That is what makes the
screen a gateable surface instead of something a human has to squint at.

The font is parsed out of <iris/font8x8.h>, the SAME header the kernel and
the ring-3 console both paint from, so this cannot drift from what is on the
screen -- a second copy of the table here is the one way this check could pass
while the screen is wrong.

Usage: fbcon_ocr.py <shot.ppm> [--rows N]
"""
import re
import sys
import os

FONT_C = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "kernel", "include", "iris", "font8x8.h")


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
    """Parse a QEMU screendump.

    Raises ValueError, not SystemExit.  A library function that exits the
    process cannot be used by a caller that wants to RETRY -- and that is
    exactly what the capture loop does, because the first screendump of a boot
    can land before the display has anything in it.  SystemExit does not derive
    from Exception, so it sailed through the loop's `except Exception` and
    killed the run; CI found that on the first try.
    """
    f = open(path, "rb")
    if f.readline().strip() != b"P6":
        raise ValueError("not a P6 ppm: " + path)
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
    try:
        w, h, d = read_ppm(path)
    except ValueError as e:
        raise SystemExit(str(e))
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
