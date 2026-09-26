#!/usr/bin/env python3
"""Convert Notion-style avatar parts into the Sketch game's SD-card pack.

Source: https://github.com/HMarzban/notion-avatar-generator (MIT licence,
Copyright (c) 2025 Hossein Marzban). The converted art is NOT committed; run
this script to generate it:

    uv run --with resvg-py --with numpy --with pillow scripts/gen_avatars.py OUT_DIR [OUT_DIR ...]

e.g. OUT_DIR = fs_/.crosspoint/avatars (simulator) or <sd-extra>/.crosspoint/avatars.

Layout follows the generator's AvatarPreview: every part is an <img> filling a
square container with object-contain (taller SVGs shrink to fit the height and
centre horizontally), the outfit is shifted down by 10% of the container, and
layers stack outfit < face < eyes < mouth < hair < accessories. Unlike the web
preview nothing is clipped: the union of all parts is fitted into the box.

1-bit conversion renders at 3x and reduces each 3x3 block: mostly-dark blocks
are black (keeps stroke weights), pure black/white blocks threshold, and blocks
with mid-tone fills get a 4x4 ordered dither by their mean luminance.

Output: parts.bin + NOTICE.txt. parts.bin layout (little endian):
  "AVT2", u16 boxW, u16 boxH, u8 categoryCount,
  per category (drawing order): u8 count, u8 randomPercent, u8 flags (bit0: index 0 is "none"),
                                 u8 nameLength, name bytes,
  u8 uiOrder[categoryCount] (the order categories are browsed in),
  per part (category-major): u32 offset, u32 length, i16 x, i16 y, u16 w, u16 h,
  then RLE data: bytes (state << 6) | (run - 1), run 1..64, over the part's w*h
  pixels row-major; state 0 = transparent, 1 = white, 2 = black.
"""

import io
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import resvg_py
from PIL import Image

REPO = "https://github.com/HMarzban/notion-avatar-generator"
ELEMENTS = "src/assets/avatar-elements"
# (folder, display name, random percent, has "none") in drawing order.
CATEGORIES = [
    ("outfit", "Outfit", 100, True),
    ("face", "Face", 100, False),
    ("eyes", "Eyes", 100, True),
    ("mouth", "Mouth", 100, True),
    ("hair", "Hair", 92, True),
    ("accessories", "Accessories", 45, True),
]
UI_ORDER = ["face", "hair", "eyes", "mouth", "outfit", "accessories"]
CONTAINER = 306.0  # the generator's SVG width; parts share this square container
OUTFIT_SHIFT = 0.10  # translateY(10%) on the outfit layer
MAX_W, MAX_H = 520, 640  # target box on the 528x792 X3 portrait
SUPER = 3
BLACK_L, WHITE_L = 60, 235
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) + 0.5
NOTICE = """Avatar parts converted from Notion Avatar Generator
https://github.com/HMarzban/notion-avatar-generator
MIT License, Copyright (c) 2025 Hossein Marzban
"""


def part_files(src: Path, folder: str) -> list:
    return sorted((src / ELEMENTS / folder).glob("*.svg"), key=lambda p: p.name.lower())


def placement(svg: str, folder: str):
    """Returns (x, y, w, h) in container units, as the web preview lays it out."""
    vb = re.search(r'viewBox="([\d.\s-]+)"', svg)
    _, _, vw, vh = (float(v) for v in vb.group(1).split())
    k = min(CONTAINER / vw, CONTAINER / vh)
    w, h = vw * k, vh * k
    x, y = (CONTAINER - w) / 2, (CONTAINER - h) / 2
    if folder == "outfit":
        y += CONTAINER * OUTFIT_SHIFT
    return x, y, w, h


def rasterize(svg: str, w: int, h: int) -> np.ndarray:
    png = bytes(resvg_py.svg_to_bytes(svg_string=svg, width=w * SUPER, height=h * SUPER))
    return np.asarray(Image.open(io.BytesIO(png)).convert("RGBA"))


def classify(rgba: np.ndarray, ox: int, oy: int) -> np.ndarray:
    """3x3-supersampled RGBA -> per-pixel state (0 transparent, 1 white, 2 black)."""
    h, w = rgba.shape[0] // SUPER, rgba.shape[1] // SUPER
    blocks = rgba[: h * SUPER, : w * SUPER].reshape(h, SUPER, w, SUPER, 4).transpose(0, 2, 1, 3, 4)
    blocks = blocks.reshape(h, w, SUPER * SUPER, 4).astype(np.float32)
    opaque = blocks[..., 3] >= 128
    lum = blocks[..., 0] * 0.299 + blocks[..., 1] * 0.587 + blocks[..., 2] * 0.114
    n_opaque = opaque.sum(axis=2)
    dark = opaque & (lum < 128)
    mid = opaque & (lum >= BLACK_L) & (lum <= WHITE_L)
    light = opaque & ~dark
    black_frac = dark.sum(axis=2) / np.maximum(n_opaque, 1)
    mean_light = np.where(light, lum, 0).sum(axis=2) / np.maximum(light.sum(axis=2), 1)
    density = np.clip((WHITE_L - mean_light) / (WHITE_L - BLACK_L), 0, 1) * 16
    ys, xs = np.mgrid[0:h, 0:w]
    dither_black = density > BAYER[(ys + oy) % 4, (xs + ox) % 4]
    state = np.where(black_frac >= 0.5, 2, np.where(mid.any(axis=2) & dither_black, 2, 1))
    return np.where(n_opaque * 2 >= SUPER * SUPER, state, 0).astype(np.uint8)


def rle(grid: np.ndarray) -> bytes:
    flat = grid.ravel()
    out = bytearray()
    i, n = 0, len(flat)
    while i < n:
        v, j = flat[i], i + 1
        while j < n and j - i < 64 and flat[j] == v:
            j += 1
        out.append((int(v) << 6) | (j - i - 1))
        i = j
    return bytes(out)


def main() -> int:
    outs = [Path(p) for p in sys.argv[1:]]
    if not outs:
        print(__doc__)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "hm"
        subprocess.run(["git", "clone", "-q", "--depth", "1", REPO, str(src)], check=True)
        parts = [
            [(f.read_text(), *placement(f.read_text(), folder)) for f in part_files(src, folder)]
            for folder, *_ in CATEGORIES
        ]

        # Pass 1 at 1 px/unit: union of drawn content picks the scale.
        ux0 = uy0 = 1e9
        ux1 = uy1 = -1e9
        for items in parts:
            for svg, x, y, w, h in items:
                grid = classify(rasterize(svg, round(w), round(h)), 0, 0)
                ys, xs = np.nonzero(grid)
                if len(xs):
                    ux0, uy0 = min(ux0, x + xs.min()), min(uy0, y + ys.min())
                    ux1, uy1 = max(ux1, x + xs.max() + 1), max(uy1, y + ys.max() + 1)
        scale = min(MAX_W / (ux1 - ux0), MAX_H / (uy1 - uy0))
        box_w, box_h = round((ux1 - ux0) * scale) + 2, round((uy1 - uy0) * scale) + 2
        print(f"scale {scale:.3f} px/unit, avatar box {box_w}x{box_h}")

        # Pass 2: render each part in place, crop to its content, encode.
        entries, blob = [], bytearray()
        for (folder, _, _, has_none), items in zip(CATEGORIES, parts):
            if has_none:
                entries.append((0, 0, 0, 0, 0, 0))
            for svg, x, y, w, h in items:
                px, py = round((x - ux0) * scale) + 1, round((y - uy0) * scale) + 1
                grid = classify(rasterize(svg, round(w * scale), round(h * scale)), px, py)
                ys, xs = np.nonzero(grid)
                if not len(xs):
                    entries.append((0, 0, 0, 0, 0, 0))
                    continue
                crop = grid[ys.min() : ys.max() + 1, xs.min() : xs.max() + 1]
                data = rle(crop)
                entries.append((len(blob), len(data), px + xs.min(), py + ys.min(), crop.shape[1], crop.shape[0]))
                blob += data

    header = bytearray(b"AVT2" + struct.pack("<HHB", box_w, box_h, len(CATEGORIES)))
    for (folder, name, percent, has_none), items in zip(CATEGORIES, parts):
        header += struct.pack("<BBBB", len(items) + has_none, percent, int(has_none), len(name)) + name.encode()
    folders = [c[0] for c in CATEGORIES]
    header += bytes(folders.index(u) for u in UI_ORDER)
    base = len(header) + len(entries) * 16
    index = b"".join(struct.pack("<IIhhHH", base + o if n else 0, n, x, y, w, h) for o, n, x, y, w, h in entries)
    pack = header + index + bytes(blob)
    for out in outs:
        out.mkdir(parents=True, exist_ok=True)
        (out / "parts.bin").write_bytes(pack)
        (out / "NOTICE.txt").write_text(NOTICE)
        print(f"wrote {out / 'parts.bin'} ({len(pack)} bytes, {len(entries)} parts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
