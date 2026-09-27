#pragma once

// Ported from CrossPlay (https://github.com/ma-r-s/crossplay, MIT,
// Copyright (c) 2026 Mario Ruiz).

// What you scored on each puzzle, one byte apiece.
//
// Stored as a flat array indexed by days since the archive's first puzzle, so a
// lookup is a seek and a one-byte read: no search, no table in RAM, and no
// dependence on a puzzle's position in the pack (which shifts every time the
// archive gains entries). Growing a byte a day, it is about 1.2KB today and
// 1.5KB in 2027.
//
// Freestanding: the encoding and the calendar arithmetic live here; the
// activity supplies the file.

#include <cstdint>

namespace connections {

// 2023-06-12, the first published puzzle. Fixed forever: it is the origin of
// every index in the file, so moving it would silently reinterpret every stored
// result as belonging to a different puzzle.
constexpr uint32_t kResultsEpoch = 20230612;

struct Record {
  bool played = false;
  bool lost = false;
  // 0-4. Meaningful only when `played`.
  uint8_t mistakes = 0;
  // The hardest group went first. Only ever set for puzzles whose source
  // published colour data, which stopped on 2025-09-20; see kLevelUnknown.
  bool hardestFirst = false;
};

uint8_t encodeRecord(const Record& record);
Record decodeRecord(uint8_t byte);

// Days between two YYYYMMDD dates, or -1 if either is not a real date. Used to
// turn a puzzle's date into its offset in the file.
int daysBetween(uint32_t from, uint32_t to);

// 0 = Sunday .. 6 = Saturday, or -1 for a malformed date. Falls straight out of
// the same civil-day count the result index uses, so a calendar needs no
// calendar arithmetic of its own.
int dayOfWeek(uint32_t date);

// Days in a month, accounting for leap years. 0 for a malformed month.
int daysInMonth(int year, int month);

// `date` moved by `days` (either sign), or 0 for a malformed date.
uint32_t addDays(uint32_t date, int days);

// Everything the menu says about your record, from one pass over the results
// file. Freestanding because two of its rules are easy to get
// subtly wrong and impossible to see in a screenshot: the streak counts back
// from the newest puzzle in the pack rather than from today, and it skips a
// newest day that is still unplayed (a streak should not read zero at
// breakfast). `recent` holds the DayResult codes the menu draws, oldest first.
struct Summary {
  int played = 0;
  int perfect = 0;
  int won = 0;
  int streak = 0;
  int bestStreak = 0;  // longest run of consecutive puzzle days won
  // Wins by mistakes made (0-3); losses are played - won.
  int winsByMistakes[4] = {};
  uint8_t recent[16] = {};
  bool newestPlayed = false;
  uint8_t newestMistakes = 0;
};

// Codes stored in Summary::recent, matching connectionsui::DayResult.
enum : uint8_t { kDayUnplayed = 0, kDayWon = 1, kDayPerfect = 2, kDayLost = 3 };

// `results` is the whole file, `count` its length in bytes, `newestIndex` the
// result index of the newest puzzle held. Out-of-range indices are treated as
// unplayed rather than read, so a short file is safe.
Summary summarise(const uint8_t* results, int count, int newestIndex);

// Index of `date` in the results file, or -1 when it is before the epoch or the
// date is malformed.
inline int resultIndex(const uint32_t date) { return daysBetween(kResultsEpoch, date); }

}  // namespace connections
