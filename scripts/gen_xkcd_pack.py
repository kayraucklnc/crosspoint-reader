# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=10", "numpy>=1.26"]
# ///
"""Build the offline xkcd pack for the X3 reader (Play > xkcd).

Downloads every comic's info.0.json and image from xkcd.com, converts each
image to the panel's 1-bit format at its final on-screen size, and writes a
pack the firmware draws without decoding PNGs or touching the network:

    uv run scripts/gen_xkcd_pack.py OUT_DIR [OUT_DIR ...] [--cache DIR]
                                    [--jobs 4] [--limit N] [--preview DIR]

e.g. OUT_DIR = fs_/.crosspoint/xkcd/pack (simulator) or
<sd-card>/.crosspoint/xkcd/pack.

Downloads are cached (default .cache/xkcd, gitignored), so a rerun only
fetches comics published since the last one. At most --jobs (<= 4) requests
run at once, each retried with backoff. Comics without a usable image (#404,
some interactive ones) are skipped.

Pack layout (all little endian):

  index.bin   "XKCDPAK1", u16 version (1), u16 recordSize (32), u32 count,
              u16 maxNum, u16 dataFiles, 16 reserved bytes; then `count`
              32-byte records sorted by comic number:
                u16 num, u8 flags (bit0 landscape), u8 dataFile,
                u16 width, u16 height, u16 year, u8 month, u8 day,
                u8 titleLen, u8 reserved, u16 altLen, u32 textOffset,
                u32 dataOffset, u32 dataSize, u32 reserved
  text.bin    title bytes then alt bytes (UTF-8) at each record's textOffset.
  dataNN.bin  images, each PackBits-compressed 1-bit rows (MSB first,
              1 = black, rows padded to whole bytes), sized exactly to the
              box the reader draws into in the record's orientation.

Why a few big files instead of one BMP per comic: FAT looks a name up by
scanning the directory, so 3,000+ files in one folder cost a ~100 KB scan
per open on an SPI card, while a record here is one 32-byte read at a
computed offset and the image one contiguous read. Data files are split at
DATA_FILE_LIMIT so a seek walks a short cluster chain.
"""

from __future__ import annotations

import argparse
import concurrent.futures as futures
import io
import json
import struct
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter, ImageOps

# Box the reader draws the comic into (XkcdActivity: 8 px side margins, a
# 34 px header + 4 px gap on top, 4 px above the 40 px button hints).
PORTRAIT_BOX = (512, 710)
LANDSCAPE_BOX = (736, 486)
# Landscape only when it makes the comic clearly bigger (turning the reader
# is a cost), matching the firmware's rule for online comics.
LANDSCAPE_GAIN = 1.15
MAX_UPSCALE = 2.0
DATA_FILE_LIMIT = 32 * 1024 * 1024
RECORD = struct.Struct("<HBBHHHBBBBHIIII")
HEADER = struct.Struct("<8sHHIHH16s")
MAGIC = b"XKCDPAK1"
VERSION = 1

USER_AGENT = "crosspoint-xkcd-pack/1.0 (personal offline archive; one-time bulk fetch)"
RETRIES = 4
TIMEOUT_S = 30
# xkcd.com has served high-resolution "_2x" images since roughly #1084.
FIRST_2X = 1080

# Thresholds on the 0..255 gray after levels. Pixels darker than LINE_THRESHOLD
# become ink unless they sit in a flat gray area, which is dithered instead.
LINE_THRESHOLD = 184
FLAT_MIN, FLAT_MAX = 48, 232
FLAT_STDDEV = 14.0


# --- download -------------------------------------------------------------------


def http_get(url: str) -> bytes | None:
    """Body of url, None on 404. Retries other failures with backoff."""
    delay = 1.0
    for attempt in range(RETRIES):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=TIMEOUT_S) as resp:
                return resp.read()
        except urllib.error.HTTPError as err:
            if err.code == 404:
                return None
            if attempt == RETRIES - 1:
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            if attempt == RETRIES - 1:
                raise
        time.sleep(delay)
        delay *= 2
    return None


def image_url(meta: dict) -> str:
    url = meta.get("img", "") or ""
    if url.startswith("http://"):
        url = "https://" + url[len("http://") :]
    return url


def fetch_comic(cache: Path, num: int) -> tuple[int, str]:
    """Caches comic num's JSON and best image. Returns (num, status)."""
    meta_path = cache / "json" / f"{num}.json"
    missing = cache / "json" / f"{num}.missing"
    if missing.exists():
        return num, "missing"
    if not meta_path.exists():
        body = http_get(f"https://xkcd.com/{num}/info.0.json")
        if body is None:
            missing.touch()
            return num, "missing"
        meta_path.write_bytes(body)
    meta = json.loads(meta_path.read_text(encoding="utf-8"))

    img_dir = cache / "img"
    if any(img_dir.glob(f"{num}.*")):
        return num, "cached"
    url = image_url(meta)
    name = url.rsplit("/", 1)[-1]
    if "." not in name:
        (img_dir / f"{num}.none").touch()
        return num, "no image"
    ext = name.rsplit(".", 1)[-1].lower()
    data = None
    if num >= FIRST_2X and ext == "png" and not name.endswith("_2x.png"):
        data = http_get(url[: -len(".png")] + "_2x.png")
    if data is None:
        data = http_get(url)
    if data is None:
        (img_dir / f"{num}.none").touch()
        return num, "no image"
    (img_dir / f"{num}.{ext}").write_bytes(data)
    return num, "downloaded"


def download(cache: Path, jobs: int, limit: int | None) -> int:
    (cache / "json").mkdir(parents=True, exist_ok=True)
    (cache / "img").mkdir(parents=True, exist_ok=True)
    latest = json.loads(http_get("https://xkcd.com/info.0.json"))
    newest = int(latest["num"])
    nums = list(range(1, newest + 1))
    if limit:
        nums = nums[-limit:]
    counts: dict[str, int] = {}
    with futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        tasks = [pool.submit(fetch_comic, cache, n) for n in nums]
        for i, task in enumerate(futures.as_completed(tasks), 1):
            try:
                num, status = task.result()
            except Exception as err:  # noqa: BLE001 - report and keep going
                status = f"error ({err})"
                num = -1
            counts[status.split(" (")[0]] = counts.get(status.split(" (")[0], 0) + 1
            if status.startswith("error"):
                print(f"  {status}", file=sys.stderr)
            if i % 250 == 0 or i == len(tasks):
                print(f"  {i}/{len(tasks)} {counts}", flush=True)
    return newest


# --- conversion -----------------------------------------------------------------


def flatten_gray(img: Image.Image) -> Image.Image:
    """Grayscale over a white background (transparent PNGs are ink on clear)."""
    img.seek(0)  # first frame of animated GIFs
    if img.mode in ("RGBA", "LA", "PA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        bg = Image.new("RGBA", rgba.size, "white")
        img = Image.alpha_composite(bg, rgba)
    return img.convert("L")


def levels(gray: np.ndarray) -> np.ndarray:
    """Stretch paper to white and ink to black without clipping mid-grays.

    Scanned early comics sit on ~235 gray paper with noise; newer ones are
    already 0/255. Percentiles keep a few stray pixels from setting the ends.
    """
    white = float(np.percentile(gray, 90))
    black = float(np.percentile(gray, 0.5))
    white = min(255.0, max(white, 200.0))
    black = max(0.0, min(black, 70.0))
    if white - black < 64:
        return gray.astype(np.float32)
    out = (gray.astype(np.float32) - black) * (255.0 / (white - black))
    return np.clip(out, 0.0, 255.0)


def choose_fit(width: int, height: int) -> tuple[bool, int, int]:
    """(landscape, w, h) for the orientation that shows the comic larger."""
    def fit(box: tuple[int, int]) -> float:
        return min(box[0] / width, box[1] / height, MAX_UPSCALE)

    portrait, wide = fit(PORTRAIT_BOX), fit(LANDSCAPE_BOX)
    landscape = wide > portrait * LANDSCAPE_GAIN
    scale = wide if landscape else portrait
    return landscape, max(1, round(width * scale)), max(1, round(height * scale))


def to_one_bit(gray: Image.Image, size: tuple[int, int]) -> np.ndarray:
    """Scaled, 1-bit (True = ink) rendering tuned for line art.

    Lines: a threshold well above mid-gray, so a 1 px stroke that scaling
    spread into a light gray still prints. Flat gray fills (low local
    variance) would turn solid black under that threshold, so those pixels
    take Floyd-Steinberg dithering instead.
    """
    src_img = Image.fromarray(levels(np.asarray(gray)).astype(np.uint8))
    tone_img = src_img.resize(size, Image.LANCZOS)
    tone = np.asarray(tone_img).astype(np.float32)

    # Strokes: average the plain resize with one of a min-filtered source
    # (min over roughly one output pixel's footprint), so a line thinner than
    # an output pixel keeps most of its darkness instead of fading to gray.
    reduction = min(gray.size[0] / size[0], gray.size[1] / size[1])
    footprint = int(reduction + 0.5) | 1  # odd kernel; 1 (off) below a 1.5x reduction
    if footprint >= 3:
        thick = np.asarray(src_img.filter(ImageFilter.MinFilter(footprint)).resize(size, Image.LANCZOS))
        stroke = 0.4 * tone + 0.6 * thick.astype(np.float32)
    else:
        stroke = tone

    # Local standard deviation over 5x5: small in fills, large on edges.
    blur = np.asarray(tone_img.filter(ImageFilter.BoxBlur(2))).astype(np.float32)
    sq = Image.fromarray(np.clip(tone * tone / 255.0, 0, 255).astype(np.uint8))
    blur_sq = np.asarray(sq.filter(ImageFilter.BoxBlur(2))).astype(np.float32) * 255.0
    stddev = np.sqrt(np.maximum(blur_sq - blur * blur, 0.0))
    flat = (blur > FLAT_MIN) & (blur < FLAT_MAX) & (stddev < FLAT_STDDEV)
    # Grow the flat mask a little so a fill's rim is not a solid black band.
    flat_img = Image.fromarray((flat * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))
    flat = np.asarray(flat_img) > 0

    lines = stroke < LINE_THRESHOLD
    dithered = np.asarray(tone_img.convert("1", dither=Image.Dither.FLOYDSTEINBERG)) == 0
    return np.where(flat & ~(stroke < LINE_THRESHOLD - 64), dithered, lines)


def packbits(data: bytes) -> bytes:
    """Apple PackBits: n>=0 copies n+1 literal bytes, n<0 repeats one byte 1-n times."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 2:
            out.append((1 - run) & 0xFF)
            out.append(data[i])
            i += run
            continue
        start = i
        i += 1
        while i < n and i - start < 128:
            if i + 1 < n and data[i] == data[i + 1]:
                break
            i += 1
        out.append(i - start - 1)
        out += data[start:i]
    return bytes(out)


@dataclass
class Converted:
    num: int
    landscape: bool
    width: int
    height: int
    blob: bytes
    raw_size: int


def convert(path: Path, num: int, preview_dir: Path | None) -> Converted | None:
    try:
        with Image.open(path) as img:
            gray = flatten_gray(img)
    except Exception as err:  # noqa: BLE001 - one bad image must not stop the pack
        print(f"  #{num}: unreadable image ({err})", file=sys.stderr)
        return None
    landscape, w, h = choose_fit(*gray.size)
    ink = to_one_bit(gray, (w, h))
    bits = np.packbits(ink, axis=1)  # MSB first, rows padded to whole bytes
    raw = bits.tobytes()
    if preview_dir is not None:
        Image.fromarray(np.where(ink, 0, 255).astype(np.uint8)).save(preview_dir / f"{num}.png")
    return Converted(num, landscape, w, h, packbits(raw), len(raw))


def convert_job(args: tuple[str, int, str | None]) -> Converted | None:
    path, num, preview = args
    return convert(Path(path), num, Path(preview) if preview else None)


# --- pack -----------------------------------------------------------------------


def clamp_text(text: str, limit: int) -> bytes:
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return raw
    return raw[:limit].decode("utf-8", "ignore").encode("utf-8")


def write_pack(out_dirs: list[Path], cache: Path, comics: list[Converted]) -> None:
    for out in out_dirs:
        out.mkdir(parents=True, exist_ok=True)
        for stale in out.glob("data*.bin"):
            stale.unlink()
    records, text, data_files = [], bytearray(), [bytearray()]
    for c in comics:
        meta = json.loads((cache / "json" / f"{c.num}.json").read_text(encoding="utf-8"))
        title = clamp_text(meta.get("safe_title") or meta.get("title") or "", 255)
        alt = clamp_text(meta.get("alt") or "", 65535)
        if len(data_files[-1]) + len(c.blob) > DATA_FILE_LIMIT:
            data_files.append(bytearray())
        data = data_files[-1]
        records.append(
            RECORD.pack(
                c.num,
                1 if c.landscape else 0,
                len(data_files) - 1,
                c.width,
                c.height,
                int(meta.get("year") or 0),
                int(meta.get("month") or 0),
                int(meta.get("day") or 0),
                len(title),
                0,
                len(alt),
                len(text),
                len(data),
                len(c.blob),
                0,
            )
        )
        text += title + alt
        data += c.blob
    header = HEADER.pack(MAGIC, VERSION, RECORD.size, len(records), comics[-1].num, len(data_files), bytes(16))
    for out in out_dirs:
        for i, data in enumerate(data_files):
            (out / f"data{i:02d}.bin").write_bytes(data)
        (out / "text.bin").write_bytes(text)
        # Index last: a pack is only picked up once its index exists.
        (out / "index.bin").write_bytes(header + b"".join(records))
        (out / "NOTICE.txt").write_text(
            "xkcd comics by Randall Munroe, https://xkcd.com, licensed CC BY-NC 2.5.\n"
            "Converted for offline personal reading on the CrossPoint reader.\n",
            encoding="utf-8",
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out", nargs="+", type=Path, help="pack directories to write")
    parser.add_argument("--cache", type=Path, default=Path(__file__).resolve().parent.parent / ".cache" / "xkcd")
    parser.add_argument("--jobs", type=int, default=4, help="parallel downloads (max 4)")
    parser.add_argument("--limit", type=int, help="only the newest N comics (testing)")
    parser.add_argument("--max-num", type=int, help="leave out comics after this number (testing)")
    parser.add_argument("--offline", action="store_true", help="use the cache only, no downloads")
    parser.add_argument("--preview", type=Path, help="also save each converted comic as PNG here")
    args = parser.parse_args()
    jobs = max(1, min(4, args.jobs))

    started = time.time()
    if not args.offline:
        print(f"Downloading into {args.cache} ({jobs} at a time)...")
        download(args.cache, jobs, args.limit)
    images = sorted(
        (int(p.stem), p)
        for p in (args.cache / "img").glob("*.*")
        if p.suffix.lower() in (".png", ".jpg", ".jpeg", ".gif") and p.stem.isdigit()
    )
    if args.max_num:
        images = [(n, p) for n, p in images if n <= args.max_num]
    if args.limit:
        images = images[-args.limit :]
    if args.preview:
        args.preview.mkdir(parents=True, exist_ok=True)
    print(f"Converting {len(images)} comics...")
    work = [(str(p), n, str(args.preview) if args.preview else None) for n, p in images]
    with futures.ProcessPoolExecutor() as pool:
        comics = [c for c in pool.map(convert_job, work, chunksize=8) if c is not None]
    comics.sort(key=lambda c: c.num)
    if not comics:
        sys.exit("no comics converted")
    write_pack(args.out, args.cache, comics)

    packed = sum(len(c.blob) for c in comics)
    raw = sum(c.raw_size for c in comics)
    wide = sum(c.landscape for c in comics)
    print(
        f"Packed {len(comics)} comics (#{comics[0].num}..#{comics[-1].num}, {wide} landscape): "
        f"{packed / 1e6:.1f} MB images ({raw / 1e6:.1f} MB unpacked), "
        f"{time.time() - started:.0f} s"
    )


if __name__ == "__main__":
    main()
