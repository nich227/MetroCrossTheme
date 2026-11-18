#!/usr/bin/env python3
"""Preview the Lock Screen sleep wallpaper on a computer.

Ports the integer pattern of src/components/LockWallpaper.cpp and writes a PNG, so the look can
be judged without a device. The tuning constants are read from the C++ file; the functions below
must be kept in step with it by hand (same names as the C++ helpers).

    python3 scripts/preview_lock_wallpaper.py 528x792 out.png
    python3 scripts/preview_lock_wallpaper.py 480x800 out.png --check cached.bmp

--check compares a BMP written by the firmware code (2 bits per pixel) with this port, pixel by
pixel, and fails when they differ.

Requires Pillow.
"""

import argparse
import re
import sys
from pathlib import Path

from PIL import Image

SOURCE = Path(__file__).resolve().parent.parent / "src" / "components" / "LockWallpaper.cpp"

# Display grays used for hardware levels 0..3 in the preview.
LEVEL_GRAYS = (0, 96, 200, 255)

TOP, RIGHT, BOTTOM, LEFT = range(4)
MASK = 0xFFFFFFFF


def load_constants(path):
    """Read `constexpr <type> NAME = value;` and `NAME[...] = {a, b};` from the C++ source."""
    text = path.read_text()
    constants = {}
    for name, array, value in re.findall(
        r"^constexpr\s+[\w:]+\s+([A-Z][A-Z0-9_]*)(\[[^\]]*\])?\s*=\s*([^;]+);", text, re.MULTILINE
    ):
        try:
            if array:
                constants[name] = [int(v.strip().rstrip("uU"), 0) for v in value.strip("{} \n").split(",") if v.strip()]
            else:
                constants[name] = int(value.strip().rstrip("uU"), 0)
        except ValueError:
            continue  # not a plain integer constant
    required = (
        "CELL_COLUMNS MIN_CELL_SIZE PATCH_SIZE HASH_SEED TONE_COUNT PATCH_TONE_WEIGHTS PATCH_TONE_MIN "
        "PATCH_TONE_MAX TRIANGLE_SHIFTS ACCENT_ONE_IN ACCENT_TONE_WEIGHTS COORD_BIAS PATCH_SALT"
    ).split()
    missing = [name for name in required if name not in constants]
    if missing:
        sys.exit(f"{path}: cannot find constants: {', '.join(missing)}")
    return constants


class Pattern:
    def __init__(self, c, width, height):
        self.c = c
        self.width = width
        self.height = height
        # makeGrid()
        cell = (width + c["CELL_COLUMNS"]) // (2 * c["CELL_COLUMNS"]) * 2
        self.cell = max(cell, c["MIN_CELL_SIZE"])
        self.offset_x = self.cell - (width % self.cell) // 2
        self.offset_y = self.cell - (height % self.cell) // 2
        self.tones = {}

    @staticmethod
    def mix(h):
        h ^= h >> 16
        h = (h * 0x7FEB352D) & MASK
        h ^= h >> 15
        h = (h * 0x846CA68B) & MASK
        h ^= h >> 16
        return h

    def hash3(self, a, b, c):
        mix = self.mix
        return mix((mix((mix((self.c["HASH_SEED"] ^ a) & MASK) + b) & MASK) + c) & MASK)

    def pick_tone(self, weights, h):
        r = h % sum(weights)
        for tone in range(self.c["TONE_COUNT"] - 1):
            if r < weights[tone]:
                return tone
            r -= weights[tone]
        return self.c["TONE_COUNT"] - 1

    def triangle_tone(self, cell_x, cell_y, triangle):
        key = (cell_x, cell_y, triangle)
        if key in self.tones:
            return self.tones[key]
        c = self.c
        own = self.hash3(cell_x & MASK, cell_y & MASK, triangle)
        if own % c["ACCENT_ONE_IN"] == 0:
            tone = self.pick_tone(c["ACCENT_TONE_WEIGHTS"], own >> 8)
        else:
            a = cell_x + cell_y + (1 if triangle in (RIGHT, BOTTOM) else 0)
            b = cell_x - cell_y + (1 if triangle in (TOP, RIGHT) else 0) + c["COORD_BIAS"]
            patch = self.hash3((a // c["PATCH_SIZE"]) & MASK, (b // c["PATCH_SIZE"]) & MASK, c["PATCH_SALT"])
            shifts = c["TRIANGLE_SHIFTS"]
            tone = self.pick_tone(c["PATCH_TONE_WEIGHTS"], patch) + shifts[(own >> 8) % len(shifts)]
            tone = min(max(tone, c["PATCH_TONE_MIN"]), c["PATCH_TONE_MAX"])
        self.tones[key] = tone
        return tone

    def level_at(self, x, y):
        """Hardware gray level 0..3 of one pixel; pixelLevel() in LockWallpaper.cpp."""
        cell = self.cell
        gx = x + self.offset_x
        gy = y + self.offset_y
        lx = gx % cell
        ly = gy % cell
        if ly < lx:
            triangle = TOP if lx + ly < cell else RIGHT
        else:
            triangle = LEFT if lx + ly < cell else BOTTOM
        tone = self.triangle_tone(gx // cell, gy // cell, triangle)
        return (tone >> 1) + (tone & (gx + gy) & 1)

    def levels(self):
        return bytes(self.level_at(x, y) for y in range(self.height) for x in range(self.width))


def read_bmp_levels(path, width, height):
    """Palette indexes of a 2-bit BMP; Pillow does not read this bit depth."""
    data = Path(path).read_bytes()
    row_bytes = (width * 2 + 31) // 32 * 4
    if data[:2] != b"BM" or int.from_bytes(data[28:30], "little") != 2:
        sys.exit(f"{path}: not a 2-bit BMP")
    bmp_width = int.from_bytes(data[18:22], "little", signed=True)
    bmp_height = int.from_bytes(data[22:26], "little", signed=True)
    offset = int.from_bytes(data[10:14], "little")
    if bmp_width != width or bmp_height != -height:
        sys.exit(f"{path}: is {bmp_width}x{bmp_height}, expected top-down {width}x{height}")
    if len(data) != offset + row_bytes * height:
        sys.exit(f"{path}: {len(data)} bytes, expected {offset + row_bytes * height}")
    palette = [data[54 + 4 * i : 58 + 4 * i] for i in range(4)]
    if palette != [bytes((g, g, g, 0)) for g in (0, 85, 170, 255)]:
        sys.exit(f"{path}: unexpected palette {palette}")
    out = bytearray()
    for y in range(height):
        row = data[offset + y * row_bytes : offset + (y + 1) * row_bytes]
        out.extend((row[x >> 2] >> (6 - 2 * (x & 3))) & 3 for x in range(width))
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("size", help="logical screen size, for example 528x792 (X3) or 480x800 (X4)")
    parser.add_argument("output", help="PNG file to write")
    parser.add_argument("--source", type=Path, default=SOURCE, help="LockWallpaper.cpp to read the constants from")
    parser.add_argument("--check", metavar="BMP", help="compare a BMP written by the firmware code with this port")
    args = parser.parse_args()

    match = re.fullmatch(r"(\d+)x(\d+)", args.size)
    if not match:
        parser.error("size must look like 528x792")
    width, height = int(match.group(1)), int(match.group(2))

    pattern = Pattern(load_constants(args.source), width, height)
    levels = pattern.levels()

    image = Image.frombytes("L", (width, height), bytes(LEVEL_GRAYS[level] for level in levels))
    image.save(args.output)
    counts = [levels.count(bytes([level])) * 100 // len(levels) for level in range(4)]
    print(f"{args.output}: {width}x{height}, cell {pattern.cell} px, levels 0..3 = {counts} %")

    if args.check:
        bmp_levels = read_bmp_levels(args.check, width, height)
        if bmp_levels != levels:
            first = next(i for i in range(len(levels)) if bmp_levels[i] != levels[i])
            sys.exit(f"{args.check}: differs from the port, first at x={first % width} y={first // width}")
        print(f"{args.check}: matches the port")


if __name__ == "__main__":
    main()
