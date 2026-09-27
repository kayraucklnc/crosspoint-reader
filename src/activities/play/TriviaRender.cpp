// Drawing for TriviaActivity: home, question, result card, filters and stats.
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "PlayCommon.h"
#include "TriviaActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 24;
constexpr int NOTE_H = 32;
constexpr int OPTION_GAP = 10;
constexpr int MAX_LINE = 100;  // bytes per wrapped line; lines live on the 8 KB render stack
constexpr int BADGE = 34;

constexpr StrId DIFFICULTY_NAMES[4] = {StrId::STR_TRIVIA_EASY, StrId::STR_TRIVIA_MEDIUM, StrId::STR_TRIVIA_HARD,
                                       StrId::STR_TRIVIA_ANY_DIFFICULTY};

int hintsTop(const GfxRenderer& r) {
  return r.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
}

// Greedy word wrap. Writes at most maxLines lines of up to MAX_LINE bytes;
// returns the number of lines the whole text needs (can exceed maxLines).
int wrap(const GfxRenderer& r, const int fontId, const EpdFontFamily::Style style, const char* text, const int width,
         char (*lines)[MAX_LINE], const int maxLines) {
  int count = 0;
  const char* p = text;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    char line[MAX_LINE] = {};
    int length = 0;
    const char* rest = p;
    while (*rest) {
      const char* wordEnd = rest;
      while (*wordEnd && *wordEnd != ' ') wordEnd++;
      const int wordLength = static_cast<int>(wordEnd - rest);
      const int candidate = length + (length ? 1 : 0) + wordLength;
      if (candidate >= MAX_LINE) break;
      char trial[MAX_LINE];
      memcpy(trial, line, length);
      int at = length;
      if (length) trial[at++] = ' ';
      memcpy(trial + at, rest, wordLength);
      trial[at + wordLength] = '\0';
      if (length && r.getTextWidth(fontId, trial, style) > width) break;
      memcpy(line, trial, at + wordLength + 1);
      length = at + wordLength;
      rest = wordEnd;
      while (*rest == ' ') rest++;
    }
    if (length == 0) {  // a single word wider than the line: cut it
      length = std::min(static_cast<int>(strlen(p)), MAX_LINE - 1);
      memcpy(line, p, length);
      line[length] = '\0';
      rest = p + length;
    }
    if (count < maxLines) memcpy(lines[count], line, length + 1);
    count++;
    p = rest;
  }
  return count;
}

void drawPill(const GfxRenderer& r, const char* text, const int y, const bool solid) {
  const int width = r.getScreenWidth();
  const int tw = r.getTextWidth(UI_10_FONT_ID, text, EpdFontFamily::BOLD);
  const int pw = std::min(width - 2 * SIDE, tw + 36);
  if (solid) {
    r.fillRoundedRect((width - pw) / 2, y, pw, NOTE_H, NOTE_H / 2, Color::Black);
  } else {
    r.drawRoundedRect((width - pw) / 2, y, pw, NOTE_H, 2, NOTE_H / 2, true);
  }
  r.drawText(UI_10_FONT_ID, (width - tw) / 2, y + (NOTE_H - r.getLineHeight(UI_10_FONT_ID)) / 2, text, !solid,
             EpdFontFamily::BOLD);
}

void drawCheck(const GfxRenderer& r, const int cx, const int cy, const bool black) {
  r.drawLine(cx - 10, cy, cx - 3, cy + 8, 4, black);
  r.drawLine(cx - 3, cy + 8, cx + 12, cy - 10, 4, black);
}

void drawCross(const GfxRenderer& r, const int cx, const int cy, const bool black) {
  r.drawLine(cx - 9, cy - 9, cx + 9, cy + 9, 4, black);
  r.drawLine(cx + 9, cy - 9, cx - 9, cy + 9, 4, black);
}

void hatch(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int step) {
  const auto clip = r.getClipRect();
  r.setClipRect(x, y, w, h);
  for (int d = -h; d < w; d += step) r.drawLine(x + d, y + h, x + d + h, y, 2, true);
  r.setClipRect(clip[0], clip[1], clip[2], clip[3]);
}

// Three rising bars, `level` of them filled (1 easy .. 3 hard).
void drawDifficulty(const GfxRenderer& r, const int x, const int y, const int level) {
  for (int i = 0; i < 3; i++) {
    const int h = 6 + i * 4;
    if (i < level) {
      r.fillRect(x + i * 7, y + 14 - h, 5, h, true);
    } else {
      r.drawRect(x + i * 7, y + 14 - h, 5, h, 1, true);
    }
  }
}
}  // namespace

// ---- shared -------------------------------------------------------------------

void TriviaActivity::drawMenuIcon(const GfxRenderer& r, const int x, const int y, const int size) {
  // A speech bubble with a question mark.
  const int h = size * 3 / 4;
  r.fillRoundedRect(x, y, size, h, size / 5, Color::Black);
  const int tx = x + size / 5;
  const int xs[3] = {tx, tx + size / 4, tx};
  const int ys[3] = {y + h - 2, y + h - 2, y + size - 2};
  r.fillPolygon(xs, ys, 3, true);
  const int fontId = size >= 64 ? NOTOSANS_18_FONT_ID : UI_12_FONT_ID;
  const int tw = r.getTextWidth(fontId, "?", EpdFontFamily::BOLD);
  r.drawText(fontId, x + (size - tw) / 2, y + (h - r.getLineHeight(fontId)) / 2, "?", false, EpdFontFamily::BOLD);
  r.fillRoundedRect(x + size - size / 3, y + size / 8, size / 5, size / 5, size / 10, Color::White);
}

const char* TriviaActivity::modeName(const Mode which) const {
  switch (which) {
    case Mode::DAILY:
      return tr(STR_TRIVIA_DAILY);
    case Mode::SUDDEN:
      return tr(STR_TRIVIA_SUDDEN);
    case Mode::QUICK:
      break;
  }
  return tr(STR_TRIVIA_QUICK);
}

void TriviaActivity::filterSummary(char* out, const int size) const {
  const char* category =
      stats.category == trivia::ALL_CATEGORIES ? tr(STR_TRIVIA_ALL_CATEGORIES) : pack.categoryName(stats.category);
  snprintf(out, size, "%s  ·  %s", I18N.get(DIFFICULTY_NAMES[stats.difficulty]), category);
}

void TriviaActivity::drawHeader(const char* title, const char* subtitle) const {
  renderer.drawText(UI_12_FONT_ID, SIDE, 12, title, true, EpdFontFamily::BOLD);
  if (!subtitle) return;
  const int w = renderer.getTextWidth(UI_10_FONT_ID, subtitle);
  renderer.drawText(UI_10_FONT_ID, renderer.getScreenWidth() - SIDE - w, 15, subtitle);
}

void TriviaActivity::drawStatTiles(const int y, const int* values, const StrId* labels) const {
  const int width = renderer.getScreenWidth();
  const int cellW = (width - 2 * SIDE) / 4;
  for (int i = 0; i < 4; i++) {
    const int cx = SIDE + i * cellW + cellW / 2;
    char text[12];
    snprintf(text, sizeof(text), "%d", values[i]);
    const int tw = renderer.getTextWidth(NOTOSANS_18_FONT_ID, text, EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_18_FONT_ID, cx - tw / 2, y, text, true, EpdFontFamily::BOLD);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, I18N.get(labels[i]), cellW - 4);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
    renderer.drawText(SMALL_FONT_ID, cx - lw / 2, y + 42, label.c_str());
  }
}

// One mark per question of a ten-question round: solid = right, cross = wrong,
// ring = the question on screen, thin ring = still to come.
void TriviaActivity::drawPips(const int y, const uint16_t mask, const int answered, const int currentIndex) const {
  const int width = renderer.getScreenWidth();
  constexpr int PIP = 22, PITCH = 34;
  const int x0 = (width - (trivia::ROUND_LENGTH - 1) * PITCH - PIP) / 2;
  for (int i = 0; i < trivia::ROUND_LENGTH; i++) {
    const int x = x0 + i * PITCH;
    if (i < answered && (mask & (1u << i))) {
      renderer.fillRoundedRect(x, y, PIP, PIP, PIP / 2, Color::Black);
    } else if (i < answered) {
      renderer.drawRoundedRect(x, y, PIP, PIP, 2, PIP / 2, true);
      renderer.drawLine(x + 6, y + 6, x + PIP - 7, y + PIP - 7, 2, true);
      renderer.drawLine(x + PIP - 7, y + 6, x + 6, y + PIP - 7, 2, true);
    } else if (i == currentIndex) {
      renderer.drawRoundedRect(x - 3, y - 3, PIP + 6, PIP + 6, 3, PIP / 2 + 3, true);
    } else {
      renderer.drawRoundedRect(x, y, PIP, PIP, 1, PIP / 2, true);
    }
  }
}

int TriviaActivity::drawWrapped(const int fontId, const char* text, const int x, const int y, const int width,
                                const int maxLines, const bool black, const EpdFontFamily::Style style) const {
  constexpr int MAX_LINES = 7;
  char lines[MAX_LINES][MAX_LINE];
  const int limit = std::min(maxLines, MAX_LINES);
  const int needed = wrap(renderer, fontId, style, text, width, lines, limit);
  const int shown = std::min(needed, limit);
  if (needed > limit) {
    const std::string cut = renderer.truncatedText(fontId, lines[shown - 1], width - 10, style);
    snprintf(lines[shown - 1], MAX_LINE, "%s", cut.c_str());
  }
  const int lh = renderer.getLineHeight(fontId);
  for (int i = 0; i < shown; i++) renderer.drawText(fontId, x, y + i * lh, lines[i], black, style);
  return shown * lh;
}

// ---- home ---------------------------------------------------------------------

void TriviaActivity::renderHome() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();

  drawMenuIcon(renderer, SIDE, 16, 56);
  renderer.drawText(NOTOSANS_18_FONT_ID, SIDE + 72, 8, tr(STR_PLAY_TRIVIA), true, EpdFontFamily::BOLD);
  char text[80];
  if (pack.isOpen()) {
    snprintf(text, sizeof(text), tr(STR_TRIVIA_TAGLINE), pack.count());
  } else {
    snprintf(text, sizeof(text), "%s", tr(STR_TRIVIA_NO_PACK));
  }
  renderer.drawText(UI_10_FONT_ID, SIDE + 72, 50, text);

  const int accuracy =
      stats.answered ? static_cast<int>((stats.correct * 100 + stats.answered / 2) / stats.answered) : 0;
  const int values[4] = {static_cast<int>(stats.answered), accuracy, trivia::currentDailyStreak(stats, today),
                         stats.bestRun};
  static constexpr StrId LABELS[4] = {StrId::STR_TRIVIA_ANSWERED, StrId::STR_TRIVIA_ACCURACY,
                                      StrId::STR_TRIVIA_DAILY_STREAK, StrId::STR_TRIVIA_BEST_RUN};
  drawStatTiles(88, values, LABELS);

  const int top = 172;
  constexpr int GAP = 10;
  const int itemH = std::min(96, (hintsTop(renderer) - 12 - top - GAP * (HOME_ITEMS - 1)) / HOME_ITEMS);
  for (int i = 0; i < HOME_ITEMS; i++) {
    const int y = top + i * (itemH + GAP);
    const int x = SIDE, w = width - 2 * SIDE;
    const bool focused = i == homeSelected;
    renderer.drawRoundedRect(x, y, w, itemH, focused ? 3 : 1, 10, true);
    constexpr int ICON = 44;
    const int ix = x + 16, iy = y + (itemH - ICON) / 2;
    char title[48] = {}, line[96] = {};
    switch (i) {
      case HOME_DAILY: {
        renderer.drawRoundedRect(ix, iy + 4, ICON, ICON - 4, 2, 6, true);
        renderer.fillRoundedRect(ix, iy + 4, ICON, 12, 6, true, true, false, false, Color::Black);
        renderer.fillRect(ix + 11, iy, 4, 10, true);
        renderer.fillRect(ix + ICON - 15, iy, 4, 10, true);
        const int tw = renderer.getTextWidth(UI_10_FONT_ID, "10", EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, ix + (ICON - tw) / 2, iy + 19, "10", true, EpdFontFamily::BOLD);
        snprintf(title, sizeof(title), "%s", tr(STR_TRIVIA_DAILY));
        const bool doneToday = today >= 0 && stats.dailyDay == today && stats.dailyPos >= trivia::ROUND_LENGTH;
        const uint16_t streak = trivia::currentDailyStreak(stats, today);
        if (today < 0) {
          snprintf(line, sizeof(line), "%s", tr(STR_WORDLE_NO_CLOCK));
        } else if (doneToday) {
          snprintf(line, sizeof(line), tr(STR_TRIVIA_DAILY_DONE), dailyScore(), trivia::ROUND_LENGTH, streak);
          drawCheck(renderer, x + w - 34, y + itemH / 2, true);
        } else if (stats.dailyDay == today && stats.dailyPos > 0) {
          snprintf(line, sizeof(line), tr(STR_TRIVIA_DAILY_RESUME), stats.dailyPos, trivia::ROUND_LENGTH);
        } else {
          snprintf(line, sizeof(line), "%s", tr(STR_TRIVIA_DAILY_NEW));
        }
        break;
      }
      case HOME_QUICK: {
        // Round of ten: a ring of dots.
        for (int k = 0; k < 10; k++) {
          const int px = ix + (k % 5) * 9 + 2, py = iy + 8 + (k / 5) * 16;
          if (k < 7) {
            renderer.fillRoundedRect(px, py, 7, 7, 3, Color::Black);
          } else {
            renderer.drawRoundedRect(px, py, 7, 7, 1, 3, true);
          }
        }
        snprintf(title, sizeof(title), "%s", tr(STR_TRIVIA_QUICK));
        filterSummary(line, sizeof(line));
        break;
      }
      case HOME_SUDDEN: {
        // A lightning bolt.
        const int xs[6] = {ix + 26, ix + 10, ix + 22, ix + 16, ix + 34, ix + 22};
        const int ys[6] = {iy + 2, iy + 26, iy + 26, iy + 42, iy + 16, iy + 16};
        renderer.fillPolygon(xs, ys, 6, true);
        snprintf(title, sizeof(title), "%s", tr(STR_TRIVIA_SUDDEN));
        snprintf(line, sizeof(line), tr(STR_TRIVIA_SUDDEN_LINE), stats.bestRun);
        break;
      }
      case HOME_FILTERS: {
        for (int k = 0; k < 3; k++) {
          const int ly = iy + 8 + k * 14;
          renderer.fillRect(ix + 2, ly, ICON - 4, 3, true);
          const int knob = ix + 8 + ((k * 13 + 7) % 28);
          renderer.fillRoundedRect(knob, ly - 4, 10, 11, 5, Color::White);
          renderer.drawRoundedRect(knob, ly - 4, 10, 11, 2, 5, true);
        }
        snprintf(title, sizeof(title), "%s", tr(STR_TRIVIA_FILTERS));
        filterSummary(line, sizeof(line));
        break;
      }
      default: {
        static constexpr int BARS[3] = {18, 30, 42};
        for (int k = 0; k < 3; k++) renderer.fillRect(ix + 4 + k * 14, iy + ICON - BARS[k], 10, BARS[k], true);
        snprintf(title, sizeof(title), "%s", tr(STR_WORDLE_STATS));
        snprintf(line, sizeof(line), tr(STR_TRIVIA_SEEN_LINE), seen.size(), pack.count());
        break;
      }
    }
    if (!pack.isOpen() && i != HOME_STATS) snprintf(line, sizeof(line), "%s", tr(STR_TRIVIA_NO_PACK));
    const int textX = ix + ICON + 24;
    const int textW = x + w - 56 - textX;
    renderer.drawText(UI_12_FONT_ID, textX, y + itemH / 2 - 28, title, true,
                      focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, textX, y + itemH / 2 + 4,
                      renderer.truncatedText(UI_10_FONT_ID, line, textW).c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- question -----------------------------------------------------------------

void TriviaActivity::drawOption(const int option, const int x, const int y, const int w, const int h) const {
  const bool answered = chosen >= 0;
  const bool isCorrect = option == correctOption;
  const bool solid = answered && isCorrect;
  const bool wrongPick = answered && option == chosen && !isCorrect;
  const bool focused = !answered && option == cursor;

  if (solid) {
    renderer.fillRoundedRect(x, y, w, h, 12, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, focused || wrongPick ? 3 : 1, 12, true);
  }
  // Letter badge.
  const int bx = x + 12, by = y + (h - BADGE) / 2;
  if (wrongPick) {
    renderer.drawRoundedRect(bx, by, BADGE, BADGE, 2, BADGE / 2, true);
    hatch(renderer, bx + 3, by + 3, BADGE - 6, BADGE - 6, 5);
  } else if (solid || focused) {
    renderer.fillRoundedRect(bx, by, BADGE, BADGE, BADGE / 2, solid ? Color::White : Color::Black);
  } else {
    renderer.drawRoundedRect(bx, by, BADGE, BADGE, 2, BADGE / 2, true);
  }
  const char letter[2] = {static_cast<char>('A' + option), '\0'};
  const int lw = renderer.getTextWidth(UI_12_FONT_ID, letter, EpdFontFamily::BOLD);
  if (!wrongPick) {
    // White on the focused black badge; black on outlined and solid-card (white) badges.
    renderer.drawText(UI_12_FONT_ID, bx + (BADGE - lw) / 2, by + (BADGE - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                      letter, !focused, EpdFontFamily::BOLD);
  }

  const int textX = bx + BADGE + 14;
  const int markW = answered && (solid || wrongPick) ? 36 : 0;
  const int textW = x + w - 14 - markW - textX;
  const EpdFontFamily::Style style = focused || solid ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  char lines[2][MAX_LINE];
  const int fontId = NOTOSANS_14_FONT_ID;
  const int needed = wrap(renderer, fontId, style, options[option], textW, lines, 2);
  const int shown = std::min(needed, 2);
  if (needed > 2) {
    const std::string cut = renderer.truncatedText(fontId, lines[1], textW - 10, style);
    snprintf(lines[1], MAX_LINE, "%s", cut.c_str());
  }
  const int lh = renderer.getLineHeight(fontId);
  const int top = y + (h - shown * lh) / 2;
  for (int i = 0; i < shown; i++) renderer.drawText(fontId, textX, top + i * lh, lines[i], !solid, style);
  if (solid) drawCheck(renderer, x + w - 30, y + h / 2, false);
  if (wrongPick) drawCross(renderer, x + w - 30, y + h / 2, true);
}

void TriviaActivity::renderGame() const {
  const int width = renderer.getScreenWidth();
  const int bottom = hintsTop(renderer) - 12;
  renderer.clearScreen();

  char subtitle[48];
  const bool answered = chosen >= 0;
  if (mode == Mode::SUDDEN) {
    snprintf(subtitle, sizeof(subtitle), tr(STR_TRIVIA_RUN_STATUS), runLength, stats.bestRun);
  } else {
    const int number = std::min(answered ? roundPos : roundPos + 1, static_cast<int>(trivia::ROUND_LENGTH));
    snprintf(subtitle, sizeof(subtitle), tr(STR_TRIVIA_QUESTION_OF), number, trivia::ROUND_LENGTH);
  }
  drawHeader(modeName(mode), subtitle);

  int y = 52;
  if (mode != Mode::SUDDEN) {
    drawPips(y, roundMask, roundPos, answered ? -1 : roundPos);
    y += 40;
  }
  // Category and difficulty.
  const int difficulty = trivia::metaDifficulty(question.meta);
  drawDifficulty(renderer, SIDE, y + 2, difficulty + 1);
  char meta[64];
  snprintf(meta, sizeof(meta), "%s  ·  %s", pack.categoryName(trivia::metaCategory(question.meta)),
           I18N.get(DIFFICULTY_NAMES[difficulty]));
  renderer.drawText(SMALL_FONT_ID, SIDE + 28, y, meta);
  y += 34;

  // Answers stack up from the bottom; the question takes what is left.
  const int optionH = optionCount > 2 ? 72 : 84;
  const int optionsTop = bottom - optionCount * optionH - (optionCount - 1) * OPTION_GAP;
  const int noteY = optionsTop - 14 - NOTE_H;
  const int textW = width - 2 * SIDE;
  const int room = noteY - 12 - y;
  // The biggest type that fits the question in the space above the answers.
  static constexpr int FONTS[3] = {NOTOSANS_18_FONT_ID, NOTOSANS_16_FONT_ID, NOTOSANS_14_FONT_ID};
  int fontId = NOTOSANS_14_FONT_ID;
  for (const int candidate : FONTS) {
    char probe[1][MAX_LINE];
    const int lines = wrap(renderer, candidate, EpdFontFamily::BOLD, question.text, textW, probe, 1);
    if (lines * renderer.getLineHeight(candidate) <= room && lines <= 5) {
      fontId = candidate;
      break;
    }
  }
  const int maxLines = std::max(1, room / renderer.getLineHeight(fontId));
  drawWrapped(fontId, question.text, SIDE, y, textW, maxLines, true, EpdFontFamily::BOLD);

  for (int i = 0; i < optionCount; i++) {
    drawOption(i, SIDE, optionsTop + i * (optionH + OPTION_GAP), width - 2 * SIDE, optionH);
  }

  if (answered) {
    const bool right = chosen == correctOption;
    char note[64];
    if (mode == Mode::SUDDEN && !right) {
      snprintf(note, sizeof(note), tr(STR_TRIVIA_RUN_OVER), runLength);
    } else if (right && stats.answerStreak >= 3) {
      snprintf(note, sizeof(note), tr(STR_TRIVIA_RIGHT_STREAK), stats.answerStreak);
    } else {
      snprintf(note, sizeof(note), "%s", right ? tr(STR_TRIVIA_RIGHT) : tr(STR_TRIVIA_WRONG));
    }
    drawPill(renderer, note, noteY, right);
  }

  const char* confirm = !answered ? tr(STR_TRIVIA_ANSWER) : roundOver() ? tr(STR_TRIVIA_RESULTS) : tr(STR_TRIVIA_NEXT);
  const auto labels = mappedInput.mapLabels(tr(STR_WORDLE_MENU), confirm, answered ? "" : tr(STR_DIR_UP),
                                            answered ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- result -------------------------------------------------------------------

void TriviaActivity::renderResult() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  drawHeader(modeName(mode), nullptr);

  const int score = mode == Mode::SUDDEN ? runLength : __builtin_popcount(roundMask);
  const char* headline;
  if (mode == Mode::SUDDEN) {
    headline = tr(STR_TRIVIA_RUN_ENDED);
  } else if (score >= 9) {
    headline = tr(STR_TRIVIA_BRILLIANT);
  } else if (score >= 7) {
    headline = tr(STR_TRIVIA_GREAT);
  } else if (score >= 4) {
    headline = tr(STR_TRIVIA_NOT_BAD);
  } else {
    headline = tr(STR_TRIVIA_KEEP_GOING);
  }
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, 60, headline, true, EpdFontFamily::BOLD);

  // Score in a ring.
  constexpr int RING = 150;
  const int rx = (width - RING) / 2, ry = 110;
  renderer.drawRoundedRect(rx, ry, RING, RING, 6, RING / 2, true);
  char text[64];
  if (mode == Mode::SUDDEN) {
    snprintf(text, sizeof(text), "%d", score);
  } else {
    snprintf(text, sizeof(text), "%d/%d", score, trivia::ROUND_LENGTH);
  }
  const int tw = renderer.getTextWidth(NOTOSANS_18_FONT_ID, text, EpdFontFamily::BOLD);
  renderer.drawText(NOTOSANS_18_FONT_ID, (width - tw) / 2, ry + RING / 2 - 30, text, true, EpdFontFamily::BOLD);
  const char* unit = mode == Mode::SUDDEN ? tr(STR_TRIVIA_IN_A_ROW) : tr(STR_TRIVIA_RIGHT_ANSWERS);
  const int uw = renderer.getTextWidth(SMALL_FONT_ID, unit);
  renderer.drawText(SMALL_FONT_ID, (width - uw) / 2, ry + RING / 2 + 12, unit);

  int y = ry + RING + 24;
  if (mode != Mode::SUDDEN) {
    drawPips(y, roundMask, trivia::ROUND_LENGTH, -1);
    y += 44;
  }
  if (newBest) {
    drawPill(renderer, tr(STR_TRIVIA_NEW_BEST), y, true);
    y += NOTE_H + 16;
  }

  const int accuracy =
      stats.answered ? static_cast<int>((stats.correct * 100 + stats.answered / 2) / stats.answered) : 0;
  int values[4];
  StrId labels[4];
  switch (mode) {
    case Mode::DAILY:
      values[0] = trivia::currentDailyStreak(stats, today);
      values[1] = stats.bestDailyStreak;
      values[2] = stats.bestDaily;
      values[3] = accuracy;
      labels[0] = StrId::STR_TRIVIA_DAILY_STREAK;
      labels[1] = StrId::STR_WORDLE_BEST_STREAK;
      labels[2] = StrId::STR_TRIVIA_BEST_DAILY;
      labels[3] = StrId::STR_TRIVIA_ACCURACY;
      break;
    case Mode::QUICK:
      values[0] = stats.rounds;
      values[1] = stats.bestRound;
      values[2] = stats.perfectRounds;
      values[3] = accuracy;
      labels[0] = StrId::STR_TRIVIA_ROUNDS;
      labels[1] = StrId::STR_TRIVIA_BEST_ROUND;
      labels[2] = StrId::STR_TRIVIA_PERFECT;
      labels[3] = StrId::STR_TRIVIA_ACCURACY;
      break;
    case Mode::SUDDEN:
      values[0] = runLength;
      values[1] = stats.bestRun;
      values[2] = stats.bestAnswerStreak;
      values[3] = accuracy;
      labels[0] = StrId::STR_TRIVIA_THIS_RUN;
      labels[1] = StrId::STR_TRIVIA_BEST_RUN;
      labels[2] = StrId::STR_TRIVIA_BEST_STREAK_ANY;
      labels[3] = StrId::STR_TRIVIA_ACCURACY;
      break;
  }
  renderer.drawLine(SIDE, y, width - SIDE, y, 1, true);
  drawStatTiles(y + 18, values, labels);
  if (mode == Mode::DAILY)
    renderer.drawCenteredText(UI_10_FONT_ID, y + 90, tr(STR_TRIVIA_TOMORROW), true, EpdFontFamily::BOLD);

  const char* confirm = mode == Mode::DAILY ? tr(STR_WORDLE_STATS) : tr(STR_TRIVIA_PLAY_AGAIN);
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), confirm, "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

// ---- filters ------------------------------------------------------------------

void TriviaActivity::renderFilters() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  drawHeader(tr(STR_TRIVIA_FILTERS), tr(STR_TRIVIA_QUICK));

  static constexpr StrId ROWS[2] = {StrId::STR_TRIVIA_DIFFICULTY, StrId::STR_TRIVIA_CATEGORY};
  constexpr int ROW_H = 104;
  for (int row = 0; row < 2; row++) {
    const int y = 64 + row * (ROW_H + 16);
    const int x = SIDE, w = width - 2 * SIDE;
    const bool focused = row == filterRow;
    renderer.drawRoundedRect(x, y, w, ROW_H, focused ? 3 : 1, 12, true);
    renderer.drawText(UI_10_FONT_ID, x + 18, y + 14, I18N.get(ROWS[row]));
    const char* value = row == 0                                   ? I18N.get(DIFFICULTY_NAMES[stats.difficulty])
                        : stats.category == trivia::ALL_CATEGORIES ? tr(STR_TRIVIA_ALL_CATEGORIES)
                                                                   : pack.categoryName(stats.category);
    const int vw = renderer.getTextWidth(NOTOSANS_18_FONT_ID, value, EpdFontFamily::BOLD);
    const int vy = y + 44;
    renderer.drawText(NOTOSANS_18_FONT_ID, x + (w - vw) / 2, vy, value, true, EpdFontFamily::BOLD);
    if (focused) {
      // Arrows on both sides of the value.
      const int cy = vy + 16;
      const int lx = x + 22, rx = x + w - 22;
      const int lxs[3] = {lx, lx + 12, lx + 12}, lys[3] = {cy, cy - 12, cy + 12};
      const int rxs[3] = {rx, rx - 12, rx - 12}, rys[3] = {cy, cy - 12, cy + 12};
      renderer.fillPolygon(lxs, lys, 3, true);
      renderer.fillPolygon(rxs, rys, 3, true);
    }
  }

  // How many questions the filters leave, and how many are new.
  int total = 0, fresh = 0;
  for (int i = 0; i < pack.count(); i++) {
    if (!trivia::matches(pack.meta(i), stats.difficulty, stats.category)) continue;
    total++;
    if (!seen.has(i)) fresh++;
  }
  char text[80];
  snprintf(text, sizeof(text), tr(STR_TRIVIA_MATCHING), total, fresh);
  renderer.drawCenteredText(UI_10_FONT_ID, 64 + 2 * (ROW_H + 16) + 8, text);
  renderer.drawCenteredText(SMALL_FONT_ID, 64 + 2 * (ROW_H + 16) + 40, tr(STR_TRIVIA_FILTER_HINT));
  renderer.drawCenteredText(SMALL_FONT_ID, hintsTop(renderer) - 34, tr(STR_TRIVIA_SOURCE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TRIVIA_PLAY), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- stats --------------------------------------------------------------------

void TriviaActivity::renderStats() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  drawHeader(tr(STR_WORDLE_STATS), tr(STR_PLAY_TRIVIA));

  constexpr int HERO_Y = 60;
  renderer.drawRoundedRect(SIDE, HERO_Y, width - 2 * SIDE, 118, 2, 12, true);
  const int accuracy =
      stats.answered ? static_cast<int>((stats.correct * 100 + stats.answered / 2) / stats.answered) : 0;
  char text[64];
  snprintf(text, sizeof(text), "%d%%", accuracy);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, HERO_Y + 22, text, true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), tr(STR_TRIVIA_RIGHT_OF), static_cast<unsigned long>(stats.correct),
           static_cast<unsigned long>(stats.answered));
  renderer.drawCenteredText(UI_12_FONT_ID, HERO_Y + 70, text, true, EpdFontFamily::BOLD);

  const int values[4] = {stats.rounds, stats.bestRound, stats.bestRun, stats.bestAnswerStreak};
  static constexpr StrId LABELS[4] = {StrId::STR_TRIVIA_ROUNDS, StrId::STR_TRIVIA_BEST_ROUND,
                                      StrId::STR_TRIVIA_BEST_RUN, StrId::STR_TRIVIA_BEST_STREAK_ANY};
  drawStatTiles(HERO_Y + 142, values, LABELS);

  // Accuracy by difficulty.
  int y = HERO_Y + 226;
  renderer.drawText(UI_10_FONT_ID, SIDE, y, tr(STR_TRIVIA_BY_DIFFICULTY), true, EpdFontFamily::BOLD);
  y += 32;
  constexpr int ROW_H = 40, LABEL_W = 96;
  for (int d = 0; d < trivia::DIFFICULTIES; d++) {
    const int ry = y + d * ROW_H;
    const uint32_t asked = stats.byDifficulty[d][0], right = stats.byDifficulty[d][1];
    const int pct = asked ? static_cast<int>((right * 100 + asked / 2) / asked) : 0;
    renderer.drawText(UI_10_FONT_ID, SIDE, ry + 5, I18N.get(DIFFICULTY_NAMES[d]), true, EpdFontFamily::BOLD);
    const int span = width - 2 * SIDE - LABEL_W;
    renderer.drawRect(SIDE + LABEL_W, ry + 2, span, ROW_H - 10, 1, true);
    const int fill = span * pct / 100;
    if (fill > 4) renderer.fillRect(SIDE + LABEL_W + 2, ry + 4, fill - 4, ROW_H - 14, true);
    snprintf(text, sizeof(text), "%d%%  (%lu)", pct, static_cast<unsigned long>(asked));
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, text, EpdFontFamily::BOLD);
    const bool inside = fill > tw + 16;
    renderer.drawText(SMALL_FONT_ID, inside ? SIDE + LABEL_W + fill - tw - 8 : SIDE + LABEL_W + fill + 8, ry + 7, text,
                      !inside, EpdFontFamily::BOLD);
  }
  y += trivia::DIFFICULTIES * ROW_H + 16;

  snprintf(text, sizeof(text), tr(STR_TRIVIA_DAILY_LINE), stats.dailyPlayed, trivia::currentDailyStreak(stats, today),
           stats.bestDailyStreak);
  renderer.drawText(UI_10_FONT_ID, SIDE, y, text);
  snprintf(text, sizeof(text), tr(STR_TRIVIA_SEEN_LINE), seen.size(), pack.count());
  renderer.drawText(UI_10_FONT_ID, SIDE, y + 30, text);
  renderer.drawText(SMALL_FONT_ID, SIDE, y + 70, tr(STR_TRIVIA_SOURCE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TriviaActivity::render(RenderLock&& lock) {
  switch (screen) {
    case Screen::HOME:
      renderHome();
      break;
    case Screen::GAME:
      renderGame();
      break;
    case Screen::RESULT:
      renderResult();
      break;
    case Screen::FILTERS:
      renderFilters();
      break;
    case Screen::STATS:
      renderStats();
      break;
  }
  const bool clean = cleanNext || ++framesSinceClean >= play::FRAMES_PER_CLEAN;
  if (clean) framesSinceClean = 0;
  cleanNext = false;
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
