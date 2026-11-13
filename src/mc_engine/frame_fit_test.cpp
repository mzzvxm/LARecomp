// Check of the picture's frame (frame_fit.h): window x choice -> the view's aspect ratio and rectangle,
// the black bars, the HUD's block, the pointer's mapping, where a source picture is placed in a window
// (the present's fit) and what a live change of the setting or the window changes. A program of its own:
// it needs no game, no renderer and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_frame_fit_test       (CMakeLists.txt)
//   <build dir>/larecomp_frame_fit_test
//   or: clang++ -std=c++23 -Wall -Wextra src/mc_engine/frame_fit_test.cpp -o frame_fit_test
//
// The expected numbers are worked out by hand in the comments and written out here on purpose, so that a
// change to frame_fit.h's rounding or rules fails a check and has to be made here too.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "frame_fit.h"

using namespace mc::frame;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

std::string Text(const Rect& r) {
  return std::to_string(r.w) + "x" + std::to_string(r.h) + "+" + std::to_string(r.x) + "+" +
         std::to_string(r.y);
}

void CheckRect(const Rect& got, const Rect& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %s, want %s\n", what.c_str(), Text(got).c_str(), Text(want).c_str());
  }
}

void CheckNear(double got, double want, double tolerance, const std::string& what) {
  ++g_checks;
  if (!(std::abs(got - want) <= tolerance)) {
    ++g_failed;
    std::printf("FAILED: %s: got %.6f, want %.6f\n", what.c_str(), got, want);
  }
}

// One row of the table: window, choice -> the aspect ratio, the frame and the HUD block.
struct Row {
  uint32_t w, h;
  Choice choice;
  double aspect;
  Rect frame;
  Rect hud;
};

void CheckTable() {
  // 21:9 is 64:27 = 2.37037, 32:9 = 3.55556, 16:9 = 1.77778.
  const Row rows[] = {
      // 1920x1080 (a 16:9 monitor).
      {1920, 1080, Choice::kAuto, kAspect16x9, {0, 0, 1920, 1080}, {0, 0, 1920, 1080}},
      {1920, 1080, Choice::k16x9, kAspect16x9, {0, 0, 1920, 1080}, {0, 0, 1920, 1080}},
      // 21:9: 1920 / 2.37037 = 810 rows, 135 black rows above and below; the HUD block is the middle 16:9
      // of it: 810 * 16 / 9 = 1440 wide, 240 in.
      {1920, 1080, Choice::k21x9, kAspect21x9, {0, 135, 1920, 810}, {240, 135, 1440, 810}},
      // 32:9: 1920 / 3.55556 = 540 rows, 270 above and below; the HUD block 960x540.
      {1920, 1080, Choice::k32x9, kAspect32x9, {0, 270, 1920, 540}, {480, 270, 960, 540}},

      // 2560x1080 (a 21:9 monitor).
      {2560, 1080, Choice::kAuto, kAspect21x9, {0, 0, 2560, 1080}, {320, 0, 1920, 1080}},
      {2560, 1080, Choice::k16x9, kAspect16x9, {320, 0, 1920, 1080}, {320, 0, 1920, 1080}},
      {2560, 1080, Choice::k21x9, kAspect21x9, {0, 0, 2560, 1080}, {320, 0, 1920, 1080}},
      {2560, 1080, Choice::k32x9, kAspect32x9, {0, 180, 2560, 720}, {640, 180, 1280, 720}},

      // 3440x1440 (43:18 = 2.38889, 1 % wider than 21:9: AUTO covers the whole screen).
      {3440, 1440, Choice::kAuto, 3440.0 / 1440.0, {0, 0, 3440, 1440}, {440, 0, 2560, 1440}},
      {3440, 1440, Choice::k16x9, kAspect16x9, {440, 0, 2560, 1440}, {440, 0, 2560, 1440}},
      // 21:9 forced: 1440 * 64 / 27 = 3413.33 -> 3413 wide, 13 and 14 black columns.
      {3440, 1440, Choice::k21x9, kAspect21x9, {13, 0, 3413, 1440}, {13 + 426, 0, 2560, 1440}},
      // 32:9 forced on a window that is not (3400x1440): 3400 / 3.55556 = 956.25 rows, 242 above, 242 below.
      {3400, 1440, Choice::k32x9, kAspect32x9, {0, 242, 3400, 956}, {850, 242, 1700, 956}},

      // 3840x1080 and 5120x1440 (32:9 monitors): AUTO is the whole screen.
      {3840, 1080, Choice::kAuto, kAspect32x9, {0, 0, 3840, 1080}, {960, 0, 1920, 1080}},
      {3840, 1080, Choice::k16x9, kAspect16x9, {960, 0, 1920, 1080}, {960, 0, 1920, 1080}},
      {3840, 1080, Choice::k21x9, kAspect21x9, {640, 0, 2560, 1080}, {960, 0, 1920, 1080}},
      {3840, 1080, Choice::k32x9, kAspect32x9, {0, 0, 3840, 1080}, {960, 0, 1920, 1080}},
      {5120, 1440, Choice::kAuto, kAspect32x9, {0, 0, 5120, 1440}, {1280, 0, 2560, 1440}},

      // Wider than 32:9 (5120x1080 is 4.74, 7680x1080 is 64:9): AUTO stops at 32:9 with bars at the sides.
      {5120, 1080, Choice::kAuto, kAspect32x9, {640, 0, 3840, 1080}, {1600, 0, 1920, 1080}},
      {7680, 1080, Choice::kAuto, kAspect32x9, {1920, 0, 3840, 1080}, {2880, 0, 1920, 1080}},

      // Taller than 16:9 (a 1280x1024 window, 5:4; a portrait one): AUTO keeps 16:9 with bars above and below.
      {1280, 1024, Choice::kAuto, kAspect16x9, {0, 152, 1280, 720}, {0, 152, 1280, 720}},
      {1280, 1024, Choice::k21x9, kAspect21x9, {0, 242, 1280, 540}, {160, 242, 960, 540}},
      // A portrait window (900x1600): 900 / 1.77778 = 506.25 rows.
      {900, 1600, Choice::kAuto, kAspect16x9, {0, 547, 900, 506}, {0, 547, 900, 506}},

      // A window a few pixels off 16:9 is 16:9 (a frame around the window, the rounding of a scale factor).
      {1921, 1080, Choice::kAuto, kAspect16x9, {0, 0, 1920, 1080}, {0, 0, 1920, 1080}},
      {1366, 768, Choice::kAuto, kAspect16x9, {0, 0, 1365, 768}, {0, 0, 1365, 768}},
  };
  for (const Row& r : rows) {
    const std::string name = std::string(ChoiceName(r.choice)) + " in " + std::to_string(r.w) + "x" +
                             std::to_string(r.h);
    const Layout l = LayoutOf(r.choice, r.w, r.h);
    CheckNear(l.aspect, r.aspect, 1e-9, name + ": aspect ratio");
    CheckRect(l.frame, r.frame, name + ": frame");
    if (r.hud.w != 0) {
      CheckRect(l.hud, r.hud, name + ": HUD block");
    }
    // The bars add up to the window and hold nothing of the frame.
    Check(l.bar_left() + l.frame.w + l.bar_right() == r.w, name + ": columns add up");
    Check(l.bar_top() + l.frame.h + l.bar_bottom() == r.h, name + ": rows add up");
    Check(l.wide == (r.aspect > kAspect16x9 * 1.001), name + ": wide");
    Check(l.boxed() == (l.frame != Rect{0, 0, r.w, r.h}), name + ": boxed");
  }
  // The named cases of the brief: bars above and below for a ratio wider than the window, at the sides for
  // a narrower one.
  {
    const Layout a = LayoutOf(Choice::k21x9, 1920, 1080);
    Check(a.bar_top() == 135 && a.bar_bottom() == 135 && a.bar_left() == 0 && a.bar_right() == 0,
          "21:9 on 16:9: bars above and below only");
    const Layout b = LayoutOf(Choice::k16x9, 2560, 1080);
    Check(b.bar_left() == 320 && b.bar_right() == 320 && b.bar_top() == 0 && b.bar_bottom() == 0,
          "16:9 on 21:9: bars at the sides only");
    const Layout c = LayoutOf(Choice::kAuto, 2560, 1080);
    Check(!c.boxed(), "AUTO on a 21:9 window has no bars");
    const Layout d = LayoutOf(Choice::kAuto, 3440, 1440);
    Check(!d.boxed(), "AUTO on a 3440x1440 window has no bars");
  }
  // The HUD block is 16:9 at the frame's height, in the middle of the frame.
  {
    const Layout l = LayoutOf(Choice::k32x9, 3400, 1440);  // frame 3400x956, the block 1700x956
    Check(l.hud.h == l.frame.h && l.hud.w == uint32_t(std::lround(double(l.frame.h) * kAspect16x9)),
          "32:9 in 3400x1440: the HUD block is 16:9 at the frame's height");
    Check(std::abs(double(l.hud.w) / double(l.hud.h) - kAspect16x9) < 0.002, "32:9 in 3400x1440: the HUD block's shape is 16:9");
  }
  // No window: nothing.
  Check(LayoutOf(Choice::kAuto, 0, 0).frame.empty(), "no window: no frame");
  Check(LayoutOf(Choice::kAuto, 0, 0).aspect == kAspect16x9, "no window: 16:9");
  Check(LayoutOf(Choice::k21x9, 0, 1080).frame.empty(), "no width: no frame");
}

// What must hold for every window and every choice.
void CheckInvariants() {
  const uint32_t widths[] = {640, 800, 1024, 1280, 1366, 1600, 1707, 1920, 2048, 2293, 2560, 2561,
                             2880, 3000, 3440, 3840, 4096, 5120, 7680};
  const uint32_t heights[] = {480, 540, 720, 768, 900, 960, 1080, 1200, 1440, 1620, 2160};
  for (uint32_t w : widths) {
    for (uint32_t h : heights) {
      for (int i = 0; i < kChoiceCount; ++i) {
        const Choice choice = Choice(i);
        const Layout l = LayoutOf(choice, w, h);
        const std::string name = std::string(ChoiceName(choice)) + " in " + std::to_string(w) + "x" +
                                 std::to_string(h);
        const Rect& f = l.frame;
        // 16:9 up to 32:9, never anything else.
        Check(l.aspect >= kAspect16x9 - 1e-12 && l.aspect <= kAspect32x9 + 1e-12, name + ": aspect in range");
        // Inside the window, centred to the pixel, touching two opposite edges.
        Check(f.w >= 1 && f.h >= 1 && f.right() <= w && f.bottom() <= h, name + ": inside the window");
        Check(l.bar_left() == f.x && (l.bar_right() == f.x || l.bar_right() == f.x + 1), name + ": centred across");
        Check(l.bar_top() == f.y && (l.bar_bottom() == f.y || l.bar_bottom() == f.y + 1), name + ": centred down");
        Check((f.w == w) != (f.h == h) || (f.w == w && f.h == h), name + ": fills one axis");
        // The frame's shape is the aspect ratio, to the pixel of rounding.
        const double shape = double(f.w) / double(f.h);
        Check(std::abs(shape - l.aspect) <= 1.0 / double(f.h) * l.aspect + 1.0 / double(f.h), name + ": frame shape");
        // The HUD block is inside the frame, centred in it, 16:9 or the frame itself.
        const Rect& hud = l.hud;
        Check(hud.x >= f.x && hud.right() <= f.right() && hud.y == f.y && hud.h == f.h, name + ": HUD inside the frame");
        Check(hud.x - f.x == f.right() - hud.right() || hud.x - f.x + 1 == f.right() - hud.right(),
              name + ": HUD centred in the frame");
        if (!l.wide) {
          Check(hud == f, name + ": HUD block is the frame when 16:9");
        }
        // The UI's share of the frame's width is the HUD block's.
        Check(std::abs(double(hud.w) - double(f.w) * l.ui_fraction) <= 2.0, name + ": UI fraction is the HUD's share");
        // The pointer: the frame's corners map to the frame's corners, its centre to its centre.
        int32_t fx = -1, fy = -1;
        Check(MapToFrame(f, int32_t(f.x), int32_t(f.y), &fx, &fy) && fx == 0 && fy == 0, name + ": pointer, top left");
        Check(MapToFrame(f, int32_t(f.right() - 1), int32_t(f.bottom() - 1), &fx, &fy) &&
                  fx == int32_t(f.w) - 1 && fy == int32_t(f.h) - 1,
              name + ": pointer, bottom right");
      }
    }
  }
}

void CheckChoices() {
  Choice c = Choice::k32x9;
  Check(ParseChoice("auto", &c) && c == Choice::kAuto, "parse auto");
  Check(ParseChoice("16:9", &c) && c == Choice::k16x9, "parse 16:9");
  Check(ParseChoice("21:9", &c) && c == Choice::k21x9, "parse 21:9");
  Check(ParseChoice("32:9", &c) && c == Choice::k32x9, "parse 32:9");
  Check(ParseChoice("  AUTO ", &c) && c == Choice::kAuto, "parse is trimmed and not case sensitive");
  Check(ParseChoice("", &c) && c == Choice::kAuto, "parse empty is auto");
  c = Choice::k21x9;
  Check(!ParseChoice("16:10", &c) && c == Choice::k21x9, "16:10 is no choice (the debug setting's old value): untouched");
  Check(!ParseChoice("4:3", &c) && c == Choice::k21x9, "4:3 is no choice");
  Check(!ParseChoice("21x9", &c) && c == Choice::k21x9, "21x9 is no cvar value (the menu's label only)");
  for (int i = 0; i < kChoiceCount; ++i) {
    Choice back = Choice::kAuto;
    Check(ParseChoice(ChoiceName(Choice(i)), &back) && back == Choice(i), std::string("round trip of ") + ChoiceName(Choice(i)));
  }
  CheckNear(kAspect21x9, 2560.0 / 1080.0, 1e-12, "21:9 is the shape of 2560x1080");
  CheckNear(kAspect32x9, 5120.0 / 1440.0, 1e-12, "32:9 is the shape of 5120x1440");
  CheckNear(3440.0 / 1440.0 / kAspect21x9, 1.008, 0.001, "3440x1440 is 1 % wider than 21:9");
}

// The UI fraction the renderer squeezes the HUD by.
void CheckUiFraction() {
  CheckNear(UiFraction(kAspect16x9), 1.0, 0.0, "16:9: the UI keeps the whole width");
  CheckNear(UiFraction(kAspect16x9 * 1.0005), 1.0, 0.0, "16:9 within the margin: the whole width");
  CheckNear(UiFraction(kAspect21x9), 0.75, 1e-12, "21:9: three quarters");
  CheckNear(UiFraction(3440.0 / 1440.0), 0.744, 0.001, "3440x1440: 0.744");
  CheckNear(UiFraction(kAspect32x9), 0.5, 1e-12, "32:9: half");
}

// The pointer in a frame that leaves bars.
void CheckPointer() {
  const Layout l = LayoutOf(Choice::k21x9, 1920, 1080);  // frame 1920x810 at y = 135
  int32_t fx = 0, fy = 0;
  Check(MapToFrame(l.frame, 960, 540, &fx, &fy) && fx == 960 && fy == 405, "21:9 on 16:9: the window's centre is the frame's centre");
  Check(MapToFrame(l.frame, 0, 135, &fx, &fy) && fx == 0 && fy == 0, "21:9 on 16:9: the frame's top left");
  Check(MapToFrame(l.frame, 1919, 944, &fx, &fy) && fx == 1919 && fy == 809, "21:9 on 16:9: the frame's last pixel");
  Check(!MapToFrame(l.frame, 960, 100, &fx, &fy) && fx == 960 && fy == 0, "21:9 on 16:9: the bar above is outside, held at the top edge");
  Check(!MapToFrame(l.frame, 960, 1000, &fx, &fy) && fx == 960 && fy == 809, "21:9 on 16:9: the bar below is outside, held at the bottom edge");
  Check(!MapToFrame(l.frame, 960, 945, &fx, &fy) && fy == 809, "21:9 on 16:9: the first row of the lower bar is outside");
  Check(!MapToFrame(l.frame, 960, 134, &fx, &fy) && fy == 0, "21:9 on 16:9: the last row of the upper bar is outside");
  Check(!MapToFrame(l.frame, -5, 540, &fx, &fy) && fx == 0, "a pointer left of the window is outside, held at the left edge");

  const Layout p = LayoutOf(Choice::k16x9, 2560, 1080);  // frame 1920x1080 at x = 320
  Check(MapToFrame(p.frame, 1280, 540, &fx, &fy) && fx == 960 && fy == 540, "16:9 on 21:9: the window's centre is the frame's centre");
  Check(!MapToFrame(p.frame, 100, 540, &fx, &fy) && fx == 0 && fy == 540, "16:9 on 21:9: the left bar is outside, held at the left edge");
  Check(!MapToFrame(p.frame, 2500, 540, &fx, &fy) && fx == 1919 && fy == 540, "16:9 on 21:9: the right bar is outside, held at the right edge");
  Check(MapToFrame(p.frame, 320, 0, &fx, &fy) && fx == 0 && fy == 0, "16:9 on 21:9: the frame's top left");
  Check(MapToFrame(p.frame, 2239, 1079, &fx, &fy) && fx == 1919 && fy == 1079, "16:9 on 21:9: the frame's last pixel");
  Check(!MapToFrame(p.frame, 2240, 540, &fx, &fy), "16:9 on 21:9: the first column of the right bar is outside");

  // The map's unprojection takes its aspect ratio from the frame it is given: the same as the cameras'.
  CheckNear(double(l.frame.w) / double(l.frame.h), l.aspect, 0.002, "21:9 frame: the map's aspect ratio is the camera's");
  CheckNear(double(p.frame.w) / double(p.frame.h), p.aspect, 0.002, "16:9 frame: the map's aspect ratio is the camera's");

  // No frame: (0, 0), outside.
  Check(!MapToFrame(Rect{}, 10, 10, &fx, &fy) && fx == 0 && fy == 0, "no frame: outside");
}

// Where the present puts a source picture (the front buffer, 1280x720 times a scale).
void CheckPlacement() {
  // Without a frame (frame aspect 0 or 16:9): as the present always did.
  CheckRect(PlaceSource(1280, 720, 1920, 1080, 0.0, 0.0), Rect{0, 0, 1920, 1080}, "16:9 picture in 16:9 window");
  CheckRect(PlaceSource(1280, 720, 2560, 1080, 0.0, 0.0), Rect{320, 0, 1920, 1080}, "16:9 picture in 21:9 window: pillarbox");
  CheckRect(PlaceSource(1280, 720, 2560, 1080, 0.0, kAspect16x9), Rect{320, 0, 1920, 1080}, "the 16:9 frame is no frame");
  CheckRect(PlaceSource(1920, 1080, 1366, 768, 0.0, 0.0), Rect{0, 0, 1365, 768}, "1080p picture in a 1366x768 window");
  CheckRect(PlaceSource(1280, 720, 1280, 1024, 0.0, 0.0), Rect{0, 152, 1280, 720}, "16:9 picture in a 5:4 window: letterbox");
  CheckRect(PlaceSource(1280, 720, 3440, 1440, 3440.0 / 1440.0, 0.0), Rect{0, 0, 3440, 1440},
            "stretched to the window's shape, no frame: the window");
  // The squeezed 3D picture stretched to 21:9 with no frame (the window is 21:9): the window.
  CheckRect(PlaceSource(1920, 1080, 2560, 1080, kAspect21x9, 0.0), Rect{0, 0, 2560, 1080}, "no frame, stretched to 21:9 in 2560x1080");

  // With a frame (the setting forced a shape): the stretched picture is the frame...
  CheckRect(PlaceSource(1280, 720, 1920, 1080, kAspect21x9, kAspect21x9), Rect{0, 135, 1920, 810}, "21:9 picture in a 16:9 window: letterbox");
  CheckRect(PlaceSource(1920, 1080, 1920, 1080, kAspect21x9, kAspect21x9), Rect{0, 135, 1920, 810}, "the same from a 1080p source");
  CheckRect(PlaceSource(1280, 720, 3840, 1080, kAspect21x9, kAspect21x9), Rect{640, 0, 2560, 1080}, "21:9 picture in a 32:9 window: pillarbox");
  CheckRect(PlaceSource(1280, 720, 3440, 1440, kAspect21x9, kAspect21x9), LayoutOf(Choice::k21x9, 3440, 1440).frame, "21:9 picture in 3440x1440 is the frame");
  CheckRect(PlaceSource(1280, 720, 1921, 1080, kAspect21x9, kAspect21x9), LayoutOf(Choice::k21x9, 1921, 1080).frame, "21:9 picture in an odd window is the frame to the pixel");
  CheckRect(PlaceSource(1280, 720, 1920, 1080, kAspect32x9, kAspect32x9), Rect{0, 270, 1920, 540}, "32:9 picture in a 16:9 window");
  // ...and a picture that is not stretched (a movie, a loading screen: no 3D scene) keeps its own shape,
  // in the middle of the frame, which is where the HUD block is.
  CheckRect(PlaceSource(1280, 720, 1920, 1080, 0.0, kAspect21x9), Rect{240, 135, 1440, 810}, "flat 16:9 picture in the 21:9 frame of a 16:9 window");
  CheckRect(PlaceSource(1280, 720, 1920, 1080, 0.0, kAspect21x9), LayoutOf(Choice::k21x9, 1920, 1080).hud, "the flat picture is where the HUD block is");
  CheckRect(PlaceSource(1280, 720, 3840, 1080, 0.0, kAspect32x9), LayoutOf(Choice::k32x9, 3840, 1080).hud, "the same in a 32:9 frame");
  CheckRect(PlaceSource(1280, 720, 2560, 1080, 0.0, kAspect21x9), Rect{320, 0, 1920, 1080}, "flat picture in AUTO's 21:9 window: as before the setting");
  // Everything is inside the frame, and the frame inside the window.
  for (Choice choice : {Choice::kAuto, Choice::k16x9, Choice::k21x9, Choice::k32x9}) {
    for (uint32_t w : {1280u, 1366u, 1920u, 2560u, 3440u, 3840u, 5120u}) {
      for (uint32_t h : {720u, 768u, 1080u, 1440u}) {
        const Layout l = LayoutOf(choice, w, h);
        for (double stretch : {0.0, l.aspect}) {
          const Rect r = PlaceSource(1280, 720, w, h, stretch, l.aspect);
          const std::string name = std::string(ChoiceName(choice)) + " " + std::to_string(w) + "x" +
                                   std::to_string(h) + (stretch > 0.0 ? " stretched" : " flat");
          Check(r.w >= 1 && r.h >= 1 && r.right() <= w && r.bottom() <= h, name + ": placed inside the window");
          if (l.wide) {
            Check(r.x >= l.frame.x && r.right() <= l.frame.right() && r.y >= l.frame.y && r.bottom() <= l.frame.bottom(),
                  name + ": placed inside the frame");
          }
          if (stretch > 0.0 && l.wide) {
            Check(r == l.frame, name + ": the stretched picture is the frame");
          }
        }
      }
    }
  }
}

// The size of an unscaled copy of the stretched picture (a capture, a screenshot).
void CheckPictureWidth() {
  Check(PictureWidth(720, kAspect16x9) == 1280, "capture width of 720 rows at 16:9");
  Check(PictureWidth(1080, kAspect21x9) == 2560, "capture width of 1080 rows at 21:9");
  Check(PictureWidth(1440, kAspect32x9) == 5120, "capture width of 1440 rows at 32:9");
  Check(PictureWidth(1440, 3440.0 / 1440.0) == 3440, "capture width of 1440 rows at 3440x1440's shape");
}

// The live switch: what a change of the setting or the window touches.
void CheckLiveSwitch() {
  Tracker t;
  // The first window, AUTO at 1920x1080: a new window and a new frame, and the aspect ratio is the game's own.
  uint32_t c = t.Update(Choice::kAuto, 1920, 1080);
  Check((c & kWindowChanged) && (c & kFrameChanged) && !(c & kAspectChanged) && !(c & kChoiceChanged),
        "first window at 16:9: window and frame, no new aspect ratio");
  Check(t.Update(Choice::kAuto, 1920, 1080) == kNothing, "the same again changes nothing");

  // 16:9 -> 21:9 on that window: the setting, the aspect ratio (the cameras) and the frame (the present,
  // the render scale) change; the window does not.
  c = t.Update(Choice::k21x9, 1920, 1080);
  Check(c == (kChoiceChanged | kAspectChanged | kFrameChanged), "AUTO -> 21:9 on 16:9: setting, aspect ratio and frame");
  CheckRect(t.layout().frame, Rect{0, 135, 1920, 810}, "live 21:9: the frame");
  // 21:9 -> 32:9.
  c = t.Update(Choice::k32x9, 1920, 1080);
  Check(c == (kChoiceChanged | kAspectChanged | kFrameChanged), "21:9 -> 32:9: setting, aspect ratio and frame");
  // 32:9 -> 16:9: back to the game's own picture.
  c = t.Update(Choice::k16x9, 1920, 1080);
  Check(c == (kChoiceChanged | kAspectChanged | kFrameChanged), "32:9 -> 16:9: setting, aspect ratio and frame");
  Check(!t.layout().wide && !t.layout().boxed(), "16:9 on 16:9: the game's own picture, no bars");
  // 16:9 -> AUTO on a 16:9 window: the setting only; the picture does not move (no camera rewrite, no new scale).
  c = t.Update(Choice::kAuto, 1920, 1080);
  Check(c == kChoiceChanged, "16:9 -> AUTO on a 16:9 window: only the setting changes");

  // The window is resized under a forced 21:9: the frame follows, the aspect ratio does not move.
  t.Update(Choice::k21x9, 1920, 1080);
  c = t.Update(Choice::k21x9, 2560, 1440);
  Check(c == (kWindowChanged | kFrameChanged), "forced 21:9, window 1920x1080 -> 2560x1440: window and frame, not the aspect ratio");
  CheckRect(t.layout().frame, Rect{0, 180, 2560, 1080}, "forced 21:9 at 2560x1440: the frame");
  // The same resize under AUTO: the aspect ratio moves with the window.
  t.Update(Choice::kAuto, 1920, 1080);
  c = t.Update(Choice::kAuto, 2560, 1080);
  Check(c == (kWindowChanged | kAspectChanged | kFrameChanged), "AUTO, window 1920x1080 -> 2560x1080: window, aspect ratio and frame");
  // AUTO -> 21:9 on a 2560x1080 window: the picture is the same: only the setting (no rewrite, no new scale).
  c = t.Update(Choice::k21x9, 2560, 1080);
  Check(c == kChoiceChanged, "AUTO -> 21:9 on a 2560x1080 window: the picture is the same, only the setting changes");
  // 16:9 on the ultrawide window: pillarbox.
  c = t.Update(Choice::k16x9, 2560, 1080);
  Check(c == (kChoiceChanged | kAspectChanged | kFrameChanged), "21:9 -> 16:9 on 2560x1080: setting, aspect ratio and frame");
  CheckRect(t.layout().frame, Rect{320, 0, 1920, 1080}, "16:9 on 2560x1080: the frame");
  // A window of no size (minimised) does not make a frame; coming back restores it.
  c = t.Update(Choice::k16x9, 0, 0);
  Check(t.layout().frame.empty(), "a window with no size has no frame");
  c = t.Update(Choice::k16x9, 2560, 1080);
  Check(!t.layout().frame.empty() && (c & kFrameChanged), "the window is back: the frame is back");

  // A long sequence of switches ends where a fresh tracker would: the layout depends on the last setting
  // and window only, nothing is left over from the path.
  Tracker walk;
  const Choice order[] = {Choice::k32x9, Choice::kAuto, Choice::k21x9, Choice::k16x9, Choice::k32x9, Choice::k21x9, Choice::kAuto};
  for (Choice choice : order) walk.Update(choice, 3440, 1440);
  Tracker fresh;
  fresh.Update(Choice::kAuto, 3440, 1440);
  Check(walk.layout().frame == fresh.layout().frame && walk.layout().aspect == fresh.layout().aspect,
        "after a walk through every choice AUTO is AUTO again");
}

}  // namespace

int main() {
  CheckChoices();
  CheckTable();
  CheckInvariants();
  CheckUiFraction();
  CheckPointer();
  CheckPlacement();
  CheckPictureWidth();
  CheckLiveSwitch();
  if (g_failed != 0) {
    std::printf("%d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("%d checks passed\n", g_checks);
  return 0;
}
