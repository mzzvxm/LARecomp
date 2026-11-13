#pragma once
//
// The picture's frame: where the game's picture lies in the window, for the ASPECT RATIO setting.
//
// The window is whatever the player made it. The FRAME is the part of it the picture fills: the
// largest rectangle of the chosen aspect ratio, centred, on whole pixels. What is left of the window
// (bars above and below when the chosen ratio is wider than the window's, at the sides when it is narrower)
// is pure black and holds nothing of the game. Everything that depends on the picture's shape follows
// the frame and not the window: the cameras' aspect ratio and the HUD's squeeze (mc_engine/ultrawide.h),
// the present's viewport (gpu/present.h), the render scale and the upscalers' sizes (gpu/render_scale.h:
// "the screen" is the frame's 16:9 area), and the map's mouse (mc_engine/map_mouse.cpp).
//
// This file is the arithmetic and nothing else: it includes nothing of the game or the renderer, so
// that a standalone test (frame_fit_test.cpp) can check the window x choice -> frame, bars, HUD block,
// mouse and live-switch tables.
//
// The choices: Auto is the window's own shape (the way the game was shown before the setting existed: a
// 21:9 monitor's window fills with the picture), kept between 16:9 and 32:9; 16:9, 21:9 and 32:9 are the
// ratios themselves, whatever the window is. 21:9 is 64:27, the shape of a 2560x1080 monitor (a 3440x1440
// one is 43:18, 1 % wider: Auto covers it). Narrower than 16:9 is not a choice: the game's own picture is
// 16:9 and a window that is taller keeps it with bars above and below.
//
// Threading: pure functions, any thread.
//

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace mc::frame {

inline constexpr double kAspect16x9 = 16.0 / 9.0;
inline constexpr double kAspect21x9 = 64.0 / 27.0;
inline constexpr double kAspect32x9 = 32.0 / 9.0;

// A window a few pixels off 16:9 (a frame around it, a scaling factor's rounding) is 16:9: nothing to
// widen, and the picture keeps its place.
inline constexpr double kSnapToBase = 1.005;
// An aspect ratio wider than the game's 16:9 by more than this widens the view (Wide).
inline constexpr double kWideMargin = 1.001;

enum class Choice : uint8_t { kAuto = 0, k16x9, k21x9, k32x9 };
inline constexpr int kChoiceCount = 4;

// The cvar's values.
inline const char* ChoiceName(Choice choice) {
  switch (choice) {
    case Choice::k16x9: return "16:9";
    case Choice::k21x9: return "21:9";
    case Choice::k32x9: return "32:9";
    default: return "auto";
  }
}

// A value as ChoiceName gives it (any case, spaces round it ignored; "" is auto). False for anything else,
// *out untouched.
inline bool ParseChoice(std::string_view text, Choice* out) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  const auto same = [&](const char* name) {
    const std::string_view want(name);
    if (text.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) {
      char c = text[i];
      if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
      if (c != want[i]) return false;
    }
    return true;
  };
  for (int i = 0; i < kChoiceCount; ++i) {
    if (same(ChoiceName(Choice(i)))) {
      *out = Choice(i);
      return true;
    }
  }
  if (text.empty()) {
    *out = Choice::kAuto;
    return true;
  }
  return false;
}

// A rectangle in the window's pixels.
struct Rect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t w = 0;
  uint32_t h = 0;
  bool empty() const { return w == 0 || h == 0; }
  uint32_t right() const { return x + w; }
  uint32_t bottom() const { return y + h; }
  friend bool operator==(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
  }
  friend bool operator!=(const Rect& a, const Rect& b) { return !(a == b); }
};

// The aspect ratio the picture is made and shown at, between 16:9 and 32:9. `window_*` 0: 16:9.
inline double AspectOf(Choice choice, uint32_t window_w, uint32_t window_h) {
  double aspect = kAspect16x9;
  switch (choice) {
    case Choice::kAuto:
      if (window_w != 0 && window_h != 0) {
        aspect = double(window_w) / double(window_h);
      }
      break;
    case Choice::k16x9: aspect = kAspect16x9; break;
    case Choice::k21x9: aspect = kAspect21x9; break;
    case Choice::k32x9: aspect = kAspect32x9; break;
  }
  aspect = std::clamp(aspect, kAspect16x9, kAspect32x9);
  if (aspect < kAspect16x9 * kSnapToBase) {
    aspect = kAspect16x9;
  }
  return aspect;
}

// The largest rectangle of `aspect` in a window, centred, on whole pixels: the whole window when it is
// that shape. Empty for a window with no size.
inline Rect FrameOf(uint32_t window_w, uint32_t window_h, double aspect) {
  Rect r;
  if (window_w == 0 || window_h == 0 || !(aspect > 0.0)) {
    return r;
  }
  const double shown = double(window_h) * aspect;
  const double scale = std::min(double(window_w) / shown, 1.0);
  r.w = std::min(std::max(uint32_t(std::lround(shown * scale)), 1u), window_w);
  r.h = std::min(std::max(uint32_t(std::lround(double(window_h) * scale)), 1u), window_h);
  r.x = (window_w - r.w) / 2;
  r.y = (window_h - r.h) / 2;
  return r;
}

// What the UI keeps of the frame's width: 16:9 over the aspect ratio when the picture is wider than the
// game's own, else 1. (The HUD and every Flash menu are laid out 16:9 and sit in the middle.)
inline double UiFraction(double aspect) {
  return aspect > kAspect16x9 * kWideMargin ? kAspect16x9 / aspect : 1.0;
}

// The frame of a choice in a window, with everything that comes of it.
struct Layout {
  Choice choice = Choice::kAuto;
  uint32_t window_w = 0;
  uint32_t window_h = 0;
  double aspect = kAspect16x9;  // the frame's, which the cameras are built with
  Rect frame;                   // where the picture lies
  Rect hud;                     // the middle 16:9 block of the frame: where the HUD and the menus are
  double ui_fraction = 1.0;     // UiFraction(aspect)
  // The view is wider than the game's 16:9 (the cameras widen and the UI is squeezed into `hud`).
  bool wide = false;
  // The frame is not the whole window: there are bars.
  bool boxed() const {
    return frame.x != 0 || frame.y != 0 || frame.w != window_w || frame.h != window_h;
  }
  uint32_t bar_left() const { return frame.x; }
  uint32_t bar_right() const { return window_w - frame.right(); }
  uint32_t bar_top() const { return frame.y; }
  uint32_t bar_bottom() const { return window_h - frame.bottom(); }
};

inline Layout LayoutOf(Choice choice, uint32_t window_w, uint32_t window_h) {
  Layout l;
  l.choice = choice;
  l.window_w = window_w;
  l.window_h = window_h;
  l.aspect = AspectOf(choice, window_w, window_h);
  l.frame = FrameOf(window_w, window_h, l.aspect);
  l.ui_fraction = UiFraction(l.aspect);
  l.wide = l.aspect > kAspect16x9 * kWideMargin;
  l.hud = l.frame;
  if (l.wide && !l.frame.empty()) {
    const uint32_t hud_w = std::min(uint32_t(std::lround(double(l.frame.h) * kAspect16x9)), l.frame.w);
    l.hud.w = std::max(hud_w, 1u);
    l.hud.x = l.frame.x + (l.frame.w - l.hud.w) / 2;
  }
  return l;
}

// Where a source picture of source_w x source_h lies in a window_w x window_h target (the present's
// fit). The source is shown at its own shape, or, with `stretch_aspect` above 0, stretched sideways to
// that aspect ratio (the 3D picture the game drew squeezed into its 16:9 image). `frame_aspect` is the
// frame's: 0, or the game's own 16:9, is no frame (the picture is placed in the window as it always
// was); wider, the picture is placed in the frame of that aspect ratio instead: a picture stretched
// to the frame's own aspect ratio is the frame, any other (a movie, a loading screen: no 3D scene in
// it) is placed in the frame at its own shape, in the middle. On whole pixels.
inline Rect PlaceSource(uint32_t source_w, uint32_t source_h, uint32_t window_w, uint32_t window_h,
                        double stretch_aspect, double frame_aspect) {
  Rect r;
  if (source_w == 0 || source_h == 0 || window_w == 0 || window_h == 0) {
    return r;
  }
  const auto fit_in = [&](const Rect& area, double aspect) {
    Rect f;
    const double shown = aspect > 0.0 ? double(source_h) * aspect : double(source_w);
    const double scale = std::min(double(area.w) / shown, double(area.h) / double(source_h));
    f.w = std::max(uint32_t(std::lround(shown * scale)), 1u);
    f.h = std::max(uint32_t(std::lround(double(source_h) * scale)), 1u);
    f.x = area.x + (area.w - std::min(f.w, area.w)) / 2;
    f.y = area.y + (area.h - std::min(f.h, area.h)) / 2;
    return f;
  };
  const Rect window{0, 0, window_w, window_h};
  if (!(frame_aspect > kAspect16x9 * kWideMargin)) {
    return fit_in(window, stretch_aspect);
  }
  const Rect frame = FrameOf(window_w, window_h, frame_aspect);
  if (stretch_aspect > 0.0 && std::abs(stretch_aspect - frame_aspect) < 1e-6) {
    return frame;
  }
  return fit_in(frame, stretch_aspect);
}

// A window pixel in the frame's own pixels, for a pointer: false in the bars (the pointer is outside the
// picture), with the point moved to the nearest edge of the frame so that a caller may use it anyway.
// The result is always inside a frame that has an area; (0, 0) for one that has none.
inline bool MapToFrame(const Rect& frame, int32_t x, int32_t y, int32_t* frame_x, int32_t* frame_y) {
  if (frame.empty()) {
    *frame_x = 0;
    *frame_y = 0;
    return false;
  }
  const int64_t fx = int64_t(x) - int64_t(frame.x);
  const int64_t fy = int64_t(y) - int64_t(frame.y);
  const bool inside = fx >= 0 && fy >= 0 && fx < int64_t(frame.w) && fy < int64_t(frame.h);
  *frame_x = int32_t(std::clamp<int64_t>(fx, 0, int64_t(frame.w) - 1));
  *frame_y = int32_t(std::clamp<int64_t>(fy, 0, int64_t(frame.h) - 1));
  return inside;
}

// The width of a source picture of `source_h` rows shown at `aspect` (an unscaled copy of the stretched
// picture: a capture, a screenshot): the picture's own size at that shape.
inline uint32_t PictureWidth(uint32_t source_h, double aspect) {
  return std::max(uint32_t(std::lround(double(source_h) * aspect)), 1u);
}

// What changed between two layouts, as bits: what each part of the game must redo.
enum Change : uint32_t {
  kNothing = 0,
  kChoiceChanged = 1u << 0,  // the setting
  kWindowChanged = 1u << 1,  // the window's size
  kAspectChanged = 1u << 2,  // the cameras' aspect ratio and the UI's squeeze (the constant in guest memory)
  kFrameChanged = 1u << 3,   // the frame's rectangle: the present, the render scale, the mouse
};

inline uint32_t Diff(const Layout& before, const Layout& after) {
  uint32_t change = kNothing;
  if (before.choice != after.choice) change |= kChoiceChanged;
  if (before.window_w != after.window_w || before.window_h != after.window_h) change |= kWindowChanged;
  if (std::abs(before.aspect - after.aspect) > 1e-4) change |= kAspectChanged;
  if (before.frame != after.frame) change |= kFrameChanged;
  return change;
}

// The live switch: the layout in force, and what each new setting or window size changes. The first
// Update says everything changed that differs from an empty window at 16:9.
class Tracker {
 public:
  uint32_t Update(Choice choice, uint32_t window_w, uint32_t window_h) {
    const Layout next = LayoutOf(choice, window_w, window_h);
    const uint32_t change = Diff(layout_, next);
    layout_ = next;
    return change;
  }
  const Layout& layout() const { return layout_; }

 private:
  Layout layout_ = LayoutOf(Choice::kAuto, 0, 0);
};

}  // namespace mc::frame
