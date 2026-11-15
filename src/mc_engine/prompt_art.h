#pragma once
//
// The picture work of the live button prompts (KBM-DESIGN.md phase P4): fitting a replacement glyph into the ink of the glyph it replaces. Pure: no game memory, no files, no
// compressor (keyboard_native.cpp decodes the pictures into a Bitmap, calls FitArt and compresses what comes back); the unit test is prompt_glyphs_test.cpp.
//
// THE RULE (owner, 2026-11-15, after the first P4 run: the PlayStation Create button and the TAB cap filled the whole 64x64 bitmap of the Sidekick phone's accept glyph, spilled past the
// ring they replace, and were nearly invisible on the phone's light bar; only the Xbox original, a thin dark ring with a dark arrow and a clear margin, looked right):
//
//   MEASURE  the glyph being replaced, once, when its art is built: the box of its ink (the pixels whose alpha is above kInkAlpha) and its ink colours AS ENCODED IN ITS BITMAP: the colour of its
//            dark band (the pixels no lighter than the middle of its luma range: a ring, an outline), the colour of its marks (the darkest quarter: an arrow, a letter), the alpha its strokes
//            reach, and the width of its stroke.
//   FIT      every replacement goes into THAT box, keeping its own aspect ratio, centred in it: the original's margin is the margin of the box. Nothing is ever drawn outside the box
//            (the check, CheckFit: the replacement's own ink box lies inside the original's plus kBoxSlack pixel, the slack being the soft edge of a scaled picture).
//   INK      where the original's marks are clearly darker than the surface it sits on (kToneGap; prompt_glyphs.h SlotInfo::backdrop: only the Sidekick phone's light bar), the replacement is
//            LINE ART whose pixels take the ORIGINAL's ink colours and alpha, transparent elsewhere, so that whatever transform the movie applies when it draws the bitmap (the phone darkens it,
//            which is why its light grey disc with a dark rim and a dark arrow shows as a thin dark ring on the bar) darkens the replacement exactly as it darkens the ring: the strokes in the
//            dark band's colour, the symbol or key name in the marks' colour, at the original's stroke weight:
//              PlayStation (Shape::Pictogram)  a ring on the original's box + the picture's silhouette (the Create button) fitted inside it, like the original's ring and arrow
//              Keyboard (Shape::Cap)           the key cap's dark lines, i.e. its outline and its key name, scaled into the box; the outline thickened to the original's stroke weight, the
//                                              name left as drawn (a long name such as SPACE already has a narrow font in its cap picture: it is scaled to fit with the cap)
//            Every other glyph (the balls, tabs and discs on the dark prompt bars) is Look::Solid: the replacement keeps its own tones (a white cap, a white tab, the game's own PlayStation
//            faces), which is what passed; only its footprint is the original's.
//   MEASURE ONCE  the original's Ink is taken when the dictionary (or the Sidekick glyph) is found and kept with the texture; FitArt runs when the art is built (WarmArt), never on a repaint.
//
// Numbers (CheckFit, logged by keyboard_native.cpp as `[prompts] fit ...` and read back by perf\scripts\kbm\kbm_p4_report.py): the replacement's ink box within the original's + kBoxSlack
// pixel; and, for line art, its ink COLOUR (the alpha-weighted mean of its ink pixels) within kColourTolerance levels per channel of the original's dark-band colour and its alpha (the level
// its strokes reach) within kAlphaTolerance of the original's: the replacement is encoded like the original, not judged by a guess about brightness. A Look::Solid one is checked for the
// tonal SIDE of the backdrop (a white key cap against a green ball is brighter by design and the pictures that look right today must not be dulled to match); its numbers are only logged.
//

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace larecomp::prompts {

// ---- numbers of the rule ------------------------------------------------------------------------------------------------------------------------

constexpr int kInkAlpha = 32;                  // an ink pixel: alpha above this (of 255); the same for the original and the replacement
constexpr int kRunFloor = 8;                   // StrokeWidth: a stroke goes on while alpha stays above this (the soft edge of a stroke)
constexpr double kToneGap = 40.0;              // marks this much darker than the surface they sit on make a glyph line art (the phone's arrow 69 on a bar of 155; a dark bar is 24: nothing is 40 darker)
constexpr double kDarkBand = 0.5;              // the dark band: ink pixels no lighter than this fraction of the luma range above the darkest (a ring, an outline)
constexpr double kMarkBand = 0.25;             // the marks: the darkest quarter of that range (an arrow, a letter)
constexpr double kColourTolerance = 20.0;      // CheckFit, line art: levels per channel between the replacement's ink colour and the original's dark band colour
constexpr double kAlphaTolerance = 16.0;       // CheckFit, line art: levels between the alpha the replacement's strokes reach and the original's
constexpr double kSymbolFraction = 0.70;       // a PlayStation symbol inside its ring: this much of the ring's inner width
constexpr int kSuperSample = 4;                // line art is built at 4x and scaled down (soft, even edges)
constexpr int kBoxSlack = 1;                   // CheckFit: pixels of soft edge the replacement may be outside the original's box
constexpr double kMinStroke = 1.2;             // the stroke of a ring made for a thin or unreadable original (pixels)
constexpr double kBandOfStroke = 1.5;          // Shape::Cap: the cap's outline band is this many strokes wide (what is thickened; the name inside is not)

// ---- a picture --------------------------------------------------------------------------------------------------------------------------------------

struct Bitmap {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // R8G8B8A8, straight (not premultiplied) alpha, tightly packed
  bool empty() const { return width == 0 || height == 0 || rgba.size() != size_t(width) * height * 4; }
};

inline Bitmap MakeBitmap(uint32_t w, uint32_t h) {
  Bitmap b;
  b.width = w;
  b.height = h;
  b.rgba.assign(size_t(w) * h * 4, 0);
  return b;
}

inline uint8_t AlphaAt(const Bitmap& b, int x, int y) { return b.rgba[(size_t(y) * b.width + size_t(x)) * 4 + 3]; }

inline double Luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

// ---- the ink of a picture ---------------------------------------------------------------------------------------------------------------------------

struct InkBox {
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // inclusive; x1 < x0 = no ink
  bool valid() const { return x1 >= x0 && y1 >= y0; }
  int width() const { return valid() ? x1 - x0 + 1 : 0; }
  int height() const { return valid() ? y1 - y0 + 1 : 0; }
  bool operator==(const InkBox& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

// `inner` lies inside `outer` grown by `slack` pixels on every side.
inline bool BoxWithin(const InkBox& inner, const InkBox& outer, int slack) {
  return inner.valid() && outer.valid() && inner.x0 >= outer.x0 - slack && inner.y0 >= outer.y0 - slack && inner.x1 <= outer.x1 + slack && inner.y1 <= outer.y1 + slack;
}

// The width of the strokes of a picture of line art, in pixels: over the rows in the middle fifth of the box, the alpha-weighted length of the first run of ink from the box's left edge
// (a ring's left side, a key cap's left border), and of those rows the median. Only pixels no lighter than `max_luma` count (the original's dark band: the rim of a light disc is a stroke, the
// disc inside it is not). `alpha_unit` is the alpha a fully covered pixel has (a stroke of alpha 200 is as wide as one of 255). 0 when there is no such run. A solid picture gives its whole width (the caller clamps).
inline double StrokeWidth(const Bitmap& b, const InkBox& box, int threshold = kInkAlpha, double max_luma = 1e9, double alpha_unit = 255.0) {
  if (b.empty() || !box.valid() || box.x0 < 0 || box.y0 < 0 || box.x1 >= int(b.width) || box.y1 >= int(b.height)) return 0.0;
  const auto pixel = [&](int x, int y) { return &b.rgba[(size_t(y) * b.width + size_t(x)) * 4]; };
  const auto dark = [&](int x, int y) {
    const uint8_t* p = pixel(x, y);
    return Luma(p[0], p[1], p[2]) <= max_luma;
  };
  const int h = box.height();
  const int ya = box.y0 + (h * 2) / 5;
  const int yb = box.y0 + (h * 3) / 5;
  std::vector<double> runs;
  for (int y = ya; y <= yb; ++y) {
    int x = box.x0;
    while (x <= box.x1 && !(AlphaAt(b, x, y) > threshold && dark(x, y))) ++x;
    if (x > box.x1) continue;
    double sum = 0.0;
    while (x <= box.x1 && AlphaAt(b, x, y) > kRunFloor && dark(x, y)) {
      sum += double(AlphaAt(b, x, y)) / alpha_unit;
      ++x;
    }
    runs.push_back(sum);
  }
  if (runs.empty()) return 0.0;
  std::sort(runs.begin(), runs.end());
  return runs[runs.size() / 2];
}

struct Ink {
  InkBox box;
  uint32_t pixels = 0;      // ink pixels
  double solidity = 0.0;    // pixels / box area (a ring is about a third, a disc about four fifths)
  double rgb[3] = {0.0, 0.0, 0.0};       // alpha-weighted mean colour of all the ink pixels
  double luma = 0.0;                     // the luma of that colour (0..255)
  double line_rgb[3] = {0.0, 0.0, 0.0};  // alpha-weighted mean colour of the dark band (kDarkBand): the colour of the strokes, as the bitmap holds it
  double mark_rgb[3] = {0.0, 0.0, 0.0};  // ... of the marks (kMarkBand), the darkest ink: an arrow, a letter
  double mark_luma = 0.0;                // the luma of the marks' colour
  double line_alpha = 0.0;               // the highest alpha of a dark band pixel: the level its strokes reach
  double peak_alpha = 0.0;               // the highest alpha of any ink pixel
  double stroke = 0.0;                   // StrokeWidth of the dark band
  double contrast = 0.0;                 // InkContrast against the slot's backdrop; set by MeasureOriginal (MeasureInk does not know the backdrop)
  bool valid() const { return box.valid(); }
};

inline Ink MeasureInk(const Bitmap& b, int threshold = kInkAlpha) {
  Ink ink;
  if (b.empty()) return ink;
  int x0 = int(b.width), y0 = int(b.height), x1 = -1, y1 = -1;
  double wsum = 0.0, sum[3] = {0.0, 0.0, 0.0};
  double lo = 1e9, hi = -1e9;
  int peak = 0;
  uint32_t n = 0;
  for (int y = 0; y < int(b.height); ++y) {
    for (int x = 0; x < int(b.width); ++x) {
      const uint8_t* p = &b.rgba[(size_t(y) * b.width + size_t(x)) * 4];
      if (p[3] <= threshold) continue;
      x0 = std::min(x0, x);
      y0 = std::min(y0, y);
      x1 = std::max(x1, x);
      y1 = std::max(y1, y);
      const double w = double(p[3]) / 255.0;
      wsum += w;
      for (int c = 0; c < 3; ++c) sum[c] += w * double(p[c]);
      const double l = Luma(p[0], p[1], p[2]);
      lo = std::min(lo, l);
      hi = std::max(hi, l);
      peak = std::max(peak, int(p[3]));
      ++n;
    }
  }
  if (n == 0) return ink;
  const double dark_limit = lo + kDarkBand * (hi - lo) + 1e-6, mark_limit = lo + kMarkBand * (hi - lo) + 1e-6;
  double dsum[3] = {0.0, 0.0, 0.0}, msum[3] = {0.0, 0.0, 0.0}, dw = 0.0, mw = 0.0;
  int dpeak = 0;
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const uint8_t* p = &b.rgba[(size_t(y) * b.width + size_t(x)) * 4];
      if (p[3] <= threshold) continue;
      const double l = Luma(p[0], p[1], p[2]);
      if (l > dark_limit) continue;
      const double w = double(p[3]) / 255.0;
      dw += w;
      dpeak = std::max(dpeak, int(p[3]));
      for (int c = 0; c < 3; ++c) dsum[c] += w * double(p[c]);
      if (l > mark_limit) continue;
      mw += w;
      for (int c = 0; c < 3; ++c) msum[c] += w * double(p[c]);
    }
  }
  ink.box = {x0, y0, x1, y1};
  ink.pixels = n;
  ink.solidity = double(n) / (double(x1 - x0 + 1) * double(y1 - y0 + 1));
  for (int c = 0; c < 3; ++c) {
    ink.rgb[c] = sum[c] / wsum;
    ink.line_rgb[c] = dsum[c] / dw;  // the darkest pixel is in both bands: dw, mw > 0
    ink.mark_rgb[c] = msum[c] / mw;
  }
  ink.luma = Luma(ink.rgb[0], ink.rgb[1], ink.rgb[2]);
  ink.mark_luma = Luma(ink.mark_rgb[0], ink.mark_rgb[1], ink.mark_rgb[2]);
  ink.line_alpha = double(dpeak);
  ink.peak_alpha = double(peak);
  ink.stroke = StrokeWidth(b, ink.box, threshold, dark_limit, std::max(1.0, double(dpeak)));
  return ink;
}

// Mean ink contrast: over the ink pixels, the alpha-weighted mean of |luma - backdrop|.
inline double InkContrast(const Bitmap& b, double backdrop, int threshold = kInkAlpha) {
  if (b.empty()) return 0.0;
  double wsum = 0.0, acc = 0.0;
  for (size_t i = 0; i < size_t(b.width) * b.height; ++i) {
    const uint8_t* p = &b.rgba[i * 4];
    if (p[3] <= threshold) continue;
    const double w = double(p[3]) / 255.0;
    acc += w * std::fabs(Luma(p[0], p[1], p[2]) - backdrop);
    wsum += w;
  }
  return wsum > 0.0 ? acc / wsum : 0.0;
}

// The ink of the glyph a slot shows today, measured once when its art is built: MeasureInk + the contrast against what the slot sits on.
inline Ink MeasureOriginal(const Bitmap& b, double backdrop) {
  Ink ink = MeasureInk(b);
  ink.contrast = InkContrast(b, backdrop);
  return ink;
}

// Marks that stand out dark against the surface (the phone's arrow and rim on its light bar), as opposed to the glyphs of the dark prompt bars, whose marks are never kToneGap darker than a
// bar of 24. Read off the marks, not the mean ink: the phone's bitmap is a light disc (231) with a dark rim (107) and a dark arrow (69), and its mean says "light".
inline bool IsDarkOnLight(const Ink& original, double backdrop) { return original.valid() && original.mark_luma + kToneGap <= backdrop; }

// ---- fitting ----------------------------------------------------------------------------------------------------------------------------------------

struct Size {
  int w = 0, h = 0;
};

// The largest size with src's aspect ratio that fits inside box_w x box_h (each side rounded down, at least 1 pixel, never over the box); 0 x 0 when any side is not positive.
inline Size FitSize(int src_w, int src_h, int box_w, int box_h) {
  if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) return {};
  const double s = std::min(double(box_w) / double(src_w), double(box_h) / double(src_h));
  Size r;
  r.w = std::clamp(int(std::floor(double(src_w) * s + 1e-6)), 1, box_w);
  r.h = std::clamp(int(std::floor(double(src_h) * s + 1e-6)), 1, box_h);
  return r;
}

inline Bitmap Crop(const Bitmap& src, const InkBox& box) {
  if (src.empty() || !box.valid()) return {};
  const int x0 = std::max(box.x0, 0), y0 = std::max(box.y0, 0);
  const int x1 = std::min(box.x1, int(src.width) - 1), y1 = std::min(box.y1, int(src.height) - 1);
  if (x1 < x0 || y1 < y0) return {};
  Bitmap out = MakeBitmap(uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      for (int c = 0; c < 4; ++c) out.rgba[(size_t(y - y0) * out.width + size_t(x - x0)) * 4 + size_t(c)] = src.rgba[(size_t(y) * src.width + size_t(x)) * 4 + size_t(c)];
  return out;
}

// Scales to w x h on premultiplied colour (a transparent pixel's colour does not tint its neighbours): the average of the covered area when the picture gets smaller, bilinear when it
// grows. The colour of a pixel that ends up fully transparent is 0.
inline Bitmap Resample(const Bitmap& src, uint32_t w, uint32_t h) {
  if (src.empty() || w == 0 || h == 0) return {};
  const size_t n = size_t(src.width) * src.height;
  std::vector<double> pm(n * 4);  // r g b premultiplied by alpha / 255, a in 0..255
  for (size_t i = 0; i < n; ++i) {
    const double a = double(src.rgba[i * 4 + 3]) / 255.0;
    for (size_t c = 0; c < 3; ++c) pm[i * 4 + c] = double(src.rgba[i * 4 + c]) * a;
    pm[i * 4 + 3] = double(src.rgba[i * 4 + 3]);
  }
  Bitmap out = MakeBitmap(w, h);
  const bool up = w >= src.width && h >= src.height;
  const double sx = double(src.width) / double(w), sy = double(src.height) / double(h);
  for (uint32_t dy = 0; dy < h; ++dy) {
    for (uint32_t dx = 0; dx < w; ++dx) {
      double p[4] = {0.0, 0.0, 0.0, 0.0};
      if (up) {
        const double u = std::clamp((double(dx) + 0.5) * sx - 0.5, 0.0, double(src.width - 1));
        const double v = std::clamp((double(dy) + 0.5) * sy - 0.5, 0.0, double(src.height - 1));
        const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
        const int x1 = std::min(x0 + 1, int(src.width) - 1), y1 = std::min(y0 + 1, int(src.height) - 1);
        const double fx = u - double(x0), fy = v - double(y0);
        for (size_t c = 0; c < 4; ++c) {
          p[c] = pm[(size_t(y0) * src.width + size_t(x0)) * 4 + c] * (1 - fx) * (1 - fy) + pm[(size_t(y0) * src.width + size_t(x1)) * 4 + c] * fx * (1 - fy) +
                 pm[(size_t(y1) * src.width + size_t(x0)) * 4 + c] * (1 - fx) * fy + pm[(size_t(y1) * src.width + size_t(x1)) * 4 + c] * fx * fy;
        }
      } else {
        const double xa = double(dx) * sx, xb = double(dx + 1) * sx, ya = double(dy) * sy, yb = double(dy + 1) * sy;
        double ws = 0.0;
        const int yend = std::min(int(std::ceil(yb)), int(src.height)), xend = std::min(int(std::ceil(xb)), int(src.width));
        for (int yy = int(std::floor(ya)); yy < yend; ++yy) {
          const double wy = std::min(yb, double(yy + 1)) - std::max(ya, double(yy));
          for (int xx = int(std::floor(xa)); xx < xend; ++xx) {
            const double wgt = (std::min(xb, double(xx + 1)) - std::max(xa, double(xx))) * wy;
            for (size_t c = 0; c < 4; ++c) p[c] += pm[(size_t(yy) * src.width + size_t(xx)) * 4 + c] * wgt;
            ws += wgt;
          }
        }
        if (ws > 0.0)
          for (size_t c = 0; c < 4; ++c) p[c] /= ws;
      }
      uint8_t* o = &out.rgba[(size_t(dy) * w + dx) * 4];
      const double a = std::clamp(p[3], 0.0, 255.0);
      o[3] = uint8_t(std::lround(a));
      if (a > 0.0)
        for (size_t c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(std::lround(p[c] / (a / 255.0)), 0L, 255L));
    }
  }
  return out;
}

// Copies src into dst at (ox, oy), pixels outside dst dropped.
inline void Blit(Bitmap& dst, const Bitmap& src, int ox, int oy) {
  for (int y = 0; y < int(src.height); ++y) {
    for (int x = 0; x < int(src.width); ++x) {
      const int X = ox + x, Y = oy + y;
      if (X < 0 || Y < 0 || X >= int(dst.width) || Y >= int(dst.height)) continue;
      for (size_t c = 0; c < 4; ++c) dst.rgba[(size_t(Y) * dst.width + size_t(X)) * 4 + c] = src.rgba[(size_t(y) * src.width + size_t(x)) * 4 + c];
    }
  }
}

// src over dst at (ox, oy), straight alpha, colour and alpha both (the symbol of a ring has its own colour).
inline void Over(Bitmap& dst, const Bitmap& src, int ox, int oy) {
  for (int y = 0; y < int(src.height); ++y) {
    for (int x = 0; x < int(src.width); ++x) {
      const int X = ox + x, Y = oy + y;
      if (X < 0 || Y < 0 || X >= int(dst.width) || Y >= int(dst.height)) continue;
      const uint8_t* s = &src.rgba[(size_t(y) * src.width + size_t(x)) * 4];
      uint8_t* d = &dst.rgba[(size_t(Y) * dst.width + size_t(X)) * 4];
      const double sa = double(s[3]) / 255.0, da = double(d[3]) / 255.0, oa = sa + da * (1.0 - sa);
      if (oa > 0.0)
        for (size_t c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(std::lround((double(s[c]) * sa + double(d[c]) * da * (1.0 - sa)) / oa), 0L, 255L));
      d[3] = uint8_t(std::clamp(std::lround(255.0 * oa), 0L, 255L));
    }
  }
}

inline void ScaleAlpha(Bitmap& b, double factor) {
  for (size_t i = 0; i < size_t(b.width) * b.height; ++i) b.rgba[i * 4 + 3] = uint8_t(std::clamp(std::lround(double(b.rgba[i * 4 + 3]) * factor), 0L, 255L));
}

// Every pixel's colour = rgb (alpha untouched): line art is one colour, and a transparent pixel next to ink carries it too (a block compressor reads their colours as well).
inline void Tint(Bitmap& b, const double rgb[3]) {
  for (size_t i = 0; i < size_t(b.width) * b.height; ++i)
    for (size_t c = 0; c < 3; ++c) b.rgba[i * 4 + c] = uint8_t(std::clamp(std::lround(rgb[c]), 0L, 255L));
}

// The colour of the fully transparent pixels = rgb: without it a scaled picture's transparent surround is black, and the compressor (and the GPU's filter) pull the edge of the art dark.
inline void BleedTransparent(Bitmap& b, const double rgb[3]) {
  for (size_t i = 0; i < size_t(b.width) * b.height; ++i) {
    if (b.rgba[i * 4 + 3] != 0) continue;
    for (size_t c = 0; c < 3; ++c) b.rgba[i * 4 + c] = uint8_t(std::clamp(std::lround(rgb[c]), 0L, 255L));
  }
}

// The dark lines of an opaque picture (a key cap: black outline and name on white): alpha = picture alpha * darkness, colour 0.
inline Bitmap DarknessMask(const Bitmap& b) {
  Bitmap out = MakeBitmap(b.width, b.height);
  if (b.empty()) return out;
  for (size_t i = 0; i < size_t(b.width) * b.height; ++i) {
    const uint8_t* p = &b.rgba[i * 4];
    out.rgba[i * 4 + 3] = uint8_t(std::clamp(std::lround(double(p[3]) * (255.0 - Luma(p[0], p[1], p[2])) / 255.0), 0L, 255L));
  }
  return out;
}

// Alpha grown by `radius` pixels (the largest alpha under a disc of that radius); colour untouched. radius 0 = a copy.
inline Bitmap DilateAlpha(const Bitmap& b, int radius) {
  if (b.empty() || radius <= 0) return b;
  Bitmap out = b;
  for (int y = 0; y < int(b.height); ++y) {
    for (int x = 0; x < int(b.width); ++x) {
      uint8_t best = AlphaAt(b, x, y);
      for (int dy = -radius; dy <= radius && best < 255; ++dy) {
        const int Y = y + dy;
        if (Y < 0 || Y >= int(b.height)) continue;
        for (int dx = -radius; dx <= radius; ++dx) {
          const int X = x + dx;
          if (X < 0 || X >= int(b.width) || dx * dx + dy * dy > radius * radius + radius) continue;
          best = std::max(best, AlphaAt(b, X, Y));
        }
      }
      out.rgba[(size_t(y) * b.width + size_t(x)) * 4 + 3] = best;
    }
  }
  return out;
}

// The silhouette's pixels that are still inside after `steps` rounds of eroding it by one pixel (4 neighbours; the picture's edge counts as outside): 1 = inside the outline band, 0 = in it
// or outside. One byte per pixel, row by row.
inline std::vector<uint8_t> InnerOfSilhouette(const Bitmap& b, int steps, int threshold = kInkAlpha) {
  const int w = int(b.width), h = int(b.height);
  std::vector<uint8_t> cur(size_t(w) * size_t(h), 0), nxt;
  if (b.empty()) return cur;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) cur[size_t(y) * size_t(w) + size_t(x)] = AlphaAt(b, x, y) > threshold ? 1 : 0;
  for (int s = 0; s < steps; ++s) {
    nxt.assign(cur.size(), 0);
    for (int y = 1; y + 1 < h; ++y)
      for (int x = 1; x + 1 < w; ++x) {
        const size_t i = size_t(y) * size_t(w) + size_t(x);
        nxt[i] = (cur[i] && cur[i - 1] && cur[i + 1] && cur[i - size_t(w)] && cur[i + size_t(w)]) ? 1 : 0;
      }
    cur.swap(nxt);
  }
  return cur;
}

// A ring (alpha only; colour 0) whose outer edge is the ellipse inscribed in `box`, `stroke` pixels wide, 4x4 supersampled.
inline void DrawRing(Bitmap& canvas, const InkBox& box, double stroke) {
  if (canvas.empty() || !box.valid()) return;
  const double cx = double(box.x0 + box.x1 + 1) / 2.0, cy = double(box.y0 + box.y1 + 1) / 2.0;
  const double rx = double(box.width()) / 2.0, ry = double(box.height()) / 2.0;
  const double ix = std::max(rx - stroke, 0.01), iy = std::max(ry - stroke, 0.01);
  for (int y = std::max(0, box.y0 - 1); y < std::min(int(canvas.height), box.y1 + 2); ++y) {
    for (int x = std::max(0, box.x0 - 1); x < std::min(int(canvas.width), box.x1 + 2); ++x) {
      int hit = 0;
      for (int sy = 0; sy < kSuperSample; ++sy) {
        for (int sx = 0; sx < kSuperSample; ++sx) {
          const double px = double(x) + (double(sx) + 0.5) / kSuperSample - cx;
          const double py = double(y) + (double(sy) + 0.5) / kSuperSample - cy;
          const bool outer = (px / rx) * (px / rx) + (py / ry) * (py / ry) <= 1.0;
          const bool inner = (px / ix) * (px / ix) + (py / iy) * (py / iy) <= 1.0;
          if (outer && !inner) ++hit;
        }
      }
      uint8_t& a = canvas.rgba[(size_t(y) * canvas.width + size_t(x)) * 4 + 3];
      a = std::max<uint8_t>(a, uint8_t(std::lround(255.0 * hit / (kSuperSample * kSuperSample))));
    }
  }
}

// ---- the rule -----------------------------------------------------------------------------------------------------------------------------------

enum class Shape : uint8_t { Cap, Pictogram };  // what a source picture is: an opaque key cap with dark lines, or a shape cut out of transparency (the shipped pictograms, the game's own faces)
enum class Look : uint8_t { Solid, Line };      // Solid: the replacement keeps its own tones; Line: dark-on-light line art in the original's ink colour

struct Fitted {
  Bitmap bitmap;  // the canvas: width x height of the slot, empty when the source has no ink
  Look look = Look::Solid;
  double stroke = 0.0;  // Look::Line: the stroke weight used
};

// `src` fitted into the ink box of `original` (the whole canvas when the original has none) on a cw x ch canvas, in the original's tonal scheme against `backdrop`.
inline Fitted FitArt(const Bitmap& src, Shape shape, const Ink& original, double backdrop, uint32_t cw, uint32_t ch) {
  Fitted out;
  const Ink si = MeasureInk(src);
  if (!si.valid() || cw == 0 || ch == 0) return out;
  InkBox target = original.box;
  if (!target.valid()) target = {0, 0, int(cw) - 1, int(ch) - 1};
  target.x0 = std::max(target.x0, 0);
  target.y0 = std::max(target.y0, 0);
  target.x1 = std::min(target.x1, int(cw) - 1);
  target.y1 = std::min(target.y1, int(ch) - 1);
  const int tw = target.width(), th = target.height();
  if (tw <= 0 || th <= 0) return out;
  const Bitmap cropped = Crop(src, si.box);
  Bitmap canvas = MakeBitmap(cw, ch);

  if (!IsDarkOnLight(original, backdrop)) {  // light on dark: the replacement's own tones, in the original's box
    const Size d = FitSize(cropped.width, cropped.height, tw, th);
    Blit(canvas, Resample(cropped, uint32_t(d.w), uint32_t(d.h)), target.x0 + (tw - d.w) / 2, target.y0 + (th - d.h) / 2);
    BleedTransparent(canvas, si.rgb);
    out.bitmap = std::move(canvas);
    return out;
  }

  // marks darker than the surface: line art in the original's own ink colours and alpha, at the original's stroke weight, transparent elsewhere
  out.look = Look::Line;
  const double stroke = std::clamp(original.stroke, kMinStroke, std::max(kMinStroke, double(std::min(tw, th)) / 6.0));
  out.stroke = stroke;
  if (shape == Shape::Pictogram) {
    DrawRing(canvas, target, stroke);
    Tint(canvas, original.line_rgb);  // the ring: the colour of the original's rim
    const int side = std::max(1, int(std::floor(kSymbolFraction * (double(std::min(tw, th)) - 2.0 * stroke) + 1e-6)));
    Bitmap symbol = cropped;  // its silhouette (alpha) is the symbol
    for (size_t i = 0; i < size_t(symbol.width) * symbol.height; ++i) symbol.rgba[i * 4 + 0] = symbol.rgba[i * 4 + 1] = symbol.rgba[i * 4 + 2] = 0;
    const Size d = FitSize(symbol.width, symbol.height, side, side);
    Bitmap placed = Resample(symbol, uint32_t(d.w), uint32_t(d.h));
    Tint(placed, original.mark_rgb);  // the symbol: the colour of the original's arrow
    Over(canvas, placed, target.x0 + (tw - d.w) / 2, target.y0 + (th - d.h) / 2);
  } else {
    const Bitmap mask = DarknessMask(cropped);
    const double source_stroke = StrokeWidth(mask, {0, 0, int(mask.width) - 1, int(mask.height) - 1});
    const Size s0 = FitSize(mask.width, mask.height, tw, th);
    const double add = std::max(0.0, stroke - source_stroke * (double(s0.w) / double(mask.width)));  // what the outline lacks of the ring's weight once scaled
    const Size d = FitSize(mask.width, mask.height, std::max(1, int(std::floor(double(tw) - add + 1e-6))), std::max(1, int(std::floor(double(th) - add + 1e-6))));
    const int pad = int(std::ceil(add / 2.0));
    // the outline band (the silhouette minus what stays inside after `band` erosions) is thickened, the key name inside is not
    const int band = source_stroke > 0.0 ? int(std::ceil(source_stroke * kBandOfStroke)) : 0;
    const std::vector<uint8_t> inside = InnerOfSilhouette(cropped, band);
    Bitmap frame = mask, text = mask;
    for (size_t i = 0; i < inside.size(); ++i) {
      (inside[i] ? frame : text).rgba[i * 4 + 3] = 0;
    }
    const uint32_t bw = uint32_t(d.w + 2 * pad) * kSuperSample, bh = uint32_t(d.h + 2 * pad) * kSuperSample;
    Bitmap frame_ss = MakeBitmap(bw, bh), text_ss = MakeBitmap(bw, bh);
    Blit(frame_ss, Resample(frame, uint32_t(d.w) * kSuperSample, uint32_t(d.h) * kSuperSample), pad * kSuperSample, pad * kSuperSample);
    Blit(text_ss, Resample(text, uint32_t(d.w) * kSuperSample, uint32_t(d.h) * kSuperSample), pad * kSuperSample, pad * kSuperSample);
    frame_ss = DilateAlpha(frame_ss, int(std::lround(add / 2.0 * kSuperSample)));
    Tint(frame_ss, original.line_rgb);  // the outline: the colour of the original's rim
    Tint(text_ss, original.mark_rgb);   // the key name: the colour of the original's arrow
    Over(frame_ss, text_ss, 0, 0);
    const Bitmap lines = Resample(frame_ss, uint32_t(d.w + 2 * pad), uint32_t(d.h + 2 * pad));
    Blit(canvas, lines, target.x0 + (tw - int(lines.width)) / 2, target.y0 + (th - int(lines.height)) / 2);
  }
  if (original.line_alpha < 250.0) ScaleAlpha(canvas, original.line_alpha / 255.0);  // the alpha level the original's strokes reach
  out.bitmap = std::move(canvas);
  return out;
}

// ---- the check ----------------------------------------------------------------------------------------------------------------------------------

struct FitCheck {
  Look look = Look::Solid;
  InkBox original_box, box;
  double original_luma = 0.0, luma = 0.0;
  double original_contrast = 0.0, contrast = 0.0;  // against the backdrop (logged for every surface; judged by nothing)
  double original_rgb[3] = {0.0, 0.0, 0.0};        // the original's dark band colour
  double rgb[3] = {0.0, 0.0, 0.0};                 // the replacement's ink colour (alpha-weighted mean)
  double colour_diff = 0.0;                        // the largest channel difference between the two
  double original_alpha = 0.0, alpha = 0.0;        // the alpha level the strokes reach
  bool box_ok = false;   // the replacement's ink box within the original's + kBoxSlack
  bool tone_ok = false;  // Look::Line: ink colour within kColourTolerance levels of the original's and alpha within kAlphaTolerance; Look::Solid: on the original's side of the backdrop
  bool ok() const { return box_ok && tone_ok; }
  double ratio() const { return original_contrast > 0.0 ? contrast / original_contrast : 0.0; }
};

inline FitCheck CheckFit(const Ink& original, const Bitmap& art, double backdrop) {
  FitCheck c;
  const Ink ink = MeasureInk(art);
  c.look = IsDarkOnLight(original, backdrop) ? Look::Line : Look::Solid;
  c.original_box = original.box;
  c.box = ink.box;
  c.original_luma = original.luma;
  c.luma = ink.luma;
  c.original_contrast = original.contrast > 0.0 ? original.contrast : std::fabs(original.luma - backdrop);
  c.contrast = InkContrast(art, backdrop);
  for (int i = 0; i < 3; ++i) {
    c.original_rgb[i] = original.line_rgb[i];
    c.rgb[i] = ink.rgb[i];
    c.colour_diff = std::max(c.colour_diff, std::fabs(ink.rgb[i] - original.line_rgb[i]));
  }
  c.original_alpha = original.line_alpha;
  c.alpha = ink.peak_alpha;
  c.box_ok = !original.valid() || BoxWithin(ink.box, original.box, kBoxSlack);
  if (!original.valid() || !ink.valid()) {
    c.tone_ok = ink.valid();
  } else if (c.look == Look::Line) {
    c.tone_ok = c.colour_diff <= kColourTolerance && std::fabs(c.alpha - c.original_alpha) <= kAlphaTolerance;
  } else {
    c.tone_ok = (ink.luma > backdrop) == (original.luma > backdrop);
  }
  return c;
}

}  // namespace larecomp::prompts
