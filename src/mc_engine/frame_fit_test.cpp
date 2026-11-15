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

      // Taller than 16:9: AUTO fills the window, whatever its shape, down to a portrait 9:16. The HUD block is
      // the middle 16:9 of the frame: as wide as the frame, width / (16/9) tall.
      // 16:10 (1920x1200, the Steam Deck's 1280x800): 1920 / 1.77778 = 1080 rows, 60 above and below; 720 rows, 40.
      {1920, 1200, Choice::kAuto, kAspect16x10, {0, 0, 1920, 1200}, {0, 60, 1920, 1080}},
      {1920, 1200, Choice::k16x10, kAspect16x10, {0, 0, 1920, 1200}, {0, 60, 1920, 1080}},
      {1280, 800, Choice::kAuto, kAspect16x10, {0, 0, 1280, 800}, {0, 40, 1280, 720}},
      // 16:9 and 21:9 forced on that window: bars above and below.
      {1920, 1200, Choice::k16x9, kAspect16x9, {0, 60, 1920, 1080}, {0, 60, 1920, 1080}},
      {1920, 1200, Choice::k21x9, kAspect21x9, {0, 195, 1920, 810}, {240, 195, 1440, 810}},
      // 4:3 (1440x1080): 1440 / 1.77778 = 810 rows, 135 above and below. 5:4 (1280x1024): 720 rows, 152.
      {1440, 1080, Choice::kAuto, 4.0 / 3.0, {0, 0, 1440, 1080}, {0, 135, 1440, 810}},
      {1280, 1024, Choice::kAuto, 1.25, {0, 0, 1280, 1024}, {0, 152, 1280, 720}},
      {1500, 1000, Choice::kAuto, 1.5, {0, 0, 1500, 1000}, {0, 78, 1500, 844}},
      // A portrait window (900x1600 is 9:16 itself; 1080x1920 too): AUTO fills it; the HUD block is 506 / 608
      // rows tall (900 / 1.77778 = 506.25, 1080 / 1.77778 = 607.5 rounds up).
      {900, 1600, Choice::kAuto, kAspectMin, {0, 0, 900, 1600}, {0, 547, 900, 506}},
      {1080, 1920, Choice::kAuto, kAspectMin, {0, 0, 1080, 1920}, {0, 656, 1080, 608}},
      // Taller than 9:16 (1000x2400 is 0.4167): AUTO stops at 9:16 with bars above and below: 1350 x 2400 scaled
      // to the width, 1000 x 1778.
      {1000, 2400, Choice::kAuto, kAspectMin, {0, 311, 1000, 1778}, {0, 918, 1000, 563}},
      // 16:10 forced on a 16:9 window: 1728 columns (1080 * 1.6), 96 black at each side; the block 1728x972.
      {1920, 1080, Choice::k16x10, kAspect16x10, {96, 0, 1728, 1080}, {96, 54, 1728, 972}},
      {2560, 1080, Choice::k16x10, kAspect16x10, {416, 0, 1728, 1080}, {416, 54, 1728, 972}},
      {3840, 1080, Choice::k16x10, kAspect16x10, {1056, 0, 1728, 1080}, {1056, 54, 1728, 972}},
      // 16:10 forced on a 5:4 window: 1280 x 800.
      {1280, 1024, Choice::k16x10, kAspect16x10, {0, 112, 1280, 800}, {0, 152, 1280, 720}},
      // 21:9 and 16:9 forced on a 5:4 window.
      {1280, 1024, Choice::k21x9, kAspect21x9, {0, 242, 1280, 540}, {160, 242, 960, 540}},
      {1280, 1024, Choice::k16x9, kAspect16x9, {0, 152, 1280, 720}, {0, 152, 1280, 720}},

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
    Check(l.tall == (r.aspect < kAspect16x9 / 1.001), name + ": tall");
    Check(!(l.wide && l.tall), name + ": not both wide and tall");
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
    // A taller window: AUTO has no bars at any shape (the owner's decision of 2026-11-13).
    for (const auto& size : {std::pair<uint32_t, uint32_t>{1920, 1200}, {1280, 800}, {1440, 1080}, {1280, 1024},
                             {1500, 1000}, {1080, 1920}, {900, 1600}}) {
      const Layout t = LayoutOf(Choice::kAuto, size.first, size.second);
      Check(!t.boxed() && t.tall, "AUTO on " + std::to_string(size.first) + "x" + std::to_string(size.second) +
                                      " has no bars and is tall");
    }
    const Layout e = LayoutOf(Choice::k16x9, 1920, 1200);
    Check(e.bar_top() == 60 && e.bar_bottom() == 60 && e.bar_left() == 0 && e.bar_right() == 0,
          "16:9 on 16:10: bars above and below only");
    const Layout f16 = LayoutOf(Choice::k16x10, 1920, 1080);
    Check(f16.bar_left() == 96 && f16.bar_right() == 96 && f16.bar_top() == 0 && f16.bar_bottom() == 0,
          "16:10 on 16:9: bars at the sides only");
  }
  // The HUD block is 16:9 at the frame's height, in the middle of the frame.
  {
    const Layout l = LayoutOf(Choice::k32x9, 3400, 1440);  // frame 3400x956, the block 1700x956
    Check(l.hud.h == l.frame.h && l.hud.w == uint32_t(std::lround(double(l.frame.h) * kAspect16x9)),
          "32:9 in 3400x1440: the HUD block is 16:9 at the frame's height");
    Check(std::abs(double(l.hud.w) / double(l.hud.h) - kAspect16x9) < 0.002, "32:9 in 3400x1440: the HUD block's shape is 16:9");
  }
  // The HUD block of a tall frame is 16:9 at the frame's width, in the middle of it, full width.
  {
    const Layout l = LayoutOf(Choice::kAuto, 1440, 1080);  // 4:3: the block 1440x810
    Check(l.hud.w == l.frame.w && l.hud.x == l.frame.x && l.hud.h == uint32_t(std::lround(double(l.frame.w) / kAspect16x9)),
          "4:3 in 1440x1080: the HUD block is 16:9 at the frame's width");
    Check(std::abs(double(l.hud.w) / double(l.hud.h) - kAspect16x9) < 0.002, "4:3 in 1440x1080: the HUD block's shape is 16:9");
    Check(l.ui_fraction == 1.0 && std::abs(l.ui_height_fraction - 0.75) < 1e-9, "4:3: the UI keeps the whole width and 3/4 of the height");
  }
  // The Xenia renderer cannot show a taller picture: its minimum is 16:9 and a taller window keeps 16:9 with bars.
  {
    const Layout x = LayoutOf(Choice::kAuto, 1920, 1200, kAspect16x9);
    CheckRect(x.frame, Rect{0, 60, 1920, 1080}, "min 16:9: AUTO in 1920x1200 keeps 16:9 with bars above and below");
    Check(!x.tall && !x.wide, "min 16:9: neither tall nor wide");
    const Layout y = LayoutOf(Choice::kAuto, 2560, 1080, kAspect16x9);
    CheckRect(y.frame, Rect{0, 0, 2560, 1080}, "min 16:9: a wide window is still filled");
    const Layout z = LayoutOf(Choice::k16x10, 1920, 1080, kAspect16x9);
    CheckRect(z.frame, Rect{0, 0, 1920, 1080}, "min 16:9: a forced 16:10 is held at 16:9 (a renderer that cannot show a taller picture)");
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
        // 9:16 up to 32:9, never anything else.
        Check(l.aspect >= kAspectMin - 1e-12 && l.aspect <= kAspect32x9 + 1e-12, name + ": aspect in range");
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
        Check(hud.x >= f.x && hud.right() <= f.right() && hud.y >= f.y && hud.bottom() <= f.bottom(), name + ": HUD inside the frame");
        Check(hud.x - f.x == f.right() - hud.right() || hud.x - f.x + 1 == f.right() - hud.right(),
              name + ": HUD centred across");
        Check(hud.y - f.y == f.bottom() - hud.bottom() || hud.y - f.y + 1 == f.bottom() - hud.bottom(),
              name + ": HUD centred down");
        if (l.wide) {
          Check(hud.y == f.y && hud.h == f.h, name + ": the HUD block of a wide frame is its height");
        } else if (l.tall) {
          Check(hud.x == f.x && hud.w == f.w, name + ": the HUD block of a tall frame is its width");
        } else {
          Check(hud == f, name + ": HUD block is the frame when 16:9");
        }
        // The UI's share of the frame's width (wide) or height (tall) is the HUD block's.
        Check(std::abs(double(hud.w) - double(f.w) * l.ui_fraction) <= 2.0, name + ": UI fraction is the HUD's share of the width");
        Check(std::abs(double(hud.h) - double(f.h) * l.ui_height_fraction) <= 2.0, name + ": UI height fraction is the HUD's share of the height");
        Check(l.ui_fraction == 1.0 || l.ui_height_fraction == 1.0, name + ": the UI is squeezed along one axis only");
        // The HUD block is 16:9 to the pixel of rounding.
        Check(std::abs(double(hud.w) / double(hud.h) - kAspect16x9) <= 2.0 / double(hud.h) * kAspect16x9 + 1e-9 ||
                  !(l.wide || l.tall),
              name + ": the HUD block is 16:9");
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
  Check(ParseChoice("16:10", &c) && c == Choice::k16x10, "parse 16:10");
  Check(ParseChoice("21:9", &c) && c == Choice::k21x9, "parse 21:9");
  Check(ParseChoice("32:9", &c) && c == Choice::k32x9, "parse 32:9");
  Check(ParseChoice("  AUTO ", &c) && c == Choice::kAuto, "parse is trimmed and not case sensitive");
  Check(ParseChoice("", &c) && c == Choice::kAuto, "parse empty is auto");
  c = Choice::k21x9;
  Check(!ParseChoice("16x10", &c) && c == Choice::k21x9, "16x10 is no cvar value (the menu's label only): untouched");
  Check(!ParseChoice("4:3", &c) && c == Choice::k21x9, "4:3 is no choice");
  Check(!ParseChoice("21x9", &c) && c == Choice::k21x9, "21x9 is no cvar value (the menu's label only)");
  for (int i = 0; i < kChoiceCount; ++i) {
    Choice back = Choice::kAuto;
    Check(ParseChoice(ChoiceName(Choice(i)), &back) && back == Choice(i), std::string("round trip of ") + ChoiceName(Choice(i)));
  }
  CheckNear(kAspect16x10, 1920.0 / 1200.0, 1e-12, "16:10 is the shape of 1920x1200");
  CheckNear(kAspect16x10, 1280.0 / 800.0, 1e-12, "16:10 is the shape of the Steam Deck's 1280x800");
  CheckNear(kAspectMin, 1080.0 / 1920.0, 1e-12, "the tallest shape is a portrait 1080x1920");
  Check(kChoiceCount == 5, "five choices: auto, 16:9, 16:10, 21:9, 32:9");
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
  // Nothing narrows a UI in a tall picture, and nothing shortens it in a wide one.
  CheckNear(UiFraction(kAspect16x10), 1.0, 0.0, "16:10: the UI keeps the whole width");
  CheckNear(UiFraction(4.0 / 3.0), 1.0, 0.0, "4:3: the UI keeps the whole width");
  CheckNear(UiFraction(kAspectMin), 1.0, 0.0, "9:16: the UI keeps the whole width");
  CheckNear(UiHeightFraction(kAspect16x9), 1.0, 0.0, "16:9: the UI keeps the whole height");
  CheckNear(UiHeightFraction(kAspect16x9 / 1.0005), 1.0, 0.0, "16:9 within the margin: the whole height");
  CheckNear(UiHeightFraction(kAspect16x10), 0.9, 1e-12, "16:10: nine tenths of the height");
  CheckNear(UiHeightFraction(4.0 / 3.0), 0.75, 1e-12, "4:3: three quarters");
  CheckNear(UiHeightFraction(1.25), 0.703125, 1e-12, "5:4: 0.703125");
  CheckNear(UiHeightFraction(1.0), 0.5625, 1e-12, "1:1: 9/16");
  CheckNear(UiHeightFraction(kAspectMin), 0.31640625, 1e-12, "9:16: 81/256");
  CheckNear(UiHeightFraction(kAspect21x9), 1.0, 0.0, "21:9: the UI keeps the whole height");
  CheckNear(UiHeightFraction(kAspect32x9), 1.0, 0.0, "32:9: the UI keeps the whole height");
}

// The camera maths of a taller picture (Vert+): the horizontal field of view stays the game's 16:9 one and the
// vertical one grows. A picture that is not tall (16:9, ultrawide) keeps every camera as the game made it.
void CheckVertPlus() {
  constexpr double kPi = 3.14159265358979323846;
  const auto tan_half = [&](double fov_degrees) { return std::tan(fov_degrees * kPi / 360.0); };
  // The worked values, from an independent evaluation of 2 * atan(tan(fov / 2) * (16/9) / aspect).
  struct Case {
    double fov;
    double aspect;
    double want;
    const char* name;
  };
  const Case cases[] = {
      {60.0, kAspect16x10, 65.360367895, "60 degrees at 16:10"},
      {60.0, 4.0 / 3.0, 75.178178938, "60 degrees at 4:3"},
      {60.0, 1.25, 78.780230862, "60 degrees at 5:4"},
      {60.0, 1.5, 68.765096099, "60 degrees at 3:2"},
      {60.0, 1.0, 91.49284452, "60 degrees at 1:1"},
      {60.0, kAspectMin, 122.551723776, "60 degrees at 9:16"},
      {50.0, kAspect16x10, 54.77912028, "50 degrees at 16:10"},
      {50.0, 4.0 / 3.0, 63.742032133, "50 degrees at 4:3"},
      {50.0, 1.25, 67.104127055, "50 degrees at 5:4"},
      {50.0, kAspectMin, 111.683438418, "50 degrees at 9:16"},
      {30.0, kAspect16x10, 33.158781667, "30 degrees at 16:10"},
      {30.0, 4.0 / 3.0, 39.320120804, "30 degrees at 4:3"},
      {30.0, kAspectMin, 80.519307302, "30 degrees at 9:16"},
  };
  for (const Case& c : cases) {
    CheckNear(VertPlusFov(c.fov, c.aspect), c.want, 1e-6, std::string("Vert+: ") + c.name);
  }
  // Nothing changes at 16:9, within the margin of it, or in an ultrawide picture: the camera is the game's.
  for (double fov : {20.0, 45.0, 60.0, 90.0, 120.0}) {
    CheckNear(VertPlusFov(fov, kAspect16x9), fov, 0.0, "Vert+: 16:9 keeps the camera");
    CheckNear(VertPlusFov(fov, kAspect16x9 / 1.0005), fov, 0.0, "Vert+: 16:9 within the margin keeps the camera");
    CheckNear(VertPlusFov(fov, kAspect16x9 * 1.0005), fov, 0.0, "Vert+: 16:9 (a hair wide) keeps the camera");
    CheckNear(VertPlusFov(fov, kAspect21x9), fov, 0.0, "Vert+: 21:9 keeps the camera (Hor+)");
    CheckNear(VertPlusFov(fov, kAspect32x9), fov, 0.0, "Vert+: 32:9 keeps the camera (Hor+)");
    CheckNear(VertPlusFov(fov, 3440.0 / 1440.0), fov, 0.0, "Vert+: 3440x1440 keeps the camera (Hor+)");
  }
  CheckNear(VertPlusTan(kAspect16x9), 1.0, 0.0, "Vert+: tan scale at 16:9 is 1");
  CheckNear(VertPlusTan(kAspect21x9), 1.0, 0.0, "Vert+: tan scale at 21:9 is 1");
  CheckNear(VertPlusTan(kAspect16x10), kAspect16x9 / kAspect16x10, 1e-12, "Vert+: tan scale at 16:10 is 10/9");
  // Not a field of view: handed back as it came.
  for (double bad : {0.0, -10.0, 179.0, 180.0, 200.0}) {
    CheckNear(VertPlusFov(bad, kAspect16x10), bad, 0.0, "Vert+: a value that is no field of view is untouched");
  }
  // For every tall shape and every field of view the game uses: the horizontal field of view is the 16:9 one,
  // the vertical one grew, and what the projection and the culling see follows.
  const double aspects[] = {kAspectMin, 0.75, 1.0, 1.25, 4.0 / 3.0, 1.5, kAspect16x10, 1.7};
  for (double fov : {15.0, 25.0, 30.0, 45.0, 50.0, 60.0, 75.0, 90.0, 120.0}) {
    for (double aspect : aspects) {
      const std::string name = std::to_string(fov) + " degrees at aspect " + std::to_string(aspect);
      const double out = VertPlusFov(fov, aspect);
      // The horizontal angle: 2 atan(tan(v / 2) * aspect), the 16:9 one of the game's own camera.
      const double h_game = 2.0 * std::atan(tan_half(fov) * kAspect16x9);
      const double h_now = 2.0 * std::atan(tan_half(out) * aspect);
      CheckNear(h_now, h_game, 1e-9, "Vert+: the horizontal field of view is kept at " + name);
      Check(out > fov && out < 180.0, "Vert+: the vertical field of view grows and stays under 180 at " + name);
      // The projection (x scale 1 / (tan(v/2) * aspect), y scale 1 / tan(v/2)): x as the game's 16:9 one,
      // y smaller by aspect over 16:9 (the picture shows 16:9 / aspect times more above and below).
      const double p00_game = 1.0 / (tan_half(fov) * kAspect16x9);
      const double p00_now = 1.0 / (tan_half(out) * aspect);
      const double p11_game = 1.0 / tan_half(fov);
      const double p11_now = 1.0 / tan_half(out);
      CheckNear(p00_now / p00_game, 1.0, 1e-9, "Vert+: the projection's x scale is the game's at " + name);
      CheckNear(p11_now / p11_game, aspect / kAspect16x9, 1e-9,
                "Vert+: the projection's y scale is the game's times aspect / (16/9) at " + name);
      // The culling: the frustum at a distance d is as wide as the game's, and (16/9)/aspect times as high.
      CheckNear(tan_half(out) * aspect / (tan_half(fov) * kAspect16x9), 1.0, 1e-9,
                "Vert+: the frustum is as wide as the game's at " + name);
      CheckNear(tan_half(out) / tan_half(fov), kAspect16x9 / aspect, 1e-9,
                "Vert+: the frustum is taller by 16/9 over aspect at " + name);
    }
  }
  // The picture's own shape: what the view shows is `aspect` wide to 1 high, in the frame's pixels the
  // frame's shape; the same on both axes (a round object is round): the world's tan per pixel agree.
  {
    const Layout l = LayoutOf(Choice::kAuto, 1920, 1200);
    const double out = VertPlusFov(60.0, l.aspect);
    const double world_per_px_x = 2.0 * tan_half(out) * l.aspect / double(l.frame.w);
    const double world_per_px_y = 2.0 * tan_half(out) / double(l.frame.h);
    CheckNear(world_per_px_x / world_per_px_y, 1.0, 1e-9, "1920x1200: square pixels in the picture (a round wheel is round)");
  }
  // A camera whose horizontal field of view is the game's in the window it fills: 60 degrees vertical at 16:9 shows
  // the same width at 1920x1200 and 1920x1080, and more above and below.
  {
    const double v16x9 = 60.0;
    const double v16x10 = VertPlusFov(60.0, kAspect16x10);
    const double wide16x9 = 2.0 * tan_half(v16x9) * kAspect16x9;
    const double wide16x10 = 2.0 * tan_half(v16x10) * kAspect16x10;
    CheckNear(wide16x10, wide16x9, 1e-9, "16:10 shows the width the 16:9 picture does");
    CheckNear(2.0 * tan_half(v16x10) / (2.0 * tan_half(v16x9)), 10.0 / 9.0, 1e-9, "16:10 shows 11 % more height (10/9)");
  }
}

// A pixel through the UI's squeeze along each axis (SqueezeCentreNdc, SqueezeMoveNdc, SqueezeNdc): the image's
// pixel p lands at middle + (p - middle) * k (+ the shift of a HUD piece), whatever viewport the draw has.
void CheckSqueeze() {
  struct View {
    float start;
    float extent;
    const char* name;
  };
  const View views[] = {{0.0f, 720.0f, "the whole screen"}, {100.0f, 400.0f, "a part of it"}, {-50.0f, 1024.0f, "a larger one"}};
  for (bool vertical : {false, true}) {
    const float screen = vertical ? 720.0f : 1280.0f;
    for (const View& v : views) {
      for (float k : {1.0f, 0.9f, 0.75f, 0.5f}) {
        for (float shift : {0.0f, 0.05f, -0.05f, 0.1f}) {
          const std::string name = std::string(vertical ? "vertical" : "horizontal") + " squeeze " + std::to_string(k) +
                                   " shift " + std::to_string(shift) + " in " + v.name;
          // The draw's NDC of a pixel along the axis, as the host viewport makes it.
          const auto ndc_of = [&](float pixel) {
            const float t = 2.0f * (pixel - v.start) / v.extent - 1.0f;
            return vertical ? -t : t;
          };
          const auto pixel_of = [&](float ndc) {
            const float t = vertical ? -ndc : ndc;
            return v.start + (t + 1.0f) * 0.5f * v.extent;
          };
          const float centre = SqueezeCentreNdc(screen, v.start, v.extent, vertical);
          const float move = SqueezeMoveNdc(shift, screen, v.extent, vertical);
          CheckNear(centre, ndc_of(0.5f * screen), 1e-4, name + ": the centre's NDC");
          for (float pixel : {0.0f, 100.0f, 0.5f * screen, screen - 1.0f}) {
            const float moved = pixel_of(SqueezeNdc(ndc_of(pixel), k, centre, move));
            const float want = 0.5f * screen + (pixel - 0.5f * screen) * k + shift * screen;
            CheckNear(moved, want, 5e-3, name + ": pixel " + std::to_string(pixel));
          }
        }
      }
    }
  }
  // The squeeze of a tall picture's UI: the UI's rows 0..719 land in the middle UiHeightFraction of the image,
  // which the present stretches over the frame: that block is the HUD block's rows, and its pixels are square.
  for (const auto& size : {std::pair<uint32_t, uint32_t>{1920, 1200}, {1440, 1080}, {1280, 800}, {1280, 1024}, {1080, 1920}}) {
    const Layout l = LayoutOf(Choice::kAuto, size.first, size.second);
    const std::string where = std::to_string(size.first) + "x" + std::to_string(size.second);
    const double k = l.ui_height_fraction;
    const double top = 360.0 + (0.0 - 360.0) * k;       // the image's row of the UI's first row
    const double bottom = 360.0 + (720.0 - 360.0) * k;  // and of its last
    const double frame_row_top = top / 720.0 * double(l.frame.h) + l.frame.y;
    const double frame_row_bottom = bottom / 720.0 * double(l.frame.h) + l.frame.y;
    CheckNear(frame_row_top, double(l.hud.y), 1.0, "the squeezed UI's top is the HUD block's top in " + where);
    CheckNear(frame_row_bottom, double(l.hud.bottom()), 1.0, "the squeezed UI's bottom is the HUD block's bottom in " + where);
    // 16:9 shape in the frame's pixels: the UI's 1280 columns over the frame's width, its 720 rows over the block's height.
    CheckNear((double(l.frame.w) / 1280.0) / (double(l.hud.h) / 720.0), 1.0, 0.003, "the squeezed UI's pixels are square in " + where);
  }
  for (const auto& size : {std::pair<uint32_t, uint32_t>{2560, 1080}, {3440, 1440}, {3840, 1080}}) {
    const Layout l = LayoutOf(Choice::kAuto, size.first, size.second);
    const std::string where = std::to_string(size.first) + "x" + std::to_string(size.second);
    const double k = l.ui_fraction;
    const double left = 640.0 + (0.0 - 640.0) * k;
    const double right = 640.0 + (1280.0 - 640.0) * k;
    CheckNear(left / 1280.0 * double(l.frame.w) + l.frame.x, double(l.hud.x), 1.0, "the squeezed UI's left is the HUD block's left in " + where);
    CheckNear(right / 1280.0 * double(l.frame.w) + l.frame.x, double(l.hud.right()), 1.0, "the squeezed UI's right is the HUD block's right in " + where);
  }
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

  // A tall frame: 16:10 on a 16:9 window (frame 1728x1080 at x = 96).
  const Layout t = LayoutOf(Choice::k16x10, 1920, 1080);
  Check(MapToFrame(t.frame, 960, 540, &fx, &fy) && fx == 864 && fy == 540, "16:10 on 16:9: the window's centre is the frame's centre");
  Check(!MapToFrame(t.frame, 50, 540, &fx, &fy) && fx == 0 && fy == 540, "16:10 on 16:9: the left bar is outside, held at the left edge");
  Check(!MapToFrame(t.frame, 1900, 540, &fx, &fy) && fx == 1727 && fy == 540, "16:10 on 16:9: the right bar is outside, held at the right edge");
  CheckNear(double(t.frame.w) / double(t.frame.h), t.aspect, 0.002, "16:10 frame: the map's aspect ratio is the camera's");
  // The pointer in the frame's own pixels is the screen's: the middle of a 1920x1200 AUTO frame is the middle.
  const Layout u = LayoutOf(Choice::kAuto, 1920, 1200);
  Check(MapToFrame(u.frame, 960, 600, &fx, &fy) && fx == 960 && fy == 600, "AUTO on 1920x1200: the window's centre is the frame's centre");
  Check(MapToFrame(u.frame, 0, 0, &fx, &fy) && fx == 0 && fy == 0 && MapToFrame(u.frame, 1919, 1199, &fx, &fy) && fx == 1919 && fy == 1199,
        "AUTO on 1920x1200: the window's corners are the frame's");

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

  // A taller picture: the stretched 3D picture is the frame, a flat one is the 16:9 band of it (the HUD block), with its
  // own black above and below.
  CheckRect(PlaceSource(1280, 720, 1920, 1200, kAspect16x10, kAspect16x10), Rect{0, 0, 1920, 1200}, "16:10 picture in a 1920x1200 window: the window");
  CheckRect(PlaceSource(1920, 1080, 1920, 1200, kAspect16x10, kAspect16x10), Rect{0, 0, 1920, 1200}, "the same from a 1080p source");
  CheckRect(PlaceSource(1280, 720, 1920, 1200, 0.0, kAspect16x10), Rect{0, 60, 1920, 1080}, "flat 16:9 picture in the 16:10 frame: the 16:9 band");
  CheckRect(PlaceSource(1280, 720, 1920, 1200, 0.0, kAspect16x10), LayoutOf(Choice::kAuto, 1920, 1200).hud, "the flat picture is where the HUD block is (16:10)");
  CheckRect(PlaceSource(1280, 720, 1920, 1080, kAspect16x10, kAspect16x10), Rect{96, 0, 1728, 1080}, "16:10 picture in a 16:9 window: pillarbox");
  CheckRect(PlaceSource(1280, 720, 1920, 1080, 0.0, kAspect16x10), Rect{96, 54, 1728, 972}, "flat 16:9 picture in the 16:10 frame of a 16:9 window");
  CheckRect(PlaceSource(1280, 720, 1440, 1080, 4.0 / 3.0, 4.0 / 3.0), Rect{0, 0, 1440, 1080}, "4:3 picture in a 1440x1080 window: the window");
  CheckRect(PlaceSource(1280, 720, 1440, 1080, 0.0, 4.0 / 3.0), Rect{0, 135, 1440, 810}, "flat 16:9 picture in the 4:3 window: the 16:9 band");
  CheckRect(PlaceSource(1280, 720, 1080, 1920, kAspectMin, kAspectMin), Rect{0, 0, 1080, 1920}, "9:16 picture in a portrait window: the window");
  CheckRect(PlaceSource(1280, 720, 1080, 1920, 0.0, kAspectMin), Rect{0, 656, 1080, 608}, "flat 16:9 picture in the portrait window");
  // The 16:9 frame in a taller window is the plain 16:9 fit (the frame is no frame).
  CheckRect(PlaceSource(1280, 720, 1920, 1200, 0.0, kAspect16x9), Rect{0, 60, 1920, 1080}, "16:9 choice in a 16:10 window: letterbox as always");
  // Everything is inside the frame, and the frame inside the window.
  for (Choice choice : {Choice::kAuto, Choice::k16x9, Choice::k16x10, Choice::k21x9, Choice::k32x9}) {
    for (uint32_t w : {800u, 1080u, 1280u, 1366u, 1440u, 1920u, 2560u, 3440u, 3840u, 5120u}) {
      for (uint32_t h : {720u, 768u, 800u, 1024u, 1080u, 1200u, 1440u, 1920u}) {
        const Layout l = LayoutOf(choice, w, h);
        for (double stretch : {0.0, l.aspect}) {
          const Rect r = PlaceSource(1280, 720, w, h, stretch, l.aspect);
          const std::string name = std::string(ChoiceName(choice)) + " " + std::to_string(w) + "x" +
                                   std::to_string(h) + (stretch > 0.0 ? " stretched" : " flat");
          Check(r.w >= 1 && r.h >= 1 && r.right() <= w && r.bottom() <= h, name + ": placed inside the window");
          if (l.wide || l.tall) {
            Check(r.x >= l.frame.x && r.right() <= l.frame.right() && r.y >= l.frame.y && r.bottom() <= l.frame.bottom(),
                  name + ": placed inside the frame");
          }
          if (stretch > 0.0 && (l.wide || l.tall)) {
            Check(r == l.frame, name + ": the stretched picture is the frame");
          }
          if (stretch == 0.0 && l.tall) {
            Check(r == l.hud || (std::abs(double(r.h) - double(l.hud.h)) <= 1.0 && r.w == l.hud.w),
                  name + ": the flat picture is the HUD block of the tall frame");
          }
          if (stretch == 0.0 && l.wide) {
            Check(std::abs(double(r.w) - double(l.hud.w)) <= 1.0 && r.h == l.hud.h, name + ": the flat picture is the HUD block of the wide frame");
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
  // A taller picture: the height of the columns the source has.
  Check(PictureHeight(1280, kAspect16x10) == 800, "capture height of 1280 columns at 16:10");
  Check(PictureHeight(1920, kAspect16x10) == 1200, "capture height of 1920 columns at 16:10");
  Check(PictureHeight(1440, 4.0 / 3.0) == 1080, "capture height of 1440 columns at 4:3");
  Check(PictureHeight(1280, kAspectMin) == 2276, "capture height of 1280 columns at 9:16 (2275.6)");
  Check(PictureHeight(1280, kAspect16x9) == 720, "capture height of 1280 columns at 16:9");
  // The unscaled copy loses nothing of the source: wide keeps the rows, tall the columns, 16:9 is the source.
  const PictureSize a = PictureSizeOf(1280, 720, kAspect16x9);
  Check(a.w == 1280 && a.h == 720, "capture size at 16:9 is the source's");
  const PictureSize b = PictureSizeOf(1280, 720, kAspect21x9);
  Check(b.w == 1707 && b.h == 720, "capture size at 21:9 keeps the rows (720 * 64/27 = 1706.7)");
  const PictureSize c = PictureSizeOf(1280, 720, kAspect16x10);
  Check(c.w == 1280 && c.h == 800, "capture size at 16:10 keeps the columns");
  const PictureSize d = PictureSizeOf(1920, 1080, kAspect16x10);
  Check(d.w == 1920 && d.h == 1200, "capture size of a 1080p source at 16:10");
  const PictureSize e = PictureSizeOf(1280, 720, 4.0 / 3.0);
  Check(e.w == 1280 && e.h == 960, "capture size at 4:3");
  const PictureSize f = PictureSizeOf(1280, 720, kAspectMin);
  Check(f.w == 1280 && f.h == 2276, "capture size at 9:16");
  const PictureSize g = PictureSizeOf(1280, 720, kAspect16x9 * 1.0005);
  Check(g.w == 1280 && g.h == 720, "capture size at 16:9 within the margin is the source's");
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

  // A taller window and the new choice. AUTO fills a 1920x1200 window; the window grows from 16:9: window, aspect ratio
  // (the cameras take the new Vert+) and frame move.
  t.Update(Choice::kAuto, 1920, 1080);
  c = t.Update(Choice::kAuto, 1920, 1200);
  Check(c == (kWindowChanged | kAspectChanged | kFrameChanged), "AUTO, window 1920x1080 -> 1920x1200: window, aspect ratio and frame");
  Check(t.layout().tall && !t.layout().boxed(), "AUTO on 1920x1200: tall, no bars");
  // AUTO -> 16:10 on that window: the same picture, only the setting.
  c = t.Update(Choice::k16x10, 1920, 1200);
  Check(c == kChoiceChanged, "AUTO -> 16:10 on a 1920x1200 window: the picture is the same, only the setting changes");
  // 16:10 -> 16:9 on it: bars above and below, new aspect ratio.
  c = t.Update(Choice::k16x9, 1920, 1200);
  Check(c == (kChoiceChanged | kAspectChanged | kFrameChanged), "16:10 -> 16:9 on 1920x1200: setting, aspect ratio and frame");
  CheckRect(t.layout().frame, Rect{0, 60, 1920, 1080}, "16:9 on 1920x1200: the frame");
  // 16:10 on the 16:9 window: bars at the sides.
  c = t.Update(Choice::k16x10, 1920, 1080);
  Check(c == (kChoiceChanged | kWindowChanged | kAspectChanged | kFrameChanged), "16:9 on 1920x1200 -> 16:10 on 1920x1080: everything");
  CheckRect(t.layout().frame, Rect{96, 0, 1728, 1080}, "16:10 on 1920x1080: the frame");
  // The window is resized under a forced 16:10: the frame follows, the aspect ratio does not move.
  c = t.Update(Choice::k16x10, 2560, 1440);
  Check(c == (kWindowChanged | kFrameChanged), "forced 16:10, window 1920x1080 -> 2560x1440: window and frame, not the aspect ratio");
  // A tall window and a wide one, under AUTO: both move the aspect ratio; and a window too tall (beyond 9:16) is held at 9:16.
  t.Update(Choice::kAuto, 1080, 1920);
  c = t.Update(Choice::kAuto, 1000, 2400);
  Check((c & kWindowChanged) && (c & kFrameChanged) && !(c & kAspectChanged), "AUTO, 1080x1920 -> 1000x2400: held at 9:16, the aspect ratio does not move");
  // The Xenia renderer's minimum (16:9): a taller window is never tall.
  Tracker xenia;
  xenia.Update(Choice::kAuto, 1920, 1080, kAspect16x9);
  c = xenia.Update(Choice::kAuto, 1920, 1200, kAspect16x9);
  Check(!(c & kAspectChanged) && !xenia.layout().tall && xenia.layout().boxed(), "min 16:9: a taller window changes no aspect ratio and keeps its bars");

  // A long sequence of switches ends where a fresh tracker would: the layout depends on the last setting
  // and window only, nothing is left over from the path.
  Tracker walk;
  const Choice order[] = {Choice::k32x9, Choice::kAuto, Choice::k21x9, Choice::k16x10, Choice::k16x9, Choice::k32x9, Choice::k16x10, Choice::k21x9, Choice::kAuto};
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
  CheckVertPlus();
  CheckSqueeze();
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
