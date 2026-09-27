#!/usr/bin/env python3
"""Build the Trivia game's SD-card question pack from the Open Trivia Database.

Source: every verified question of the Open Trivia Database
(https://opentdb.com), licensed CC BY-SA 4.0. The API serves 50 questions per
request with a session token and a 5 s rate limit, so a full download takes
about ten minutes. The converted pack is NOT committed; run this script to
generate it:

    python3 scripts/gen_trivia.py OUT_DIR [OUT_DIR ...] [--source FILE] [--save FILE]

e.g. OUT_DIR = fs_/.crosspoint/games/trivia (simulator) or
<sd-extra>/.crosspoint/games/trivia. --source reads a previous download (a JSON
list of API results, base64 already decoded); --save keeps this download.

Output: questions.bin + NOTICE.txt. questions.bin layout (little endian):
  header (16 bytes): "TRV1", u16 version 1, u8 categoryCount, u8 0,
                     u32 count, u32 categoryTableOffset
  meta[count]:       u8 per question: bits 0-1 difficulty (0 easy, 1 medium,
                     2 hard), bit 2 true/false question, bits 3-7 category
  offset[count + 1]: u32 per question, into the record blob; the last entry is
                     the blob size
  records:           u8 wrongCount, u16 questionLen + question,
                     u8 answerLen + answer, wrongCount x (u8 len + text)
  category table:    per category: u8 nameLen + name, u16 questionCount
Text is UTF-8 limited to what the device's Noto Sans can draw; questions with
other characters are dropped. Questions are shuffled with a fixed seed so
neighbouring records are unrelated.
"""

import argparse
import base64
import json
import random
import struct
import sys
import time
import unicodedata
import urllib.request
from pathlib import Path

API = "https://opentdb.com"
NOTICE = """Trivia questions: the Open Trivia Database (https://opentdb.com),
licensed CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/).
Converted by scripts/gen_trivia.py: text normalised for the e-ink font and
questions the font cannot draw removed. This pack is shared under the same licence.
"""
MAGIC = b"TRV1"
VERSION = 1
MAX_RECORD = 512
DIFFICULTY = {"easy": 0, "medium": 1, "hard": 2}
SHORT_NAMES = {
    "Entertainment: Japanese Anime & Manga": "Anime & Manga",
    "Entertainment: Cartoon & Animations": "Cartoons",
    "Entertainment: Musicals & Theatres": "Musicals & Theatre",
    "Science: Mathematics": "Mathematics",
    "Science: Computers": "Computers",
    "Science: Gadgets": "Gadgets",
}
# Codepoint ranges covered by the firmware's Noto Sans (lib/EpdFont/builtinFonts).
FONT_RANGES = ((0x20, 0x7E), (0xA0, 0x17F), (0x1A0, 0x1A1), (0x1AF, 0x1B0), (0x1C4, 0x21F), (0x300, 0x36F),
               (0x400, 0x4FF), (0x1EA0, 0x1EF9), (0x2000, 0x2064), (0x2074, 0x208E), (0x20A0, 0x20C0),
               (0x2212, 0x2212))
FOLD = {"­": "", "‎": "", "‏": "", "ʻ": "'", "™": "(TM)", " ": " "}


def drawable(ch: str) -> bool:
    cp = ord(ch)
    return any(lo <= cp <= hi for lo, hi in FONT_RANGES)


def clean(text: str):
    text = unicodedata.normalize("NFC", text)
    text = "".join(FOLD.get(ch, ch) for ch in text)
    text = " ".join(text.split())
    return text if text and all(drawable(ch) for ch in text) else None


def short_category(name: str) -> str:
    if name in SHORT_NAMES:
        return SHORT_NAMES[name]
    for prefix in ("Entertainment: ", "Science: "):
        if name.startswith(prefix):
            return name[len(prefix):]
    return name


def api_get(url: str) -> dict:
    for _ in range(8):
        try:
            with urllib.request.urlopen(url, timeout=30) as response:
                return json.loads(response.read())
        except OSError as error:
            print(f"retry after {error}", file=sys.stderr)
            time.sleep(6)
    raise SystemExit(f"giving up on {url}")


def download() -> list:
    token = api_get(f"{API}/api_token.php?command=request")["token"]
    questions = []
    while True:
        time.sleep(5.5)  # the API allows one request per 5 s per IP
        batch = None
        for amount in (50, 20, 10, 5, 1):
            reply = api_get(f"{API}/api.php?amount={amount}&encode=base64&token={token}")
            if reply["response_code"] == 0:
                batch = reply["results"]
                break
            if reply["response_code"] == 5:
                time.sleep(6)
                continue
            time.sleep(5.5)
        if not batch:
            return questions
        for q in batch:
            questions.append({k: base64.b64decode(v).decode("utf-8") if isinstance(v, str)
                              else [base64.b64decode(x).decode("utf-8") for x in v] for k, v in q.items()})
        print(f"{len(questions)} questions", file=sys.stderr)


def build(raw: list) -> tuple:
    seen = set()
    kept = []
    dropped = 0
    for q in raw:
        texts = [clean(q["question"]), clean(q["correct_answer"])] + [clean(a) for a in q["incorrect_answers"]]
        if any(t is None for t in texts) or q["difficulty"] not in DIFFICULTY:
            dropped += 1
            continue
        key = (texts[0].lower(), texts[1].lower())
        if key in seen:
            dropped += 1
            continue
        seen.add(key)
        boolean = q["type"] == "boolean"
        if not boolean and len(texts) != 5:
            dropped += 1
            continue
        kept.append((short_category(q["category"]), DIFFICULTY[q["difficulty"]], boolean, texts))

    categories = sorted({c for c, _, _, _ in kept})
    if len(categories) > 32:
        raise SystemExit("more than 32 categories do not fit the meta byte")
    random.Random(20260927).shuffle(kept)

    meta = bytearray()
    offsets = []
    blob = bytearray()
    counts = [0] * len(categories)
    for category, difficulty, boolean, texts in kept:
        question, answer, wrong = texts[0].encode(), texts[1].encode(), [t.encode() for t in texts[2:]]
        record = bytes([len(wrong)]) + struct.pack("<H", len(question)) + question
        record += bytes([len(answer)]) + answer
        for w in wrong:
            record += bytes([len(w)]) + w
        if len(record) > MAX_RECORD or len(answer) > 255 or any(len(w) > 255 for w in wrong):
            dropped += 1
            continue
        index = categories.index(category)
        counts[index] += 1
        meta.append(difficulty | (4 if boolean else 0) | (index << 3))
        offsets.append(len(blob))
        blob += record
    offsets.append(len(blob))

    count = len(meta)
    table_offset = 16 + count + 4 * (count + 1) + len(blob)
    out = bytearray(MAGIC + struct.pack("<HBBII", VERSION, len(categories), 0, count, table_offset))
    out += meta
    out += b"".join(struct.pack("<I", o) for o in offsets)
    out += blob
    for name, n in zip(categories, counts):
        encoded = name.encode()
        out += bytes([len(encoded)]) + encoded + struct.pack("<H", n)
    return bytes(out), count, dropped, dict(zip(categories, counts))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dirs", nargs="+", type=Path)
    parser.add_argument("--source", type=Path, help="previous download (JSON list of decoded API results)")
    parser.add_argument("--save", type=Path, help="write this download to FILE for later --source runs")
    args = parser.parse_args()

    raw = json.loads(args.source.read_text()) if args.source else download()
    if args.save:
        args.save.write_text(json.dumps(raw, ensure_ascii=False))
    pack, count, dropped, categories = build(raw)
    if count == 0:
        print("no questions kept", file=sys.stderr)
        return 1
    for out in args.out_dirs:
        out.mkdir(parents=True, exist_ok=True)
        (out / "questions.bin").write_bytes(pack)
        (out / "NOTICE.txt").write_text(NOTICE)
    print(f"{count} questions ({len(pack)} bytes), {dropped} dropped, {len(categories)} categories")
    return 0


if __name__ == "__main__":
    sys.exit(main())
