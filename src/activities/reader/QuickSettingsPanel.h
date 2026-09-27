#pragma once

#include <cstdint>

class GfxRenderer;
class MappedInputManager;
class SdCardFontRegistry;

// In-book "Quick Settings" bottom sheet drawn over the reading page. The page
// above the sheet is the live preview: typography rows edit SETTINGS in place,
// the panel repaints at once, and the reader re-lays the page out once presses
// stop (QuickSettingsPanel::RELAYOUT_DEBOUNCE_MS).
//
// Buttons: side Up/Down move between rows, Left/Right step the focused value
// (or move between the link chips), Confirm opens a link or closes the sheet,
// Back closes. Touch boards tap a row's left/right half or a chip.
class QuickSettingsPanel {
 public:
  enum class Row : uint8_t { FontSize, FontFace, LineSpacing, Margins, Alignment, Hyphenation, Refresh, Count };
  enum class Link : uint8_t { Chapters, Bookmarks, Dictionary, Stats, GoToPercent, More, Count };

  enum class Event : uint8_t {
    None,
    Repaint,   // focus or a non-layout value changed
    Relayout,  // a typography value changed: repaint now, re-lay out the page after the debounce
    Close,
    OpenLink,  // see link()
  };

  static constexpr unsigned long RELAYOUT_DEBOUNCE_MS = 300;
  // Minimum spacing between panel repaints; presses inside the window coalesce.
  static constexpr unsigned long REPAINT_SPACING_MS = 450;

  explicit QuickSettingsPanel(const SdCardFontRegistry* registry) : registry(registry) {}

  Event handleInput(const MappedInputManager& input, const GfxRenderer& renderer);
  Link link() const { return pendingLink; }
  // True once any value was stepped (the caller persists SETTINGS on close).
  bool modified() const { return changed; }

  // Paints the sheet (and its button hints) into the framebuffer. `timeLeft`
  // is the header's reading-time estimate ("" hides it).
  void render(GfxRenderer& renderer, const MappedInputManager& input, const char* timeLeft) const;

 private:
  static constexpr int ROW_COUNT = static_cast<int>(Row::Count);
  static constexpr int LINK_COUNT = static_cast<int>(Link::Count);
  static constexpr int FOCUS_COUNT = ROW_COUNT + LINK_COUNT;

  struct Rect {
    int x, y, w, h;
  };
  struct Layout {
    Rect sheet;
    int headerY;
    int rowsY;
    int rowH;
    int columns;
    int colW;
    int chipsY;
    int chipH;
    int chipCols;
    int chipW;
    int left;
  };

  const SdCardFontRegistry* registry;
  int focus = 0;  // 0..ROW_COUNT-1 = rows, then the link chips
  bool changed = false;
  Link pendingLink = Link::Chapters;

  Layout layout(const GfxRenderer& renderer) const;
  Rect rowRect(const Layout& l, int row) const;
  Rect chipRect(const Layout& l, int chip) const;

  bool onChip() const { return focus >= ROW_COUNT; }
  // Steps the focused row's value; returns the resulting event.
  Event step(int direction);
  void stepFontSize(int direction);
  void stepFontFace(int direction);

  // Position/count of the row's value on its stepper (count 0 = no dots).
  void rowSteps(Row row, int& index, int& count) const;
  void rowValue(Row row, char* buf, int len) const;
  void drawRow(const GfxRenderer& renderer, const Rect& r, Row row, bool focused) const;
  void drawChip(const GfxRenderer& renderer, const Rect& r, Link link, bool focused) const;
  void drawHints(GfxRenderer& renderer, const MappedInputManager& input) const;
};
