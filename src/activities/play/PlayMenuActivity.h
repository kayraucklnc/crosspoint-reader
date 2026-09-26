#pragma once

#include <I18n.h>

#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Play: small e-ink games as a paged list of fixed-height cards. Side Up/Down
// and front Left/Right move the selection (wrapping, flipping pages as
// needed), Confirm opens the game, Back returns Home. Each game runs on top of
// this menu and finishes back to it with the selection kept.
//
// Games are rows of a single table in PlayMenuActivity.cpp: adding one is a
// one-line entry there (plus its #include).
class PlayMenuActivity final : public Activity {
 public:
  using GlyphFn = void (*)(const GfxRenderer& renderer, int x, int y, int size);
  using Factory = std::unique_ptr<Activity> (*)(GfxRenderer& renderer, MappedInputManager& mappedInput);

  struct Entry {
    StrId title;
    StrId description;
    GlyphFn glyph;
    Factory create;
  };

  explicit PlayMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Play", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selected = 0;
  int lastRenderedPage = -1;

  void open();
  void renderCard(const Entry& entry, bool focused, int x, int y, int width, int height) const;
  void renderPageIndicator(int page, int pageCount, int centerX, int y) const;
};
