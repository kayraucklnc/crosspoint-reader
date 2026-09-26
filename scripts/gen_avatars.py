#!/usr/bin/env python3
"""Convert Notion-style avatar parts into the Sketch game's SD-card pack.

Source: https://github.com/jd-solanki/notion-avatars (illustrations by Felix
Wong, "Noto avatar" pack, released under CC0 1.0 per the project README).
The converted art is NOT committed; run this script to generate it:

    uv run --with resvg-py --with pillow scripts/gen_avatars.py OUT_DIR [OUT_DIR ...]

e.g. OUT_DIR = fs_/.crosspoint/avatars (simulator) or <sd-extra>/.crosspoint/avatars.

Output: parts.bin + NOTICE.txt. parts.bin layout (little endian):
  "AVT1", u16 boxW, u16 boxH, u8 categoryCount, u8 count[categoryCount],
  then per part (category-major): u32 offset, u32 length, i16 x, i16 y, u16 w, u16 h,
  then RLE data. Each RLE byte is (state << 6) | (run - 1), run 1..64, over the
  part's w*h pixels row-major; state 0 = transparent, 1 = white, 2 = black.
Categories are stored in drawing order (see CATEGORIES).
"""

import io
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import resvg_py
from PIL import Image

REPO = "https://github.com/jd-solanki/notion-avatars"
PREVIEW = "app/assets/svg/avatar/preview"
# Drawing order, matching the web app's layering. Index 0 of the optional
# categories is an empty part ("none").
CATEGORIES = ["face", "nose", "mouth", "eyes", "eyebrows", "glasses", "hair", "accessories", "details", "beard"]
CANVAS = 1080
# The face spans roughly x 0.20-0.74, y 0.34-0.87 of the canvas. Render the
# canvas at SIZE px and crop a box that fills the 528-wide X3 portrait around
# the face; the longest hair styles get clipped at the box edges.
SIZE = 720
BOX_X, BOX_Y, BOX_W, BOX_H = 76, 80, 528, 640
NOTICE = """Avatar parts converted from Notion Avatars
https://github.com/jd-solanki/notion-avatars (adapted from Mayandev/notion-avatar)
Illustrations: Felix Wong, "Noto avatar" pack. Licensed CC0 1.0 (public domain),
https://creativecommons.org/publicdomain/zero/1.0/
"""


def part_files(src: Path, category: str) -> list:
    files = sorted((src / PREVIEW / category).glob("*.svg"), key=lambda p: int(p.stem))
    return files


def render(svg_path: Path, category: str, size: int) -> Image.Image:
    svg = svg_path.read_text()
    if category == "face":
        # The web app paints the face group white; the SVG itself has no fill.
        svg = svg.replace("<svg ", '<svg fill="#ffffff" ', 1)
    png = bytes(resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size))
    return Image.open(io.BytesIO(png)).convert("RGBA")


def classify(img: Image.Image) -> list:
    w, h = img.size
    px = img.load()
    rows = []
    for y in range(h):
        row = bytearray(w)
        for x in range(w):
            r, g, b, a = px[x, y]
            if a >= 128:
                row[x] = 2 if (r * 299 + g * 587 + b * 114) // 1000 < 128 else 1
        rows.append(row)
    return rows


def bbox(rows: list):
    ys = [y for y, row in enumerate(rows) if any(row)]
    if not ys:
        return None
    xs = [x for row in rows for x, v in enumerate(row) if v]
    return min(xs), ys[0], max(xs) + 1, ys[-1] + 1


def rle(rows: list, box) -> bytes:
    x0, y0, x1, y1 = box
    out = bytearray()
    state, run = None, 0
    for y in range(y0, y1):
        for v in rows[y][x0:x1]:
            if v == state and run < 64:
                run += 1
                continue
            if state is not None:
                out.append((state << 6) | (run - 1))
            state, run = v, 1
    if state is not None:
        out.append((state << 6) | (run - 1))
    return bytes(out)


def main() -> int:
    outs = [Path(p) for p in sys.argv[1:]]
    if not outs:
        print(__doc__)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "notion-avatars"
        subprocess.run(["git", "clone", "-q", "--depth", "1", REPO, str(src)], check=True)

        size, ox, oy, box_w, box_h = SIZE, BOX_X, BOX_Y, BOX_W, BOX_H
        print(f"render size {size}px, avatar box {box_w}x{box_h}")

        # Crop each part to the box, then to its own bounds; encode and index.
        counts, entries, blob = [], [], bytearray()
        for cat in CATEGORIES:
            files = part_files(src, cat)
            counts.append(len(files))
            for f in files:
                rows = [row[ox:ox + box_w] for row in classify(render(f, cat, size))[oy:oy + box_h]]
                b = bbox(rows)
                data = rle(rows, b) if b else b""
                x, y, w, h = (b[0], b[1], b[2] - b[0], b[3] - b[1]) if b else (0, 0, 0, 0)
                entries.append((len(blob), len(data), x, y, w, h))
                blob += data
        header = b"AVT1" + struct.pack("<HHB", box_w, box_h, len(CATEGORIES)) + bytes(counts)
        index_size = len(entries) * 16
        base = len(header) + index_size
        index = b"".join(struct.pack("<IIhhHH", base + o, n, x, y, w, h) for o, n, x, y, w, h in entries)
        pack = header + index + bytes(blob)

    for out in outs:
        out.mkdir(parents=True, exist_ok=True)
        (out / "parts.bin").write_bytes(pack)
        (out / "NOTICE.txt").write_text(NOTICE)
        print(f"wrote {out / 'parts.bin'} ({len(pack)} bytes, {len(entries)} parts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
