#!/usr/bin/env python3
"""Draw the Lunaria icon: the boot card's winter sky, a crescent moon and snow.

usage: make-icon.py OUT.icns [PNG]

No imaging library: the picture is evaluated per pixel from distance functions
(so edges are anti-aliased exactly) and written as a PNG with zlib.  `iconutil`
and `sips` (both part of macOS) turn that into the .icns sizes.
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SIZE = 1024


def smooth(edge):
    """Coverage for a signed distance (negative = inside) with ~1px of blur."""
    return max(0.0, min(1.0, 0.5 - edge))


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


SKY_TOP = (0, 163, 239)          # the boot card's #00a3ef
SKY_BOTTOM = (183, 231, 252)     # #b7e7fc
MOON = (255, 250, 232)
SNOW = (255, 255, 255)

# (x, y, radius) in 0..1 units
FLAKES = [(0.22, 0.24, 0.016), (0.80, 0.20, 0.022), (0.86, 0.52, 0.014),
          (0.15, 0.62, 0.012), (0.70, 0.80, 0.018), (0.34, 0.84, 0.013),
          (0.52, 0.14, 0.011), (0.90, 0.78, 0.010)]


def pixel(px, py):
    x = (px + 0.5) / SIZE
    y = (py + 0.5) / SIZE
    # rounded square ("squircle"-ish): the macOS icon grid is 824/1024 inset
    inset, radius = 0.0977, 0.18
    cx = abs(x - 0.5) - (0.5 - inset - radius)
    cy = abs(y - 0.5) - (0.5 - inset - radius)
    outside = math.hypot(max(cx, 0.0), max(cy, 0.0)) + min(max(cx, cy), 0.0) - radius
    alpha = smooth(outside * SIZE)
    if alpha <= 0.0:
        return (0, 0, 0, 0)
    colour = mix(SKY_TOP, SKY_BOTTOM, min(1.0, max(0.0, (y - 0.1) / 0.8)))
    # soft glow behind the moon
    glow = max(0.0, 1.0 - math.hypot(x - 0.46, y - 0.46) / 0.46)
    colour = mix(colour, (214, 241, 255), glow * glow * 0.55)
    # crescent: a disc with a bite taken out of its upper right
    moon = math.hypot(x - 0.45, y - 0.50) - 0.235
    bite = math.hypot(x - 0.56, y - 0.43) - 0.20
    crescent = max(moon, -bite)
    colour = mix(colour, MOON, smooth(crescent * SIZE))
    for fx, fy, fr in FLAKES:
        colour = mix(colour, SNOW, smooth((math.hypot(x - fx, y - fy) - fr) * SIZE) * 0.9)
    return (int(colour[0]), int(colour[1]), int(colour[2]), int(alpha * 255))


def write_png(path):
    rows = []
    for py in range(SIZE):
        row = bytearray([0])
        for px in range(SIZE):
            row += bytes(pixel(px, py))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    png = sys.argv[2] if len(sys.argv) > 2 else None
    work = tempfile.mkdtemp(prefix="lunaria-icon-")
    try:
        master = os.path.join(work, "icon_1024.png")
        write_png(master)
        if png:
            shutil.copyfile(master, png)
        iconset = os.path.join(work, "Lunaria.iconset")
        os.mkdir(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = "icon_%dx%d%s.png" % (size, size, "@2x" if scale == 2 else "")
                target = os.path.join(iconset, name)
                px = size * scale
                if px == SIZE:
                    shutil.copyfile(master, target)
                else:
                    subprocess.run(["sips", "-z", str(px), str(px), master, "--out", target],
                                   check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
