#!/usr/bin/env python3
"""Checks the sound output end to end: builds a tiny ROM that plays A4 on
each channel in turn, records it with `mattone-run --wav` and measures the
pitch from the zero crossings.

Usage: tone-test.py path/to/mattone-run
"""
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

# The Game Boy clocks: squares step every 4 T-cycles * 8 steps, the wave
# channel every 2 T-cycles * 32 steps, so both play 131072 / (2048 - f) Hz.
SQUARE_F = 2048 - round(131072 / 440)  # 1750 -> 439.8 Hz
WAVE_F = 2048 - round(65536 / 440)     # 1899 -> 439.8 Hz (32 samples per cycle)


def ldh(reg, value):
    """LD A,value ; LDH (reg),A"""
    return bytes([0x3E, value, 0xE0, reg])


def build_rom(channel):
    code = ldh(0x26, 0x80) + ldh(0x25, 0xFF) + ldh(0x24, 0x77)
    if channel in (1, 2):
        b = 0x10 if channel == 1 else 0x15
        f = SQUARE_F
        code += ldh(b + 1, 0x80) + ldh(b + 2, 0xF0) + ldh(b + 3, f & 0xFF) + ldh(b + 4, 0x80 | f >> 8)
    else:
        # One sine cycle in the 32 four-bit samples.
        samples = [round(7.5 + 7.5 * math.sin(2 * math.pi * i / 32)) for i in range(32)]
        for i in range(16):
            code += ldh(0x30 + i, samples[2 * i] << 4 | samples[2 * i + 1])
        f = WAVE_F
        code += ldh(0x1A, 0x80) + ldh(0x1C, 0x20) + ldh(0x1D, f & 0xFF) + ldh(0x1E, 0x80 | f >> 8)
    code += bytes([0x18, 0xFE])  # JR -2: loop forever

    rom = bytearray(0x8000)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])  # NOP; JP 0x150
    rom[0x134:0x13F] = b"TONE TEST\0\0"
    rom[0x150 : 0x150 + len(code)] = code
    checksum = 0
    for b in rom[0x134:0x14D]:
        checksum = (checksum - b - 1) & 0xFF
    rom[0x14D] = checksum
    return bytes(rom)


def pitch(path):
    w = wave.open(path)
    rate, n = w.getframerate(), w.getnframes()
    data = struct.unpack(f"<{n * 2}h", w.readframes(n))
    left = data[0::2][rate // 2 :]  # skip the first half second
    mean = sum(left) / len(left)
    crossings = sum(1 for a, b in zip(left, left[1:]) if a < mean <= b)
    return crossings * rate / len(left), max(abs(x) for x in left)


def main():
    run = sys.argv[1]
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for channel in (1, 2, 3):
            rom, out = os.path.join(tmp, "tone.gb"), os.path.join(tmp, "tone.wav")
            open(rom, "wb").write(build_rom(channel))
            subprocess.run([run, "--frames", "180", "--wav", out, rom], check=True)
            hz, peak = pitch(out)
            ok = abs(hz - 439.8) < 2 and peak > 2000
            failed += not ok
            print(f"  {'pass' if ok else 'FAIL'}  channel {channel}: {hz:.1f} Hz, peak {peak}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
