#!/usr/bin/env python3
"""Compares a mattone-run screenshot with a reference PNG.

Usage: compare.py SCREENSHOT.pgm|SCREENSHOT.ppm REFERENCE.png [DIFF.pgm]

A PGM (DMG) screenshot and the reference are reduced to the four DMG shades
(nearest of FF/AA/55/00), so the reference may use any grey or colour palette
with that brightness order. A PPM (CGB) screenshot must match the reference's
RGB values exactly. Exits 0 when every pixel matches. Standard library only.
"""
import struct
import sys
import zlib


def read_pgm(path):
    """Returns width, height and grey values; for a PPM, (r, g, b) tuples."""
    data = open(path, "rb").read()
    parts = data.split(maxsplit=4)
    if parts[0] not in (b"P5", b"P6"):
        raise ValueError(f"{path}: not a binary PGM or PPM")
    width, height = int(parts[1]), int(parts[2])
    if parts[0] == b"P5":
        return width, height, list(parts[4][: width * height])
    raw = parts[4][: width * height * 3]
    return width, height, [tuple(raw[i : i + 3]) for i in range(0, len(raw), 3)]


def read_png(path, rgb=False):
    """Decodes a non-interlaced PNG (greyscale/palette at 1-8 bits, RGB(A) at 8)
    to luma values, or to (r, g, b) tuples with rgb=True."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, palette = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos : pos + 8])
        body = data[pos + 8 : pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if interlace or depth == 16 or (ctype in (2, 4, 6) and depth != 8):
                raise ValueError(f"{path}: unsupported PNG format")
        elif kind == b"PLTE":
            palette = [tuple(body[i : i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = (width * channels * depth + 7) // 8
    bpp = max(1, channels * depth // 8)  # filter distance in bytes
    rows, prev = [], bytearray(stride)
    for y in range(height):
        filt = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        rows.append(line)
        prev = line
    luma = []
    for line in rows:
        if depth < 8:  # unpack 1/2/4-bit samples, most significant first
            per_byte, mask = 8 // depth, (1 << depth) - 1
            line = [(line[i // per_byte] >> (8 - depth * (i % per_byte + 1))) & mask for i in range(width)]
            if ctype == 0:
                line = [v * 255 // mask for v in line]
        for x in range(width):
            px = line[x * channels : (x + 1) * channels]
            if ctype == 3:
                r, g, b = palette[px[0]]
            elif channels >= 3:
                r, g, b = px[0], px[1], px[2]
            else:
                r = g = b = px[0]
            luma.append((r, g, b) if rgb else (r * 299 + g * 587 + b * 114) // 1000)
    return width, height, luma


def shade(v):
    return min(range(4), key=lambda s: abs(v - (0xFF - s * 0x55)))


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    w1, h1, got = read_pgm(sys.argv[1])
    colour = bool(got) and isinstance(got[0], tuple)
    w2, h2, want = read_png(sys.argv[2], rgb=colour)
    if (w1, h1) != (w2, h2):
        print(f"size mismatch: {w1}x{h1} vs {w2}x{h2}")
        return 1
    if colour:
        diff = [i for i in range(w1 * h1) if got[i] != want[i]]
    else:
        diff = [i for i in range(w1 * h1) if shade(got[i]) != shade(want[i])]
    if len(sys.argv) == 4:
        with open(sys.argv[3], "wb") as f:
            f.write(b"P5\n%d %d\n255\n" % (w1, h1))
            f.write(bytes(0 if i in set(diff) else 0xFF for i in range(w1 * h1)))
    if diff:
        first = diff[0]
        print(f"{len(diff)} pixels differ (first at x={first % w1}, y={first // w1})")
        return 1
    print("screens match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
