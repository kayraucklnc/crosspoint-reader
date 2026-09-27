#!/usr/bin/env python3
"""Validate the Turkish Connections puzzles and build their SD-card pack.

Source: data/connections_tr.yaml (authored for this project). Each puzzle is
four groups of four words, one group per difficulty level (0 easiest .. 3
hardest, the yellow/green/blue/purple of the original game):

    lexicon:                    # semantic classes, lowercase Turkish
      meyve: [elma, armut, ...]
    puzzles:
      - groups:
          - level: 0
            name: "Kırmızı meyveler"
            theme: meyve        # every lexicon[meyve] word could fit this group
            also: [nar]         # more words that could fit (optional)
            words: [çilek, kiraz, vişne, ahududu]
          - level: 1
            name: "___ taşı"
            fill: "{} taşı"     # compound pattern, checked against --dict
            words: [...]
          - level: 2
            name: "İçinde bir hayvan saklı"
            hidden: hayvan      # each word hides a lexicon[hayvan] word
            words: [...]

Words are written in natural Turkish case (proper nouns capitalised) and
upper-cased here with the Turkish rules (i -> İ, ı -> I), so the pack holds
exactly what the tiles draw, in UTF-8.

Checks (errors reject the puzzle from the pack, warnings are for review):
  shape       4 groups x 4 words, levels 0-3 once each, byte limits
  duplicates  a word twice in a puzzle
  unique      exactly one way to split the 16 words into the 4 groups, where a
              word may go to a group when it is in the group's plausible set:
              its words, `also`, the lexicon class of `theme`, a `fill`
              compound found in the dictionary, or a `hidden` class word
              inside it. Red herrings are allowed; ambiguity is not.
  themes      two groups of one puzzle sharing a theme, fill or hidden class
  repeats     a group name, fill pattern or word set used twice in the pack,
              or two groups sharing three or more words; a word used in more
              than MAX_WORD_REUSE puzzles is reported (REUSE)
  fit         a word too wide for a tile in every tile font (error), or only
              in the smallest (warning); a group name wider than the bar
  spelling    a word missing from the --dict word list (warning)

    python3 scripts/gen_connections_tr.py --check [--dict words.txt]
    python3 scripts/gen_connections_tr.py OUT_DIR [OUT_DIR ...] [--dict words.txt]

--dict takes a one-entry-per-line Turkish word list with compounds, such as
https://github.com/mertemin/turkish-word-list (words.txt).

Output: connections_tr.idx and connections_tr.dat in the format of
scripts/gen_connections.py (see ConnectionsPack.h), with text in UTF-8 and the
puzzle number in the date field.
"""

import argparse
import itertools
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("needs PyYAML: uv run --no-project --with pyyaml python3 scripts/gen_connections_tr.py ...")

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = ROOT / "data" / "connections_tr.yaml"
FONT_DIR = ROOT / "lib" / "EpdFont" / "builtinFonts"
NOTICE = """Bağlantılar (Turkish Connections) puzzles: authored for this CrossPoint
fork, source data/connections_tr.yaml, built by scripts/gen_connections_tr.py.
Game logic and pack format ported from CrossPlay
(https://github.com/ma-r-s/crossplay, MIT).
"""

MAGIC = 0x31505843
VERSION = 1
GROUPS = 4
MEMBERS = 4
MAX_WORD = 32
MAX_GROUP = 80
# Tile text room on the X3 in portrait (528 px): (528 - 2*40 - 3*8) / 4 - 8.
TILE_ROOM = 98
# Group bar text room on the board: 448 - 12 - 18 - 12 - 12.
BAR_ROOM = 394
MAX_WORD_REUSE = 4
MIN_HIDDEN = 3


# ---- Turkish text -------------------------------------------------------------


def tr_upper(text: str) -> str:
    return text.replace("i", "İ").replace("ı", "I").upper()


def tr_lower(text: str) -> str:
    return text.replace("I", "ı").replace("İ", "i").lower()


def key(text: str) -> str:
    """Comparison form: Turkish lower case, single spaces, no hyphens."""
    return re.sub(r"\s+", " ", tr_lower(text).replace("-", " ")).strip()


def squash(text: str) -> str:
    return key(text).replace(" ", "")


# ---- fonts ----------------------------------------------------------------------


def load_advances(name: str) -> dict:
    """Codepoint -> advance in pixels, read from a built-in font header."""
    text = (FONT_DIR / f"{name}.h").read_text()
    glyph_block = re.search(r"Glyphs\[\] = \{(.*?)\n\};", text, re.DOTALL).group(1)
    advances = [int(m.group(1)) / 16.0 for m in re.finditer(r"\{\s*\d+,\s*\d+,\s*(\d+),", glyph_block)]
    interval_block = re.search(r"Intervals\[\] = \{(.*?)\n\};", text, re.DOTALL).group(1)
    table = {}
    for m in re.finditer(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\s*\}", interval_block):
        first, last, offset = (int(v, 16) for v in m.groups())
        for cp in range(first, last + 1):
            table[cp] = advances[offset + cp - first]
    return table


TILE_FONTS = ["ubuntu_12_bold", "ubuntu_10_bold", "notosans_8_regular"]


class Metrics:
    def __init__(self):
        self.fonts = {name: load_advances(name) for name in TILE_FONTS}

    def width(self, font: str, text: str) -> float:
        table = self.fonts[font]
        return sum(table.get(ord(ch), 0) for ch in text)

    def missing(self, text: str) -> set:
        return {ch for ch in text for font in self.fonts.values() if ord(ch) not in font}

    def tile_font(self, word: str) -> int:
        """Index of the tile font the board uses for `word`, or -1 (drawn cut
        short). Mirrors fitTile() in ConnectionsRender.cpp: one line, else two
        at a space or hyphen, else two at a syllable break with a hyphen."""
        for index, font, hyphenate in TILE_ATTEMPTS:
            for head, tail in splits(word, hyphenate):
                if max(self.width(font, head), self.width(font, tail) if tail else 0) <= TILE_ROOM:
                    return index
        return -1


VOWELS = set("AEIİOÖUÜÂÎÛ")
# (font index, allow a syllable break) in the order the board tries them.
TILE_ATTEMPTS = [(0, TILE_FONTS[0], False), (1, TILE_FONTS[1], False), (0, TILE_FONTS[0], True),
                 (1, TILE_FONTS[1], True), (2, TILE_FONTS[2], False), (2, TILE_FONTS[2], True)]


def middle_break(word: str, allowed) -> int:
    """The break position nearest the middle for which allowed(i) holds, or -1."""
    best = -1
    for i in range(1, len(word) - 1):
        if allowed(i) and (best < 0 or abs(i - len(word) // 2) < abs(best - len(word) // 2)):
            best = i
    return best


def splits(word: str, hyphenate: bool):
    """Candidate renderings: (line1, line2 or "")."""
    yield word, ""
    at = middle_break(word, lambda i: word[i] in " -")
    if at > 0:
        yield word[: at + (1 if word[at] == "-" else 0)], word[at + 1 :]
    elif hyphenate:
        # Before a consonant that starts a syllable: consonant then vowel,
        # at least two letters on each side.
        def syllable(i):
            return (2 <= i <= len(word) - 2 and word[i].isalpha() and word[i] not in VOWELS
                    and word[i + 1] in VOWELS and word[i - 1].isalpha())
        at = middle_break(word, syllable)
        if at > 0:
            yield word[:at] + "-", word[at:]


# ---- dictionary -------------------------------------------------------------------


class Dictionary:
    def __init__(self, path):
        self.entries = set()
        self.squashed = set()
        if path:
            for line in Path(path).read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if line:
                    self.entries.add(key(line))
                    self.squashed.add(squash(line))

    def __bool__(self):
        return bool(self.entries)

    def has(self, text: str) -> bool:
        return key(text) in self.entries or squash(text) in self.squashed


# ---- model --------------------------------------------------------------------------


class Group:
    def __init__(self, raw: dict, lexicon: dict, where: str):
        self.where = where
        level = str(raw.get("level", ""))
        self.level = int(level) if level.isdigit() else None
        self.name = str(raw.get("name", "")).strip()
        self.words = [str(w).strip() for w in raw.get("words", [])]
        self.theme = raw.get("theme")
        self.fill = raw.get("fill")
        self.hidden = raw.get("hidden")
        self.also = {key(w) for w in raw.get("also", []) or []}
        self.lexicon = lexicon
        self.keys = [key(w) for w in self.words]

    @property
    def kind(self):
        """What makes two groups of one puzzle 'the same theme'."""
        if self.theme:
            return f"theme:{self.theme}"
        if self.fill:
            return f"fill:{key(self.fill)}"
        if self.hidden:
            return f"hidden:{self.hidden}"
        return None

    def fits(self, word_key: str, dictionary: Dictionary) -> bool:
        if word_key in self.keys or word_key in self.also:
            return True
        if self.theme and word_key in self.lexicon.get(self.theme, set()):
            return True
        if self.fill and dictionary and dictionary.has(self.fill.replace("{}", word_key)):
            return True
        if self.hidden:
            flat = word_key.replace(" ", "")
            for item in self.lexicon.get(self.hidden, set()):
                item = item.replace(" ", "")
                if len(item) >= MIN_HIDDEN and item in flat:
                    return True
        return False


class Puzzle:
    def __init__(self, raw: dict, lexicon: dict, index: int, origin: str):
        self.index = index
        self.origin = origin
        self.groups = [Group(g, lexicon, f"{origin}#{index}.{i}") for i, g in enumerate(raw.get("groups", []))]
        self.errors = []
        self.warnings = []
        self.herrings = 0

    def label(self):
        names = " / ".join(g.name for g in self.groups)
        return f"{self.origin} puzzle {self.index} [{names}]"


# ---- checks ---------------------------------------------------------------------------


def count_partitions(puzzle: Puzzle, dictionary: Dictionary) -> int:
    words = [k for g in puzzle.groups for k in g.keys]
    candidates = [[w for w in words if g.fits(w, dictionary)] for g in puzzle.groups]
    total = 0

    def assign(group_index: int, used: frozenset):
        nonlocal total
        if total > 1:
            return
        if group_index == len(puzzle.groups):
            total += 1
            return
        pool = [w for w in candidates[group_index] if w not in used]
        for combo in itertools.combinations(pool, MEMBERS):
            assign(group_index + 1, used | frozenset(combo))

    assign(0, frozenset())
    return total


def check_puzzle(puzzle: Puzzle, dictionary: Dictionary, metrics: Metrics, lexicon: dict):
    err, warn = puzzle.errors.append, puzzle.warnings.append
    if len(puzzle.groups) != GROUPS:
        err(f"has {len(puzzle.groups)} groups")
        return
    levels = sorted(g.level for g in puzzle.groups if isinstance(g.level, int))
    if levels != [0, 1, 2, 3]:
        err(f"levels {levels} are not 0,1,2,3")
    kinds = Counter(g.kind for g in puzzle.groups if g.kind)
    for kind, n in kinds.items():
        if n > 1:
            err(f"two groups share {kind}")
    for g in puzzle.groups:
        if len(g.words) != MEMBERS:
            err(f"group '{g.name}' has {len(g.words)} words")
        if not g.name:
            err("a group has no name")
        if g.theme and g.theme not in lexicon:
            err(f"group '{g.name}' theme '{g.theme}' is not in the lexicon")
        if g.hidden and g.hidden not in lexicon:
            err(f"group '{g.name}' hidden class '{g.hidden}' is not in the lexicon")
        if g.fill and "{}" not in g.fill:
            err(f"group '{g.name}' fill has no {{}}")
        name = tr_upper(g.name)
        if len(name.encode()) > MAX_GROUP:
            err(f"group name '{g.name}' is over {MAX_GROUP} bytes")
        if metrics.width("ubuntu_10_bold", name) > BAR_ROOM:
            warn(f"group name '{g.name}' wraps or is cut on the bar")
        for w in g.words:
            upper = tr_upper(w)
            if not w or len(upper.encode()) > MAX_WORD:
                err(f"word '{w}' is empty or over {MAX_WORD} bytes")
            if metrics.missing(upper):
                err(f"word '{w}' has glyphs missing from the tile fonts: {metrics.missing(upper)}")
            fit = metrics.tile_font(upper)
            if fit < 0:
                err(f"word '{w}' does not fit a tile")
            elif fit == 2:
                warn(f"word '{w}' only fits in the smallest tile font")
            if dictionary and not w[:1].isupper() and not dictionary.has(w) and key(w) not in lexicon_words(lexicon):
                warn(f"word '{w}' is not in the dictionary")
            if g.fill and dictionary and not dictionary.has(g.fill.replace("{}", key(w))):
                warn(f"'{g.fill.replace('{}', w)}' is not in the dictionary")
            if g.hidden and not hides(g, key(w)):
                err(f"word '{w}' hides no '{g.hidden}' word")
    all_keys = [k for g in puzzle.groups for k in g.keys]
    dupes = [k for k, n in Counter(all_keys).items() if n > 1]
    if dupes:
        err(f"repeated words {dupes}")
        return
    if puzzle.errors:
        return
    partitions = count_partitions(puzzle, dictionary)
    if partitions != 1:
        overlaps = []
        for g in puzzle.groups:
            for other in puzzle.groups:
                if other is g:
                    continue
                stray = [w for w in other.keys if g.fits(w, dictionary)]
                if stray:
                    overlaps.append(f"{stray} also fit '{g.name}'")
        err("ambiguous: " + "; ".join(overlaps))
    for g in puzzle.groups:
        puzzle.herrings += sum(1 for other in puzzle.groups if other is not g for w in other.keys
                               if g.fits(w, dictionary))


def hides(group: Group, word_key: str) -> bool:
    flat = word_key.replace(" ", "")
    return any(len(item.replace(" ", "")) >= MIN_HIDDEN and item.replace(" ", "") in flat
               for item in group.lexicon.get(group.hidden, set()))


_LEXICON_WORDS = {}


def lexicon_words(lexicon: dict) -> set:
    ident = id(lexicon)
    if ident not in _LEXICON_WORDS:
        _LEXICON_WORDS[ident] = {w for words in lexicon.values() for w in words}
    return _LEXICON_WORDS[ident]


def check_pack(puzzles: list):
    """Repeats across the whole pack: first use wins, later ones are errors."""
    names, fills, sets = {}, {}, {}
    by_word = defaultdict(set)
    seen_groups = []
    for p in puzzles:
        for g in p.groups:
            name_key = key(g.name)
            if name_key in names:
                p.errors.append(f"group name '{g.name}' already used by {names[name_key]}")
            else:
                names[name_key] = p.label()
            if g.fill:
                fill_key = key(g.fill)
                if fill_key in fills:
                    p.errors.append(f"fill '{g.fill}' already used by {fills[fill_key]}")
                else:
                    fills[fill_key] = p.label()
            word_set = frozenset(g.keys)
            if word_set in sets:
                p.errors.append(f"words of '{g.name}' repeat {sets[word_set]}")
            else:
                sets[word_set] = p.label()
            for other_name, other_set, other_label in seen_groups:
                if len(word_set & other_set) >= 3 and word_set != other_set:
                    p.errors.append(f"'{g.name}' shares {sorted(word_set & other_set)} with '{other_name}' ({other_label})")
            seen_groups.append((g.name, word_set, p.label()))
            for k in g.keys:
                by_word[k].add(id(p))
    overused = {w: len(ps) for w, ps in by_word.items() if len(ps) > MAX_WORD_REUSE}
    return overused


# ---- loading ------------------------------------------------------------------------------


def load(paths):
    lexicon = defaultdict(set)
    raw_puzzles = []
    for path in paths:
        # BaseLoader keeps every scalar a string: YAML 1.1 would read the
        # Turkish words "on" (ten) or "yes" as booleans.
        doc = yaml.load(Path(path).read_text(encoding="utf-8"), Loader=yaml.BaseLoader) or {}
        for cls, words in (doc.get("lexicon") or {}).items():
            lexicon[cls].update(key(w) for w in words)
        for raw in doc.get("puzzles") or []:
            raw_puzzles.append((raw, Path(path).name))
    counters = Counter()
    puzzles = []
    for raw, origin in raw_puzzles:
        counters[origin] += 1
        puzzles.append(Puzzle(raw, lexicon, counters[origin], origin))
    return dict(lexicon), puzzles


# ---- pack -------------------------------------------------------------------------------------


def build(puzzles: list):
    index = bytearray(struct.pack("<IHH", MAGIC, VERSION, 0))
    data = bytearray()
    for number, p in enumerate(puzzles, start=1):
        index += struct.pack("<II", number, len(data))
        data += struct.pack("<HI", number, number)
        for g in sorted(p.groups, key=lambda g: g.level):
            name = tr_upper(g.name).encode()
            data += bytes([g.level, len(name)]) + name
            for w in g.words:
                word = tr_upper(w).encode()
                data += bytes([len(word)]) + word
    return bytes(index), bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out_dirs", nargs="*", type=Path)
    parser.add_argument("--source", type=Path, nargs="+", default=[DEFAULT_SOURCE])
    parser.add_argument("--dict", type=Path, help="Turkish word list, one entry per line")
    parser.add_argument("--check", action="store_true", help="validate only")
    parser.add_argument("--verbose", "-v", action="store_true", help="list every warning")
    args = parser.parse_args()

    lexicon, puzzles = load(args.source)
    dictionary = Dictionary(args.dict)
    metrics = Metrics()
    for p in puzzles:
        check_puzzle(p, dictionary, metrics, lexicon)
    overused = check_pack(puzzles)

    good = [p for p in puzzles if not p.errors]
    rejected = [p for p in puzzles if p.errors]
    warned = [p for p in good if p.warnings]
    for p in rejected:
        print(f"REJECT {p.label()}")
        for e in p.errors:
            print(f"    - {e}")
    for p in warned if args.verbose else []:
        print(f"WARN   {p.label()}")
        for w in p.warnings:
            print(f"    - {w}")
    for w, n in sorted(overused.items(), key=lambda kv: -kv[1]):
        print(f"REUSE  '{w}' appears in {n} puzzles (limit {MAX_WORD_REUSE})")
    herrings = sum(1 for p in good if p.herrings)
    levels = Counter(g.kind.split(":")[0] if g.kind else "free" for p in good for g in p.groups)
    print(f"{len(puzzles)} puzzles: {len(good)} accepted, {len(rejected)} rejected, "
          f"{len(warned)} with warnings, {herrings} with red herrings, "
          f"{len(overused)} overused words; group kinds {dict(levels)}"
          + ("" if dictionary else " (no --dict: fill and spelling checks skipped)"))

    if args.check or not args.out_dirs:
        return 1 if rejected else 0
    if not good:
        print("no puzzles accepted", file=sys.stderr)
        return 1
    index, data = build(good)
    for out in args.out_dirs:
        out.mkdir(parents=True, exist_ok=True)
        (out / "connections_tr.idx").write_bytes(index)
        (out / "connections_tr.dat").write_bytes(data)
        (out / "NOTICE_TR.txt").write_text(NOTICE, encoding="utf-8")
    print(f"wrote {len(good)} puzzles ({len(data)} + {len(index)} bytes) to {len(args.out_dirs)} folder(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
