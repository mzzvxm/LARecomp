// Check of the side views' scale rules (side_views.h): the shadow detail, the car reflections' detail and the road reflections' detail
// rows, which are live. The renderer asks the settings once a frame (side_views::Wanted), compares the scales with the ones the classes
// stand at, and EdramTargets::SetSideScales makes the images of the classes that moved again at a frame boundary, one step down at a
// time when the video memory is short (StepDown). This holds the pure rules of that: a setting's word to a level on each GPU class
// (`auto` is the class table's cell; a word that is no level counts as `auto`), a level to a scale, the step down, the scale of a class
// that follows the render scale, and the declared sizes that tell a class's use of an identity from another pass's. A program of its
// own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_side_view_scales_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_side_view_scales_test
//
// The expected values are written out here again on purpose: the `auto` cells of GFX-UPGRADES-DESIGN.md section 9 are the other side of
// the check (gpu_class_test.cpp holds the whole table; the three rows of the side views are repeated here because the scales come
// from them), so a change of a cell or of a rule in the headers alone fails here.

#include <cstdint>
#include <cstdio>
#include <string>

#include "side_views.h"

using namespace mcla::gpu;
using gpu_class::Effect;
using gpu_class::GpuClass;

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

void CheckN(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), (unsigned long long)got, (unsigned long long)want);
  }
}

const char* ClassWord(GpuClass c) { return c == GpuClass::kLow ? "Low" : c == GpuClass::kMid ? "Mid" : "High"; }

void TheLevelsAndTheirScales() {
  CheckN(ScaleOfLevel(SideLevel::kGame).n, 20, "game is the guest's size, 1x");
  CheckN(ScaleOfLevel(SideLevel::kHigh).n, 40, "high is 2x");
  CheckN(ScaleOfLevel(SideLevel::kUltra).n, 60, "ultra is 3x");
  Check(std::string(LevelText(SideLevel::kGame)) == "game" && std::string(LevelText(SideLevel::kHigh)) == "high" &&
            std::string(LevelText(SideLevel::kUltra)) == "ultra",
        "the words of the levels");
  SideLevel level = SideLevel::kUltra;
  Check(ParseLevel("game", &level) && level == SideLevel::kGame, "game parses");
  Check(ParseLevel("high", &level) && level == SideLevel::kHigh, "high parses");
  Check(ParseLevel("ultra", &level) && level == SideLevel::kUltra, "ultra parses");
  level = SideLevel::kHigh;
  const char* bad[] = {"auto", "low", "medium", "Ultra", "HIGH", "", " high", "high ", "3x"};
  for (const char* w : bad) {
    Check(!ParseLevel(w, &level) && level == SideLevel::kHigh, std::string("'") + w + "' is no level and leaves *level as it was");
  }
}

void TheSettingPerClass() {
  struct Row {
    Effect effect;
    const char* name;
    SideLevel low, mid, high;  // what `auto` is on each class
  };
  const Row rows[] = {
      {Effect::kShadowResolution, "shadow detail", SideLevel::kGame, SideLevel::kHigh, SideLevel::kHigh},
      {Effect::kReflectionResolution, "road reflections", SideLevel::kGame, SideLevel::kHigh, SideLevel::kHigh},
  };
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  for (const Row& r : rows) {
    for (GpuClass c : classes) {
      const SideLevel cell = c == GpuClass::kLow ? r.low : c == GpuClass::kMid ? r.mid : r.high;
      const std::string at = std::string(r.name) + " on " + ClassWord(c);
      Check(LevelFor(c, r.effect, "auto") == cell, "auto is the class table's cell: " + at);
      // A level the player picked is that level on every class.
      Check(LevelFor(c, r.effect, "game") == SideLevel::kGame, "game is game: " + at);
      Check(LevelFor(c, r.effect, "high") == SideLevel::kHigh, "high is high: " + at);
      Check(LevelFor(c, r.effect, "ultra") == SideLevel::kUltra, "ultra is ultra: " + at);
      // A word that is no level (a hand-edited file, a command line) is `auto`.
      const char* odd[] = {"low", "medium", "", "off", "on", "2x"};
      for (const char* w : odd) {
        Check(LevelFor(c, r.effect, w) == cell, std::string("'") + w + "' counts as auto: " + at);
      }
    }
  }
}

void ALiveChangeIsAnotherScale() {
  // The row steps game -> high -> ultra -> auto on a Mid class: the scale the class is asked to stand at changes with each word, and the
  // frame boundary that sees a scale other than the one in force makes the class's images again.
  const GpuClass mid = GpuClass::kMid;
  const char* words[] = {"game", "high", "ultra", "auto", "game"};
  const uint32_t scales[] = {20, 40, 60, 40, 20};  // shadow detail on Mid: auto is high
  SideScales in_force;
  CheckN(in_force[SideView::kShadow].n, 20, "every class starts at the guest's size");
  for (size_t i = 0; i < 5; ++i) {
    SideScales asked = in_force;
    asked[SideView::kShadow] = ScaleOfLevel(LevelFor(mid, Effect::kShadowResolution, words[i]));
    CheckN(asked[SideView::kShadow].n, scales[i], std::string("shadow detail '") + words[i] + "' on Mid");
    Check((asked == in_force) == (scales[i] == in_force[SideView::kShadow].n), std::string("a change is seen only when the scale moved: '") + words[i] + "'");
    // One class moves; the others are not asked to.
    Check(asked[SideView::kEnv] == in_force[SideView::kEnv] && asked[SideView::kReflection] == in_force[SideView::kReflection] &&
              asked[SideView::kUiMap] == in_force[SideView::kUiMap],
          "the other classes stay where they are");
    in_force = asked;
  }
}

void TheStepDownWhenTheMemoryIsShort() {
  // Ultra cannot be made: 3x -> 2x -> 1x, then the guest's size stays.
  CheckN(side_views::StepDown(Scale::Whole(3)).n, 40, "3x steps down to 2x");
  CheckN(side_views::StepDown(Scale::Whole(2)).n, 20, "2x steps down to 1x");
  CheckN(side_views::StepDown(Scale::Whole(1)).n, 20, "1x is the lowest");
  CheckN(side_views::StepDown(Scale{30}).n, 20, "1.5x steps down to 1x");
  CheckN(side_views::StepDown(Scale{50}).n, 40, "2.5x steps down to 2x (the whole scale below)");
  CheckN(side_views::StepDown(Scale{41}).n, 40, "just above 2x steps down to 2x");
  CheckN(side_views::StepDown(Scale{21}).n, 20, "just above 1x steps down to 1x");
  CheckN(side_views::StepDown(Scale{10}).n, 20, "a scale under 1x comes to the guest's size");
  // The chain ends: it never loops and never reaches 0.
  Scale s = Scale::Whole(3);
  int steps = 0;
  while (s.n > 20 && steps < 10) {
    s = side_views::StepDown(s);
    ++steps;
  }
  CheckN(steps, 2, "3x reaches the guest's size in two steps");
  CheckN(s.n, 20, "... and stops there");
}

void TheClassThatFollowsTheRenderScale() {
  // The HUD's small views (the minimap, the page of the pause menu) are made at the screen's scale from 1.25x of the console's picture.
  CheckN(side_views::kFollowMinScale, 25, "the follow threshold is 1.25x");
  CheckN(side_views::OutputScale(Scale{20}).n, 20, "720p (1x): the guest's size");
  CheckN(side_views::OutputScale(Scale{24}).n, 20, "1.2x: still the guest's size");
  CheckN(side_views::OutputScale(Scale{25}).n, 25, "1.25x: follows");
  CheckN(side_views::OutputScale(Scale{30}).n, 30, "1080p: follows at 1.5x");
  CheckN(side_views::OutputScale(Scale{60}).n, 60, "4K: follows at 3x");
  CheckN(side_views::OutputScale(Scale{10}).n, 20, "a scale under 1x: the guest's size");
}

void TheDeclaredSizes() {
  // D3DSurface SizeBits: width - 1 in bits 31..18, height - 1 in bits 17..3.
  auto bits = [](uint32_t w, uint32_t h) { return ((w - 1) << 18) | ((h - 1) << 3); };
  Check(side_views::DeclaredSize(bits(640, 640), 640, 640), "the cascades' surface is 640x640");
  Check(!side_views::DeclaredSize(bits(640, 639), 640, 640), "639 high is not 640");
  Check(!side_views::DeclaredSize(bits(512, 512), 640, 640), "the night shadow's 512x512 is another use");
  Check(side_views::DeclaredSize(bits(512, 256), 512, 256), "the environment map's depth is 512x256");
  Check(!side_views::DeclaredSize(bits(256, 512), 512, 256), "width and height are not interchangeable");
  // A foreign use: the key names a class, the use (with its declared size known) does not.
  using side_views::ForeignUse;
  Check(ForeignUse(SideView::kEnv, SideView::kNone, bits(512, 512)), "the key is the environment map's, the use is not: foreign");
  Check(!ForeignUse(SideView::kEnv, SideView::kEnv, bits(512, 256)), "the class's own use is not foreign");
  Check(!ForeignUse(SideView::kEnv, SideView::kNone, 0), "an unknown size is the class's, never foreign");
  Check(!ForeignUse(SideView::kNone, SideView::kNone, bits(512, 512)), "a key no class has is nobody's");
}

void TheRegistry() {
  CheckN(kSideViewCount, 5, "five classes");
  CheckN(SideIndex(SideView::kShadow), 0, "the shadow is first");
  CheckN(SideIndex(SideView::kUiMap), 3, "the UI map is fourth");
  CheckN(SideIndex(SideView::kFoliageImpostor), 4, "foliage impostors follow the UI map");
  for (size_t i = 0; i < kAllSideViews.size(); ++i) {
    CheckN(SideIndex(kAllSideViews[i]), i, "the order of kAllSideViews is the index order");
  }
}

}  // namespace

int main() {
  TheLevelsAndTheirScales();
  TheSettingPerClass();
  ALiveChangeIsAnotherScale();
  TheStepDownWhenTheMemoryIsShort();
  TheClassThatFollowsTheRenderScale();
  TheDeclaredSizes();
  TheRegistry();
  if (g_failed != 0) {
    std::printf("side_view_scales_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("side_view_scales_test: %d checks passed\n", g_checks);
  return 0;
}
