#pragma once

#include <I18n.h>

#include <cstddef>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class GfxRenderer;

class IntervalSelectionActivity final : public Activity, private UiAppHost {
 public:
  explicit IntervalSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* activityName,
                                     StrId titleId, int initialValue, int minValue, int maxValue, int smallStep,
                                     int largeStep, StrId valueFormatId = StrId::STR_NONE_OPT,
                                     bool readerActivity = false, StrId maxBoundaryLabelId = StrId::STR_NONE_OPT);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return readerActivity; }

  // Custom readout and endpoint text (choice lists, clock times) with caller
  // hint lines in place of the numeric format and step hints. Call before the
  // activity starts; the hints are copied.
  using ValueFormatter = void (*)(int value, char* out, size_t size);
  void setFormatter(ValueFormatter valueFormatter, const char* hint1, const char* hint2);

 private:
  // The UiAppHost app hosts the shared slider dialog popup (capsule slider,
  // [-] [+] [Confirm] buttons) drawn over the screen underneath.
  static void intervalScreen(UiScreen& screen, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onOkEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildIntervalScreen(UiScreen& screen);

  StrId titleId;
  StrId valueFormatId;
  StrId maxBoundaryLabelId;
  int value;
  int minValue;
  int maxValue;
  int smallStep;
  int largeStep;
  bool readerActivity;
  ButtonNavigator buttonNavigator;
  ValueFormatter formatter = nullptr;
  char customHints[2][64] = {};

  // Swallow the swipe/tap fallout of a slider drag so its release can't trigger
  // the back gesture and cancel the dialog.
  bool draggingSlider = false;

  void adjustValue(int delta);
  // Absolute value (clamped), from slider drag/tap positions.
  void setValue(int candidate);
  int clampedValue(int candidate) const;
  void formatValue(char* buffer, size_t size, int forValue) const;
  void cancel();
  void confirm();
};
