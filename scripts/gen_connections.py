#!/usr/bin/env python3
"""Build the Connections game's SD-card puzzle pack.

Source: the community archive of NYT Connections answers at SOURCE_URL
(Eyefyre/NYT-Connections-Answers). The puzzles are the New York Times'; the
pack is built for personal use and is NOT committed. The device is offline: it
deals its own English daily from this pack (a shuffled pass, see
src/activities/play/ConnectionsDaily.h), and rerunning this script is how the
pack is refreshed. A pack with a different puzzle count deals a different
daily order (saved results stay keyed by date).

    python3 scripts/gen_connections.py OUT_DIR [OUT_DIR ...] [--source FILE]

e.g. OUT_DIR = fs_/.crosspoint/games/connections (simulator) or
<sd-extra>/.crosspoint/games/connections.

Output: connections.idx, connections.dat and NOTICE.txt, read by
src/activities/play/ConnectionsPack.cpp:

  connections.idx  u32 magic "CXP1", u16 version 1, u16 0, then per puzzle
                   u32 date (YYYYMMDD), u32 offset into connections.dat,
                   ascending by date.
  connections.dat  per puzzle: u16 id, u32 date, then 4 groups of
                   u8 level (0-3, 0xFF unknown), u8 len + name,
                   4 x (u8 len + word). Text is folded to ASCII.

Puzzles holding characters with no ASCII form (emoji) are skipped.
"""

import argparse
import json
import struct
import sys
import urllib.request
from pathlib import Path

SOURCE_URL = "https://raw.githubusercontent.com/Eyefyre/NYT-Connections-Answers/main/connections.json"
NOTICE = """Connections puzzles: the New York Times' Connections, from the community
archive https://github.com/Eyefyre/NYT-Connections-Answers. Not affiliated with
the New York Times. Pack format and game logic ported from CrossPlay
(https://github.com/ma-r-s/crossplay, MIT).
"""
MAGIC = 0x31505843
VERSION = 1
GROUPS = 4
MEMBERS = 4
MAX_WORD = 32
MAX_GROUP = 80
TEXT_CAP = MAX_GROUP * 2 + 8
LEVEL_UNKNOWN = 0xFF

SPECIAL = {
    0x2018: "'", 0x2019: "'", 0x02BC: "'", 0x201C: '"', 0x201D: '"', 0x2013: "-", 0x2014: "-",
    0x2212: "-", 0x2022: "*", 0x25CF: "*", 0x00A0: " ", 0x2026: "...", 0x00B0: " DEG",
    0x00DF: "ss", 0x00C6: "AE", 0x00E6: "ae",
}
LATIN1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy"


def fold(cp: int):
    """ASCII form of a codepoint: a string, "" to drop it, or None if it has none."""
    if 0x20 <= cp < 0x7F:
        return chr(cp)
    if cp in SPECIAL:
        return SPECIAL[cp]
    if 0xC0 <= cp <= 0xFF:
        return LATIN1[cp - 0xC0]
    if cp in (0xFE0F, 0x200D, 0xFEFF):
        return ""
    if cp in (ord("\n"), ord("\t")):
        return None
    return None


def fold_text(text: str):
    """(folded, overflowed) or None when a character cannot be drawn."""
    out = []
    length = 0
    overflow = False
    for ch in text:
        piece = fold(ord(ch))
        if piece is None:
            return None
        if length + len(piece) >= TEXT_CAP:
            overflow = True
            continue
        out.append(piece)
        length += len(piece)
    return "".join(out), overflow


def parse_date(text) -> int:
    if not isinstance(text, str) or len(text) != 10 or text[4] != "-" or text[7] != "-":
        return 0
    digits = text[:4] + text[5:7] + text[8:]
    return int(digits) if digits.isdigit() else 0


def convert(puzzle: dict):
    """A (id, date, groups) tuple, or a skip reason string."""
    groups = []
    for answer in (puzzle.get("answers") or [])[:GROUPS]:
        name = fold_text(str(answer.get("group", "")))
        if name is None:
            return "unrenderable"
        level = answer.get("level", LEVEL_UNKNOWN)
        level = level if isinstance(level, int) and 0 <= level <= 3 else LEVEL_UNKNOWN
        words = []
        for member in (answer.get("members") or [])[:MEMBERS]:
            word = fold_text(str(member))
            if word is None or word[1] or len(word[0]) > MAX_WORD:
                return "unrenderable"
            words.append(word[0])
        words += [""] * (MEMBERS - len(words))
        groups.append((level, name[0][:MAX_GROUP], words))
    date = parse_date(puzzle.get("date"))
    if len(groups) != GROUPS or date == 0:
        return "malformed"
    all_words = [w for _, _, ws in groups for w in ws]
    if any(not name for _, name, _ in groups) or any(not w for w in all_words):
        return "malformed"
    if len(set(all_words)) != len(all_words):
        return "malformed"
    pid = puzzle.get("id", 0)
    pid = max(0, min(0xFFFF, pid if isinstance(pid, int) else 0))
    return pid, date, groups


def build(puzzles: list):
    index = bytearray(struct.pack("<IHH", MAGIC, VERSION, 0))
    data = bytearray()
    last_date = 0
    stats = {"accepted": 0, "unrenderable": 0, "malformed": 0, "out_of_order": 0}
    for puzzle in puzzles:
        result = convert(puzzle)
        if isinstance(result, str):
            stats[result] += 1
            continue
        pid, date, groups = result
        stats["accepted"] += 1
        if last_date and date <= last_date:
            stats["out_of_order"] += 1
            continue
        index += struct.pack("<II", date, len(data))
        data += struct.pack("<HI", pid, date)
        for level, name, words in groups:
            data += bytes([level, len(name)]) + name.encode("ascii")
            for word in words:
                data += bytes([len(word)]) + word.encode("ascii")
        last_date = date
    return bytes(index), bytes(data), stats


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dirs", nargs="+", type=Path)
    parser.add_argument("--source", type=Path, help="local copy of connections.json")
    args = parser.parse_args()

    if args.source:
        raw = args.source.read_bytes()
    else:
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as response:
            raw = response.read()
    puzzles = json.loads(raw)
    index, data, stats = build(puzzles)
    count = (len(index) - 8) // 8
    if count == 0:
        print("no puzzles accepted", file=sys.stderr)
        return 1
    first = struct.unpack_from("<I", index, 8)[0]
    last = struct.unpack_from("<I", index, len(index) - 8)[0]
    for out in args.out_dirs:
        out.mkdir(parents=True, exist_ok=True)
        (out / "connections.idx").write_bytes(index)
        (out / "connections.dat").write_bytes(data)
        (out / "NOTICE.txt").write_text(NOTICE)
    print(f"{count} puzzles {first}..{last} ({len(data)} bytes); skipped {stats}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
