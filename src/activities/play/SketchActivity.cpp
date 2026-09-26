#include "SketchActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "MappedInputManager.h"
#include "PlayCommon.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"
#include "util/WallpaperStore.h"

// All parts are original drawings in a 300x500 design space centred on the face.
namespace {
constexpr uint8_t OPTION_COUNT[SketchActivity::PART_COUNT] = {6, 8, 7, 6, 8, 8, 6, 5, 6};
constexpr StrId PART_NAMES[SketchActivity::PART_COUNT] = {
    StrId::STR_SK_FACE,  StrId::STR_SK_EYES, StrId::STR_SK_BROWS,   StrId::STR_SK_NOSE, StrId::STR_SK_MOUTH,
    StrId::STR_SK_HAIR, StrId::STR_SK_HAT,  StrId::STR_SK_GLASSES, StrId::STR_SK_EXTRA};
constexpr int HEADER_HEIGHT = 58;
constexpr int CAPTION_HEIGHT = 44;
constexpr int DESIGN_HEIGHT = 500;
constexpr int DESIGN_CENTER_Y = 30;  // content spans about -220..+240 around the face centre
constexpr unsigned long SAVE_HOLD_MS = 800;
constexpr int MAX_POINTS = 40;
constexpr float TAU = 6.2831853f;

// rx, ry, superellipse exponent x10, forehead width %, chin width %.
struct FaceShape {
  int16_t rx, ry;
  uint8_t n10, top, jaw;
};
constexpr FaceShape FACES[6] = {{100, 130, 20, 100, 82}, {112, 118, 20, 100, 94}, {104, 126, 38, 100, 96},
                                {90, 145, 24, 100, 78},  {112, 128, 22, 108, 60}, {96, 126, 22, 86, 112}};

// A pencil: every stroke is laid down twice, the second pass nudged by a
// seeded jitter so lines look hand-drawn but stay stable between redraws.
class Pen {
 public:
  Pen(const GfxRenderer& renderer, const int cx, const int cy, const int scale, const uint16_t seed)
      : r(renderer), cx(cx), cy(cy), s(scale), seed(seed), width(1 + scale / 200) {}

  int X(const int x) const { return cx + x * s / 256; }
  int Y(const int y) const { return cy + y * s / 256; }

  void stroke(const int* xs, const int* ys, const int n, const bool closed = false) const {
    uint32_t h = seed * 2654435761u ^ static_cast<uint32_t>(xs[0] * 73856093 ^ ys[0] * 19349663 ^ n * 83492791);
    h ^= h >> 15;
    const int jx = static_cast<int>(h % 7) - 3, jy = static_cast<int>((h >> 8) % 7) - 3;
    const int last = closed ? n : n - 1;
    for (int i = 0; i < last; i++) {
      const int j = (i + 1) % n;
      r.drawLine(X(xs[i]), Y(ys[i]), X(xs[j]), Y(ys[j]), width, true);
      r.drawLine(X(xs[i]) + jx, Y(ys[i]) + jy, X(xs[j]) + jx, Y(ys[j]) + jy, 1, true);
    }
  }
  void line(const int x1, const int y1, const int x2, const int y2) const {
    const int xs[2] = {x1, x2}, ys[2] = {y1, y2};
    stroke(xs, ys, 2);
  }
  void curve(const int x0, const int y0, const int qx, const int qy, const int x1, const int y1) const {
    constexpr int N = 12;
    int xs[N + 1], ys[N + 1];
    for (int i = 0; i <= N; i++) {
      const int t = i * 256 / N, u = 256 - t;
      xs[i] = (u * u * x0 + 2 * u * t * qx + t * t * x1) >> 16;
      ys[i] = (u * u * y0 + 2 * u * t * qy + t * t * y1) >> 16;
    }
    stroke(xs, ys, N + 1);
  }
  // Elliptical arc from a0 to a1 degrees (0 = right, 90 = down).
  void arc(const int x, const int y, const int rx, const int ry, const int a0, const int a1) const {
    int xs[MAX_POINTS], ys[MAX_POINTS];
    const int n = arcPoints(x, y, rx, ry, a0, a1, xs, ys);
    stroke(xs, ys, n);
  }
  void ellipse(const int x, const int y, const int rx, const int ry) const { arc(x, y, rx, ry, -10, 360); }
  void fillEllipse(const int x, const int y, const int rx, const int ry, const bool black = true) const {
    int xs[MAX_POINTS], ys[MAX_POINTS];
    const int n = arcPoints(x, y, rx, ry, 0, 350, xs, ys);
    fill(xs, ys, n, black);
  }
  void fill(const int* xs, const int* ys, const int n, const bool black = true) const {
    int px[MAX_POINTS], py[MAX_POINTS];
    for (int i = 0; i < n; i++) px[i] = X(xs[i]), py[i] = Y(ys[i]);
    r.fillPolygon(px, py, n, black);
  }
  void dot(const int x, const int y, const int radius) const {
    const int pr = std::max(1, radius * s / 256);
    r.fillRoundedRect(X(x) - pr, Y(y) - pr, pr * 2, pr * 2, pr, Color::Black);
  }
  static int arcPoints(const int x, const int y, const int rx, const int ry, const int a0, const int a1, int* xs,
                       int* ys) {
    const int n = std::clamp((a1 - a0) / 12 + 1, 2, MAX_POINTS);
    for (int i = 0; i < n; i++) {
      const float a = (a0 + (a1 - a0) * i / (n - 1)) * TAU / 360.0f;
      xs[i] = x + static_cast<int>(rx * cosf(a));
      ys[i] = y + static_cast<int>(ry * sinf(a));
    }
    return n;
  }

 private:
  const GfxRenderer& r;
  int cx, cy, s;
  uint16_t seed;
  int width;
};

// Point on the face outline at `deg` degrees, scaled by pct/100.
void facePoint(const FaceShape& f, const int deg, const int pct, int& x, int& y) {
  const float a = deg * TAU / 360.0f;
  const float c = cosf(a), sn = sinf(a);
  const float e = 20.0f / f.n10;
  float fx = f.rx * std::copysign(powf(std::fabs(c), e), c);
  const float fy = f.ry * std::copysign(powf(std::fabs(sn), e), sn);
  const int taper = sn > 0 ? f.jaw : f.top;
  fx *= 1.0f - (100 - taper) / 100.0f * sn * sn;
  x = static_cast<int>(fx * pct / 100);
  y = static_cast<int>(fy * pct / 100);
}

void drawFace(const Pen& p, const FaceShape& f) {
  const int T = f.ry, R = f.rx;
  // Neck and shoulders first so the face outline sits on top.
  p.line(-34, T * 82 / 100, -38, T + 42);
  p.line(34, T * 82 / 100, 38, T + 42);
  p.curve(-38, T + 42, -70, T + 62, -150, T + 106);
  p.curve(38, T + 42, 70, T + 62, 150, T + 106);
  p.curve(-R + 2, -14, -R - 26, 6, -R + 4, 32);  // ears
  p.curve(R - 2, -14, R + 26, 6, R - 4, 32);
  int xs[32], ys[32];
  for (int i = 0; i < 32; i++) facePoint(f, i * 360 / 32, 100, xs[i], ys[i]);
  p.stroke(xs, ys, 32, true);
}

void drawEyes(const Pen& p, const int option) {
  for (int side = -1; side <= 1; side += 2) {
    const int x = side * 40, y = -8;
    switch (option) {
      case 0:
        p.dot(x, y, 6);
        break;
      case 1:
        p.ellipse(x, y, 17, 11);
        p.dot(x, y, 6);
        break;
      case 2:
        p.arc(x, y - 2, 16, 10, 0, 180);
        p.line(x - 17, y - 2, x + 17, y - 2);
        p.dot(x, y + 3, 5);
        break;
      case 3:
        p.curve(x - 15, y + 4, x, y - 14, x + 15, y + 4);
        break;
      case 4:
        if (side < 0) {
          p.ellipse(x, y, 16, 11);
          p.dot(x, y, 6);
        } else {
          p.curve(x - 15, y, x, y + 8, x + 15, y);
        }
        break;
      case 5:
        p.ellipse(x, y, 16, 10);
        p.dot(x, y, 5);
        for (int k = -1; k <= 1; k++) p.line(x + k * 9, y - 10, x + k * 12, y - 19);
        break;
      case 6:
        p.ellipse(x, y, 20, 20);
        p.fillEllipse(x, y + 2, 11, 12);
        p.fillEllipse(x - 4, y - 3, 3, 3, false);
        break;
      default:
        p.line(x - 16, y, x + 16, y);
        p.dot(x + side * 2, y + 2, 3);
        break;
    }
  }
}

void drawBrows(const Pen& p, const int option) {
  if (option == 6) {
    p.curve(-62, -36, 0, -46, 62, -36);
    return;
  }
  for (int side = -1; side <= 1; side += 2) {
    const int x = side * 40, y = -38;
    switch (option) {
      case 0:
        break;
      case 1:
        p.line(x - 18, y, x + 18, y);
        break;
      case 2:
        p.curve(x - 20, y + 4, x, y - 12, x + 20, y + 4);
        break;
      case 3:
        p.line(x - side * 18, y - 8, x + side * 18, y + 6);
        break;
      case 4:
        p.line(x - side * 18, y + 6, x + side * 18, y - 8);
        break;
      default: {
        const int xs[4] = {x - 20, x + 20, x + 18, x - 20}, ys[4] = {y - 4, y - 6, y + 4, y + 4};
        p.fill(xs, ys, 4);
        break;
      }
    }
  }
}

void drawNose(const Pen& p, const int option) {
  switch (option) {
    case 0: {
      const int xs[3] = {2, -10, 4}, ys[3] = {4, 36, 38};
      p.stroke(xs, ys, 3);
      break;
    }
    case 1:
      p.curve(-12, 30, 0, 44, 12, 30);
      p.dot(-6, 32, 2);
      p.dot(6, 32, 2);
      break;
    case 2:
      p.curve(4, 2, -10, 26, 2, 36);
      break;
    case 3:
      p.arc(0, 30, 14, 11, -30, 210);
      break;
    case 4: {
      const int xs[3] = {0, -13, 12}, ys[3] = {6, 36, 36};
      p.stroke(xs, ys, 3);
      break;
    }
    default:
      p.dot(-6, 32, 3);
      p.dot(6, 32, 3);
      break;
  }
}

void drawMouth(const Pen& p, const int option) {
  switch (option) {
    case 0:
      p.curve(-30, 62, 0, 84, 30, 62);
      break;
    case 1:
      p.curve(-32, 60, 0, 66, 32, 60);
      p.curve(-32, 60, 0, 98, 32, 60);
      p.curve(-28, 66, 0, 74, 28, 66);
      for (int x = -14; x <= 14; x += 14) p.line(x, 66, x, 71);
      break;
    case 2:
      p.line(-22, 68, 22, 68);
      break;
    case 3:
      p.ellipse(0, 70, 10, 13);
      break;
    case 4:
      p.curve(-22, 70, 8, 76, 28, 58);
      break;
    case 5:
      p.curve(-26, 78, 0, 58, 26, 78);
      break;
    case 6:
      p.curve(-30, 62, 0, 84, 30, 62);
      p.arc(6, 74, 10, 12, 0, 180);
      break;
    default:
      p.curve(-12, 66, 0, 74, 12, 66);
      break;
  }
}

void drawHair(const Pen& p, const FaceShape& f, const int option) {
  const int R = f.rx, T = f.ry;
  switch (option) {
    case 0:
      break;
    case 1: {
      int xs[13], ys[13];
      for (int k = 0; k <= 12; k++) {
        xs[k] = -R - 4 + k * (2 * R + 8) / 12;
        ys[k] = (k == 0 || k == 12) ? -T * 30 / 100 : (k % 2 ? -T - 18 - (k % 3) * 6 : -T * 74 / 100);
      }
      p.stroke(xs, ys, 13);
      break;
    }
    case 2:
      p.arc(0, 0, R + 16, T + 18, 160, 380);
      p.curve(-R, -T * 20 / 100, -R * 20 / 100, -T * 80 / 100, R * 95 / 100, -T * 24 / 100);
      for (int k = -2; k <= 2; k++) p.curve(k * 30, -T - 12, k * 36, -T * 70 / 100, k * 44, -T * 44 / 100);
      break;
    case 3:
      for (int a = 190; a <= 350; a += 16) {
        int x = 0, y = 0;
        facePoint(f, a, 104, x, y);
        p.ellipse(x, y, 16, 15);
      }
      break;
    case 4:
      p.arc(0, 0, R + 12, T + 16, 180, 360);
      p.line(-R - 12, 0, -R - 20, T + 54);
      p.line(R + 12, 0, R + 20, T + 54);
      p.line(-R + 4, -T * 30 / 100, -R - 2, T + 44);
      p.line(R - 4, -T * 30 / 100, R + 2, T + 44);
      p.curve(-R + 4, -T * 30 / 100, 0, -T - 4, R - 4, -T * 30 / 100);
      for (int k = -1; k <= 1; k += 2) p.curve(k * 10, -T - 10, k * 50, -T * 70 / 100, k * (R - 10), -T * 30 / 100);
      break;
    case 5: {
      int xs[11], ys[11];
      for (int k = 0; k <= 10; k++) {
        xs[k] = -26 + k * 52 / 10;
        ys[k] = (k == 0 || k == 10) ? -T + 6 : (k % 2 ? -T - 44 + std::abs(k - 5) * 5 : -T - 14);
      }
      p.stroke(xs, ys, 11);
      break;
    }
    case 6:
      p.arc(0, 0, R + 4, T + 6, 200, 340);
      p.ellipse(0, -T - 32, 30, 24);
      p.curve(-14, -T - 34, 0, -T - 52, 14, -T - 30);
      p.curve(-R, -T * 30 / 100, 0, -T * 90 / 100, R, -T * 30 / 100);
      break;
    default:
      p.curve(-R - 4, -T * 10 / 100, -R * 40 / 100, -T * 135 / 100, R * 20 / 100, -T - 8);
      p.curve(R * 20 / 100, -T - 8, R + 10, -T * 95 / 100, R + 4, -T * 10 / 100);
      p.curve(-R * 20 / 100, -T - 4, R * 40 / 100, -T * 60 / 100, R, -T * 36 / 100);
      p.curve(-R * 40 / 100, -T - 2, -R * 10 / 100, -T * 70 / 100, R * 50 / 100, -T * 55 / 100);
      p.curve(-R - 4, -T * 10 / 100, -R + 10, -T * 50 / 100, -R * 40 / 100, -T * 80 / 100);
      break;
  }
}

void drawHat(const Pen& p, const FaceShape& f, const int option) {
  const int R = f.rx, T = f.ry;
  const int base = -T * 55 / 100;
  int xs[MAX_POINTS], ys[MAX_POINTS];
  switch (option) {
    case 0:
      break;
    case 1:
    case 2: {
      const int n = Pen::arcPoints(0, base, R + 10, T * 62 / 100, 180, 360, xs, ys);
      p.fill(xs, ys, n, false);
      p.stroke(xs, ys, n, true);
      if (option == 1) {
        const int bx[4] = {-R - 12, R + 12, R + 12, -R - 12}, by[4] = {base - 6, base - 6, base + 20, base + 20};
        p.fill(bx, by, 4, false);
        p.stroke(bx, by, 4, true);
        for (int x = -R; x <= R; x += 16) p.line(x, base, x, base + 14);
        p.fillEllipse(0, base - T * 62 / 100 - 10, 16, 14, false);
        p.ellipse(0, base - T * 62 / 100 - 10, 16, 14);
      } else {
        const int bx[4] = {-R * 30 / 100, R + 70, R + 56, -R * 20 / 100}, by[4] = {base - 4, base + 6, base + 20, base + 12};
        p.fill(bx, by, 4, false);
        p.stroke(bx, by, 4, true);
        p.dot(0, base - T * 62 / 100, 6);
        p.curve(-R * 40 / 100, base - 2, -10, base - T * 50 / 100, 0, base - T * 62 / 100);
      }
      break;
    }
    case 3: {
      const int bx[4] = {-62, 62, 58, -58}, by[4] = {base + 4, base + 4, -T - 110, -T - 110};
      p.fill(bx, by, 4);
      const int wx[4] = {-61, 61, 61, -61}, wy[4] = {base - 26, base - 26, base - 12, base - 12};
      p.fill(wx, wy, 4, false);
      p.stroke(wx, wy, 4, true);
      p.fillEllipse(0, base + 6, R + 34, 11);
      break;
    }
    case 4: {
      const int n = Pen::arcPoints(-12, base - 18, R + 22, 34, 0, 350, xs, ys);
      p.fill(xs, ys, n, false);
      p.stroke(xs, ys, n, true);
      p.line(-8, base - 52, -2, base - 64);
      p.curve(-R, base - 12, 0, base + 2, R + 6, base - 14);
      break;
    }
    default: {
      int n = 0;
      xs[n] = -R + 6, ys[n++] = base + 10;
      for (int k = 0; k <= 8; k++) {
        xs[n] = -R + 6 + k * (2 * R - 12) / 8;
        ys[n++] = k % 2 ? base - 22 : base - 64;
      }
      xs[n] = R - 6, ys[n++] = base + 10;
      p.fill(xs, ys, n, false);
      p.stroke(xs, ys, n, true);
      for (int k = 0; k <= 8; k += 2) p.dot(-R + 6 + k * (2 * R - 12) / 8, base - 70, 5);
      p.line(-R + 6, base - 4, R - 6, base - 4);
      break;
    }
  }
}

void drawGlasses(const Pen& p, const FaceShape& f, const int option) {
  const int R = f.rx;
  switch (option) {
    case 0:
      return;
    case 1:
      p.ellipse(-40, -8, 25, 23);
      p.ellipse(40, -8, 25, 23);
      break;
    case 2:
      for (int side = -1; side <= 1; side += 2) {
        const int xs[4] = {side * 40 - 27, side * 40 + 27, side * 40 + 25, side * 40 - 25};
        const int ys[4] = {-28, -28, 12, 12};
        p.stroke(xs, ys, 4, true);
      }
      break;
    case 3:
      for (int side = -1; side <= 1; side += 2) {
        const int xs[6] = {side * 40 - 28, side * 40 + 28, side * 40 + 26, side * 40 + 12, side * 40 - 12, side * 40 - 26};
        const int ys[6] = {-26, -26, -4, 12, 12, -4};
        p.fill(xs, ys, 6);
        p.stroke(xs, ys, 6, true);
      }
      break;
    default:
      p.ellipse(40, -8, 24, 24);
      p.ellipse(40, -8, 20, 20);
      p.curve(64, -2, 96, 60, 58, 124);
      return;
  }
  p.curve(-15, -12, 0, -20, 15, -12);
  p.line(-66, -14, -R + 2, -18);
  p.line(66, -14, R - 2, -18);
}

void drawExtra(const Pen& p, const FaceShape& f, const int option, const uint16_t seed) {
  const int R = f.rx;
  switch (option) {
    case 0:
      break;
    case 1:
      for (int a = 20; a <= 160; a += 5) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        facePoint(f, a, 84 + (a * 7 + seed) % 9, x0, y0);
        facePoint(f, a, 108, x1, y1);
        p.line(x0, y0, x1, y1 + 8);
      }
      break;
    case 2: {
      const int xs[12] = {0, -16, -36, -44, -30, -10, 0, 10, 30, 44, 36, 16};
      const int ys[12] = {46, 44, 52, 66, 60, 56, 54, 56, 60, 66, 52, 44};
      p.fill(xs, ys, 12);
      break;
    }
    case 3:
      for (int i = 0; i < 70; i++) {
        uint32_t h = (seed + i) * 2654435761u;
        h ^= h >> 13;
        int x = 0, y = 0;
        facePoint(f, 15 + static_cast<int>(h % 150), 62 + static_cast<int>((h >> 8) % 34), x, y);
        if (y > 40) p.dot(x, y, 1);
      }
      break;
    case 4:
      for (int side = -1; side <= 1; side += 2) {
        p.ellipse(side * (R - 2), 40, 5, 5);
        p.dot(side * (R - 2), 54, 5);
      }
      break;
    default:
      for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k < 6; k++) p.dot(side * (36 + (k * 13) % 26), 20 + (k * 7) % 16, 2);
      }
      break;
  }
}
}  // namespace

void SketchActivity::onEnter() {
  Activity::onEnter();
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  randomize();
}

void SketchActivity::randomize() {
  for (int i = 0; i < PART_COUNT; i++) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    choice[i] = static_cast<uint8_t>(rng % OPTION_COUNT[i]);
  }
  if ((rng >> 20) % 2) choice[HAT] = 0;  // hats and glasses on about half the faces
  if ((rng >> 22) % 2) choice[GLASSES] = 0;
  seed = static_cast<uint16_t>(rng >> 8);
  requestUpdate();
}

void SketchActivity::cycle(const int delta) {
  choice[part] = static_cast<uint8_t>((choice[part] + OPTION_COUNT[part] + delta) % OPTION_COUNT[part]);
  requestUpdate();
}

void SketchActivity::loop() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    finish();
    return;
  }
  RenderLock lock(*this);
  if (mappedInput.wasLongPressed(B::Confirm, SAVE_HOLD_MS)) {
    saveRequested = true;
    requestUpdate();
    return;
  }
  toast = nullptr;
  if (mappedInput.wasReleased(B::Confirm)) randomize();
  if (mappedInput.wasPressed(B::Left)) cycle(-1);
  if (mappedInput.wasPressed(B::Right)) cycle(+1);
  if (mappedInput.wasPressed(B::Up)) {
    part = static_cast<uint8_t>((part + PART_COUNT - 1) % PART_COUNT);
    requestUpdate();
  }
  if (mappedInput.wasPressed(B::Down)) {
    part = static_cast<uint8_t>((part + 1) % PART_COUNT);
    requestUpdate();
  }
}

void SketchActivity::drawCharacter(const int cx, const int cy, const int scale) const {
  const Pen p(renderer, cx, cy, scale, seed);
  const FaceShape& f = FACES[choice[FACE]];
  drawFace(p, f);
  drawExtra(p, f, choice[EXTRA] == 1 ? 1 : 0, seed);  // beard sits under the features
  drawEyes(p, choice[EYES]);
  drawBrows(p, choice[BROWS]);
  drawNose(p, choice[NOSE]);
  drawMouth(p, choice[MOUTH]);
  if (choice[EXTRA] != 1) drawExtra(p, f, choice[EXTRA], seed);
  drawHair(p, f, choice[HAIR]);
  drawHat(p, f, choice[HAT]);
  drawGlasses(p, f, choice[GLASSES]);
}

int SketchActivity::areaTop() const { return HEADER_HEIGHT; }

int SketchActivity::areaBottom() const {
  return renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - CAPTION_HEIGHT;
}

void SketchActivity::drawLabel() const {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HEADER_HEIGHT, false);
  renderer.drawText(UI_12_FONT_ID, 20, 16, tr(STR_PLAY_SKETCH), true, EpdFontFamily::BOLD);
  char text[48];
  snprintf(text, sizeof(text), "%s  %d/%d", I18N.get(PART_NAMES[part]), choice[part] + 1, OPTION_COUNT[part]);
  const int w = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, width - w - 20, 16, text, true, EpdFontFamily::BOLD);
  // One tick per part, the current one filled.
  for (int i = 0; i < PART_COUNT; i++) {
    const int x = width - 20 - (PART_COUNT - i) * 14;
    if (i == part) {
      renderer.fillRect(x, 48, 10, 4, true);
    } else {
      renderer.fillRect(x + 3, 49, 4, 2, true);
    }
  }
}

void SketchActivity::drawChrome() const {
  const int width = renderer.getScreenWidth();
  const int captionY = areaBottom() + 10;
  renderer.fillRect(0, areaBottom(), width, CAPTION_HEIGHT, false);
  renderer.drawCenteredText(UI_10_FONT_ID, captionY, toast ? toast : tr(STR_SK_SAVE_HINT));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SK_RANDOM), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_SK_PREV_PART), tr(STR_SK_NEXT_PART));
}

bool SketchActivity::saveWallpaper() {
  char path[48];
  for (int i = 1; i < 1000; i++) {
    snprintf(path, sizeof(path), "%s/sketch-%03d.bmp", wallpaper::DIR, i);
    if (!Storage.exists(path)) break;
  }
  renderer.clearScreen();
  const int height = renderer.getScreenHeight();
  const int scale = height * 256 / (DESIGN_HEIGHT + 40);
  drawCharacter(renderer.getScreenWidth() / 2, height / 2 - DESIGN_CENTER_Y * scale / 256, scale);
  const bool ok = ScreenshotUtil::saveFramebufferAsBmp(path, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                                       renderer.getDisplayHeight());
  LOG_INF("SKETCH", "%s %s", ok ? "saved" : "failed to save", path);
  return ok;
}

void SketchActivity::render(RenderLock&& lock) {
  bool clean = fullRedraw;
  if (saveRequested) {
    saveRequested = false;
    toast = saveWallpaper() ? tr(STR_SK_SAVED) : tr(STR_SK_SAVE_FAILED);
    clean = true;
  }
  const int width = renderer.getScreenWidth();
  const int top = areaTop(), low = areaBottom();
  if (clean) {
    renderer.clearScreen();
    fullRedraw = false;
  } else {
    renderer.fillRect(0, top, width, low - top, false);
  }
  // Keep the side-hint strips clear of the portrait.
  const auto clip = renderer.getClipRect();
  renderer.setClipRect(36, top, width - 72, low - top);
  const int scale = (low - top) * 256 / DESIGN_HEIGHT;
  drawCharacter(width / 2, (top + low) / 2 - DESIGN_CENTER_Y * scale / 256, scale);
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  drawLabel();
  drawChrome();
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
