#!/usr/bin/env python3
"""Build the Wordle game's SD-card word lists (English and Turkish).

The lists are NOT committed; run this script to generate them:

    python3 scripts/gen_wordle_words.py OUT_DIR [OUT_DIR ...] [--header src/activities/play/WordleFallback.h]

e.g. OUT_DIR = fs_/.crosspoint/games/wordle (simulator) or <sd>/.crosspoint/games/wordle.
Downloads are cached in ~/.cache/crosspoint-wordle (override with --cache).

Sources
  English answers  cfreshman "wordle-answers-alphabetical" gist: the original
                   2,315 Wordle solutions, minus the six NYT later withdrew.
                   https://gist.github.com/cfreshman/a03ef2cba789d8cf00c08f767e0fad7b
  English guesses  tabatkins/wordle-list (MIT): every guess the NYT game accepts,
                   plus the cfreshman "allowed guesses" gist.
                   https://github.com/tabatkins/wordle-list
                   https://gist.github.com/cfreshman/cdcdf777450c5b5301e439061d29694c
  Turkish words    CanNuhlar/Turkce-Kelime-Listesi (TDK spelling guide, 76k entries,
                   via MehmetHuseyinDelipalta/Wordle-Turkce-Kelime-Listesi, MIT) and
                   mertemin/turkish-word-list (Turkish Wiktionary, 64k entries).
  Turkish freq.    hermitdave/FrequencyWords 2018 tr_full (OpenSubtitles counts,
                   CC BY-SA 4.0): ranks answers and adds common inflected forms
                   (e.g. "geldi", "evler") to the guess list.

Turkish answers are lowercase headwords present in BOTH dictionaries, ranked by
subtitle frequency, minus a hand blocklist (insults, slurs, crude words, bare
names, case-inflected forms); the top TR_ANSWERS are kept. Circumflexes fold
(â->a, î->i, û->u); dotted/dotless I are lowered the Turkish way (I->ı, İ->i).

Output per OUT_DIR: {en,tr}_{answers,valid}.bin + NOTICE.txt. Each .bin is
  u8 'W', u8 'D', u8 lang ('E'|'T'), u8 kind ('A' answers | 'V' valid),
  u32 count (little endian), then count u32 records sorted ascending.
A record packs the five letters' alphabet indices, 5 bits each, first letter
in bits 24..20: EN alphabet a..z, TR alphabet "abcçdefgğhıijklmnoöprsştuüvyz".
Numeric order equals alphabetical order, so the firmware binary-searches the
file in place. Valid lists include every answer.
"""

import argparse
import struct
import sys
import urllib.request
from pathlib import Path

EN_ALPHABET = "abcdefghijklmnopqrstuvwxyz"
TR_ALPHABET = "abcçdefgğhıijklmnoöprsştuüvyz"

URLS = {
    "en_answers.txt": "https://gist.githubusercontent.com/cfreshman/a03ef2cba789d8cf00c08f767e0fad7b/raw/wordle-answers-alphabetical.txt",
    "en_allowed.txt": "https://gist.githubusercontent.com/cfreshman/cdcdf777450c5b5301e439061d29694c/raw/wordle-allowed-guesses.txt",
    "en_nyt.txt": "https://raw.githubusercontent.com/tabatkins/wordle-list/main/words",
    "tr_tdk.txt": "https://raw.githubusercontent.com/MehmetHuseyinDelipalta/Wordle-Turkce-Kelime-Listesi/main/Turkce-Kelime-Listesi.txt",
    "tr_wiktionary.txt": "https://raw.githubusercontent.com/mertemin/turkish-word-list/master/words.txt",
    "tr_freq.txt": "https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/tr/tr_full.txt",
}

# Removed from the NYT answer pool after the 2022 acquisition.
EN_BLOCK = {"agora", "pupal", "lynch", "fibre", "slave", "wench"}

TR_ANSWERS = 1600
TR_MIN_ANSWER_FREQ = 250
TR_MIN_GUESS_FREQ = 50

# Headwords that are poor daily answers: insults, slurs, sexual or crude words,
# sensitive religious terms, bare given names, and inflected forms the
# dictionaries list as separate entries.
TR_BLOCK = set("""
lanet tanrı allah aptal salak herif seksi porno penis sperm taşak yarak kahpe zenci sapık fuhuş boklu
seviş nonoş şişko ahmak enayi angut denyo lavuk hödük moruk sidik dışkı sümük işeme kusma ishal idrar
hadım kabız eroin taciz kafir cihat haçlı iblis ırkçı bücür gıcık azgın küfür
tokyo burcu begüm cemil celal ferdi kerim cemal ismet selen erika ihsan recep şaban abbas frank
adama kalbi fikri gücün yükün bağın devri yurdu sudan barda elden günde başta koşun çıkın konuş
çalış düşün şarki yakin takim sarpa pines savma kaçta adına oraya orada orası birer onlar bende
sence sizce bizce bence olmuş verdi çıktı girdi bastı bindi yağar batar kayar geçer bilir sever
koyar kaçar biner basar duyar uymaz ölmez çalar dizim yükün tanıt uyuma gayri nakli şekli cinsi saçık
değin yatır velet papel sakat topal bunak sağır bakir
""".split())

EN_FALLBACK = (
    "about above actor adult after again agent agree alarm album alive apple award beach bread brick "
    "chair charm clock cloud dance dream earth faith field flame fruit glass grape heart house light "
    "money music night ocean paper piano plant river smile stone table train water world"
).split()


def fetch(cache: Path, name: str) -> Path:
    path = cache / name
    if not path.exists():
        print(f"downloading {URLS[name]}", file=sys.stderr)
        cache.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(URLS[name]) as response:
            path.write_bytes(response.read())
    return path


def tr_fold(word: str) -> str:
    word = word.strip().replace("İ", "i").replace("I", "ı").lower()
    return word.replace("â", "a").replace("î", "i").replace("û", "u")


def is_word(word: str, alphabet: str) -> bool:
    return len(word) == 5 and all(c in alphabet for c in word)


def read_lines(path: Path):
    return path.read_text(encoding="utf-8").splitlines()


def build_english(cache: Path):
    answers = {w.strip().lower() for w in read_lines(fetch(cache, "en_answers.txt"))}
    answers = {w for w in answers if is_word(w, EN_ALPHABET) and w not in EN_BLOCK}
    valid = set(answers)
    for name in ("en_allowed.txt", "en_nyt.txt"):
        valid |= {w.strip().lower() for w in read_lines(fetch(cache, name)) if is_word(w.strip().lower(), EN_ALPHABET)}
    return sorted(answers), sorted(valid)


def tr_dictionary(path: Path):
    """Returns (lowercase headwords, all entries) folded to 5-letter words."""
    common, every = set(), set()
    for line in read_lines(path):
        line = line.strip()
        if not line:
            continue
        word = tr_fold(line)
        if not is_word(word, TR_ALPHABET):
            continue
        every.add(word)
        if not line[0].isupper():
            common.add(word)
    return common, every


def build_turkish(cache: Path):
    tdk_common, tdk_all = tr_dictionary(fetch(cache, "tr_tdk.txt"))
    wik_common, wik_all = tr_dictionary(fetch(cache, "tr_wiktionary.txt"))
    freq = {}
    with fetch(cache, "tr_freq.txt").open(encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 2:
                continue
            word = tr_fold(parts[0])
            if is_word(word, TR_ALPHABET):
                freq[word] = freq.get(word, 0) + int(parts[1])
    headwords = tdk_common & wik_common
    ranked = sorted(
        (w for w in headwords if freq.get(w, 0) >= TR_MIN_ANSWER_FREQ and w not in TR_BLOCK),
        key=lambda w: (-freq[w], w),
    )
    answers = ranked[:TR_ANSWERS]
    valid = tdk_all | wik_all | {w for w, c in freq.items() if c >= TR_MIN_GUESS_FREQ} | set(answers)
    return sorted(answers), sorted(valid), ranked


def pack(word: str, alphabet: str) -> int:
    value = 0
    for c in word:
        value = (value << 5) | alphabet.index(c)
    return value


def write_list(path: Path, lang: str, kind: str, words, alphabet: str) -> int:
    records = sorted(pack(w, alphabet) for w in words)
    data = b"WD" + lang.encode() + kind.encode() + struct.pack("<I", len(records))
    data += b"".join(struct.pack("<I", r) for r in records)
    path.write_bytes(data)
    return len(data)


def write_header(path: Path, en_fallback, tr_fallback):
    def rows(words, alphabet):
        values = sorted(pack(w, alphabet) for w in words)
        lines, line = [], "   "
        for v in values:
            item = f" 0x{v:07X}u,"
            if len(line) + len(item) > 116:
                lines.append(line)
                line = "   "
            line += item
        lines.append(line)
        return "\n".join(lines)

    path.write_text(
        "#pragma once\n\n#include <cstdint>\n\n"
        "// Generated by scripts/gen_wordle_words.py: answers used when the SD word\n"
        "// lists are missing. Records pack five 5-bit alphabet indices (see WordleWords.h).\n"
        "namespace wordle {\n\n"
        f"constexpr uint32_t FALLBACK_EN[] = {{\n{rows(en_fallback, EN_ALPHABET)}\n}};\n\n"
        f"constexpr uint32_t FALLBACK_TR[] = {{\n{rows(tr_fallback, TR_ALPHABET)}\n}};\n\n"
        "}  // namespace wordle\n",
        encoding="utf-8",
    )


NOTICE = """Wordle word lists for CrossPoint (generated by scripts/gen_wordle_words.py)

English answers: original Wordle solution list via https://gist.github.com/cfreshman/a03ef2cba789d8cf00c08f767e0fad7b
English guesses: https://github.com/tabatkins/wordle-list (MIT) and https://gist.github.com/cfreshman/cdcdf777450c5b5301e439061d29694c
Turkish words: https://github.com/CanNuhlar/Turkce-Kelime-Listesi (TDK) via
  https://github.com/MehmetHuseyinDelipalta/Wordle-Turkce-Kelime-Listesi (MIT),
  https://github.com/mertemin/turkish-word-list (Turkish Wiktionary)
Turkish frequencies: https://github.com/hermitdave/FrequencyWords (content CC BY-SA 4.0)
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("out_dirs", nargs="+", type=Path)
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache" / "crosspoint-wordle")
    parser.add_argument("--header", type=Path, help="also write the built-in fallback header")
    args = parser.parse_args()

    en_answers, en_valid = build_english(args.cache)
    tr_answers, tr_valid, tr_ranked = build_turkish(args.cache)
    for out in args.out_dirs:
        out.mkdir(parents=True, exist_ok=True)
        sizes = {
            "en_answers.bin": write_list(out / "en_answers.bin", "E", "A", en_answers, EN_ALPHABET),
            "en_valid.bin": write_list(out / "en_valid.bin", "E", "V", en_valid, EN_ALPHABET),
            "tr_answers.bin": write_list(out / "tr_answers.bin", "T", "A", tr_answers, TR_ALPHABET),
            "tr_valid.bin": write_list(out / "tr_valid.bin", "T", "V", tr_valid, TR_ALPHABET),
        }
        (out / "NOTICE.txt").write_text(NOTICE, encoding="utf-8")
        for name, size in sizes.items():
            print(f"{out / name}: {size} bytes")
    print(f"EN answers {len(en_answers)}, guesses {len(en_valid)}; TR answers {len(tr_answers)}, guesses {len(tr_valid)}")
    if args.header:
        missing = [w for w in EN_FALLBACK if w not in en_answers]
        if missing:
            raise SystemExit(f"fallback words not in the answer list: {missing}")
        write_header(args.header, EN_FALLBACK, tr_ranked[:48])
        print(f"wrote {args.header}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
