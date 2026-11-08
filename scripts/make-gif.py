#!/usr/bin/env python3
"""Turns frames dumped by `mattone-run --dump DIR` into an animated GIF.

Usage: make-gif.py DIR OUT.gif [--from N] [--to N] [--step N] [--scale N]

Standard library only. Frames are 4-shade PGMs (DMG) or PPMs (CGB); one
colour table serves the whole GIF when 256 colours are enough, otherwise each
frame gets its own. Consecutive identical frames are merged into one longer
frame.
"""
import argparse
import glob
import os
import struct



def lzw(indices, min_size):
    clear, end = 1 << min_size, (1 << min_size) + 1
    table = {bytes([i]): i for i in range(clear)}
    size, next_code = min_size + 1, end + 1
    out, bits, nbits = bytearray(), 0, 0

    def emit(code):
        nonlocal bits, nbits
        bits |= code << nbits
        nbits += size
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8

    emit(clear)
    w = b""
    for i in indices:
        wc = w + bytes([i])
        if wc in table:
            w = wc
            continue
        emit(table[w])
        if next_code < 4096:
            table[wc] = next_code
            next_code += 1
            if next_code > (1 << size) and size < 12:
                size += 1
        else:
            emit(clear)
            table = {bytes([k]): k for k in range(clear)}
            size, next_code = min_size + 1, end + 1
        w = bytes([i])
    emit(table[w])
    emit(end)
    if nbits:
        out.append(bits & 0xFF)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("out")
    ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--to", type=int, default=1 << 30)
    ap.add_argument("--step", type=int, default=2)
    ap.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()

    frames = []
    for path in sorted(glob.glob(os.path.join(a.dir, "*.p[gp]m"))):
        n = int(os.path.basename(path)[:-4])
        if a.start <= n <= a.to and (n - a.start) % a.step == 0:
            data = open(path, "rb").read().split(maxsplit=4)
            w, h = int(data[1]), int(data[2])
            if data[0] == b"P6":
                raw = data[4][: w * h * 3]
                px = [raw[i : i + 3] for i in range(0, len(raw), 3)]
            else:
                px = [bytes([v, v, v]) for v in data[4][: w * h]]
            if frames and frames[-1][0] == px:
                frames[-1][1] += 1
            else:
                frames.append([px, 1])

    def palette(colour_set):
        colours = sorted(colour_set)
        if len(colours) > 256:
            raise SystemExit(f"a frame has {len(colours)} colours: a GIF holds 256")
        bits = max(2, (len(colours) - 1).bit_length())
        table = b"".join(colours) + b"\0\0\0" * ((1 << bits) - len(colours))
        return {c: i for i, c in enumerate(colours)}, bits, table

    all_colours = {c for px, _ in frames for c in px}
    shared = palette(all_colours) if len(all_colours) <= 256 else None

    s = a.scale
    W, H = w * s, h * s
    flags = (0x80 | (shared[1] - 1) << 4 | (shared[1] - 1)) if shared else 0
    out = bytearray(b"GIF89a" + struct.pack("<HHBBB", W, H, flags, 0, 0))
    if shared:
        out += shared[2]
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"  # loop forever
    for px, count in frames:
        index, bits, table = shared or palette(set(px))
        delay = round(count * a.step * 100 / 59.7275)
        out += b"\x21\xF9\x04\x00" + struct.pack("<H", delay) + b"\x00\x00"
        out += b"\x2C" + struct.pack("<HHHHB", 0, 0, W, H, 0 if shared else 0x80 | (bits - 1))
        if not shared:
            out += table
        idx = bytearray()
        for y in range(h):
            row = bytearray()
            for x in range(w):
                row += bytes([index[px[y * w + x]]]) * s
            idx += bytes(row) * s
        data = lzw(idx, bits)
        out.append(bits)
        for i in range(0, len(data), 255):
            chunk = data[i : i + 255]
            out += bytes([len(chunk)]) + chunk
        out.append(0)
    out.append(0x3B)
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(frames)} frames, {len(out) // 1024} KB")


if __name__ == "__main__":
    main()
