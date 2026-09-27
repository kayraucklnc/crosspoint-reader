#pragma once

// Ported from CrossPlay (https://github.com/ma-r-s/crossplay, MIT,
// Copyright (c) 2026 Mario Ruiz).

// What you scored on each puzzle, one byte apiece.
//
// Stored as a flat array indexed by days since the language's launch day (one
// daily per day), so a lookup is one byte: no search and no dependence on a
// puzzle's position in the pack. It grows a byte a day.
//
// Freestanding: the encoding and the calendar arithmetic live here; the
// activity supplies the file.

#include <cstdint>

namespace connections {

struct Record {
  bool played = false;
  bool lost = false;
  // 0-4. Meaningful only when `played`.
  uint8_t mistakes = 0;
  // The hardest group went first. Only set for puzzles with level data.
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
// file. Freestanding because one of its rules is easy to get subtly wrong and
// impossible to see in a screenshot: the streak counts back from today and
// skips today while it is still unplayed (a streak should not read zero at
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
// result index of today. Out-of-range indices are treated as unplayed rather
// than read, so a short file is safe.
Summary summarise(const uint8_t* results, int count, int newestIndex);

}  // namespace connections
