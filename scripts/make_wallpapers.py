"""Convert images into X3-native wallpapers: 528x792, pre-dithered to the panel's
4 gray levels (0/85/170/255) and saved as 8-bit palette BMPs, so the firmware
maps pixels 1:1 with no on-device dithering.

usage: uv run --with pillow scripts/make_wallpapers.py SRC_DIR OUT_DIR
"""
import sys
from pathlib import Path

from PIL import Image, ImageOps

WIDTH, HEIGHT = 528, 792
LEVELS = (0, 85, 170, 255)
EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp", ".gif"}


def four_level_palette() -> Image.Image:
    pal = Image.new("P", (1, 1))
    flat = [c for v in LEVELS for c in (v, v, v)]
    pal.putpalette(flat + [0] * (768 - len(flat)))
    return pal


def convert(src: Path, dst: Path, palette: Image.Image) -> None:
    img = ImageOps.exif_transpose(Image.open(src))
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        bg = Image.new("RGBA", img.size, "white")
        img = Image.alpha_composite(bg, img)
    gray = ImageOps.fit(img.convert("L"), (WIDTH, HEIGHT), Image.LANCZOS)
    # Stretch levels (ignore 0.5% outliers) so dark images keep visible detail.
    gray = ImageOps.autocontrast(gray, cutoff=0.5)
    # E-ink midtones print dark; lift them a little before quantizing.
    gray = gray.point(lambda v: round(255 * (v / 255) ** 0.85))
    out = gray.convert("RGB").quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)
    out.save(dst, format="BMP")


def main() -> None:
    src_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    palette = four_level_palette()
    for src in sorted(p for p in src_dir.iterdir() if p.suffix.lower() in EXTS):
        dst = out_dir / (src.stem + ".bmp")
        if dst.exists():
            dst = out_dir / f"{src.stem}-{src.suffix.lstrip('.').lower()}.bmp"
        try:
            convert(src, dst, palette)
            print(f"ok   {src.name} -> {dst.name}")
        except Exception as exc:  # report and keep going with the rest
            print(f"FAIL {src.name}: {exc}")


if __name__ == "__main__":
    main()
