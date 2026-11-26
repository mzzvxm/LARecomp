// Check of the rule that gives another pass's use of a side view class's EDRAM identity a host target of its own
// (side_views.h, ONE IDENTITY, TWO USES; EdramTargets::DepthLayer). A program of its own: it needs no renderer, no
// ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_side_view_use_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_side_view_use_test [<src/gpu dir>]
// or by hand: clang++ -std=c++23 -I src/gpu src/gpu/side_view_use_test.cpp -o side_view_use_test
//
// The bug it covers (ports\run\wg-rtx-chosen-4k, gv-off-new): the car reflections' class owns the depth kD24FS8 at
// tile 0, pitch 520, 4 samples (declared 512x256) and makes it at the class's scale (2x on a strong card). The night
// shadow's pass (dusk and night, colour k_8_8_8_8 at tile 832, declared 512x512) draws with the very same depth key,
// but its colour is no class's, so it was made at 1x: "targets of different resolution scales", every draw of the
// pass left out. The rules of the two identities and the scene's per-identity target table are restated below as a
// small model (the expected values are written out again on purpose, as in gpu_class_test.cpp), the production rule
// (side_views::ForeignUse) is called as it is, and with the source folder as its argument the program also checks
// that the model's numbers are the rule files' and that every call of EdramTargets::DepthLayer passes the size.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

#include "setting_source.h"
#include "side_view_foliage_impostor.h"
#include "side_view_ui_map.h"
#include "side_views.h"

using namespace mcla::gpu;

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

// ---- the model of the identities (side_view_env.cpp, side_view_reflection.cpp) -------------------------------------

constexpr uint8_t kFormatD24FS8 = 1;      // xenos::DepthRenderTargetFormat::kD24FS8
constexpr uint8_t kFormatFloat7e3 = 3;    // xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT
constexpr uint8_t kFormat8888 = 0;        // k_8_8_8_8

struct Key {
  uint16_t base = 0;
  uint16_t pitch = 0;
  bool depth = false;
  uint8_t format = 0;
  uint8_t samples = 1;
  uint8_t layer = 0;
  auto Tie() const { return std::tie(base, pitch, depth, format, samples, layer); }
  bool operator<(const Key& o) const { return Tie() < o.Tie(); }
};

// A declared size as the guest's SizeBits has it (width - 1 in bits 31..18, height - 1 in 17..3); 0 = not known.
constexpr uint32_t Size(uint32_t width, uint32_t height) {
  return ((width - 1) << 18) | ((height - 1) << 3);
}
constexpr uint32_t kEnvSize = Size(512, 256);
constexpr uint32_t kNightSize = Size(512, 512);
constexpr uint32_t kReflectionSize = Size(256, 256);

// The environment map: the two identities at pitch 520, 4 samples, declared 512x256 where the size is known.
bool EnvMatches(const Key& k, uint32_t size) {
  if (k.pitch != 520 || k.samples != 4 || k.layer != 0) return false;
  if (size != 0 && size != kEnvSize) return false;
  return k.depth ? (k.base == 0 && k.format == kFormatD24FS8) : (k.base == 416 && k.format == kFormatFloat7e3);
}
// The road reflection: pitch 320, 2 samples, declared 256x256.
bool ReflectionMatches(const Key& k, uint32_t size) {
  if (k.pitch != 320 || k.samples != 2 || k.layer != 0) return false;
  if (size != 0 && size != kReflectionSize) return false;
  return k.depth ? (k.base == 0 && k.format == kFormatD24FS8) : (k.base == 128 && k.format == kFormatFloat7e3);
}
SideView Classify(const Key& k, uint32_t size) {
  if (ReflectionMatches(k, size)) return SideView::kReflection;
  if (EnvMatches(k, size)) return SideView::kEnv;
  return SideView::kNone;
}

// What EdramTargets::DepthLayer does first (the rest of it is the main view's, temporal_identities.h).
uint8_t DepthLayer(Key depth, uint32_t size, bool rule_on) {
  Key identity = depth;
  identity.layer = 0;
  if (rule_on && side_views::ForeignUse(Classify(identity, 0), Classify(identity, size), size)) {
    return side_views::kForeignUseLayer;
  }
  return 0;
}

// ---- the model of the target table (EdramTargets::Get, DrawScale, Scene::Record) -----------------------------------

struct Target {
  SideView view = SideView::kNone;
  int scale = 1;  // whole scales only here
};

struct Table {
  int class_scale = 1;  // what the setting says for every class
  bool rule_on = true;
  std::map<Key, Target> targets;
  int mismatches = 0;  // draws whose colour and depth are not one scale: left out
  int draws = 0;

  // A target is made once, at its first use: the class's scale if that use classifies as a class's, else 1x.
  Target& Get(const Key& k, uint32_t size) {
    auto it = targets.find(k);
    if (it == targets.end()) {
      Target t;
      t.view = Classify(k, size);
      t.scale = t.view != SideView::kNone ? class_scale : 1;
      it = targets.emplace(k, t).first;
    }
    return it->second;
  }
  Key DepthKey(uint16_t pitch, uint8_t samples, uint32_t size) const {
    Key d{0, pitch, true, kFormatD24FS8, samples, 0};
    d.layer = DepthLayer(d, size, rule_on);
    return d;
  }
  // A draw with a colour and a depth through a surface declared `size`: Scene::Record's two targets and the check.
  bool Draw(const Key& colour, uint32_t size) {
    const Key depth = DepthKey(colour.pitch, colour.samples, size);
    const Target& c = Get(colour, size);
    const Target& d = Get(depth, size);
    ++draws;
    if (c.scale != d.scale) {
      ++mismatches;
      return false;
    }
    return true;
  }
  // A depth-only draw or clear: one target.
  Key DepthOnly(uint16_t pitch, uint8_t samples, uint32_t size) {
    const Key depth = DepthKey(pitch, samples, size);
    Get(depth, size);
    return depth;
  }
};

const Key kEnvColour{416, 520, false, kFormatFloat7e3, 4, 0};
const Key kNightColour{832, 520, false, kFormat8888, 4, 0};
const Key kReflectionColour{128, 320, false, kFormatFloat7e3, 2, 0};

// ---- checks ---------------------------------------------------------------------------------------------------------

void TruthTable() {
  using side_views::ForeignUse;
  const SideView classes[] = {SideView::kNone, SideView::kShadow, SideView::kEnv, SideView::kReflection};
  for (const SideView by_key : classes) {
    for (const SideView by_use : classes) {
      for (const uint32_t size : {0u, kEnvSize, kNightSize}) {
        // Foreign: the key names a class, the use (a known size) says it is not that use. Nothing else is.
        const bool want = size != 0 && by_key != SideView::kNone && by_use == SideView::kNone;
        Check(ForeignUse(by_key, by_use, size) == want,
              "ForeignUse(key " + std::to_string(int(by_key)) + ", use " + std::to_string(int(by_use)) + ", size " +
                  std::to_string(size) + ") is " + (want ? "true" : "false"));
      }
    }
  }
  static_assert(side_views::kForeignUseLayer != 0 && side_views::kForeignUseLayer != 1,
                "layers 0 (the scene's) and 1 (the UI's) are taken (EdramTargets::DepthLayer)");
  static_assert(side_views::ForeignUse(SideView::kEnv, SideView::kNone, Size(512, 512)), "the logged case");
}

void LoggedFrame() {
  // The log (wg-rtx-chosen-4k): the car reflections' pass first (colour at 416 and the depth, both made at 2x), then the
  // night shadow's draws (colour at 832, depth at 0, declared 512x512): colour at 1x, depth at 2x, "different scales".
  for (const int scale : {1, 2, 3}) {
    Table without;
    without.class_scale = scale;
    without.rule_on = false;
    Check(without.Draw(kEnvColour, kEnvSize), "env draw is one scale without the rule, class at " + std::to_string(scale));
    const bool night_ok = without.Draw(kNightColour, kNightSize);
    // The bug: at 1x the two agree; from 2x on the depth is the class's and the colour is not.
    Check(night_ok == (scale == 1), "without the rule the night pass is left out exactly above 1x, class at " +
                                        std::to_string(scale));

    Table with;
    with.class_scale = scale;
    Check(with.Draw(kEnvColour, kEnvSize), "env draw, class at " + std::to_string(scale));
    Check(with.Draw(kNightColour, kNightSize), "night draw is one scale, class at " + std::to_string(scale));
    Check(with.mismatches == 0, "no draw left out, class at " + std::to_string(scale));
    Check(with.targets.size() == 4, "colour 416, colour 832 and two depths, class at " + std::to_string(scale));
    const Key env_depth = with.DepthKey(520, 4, kEnvSize);
    const Key night_depth = with.DepthKey(520, 4, kNightSize);
    Check(env_depth.layer == 0 && night_depth.layer == side_views::kForeignUseLayer, "the two depths are two layers");
    Check(with.targets[env_depth].scale == scale && with.targets[env_depth].view == SideView::kEnv,
          "the env depth is the class's at " + std::to_string(scale));
    Check(with.targets[night_depth].scale == 1 && with.targets[night_depth].view == SideView::kNone,
          "the night shadow's depth is at the guest's size, class at " + std::to_string(scale));
    Check(with.targets[kNightColour].scale == 1, "the night shadow's colour stays at 1x");
  }
}

void EitherOrder() {
  // The first use decides what a target is made as: the night pass first (a game that starts at night), then the
  // env pass, many frames, a live change of the class's scale in between: no draw of either pass is ever left out.
  for (const int scale : {1, 2, 3}) {
    Table t;
    t.class_scale = scale;
    for (int frame = 0; frame < 3; ++frame) {
      Check(t.Draw(kNightColour, kNightSize), "night first, frame " + std::to_string(frame));
      Check(t.Draw(kEnvColour, kEnvSize), "env second, frame " + std::to_string(frame));
    }
    Check(t.mismatches == 0, "night pass first: no mismatch, class at " + std::to_string(scale));
    Check(t.targets[t.DepthKey(520, 4, kEnvSize)].scale == scale, "night pass first: the env depth is still the class's");
    // Depth-only uses (a clear of the depth alone, a resolve's clear after it) find the same targets as the draws.
    Check(t.DepthOnly(520, 4, kNightSize).layer == side_views::kForeignUseLayer, "a night depth clear is layer 2");
    Check(t.DepthOnly(520, 4, kEnvSize).layer == 0, "an env depth clear is layer 0");
    Check(t.targets.size() == 4, "still four targets after the clears, class at " + std::to_string(scale));
  }
}

// A class that follows the screen's scale (side_views.h OutputScale): 720p at a render scale of 1x, or any scale below 1.25x, is the guest's size, so the class changes nothing
// there (round 3: UI-SHARP's "the 720p minimap is not identical" was the phone sliding out between the two pictures, but a render scale of 1.05 used to remake the targets).
void FollowsTheScreenFromOnePointTwoFive() {
  using side_views::OutputScale;
  Check(OutputScale(Scale{}).unit() && OutputScale(Scale{20}).unit(), "at 1x the class is at the guest's size");
  Check(OutputScale(Scale{21}).unit() && OutputScale(Scale{24}).unit(), "below 1.25x too (1.05x, 1.2x)");
  Check(OutputScale(Scale{25}) == Scale{25}, "at 1.25x it follows");
  Check(OutputScale(Scale{30}) == Scale{30} && OutputScale(Scale{45}) == Scale{45} && OutputScale(Scale{60}) == Scale{60}, "at 1.5x (1080p), 2.25x and 3x (4K) it follows");
  Check(OutputScale(Scale{10}).unit(), "never below the guest's size (a render scale of 0.5x)");
  Check(side_views::kFollowMinScale == 25, "the threshold is 1.25x in twentieths");
}

struct RenderKey {
  uint16_t base_tiles;
  uint16_t pitch;
  uint8_t depth;
  uint8_t format;
  uint8_t samples;
  uint8_t reserved;
};

void QuickMap() {
  using Key = RenderKey;
  using ui_map_views::Matches;
  constexpr Key colour{0, 760, 0, kFormat8888, 4, 0};
  constexpr uint32_t size = Size(749, 430);
  Check(Matches(colour, size), "the logged 749x430 quick map belongs to the UI map views");
  Check(Matches(colour, 0), "the quick map's key also matches when size is unavailable");
  Check(!Matches(colour, Size(760, 430)), "pitch 760 is not the quick map's declared width");
  Check(!Matches(colour, Size(749, 432)), "a different declared height uses the same key outside the class");
  Check(!Matches(Key{0, 760, 1, kFormat8888, 4, 0}, size), "tile 0 depth cannot match even with the same format bits");
  Check(!Matches(Key{0, 760, 1, kFormatD24FS8, 4, 0}, 0), "pitch 760 D24FS8 depth with no declared size is outside the class");
  Check(!Matches(Key{0, 760, 0, kFormatFloat7e3, 4, 0}, size), "pitch 760 colour of a different format is outside the class");
  Check(!Matches(Key{0, 760, 0, kFormat8888, 2, 0}, size), "a two-sample target is outside the class");
  Check(!Matches(Key{0, 752, 0, kFormat8888, 4, 0}, size), "another pitch is outside the class");
  Check(!Matches(Key{16, 760, 0, kFormat8888, 4, 0}, size), "another tile is outside the class");
  Check(!Matches(Key{0, 760, 0, kFormat8888, 4, 1}, size), "a nonzero layer is outside the class");
  Check(side_views::OutputScale(Scale{60}, false).unit(), "quick map off at 3x is made at 1x");
  Check(side_views::OutputScale(Scale{24}, true).unit(), "quick map on below 1.25x is made at 1x");
  Check(side_views::OutputScale(Scale{25}, true) == Scale{25}, "quick map on at 1.25x follows the render scale");
  Check(side_views::OutputScale(Scale{60}, true) == Scale{60}, "quick map on at 3x follows the render scale");
}

void FoliageImpostors() {
  using foliage_impostor_view::FollowsOutput;
  using foliage_impostor_view::Matches;
  constexpr uint32_t size = Size(128, 128);
  for (const RenderKey key : {RenderKey{224, 160, 0, kFormat8888, 4, 0},
                              RenderKey{288, 160, 0, kFormat8888, 4, 0},
                              RenderKey{0, 160, 1, kFormatD24FS8, 4, 0}}) {
    Check(Matches(key, size) && Matches(key, 0), "both foliage colours and depth share the bake rule");
    Check(!ui_map_views::Matches(key, size) && !ui_map_views::Matches(key, 0), "foliage is outside the UI map rule");
    Check(!Matches(key, Size(256, 128)) && !Matches(key, Size(128, 256)), "keep a foreign use outside the foliage rule");
  }
  for (const RenderKey key : {RenderKey{0, 160, 0, kFormat8888, 4, 0},
                              RenderKey{224, 160, 1, kFormatD24FS8, 4, 0},
                              RenderKey{0, 160, 1, 0, 4, 0},
                              RenderKey{224, 160, 0, kFormatFloat7e3, 4, 0},
                              RenderKey{224, 160, 0, kFormat8888, 1, 0},
                              RenderKey{224, 160, 0, kFormat8888, 4, side_views::kForeignUseLayer},
                              RenderKey{1601, 160, 0, 1, 1, 0}}) {
    Check(!Matches(key, size), "same-size unrelated target remains outside the foliage rule");
  }
  for (const RenderKey key : {RenderKey{224, 280, 0, kFormat8888, 4, 0},
                              RenderKey{0, 280, 1, kFormatD24FS8, 4, 0},
                              RenderKey{168, 240, 0, kFormat8888, 4, 0}}) {
    Check(!Matches(key, 0) && ui_map_views::Matches(key, 0), "256x256 products and the radar keep their scaling rule");
  }
  for (const Scale output : {Scale{20}, Scale{24}, Scale{25}, Scale{30}, Scale{60}}) {
    Check(side_views::OutputScale(output, FollowsOutput(true, true)).unit(), "native bake stays at console size with Sharp Minimap on");
    Check(side_views::OutputScale(output, FollowsOutput(true, false)).unit(), "native bake stays at console size with Sharp Minimap off");
    Check(side_views::OutputScale(output, FollowsOutput(false, false)).unit(), "the old rule with Sharp Minimap off keeps console size");
    const Scale original = output.n >= 25 ? output : Scale{};
    Check(side_views::OutputScale(output, FollowsOutput(false, true)) == original, "the old rule with Sharp Minimap on follows the output from 1.25x");
  }
}

void UnknownSizeAndOtherIdentities() {
  Table t;
  t.class_scale = 2;
  // A use with no size known is the class's, as Classify takes it (the old behaviour).
  Check(t.DepthKey(520, 4, 0).layer == 0, "no size known: layer 0");
  // The road reflection's depth at pitch 320, 2 samples: its own use is the class's, any other declared size is
  // another pass's.
  Check(DepthLayer({0, 320, true, kFormatD24FS8, 2, 0}, kReflectionSize, true) == 0, "the road reflection's own depth");
  Check(DepthLayer({0, 320, true, kFormatD24FS8, 2, 0}, Size(320, 256), true) == side_views::kForeignUseLayer,
        "the road reflection's depth through a surface of another size");
  Table r;
  r.class_scale = 2;
  Check(r.Draw(kReflectionColour, kReflectionSize), "road reflection draw at 2x");
  // The main view (pitch 1280, 2 samples) and the pause view's 4x depth, the scene's depths at other pitches and
  // the sun cascades' (pitch 640) are no class's key: never a foreign use, at any size.
  for (const uint32_t size : {0u, Size(1280, 720), Size(512, 512), Size(640, 640), kEnvSize}) {
    Check(DepthLayer({0, 1280, true, kFormatD24FS8, 2, 0}, size, true) == 0, "the main view's depth is never foreign");
    Check(DepthLayer({0, 1280, true, kFormatD24FS8, 4, 0}, size, true) == 0, "the pause view's depth is never foreign");
    Check(DepthLayer({0, 640, true, 0, 4, 0}, size, true) == 0, "the sun cascades' depth is never foreign");
    Check(DepthLayer({0, 160, true, kFormatD24FS8, 4, 0}, size, true) == 0, "the 160x128 pass's depth is never foreign");
  }
  // The other depth identity of the env pass's pitch, with another base: not the class's key at all.
  Check(DepthLayer({16, 520, true, kFormatD24FS8, 4, 0}, kNightSize, true) == 0, "another base is no class's key");
  // A layer other than 0 given in (the key as a caller keeps it) does not change the answer: the rules see layer 0.
  Check(DepthLayer({0, 520, true, kFormatD24FS8, 4, 1}, kNightSize, true) == side_views::kForeignUseLayer,
        "the layer a caller already set does not hide the identity");
}

// ---- the foliage bake at night: its second colour (rthit4, 2026-11-21) ---------------------------------------------
// The night run's log: "colour k_8_8_8_8@224 pitch 160 4x made at 3x (the UI map views side view ...)", "colour k_8_8_8_8@288 pitch 160 4x made at 3x (an image of the screen; declared 128x128 ...)", "depth
// kD24FS8@0 pitch 160 4x made at 3x (the UI map views side view ...)": one pass with two colours and a depth. A target that is no class's is made at the draw's scale the first time (the class's 3x, as
// the draw's first target is a member) and moved by the next live change of a scale to what its declared size classifies at (EdramTargets::ScaleAfter: a 128x128 surface is no image of the screen: 1x). At 8 s
// the tour set the shadows', the reflections' and the road reflection's resolution to 'game': the pass's colour at 288 went to 1x, its companions stayed at 3x, and from then on all of its draws (25 a frame)
// were left out: "targets of different resolution scales". The rule: every identity the pass binds is a member of the class.
struct PassKey {
  uint16_t base;
  uint16_t pitch;
  bool depth;
  uint8_t samples;
};
bool PassMember(const PassKey& k, bool second_colour_listed) {
  if (k.pitch != 160 || k.samples != 4) return false;
  if (k.depth) return k.base == 0;
  return k.base == 224 || (second_colour_listed && k.base == 288);
}

void SecondColourOfTheNightPass() {
  const PassKey colour0{224, 160, false, 4}, colour1{288, 160, false, 4}, depth{0, 160, true, 4};
  for (const bool listed : {false, true}) {
    for (const int scale : {2, 3}) {
      // made at the first draw at the draw's scale (the class's: the first target is a member), then a live change of a scale moves every target by its kind
      const int made[3] = {scale, scale, scale};
      const PassKey keys[3] = {colour0, colour1, depth};
      int after[3];
      for (int i = 0; i < 3; ++i) {
        after[i] = PassMember(keys[i], listed) ? scale : 1;
      }
      const bool agree_made = made[0] == made[1] && made[1] == made[2];
      const bool agree_after = after[0] == after[1] && after[1] == after[2];
      Check(agree_made, std::string("the pass agrees when its targets are made, at ") + std::to_string(scale) + "x");
      Check(agree_after == listed, std::string(listed ? "with the second colour in the class" : "without it") + ": after a live change of a scale the pass " +
                                       (listed ? "still agrees" : "disagrees (the logged case)") + ", at " + std::to_string(scale) + "x");
    }
  }
}

// ---- the source: the model's numbers are the rule files', and no DepthLayer call forgets the size -------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// The number of top-level arguments of the call that starts at `open` (the index of its "("), -1 if unbalanced.
int Arguments(const std::string& text, size_t open) {
  int depth = 0;
  int args = 1;
  bool any = false;
  for (size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      if (--depth == 0) return any ? args : 0;
    } else if (depth == 1 && c == ',') {
      ++args;
    } else if (depth >= 1 && c != ' ' && c != '\n' && c != '\t' && c != '\r') {
      any = true;
    }
  }
  return -1;
}

void CheckSource(const std::string& dir) {
  const std::string env = ReadFile(dir + "/side_view_env.cpp");
  const std::string reflection = ReadFile(dir + "/side_view_reflection.cpp");
  Check(!env.empty() && !reflection.empty(), "the rule files are readable in " + dir);
  Check(Has(env, "kPitch = 520") && Has(env, "kSamples = 4") && Has(env, "kColorBase = 416") &&
            Has(env, "kDepthBase = 0") && Has(env, "kDeclaredWidth = 512") && Has(env, "kDeclaredHeight = 256"),
        "side_view_env.cpp has the identities the model restates");
  Check(Has(reflection, "kPitch = 320") && Has(reflection, "kSamples = 2") && Has(reflection, "kColorBase = 128") &&
            Has(reflection, "kDepthBase = 0") && Has(reflection, "kDeclared = 256"),
        "side_view_reflection.cpp has the identities the model restates");
  Check(Has(env, "key.reserved != 0") && Has(reflection, "key.reserved != 0"),
        "the rules match layer 0 only (a layer 2 depth is no class's)");

  for (const char* file : {"scene.cpp", "draw_target_memo.h", "resolve.cpp"}) {
    const std::string text = ReadFile(dir + "/" + file);
    Check(!text.empty(), std::string(file) + " is readable");
    int calls = 0;
    for (size_t at = text.find("DepthLayer("); at != std::string::npos; at = text.find("DepthLayer(", at + 1)) {
      // Not a comment's mention: the line must not start with //.
      const size_t line = text.rfind('\n', at);
      const size_t first = text.find_first_not_of(" \t", line + 1);
      if (first != std::string::npos && text.compare(first, 2, "//") == 0) continue;
      ++calls;
      Check(Arguments(text, at + 10) == 3,
            std::string(file) + ": a DepthLayer call has the three arguments (the declared size too) near byte " +
                std::to_string(at));
    }
    Check(calls > 0, std::string(file) + " has DepthLayer calls");
  }
  const std::string edram = ReadFile(dir + "/edram.cpp");
  Check(Has(edram, "side_views::IsForeignUse(depth, size_bits)"), "EdramTargets::DepthLayer applies the rule");
  {
    const std::string scene = ReadFile(dir + "/scene.cpp");
    Check(Has(scene, "all += std::format(\"{}{} at {}x\", all.empty() ? \"\" : \", \", DescribeEdramKey(colors[i]->key),") && Has(scene, "targets of different resolution scales (pitch {}): {}, {}"),
          "the scale disagreement's log line names every colour of the draw and the depth, each with its scale (the logged line named colour 0 and the depth, both at 3x)");
  }

  // The remaining UI targets follow the output and keep the guest sample count.
  const std::string ui_map = ReadFile(dir + "/side_view_ui_map.cpp");
  const std::string identities = ReadFile(dir + "/side_view_ui_map.h");
  Check(!ui_map.empty(), "side_view_ui_map.cpp is readable");
  Check(Has(identities, "{false, 224, 280, 4, 256, 256}") &&
            Has(identities, "{true, 0, 280, 4, 256, 256}") && Has(identities, "{false, 168, 240, 4, 220, 220}"),
        "side_view_ui_map.h keeps the 256x256 and radar identities with their 4 samples");
  Check(Has(identities, "{false, 960, 960, 2, 960, 640}") && Has(identities, "{true, 0, 960, 2, 960, 640}"),
        "side_view_ui_map.h has the menus' page: colour at tile 960 and depth at tile 0, pitch 960, 2 samples, declared 960x640");
  Check(Has(ui_map, ".matches = ui_map_views::Matches<EdramKey>") && Has(ui_map, "return guest_samples;") && Has(ui_map, ".follows_output = true"),
        "the UI map rule uses the tested matcher, keeps the guest's samples and follows the output");
  Check(Has(ui_map, ".enabled = Enabled") && Has(ui_map, "return larecomp::settings::mcla_ui_minimap_scale.Get();"),
        "SHARP MINIMAP enables the UI map views live");
  // The setting is a REX_SETTING row of src/settings/generated (or, before the conversion, the REXCVAR_DEFINE call of side_view_ui_map.cpp): setting_source.h reads either form.
  const setting_source::Info minimap = setting_source::Find(dir, ui_map, "mcla_ui_minimap_scale");
  Check(setting_source::DefaultIsTrue(minimap) && !minimap.debug_only, "the setting is on by default and is no debug setting any more");
  const std::string foliage = ReadFile(dir + "/side_view_foliage_impostor.cpp");
  const std::string registry = ReadFile(dir + "/side_views.cpp");
  Check(Has(foliage, ".matches = foliage_impostor_view::Matches<EdramKey>") && Has(foliage, ".follows_output = true") &&
            Has(foliage, ".enabled = Enabled") && !Has(foliage, ".host_samples"),
        "the foliage rule uses the tested matcher, conditional scaling and the guest sample count");
  Check(Has(foliage, "FollowsOutput(larecomp::settings::mcla_gpu_impostor_native_size.Get(),") &&
            Has(foliage, "larecomp::settings::mcla_ui_minimap_scale.Get()"),
        "the foliage rule reads the two live settings");
  Check(Has(registry, "case SideView::kFoliageImpostor: return kFoliageImpostorRule;") &&
            Has(registry, "SideView::kUiMap, SideView::kFoliageImpostor})"),
        "the renderer classifies the foliage rule independently");
  const setting_source::Info native_size = setting_source::Find(dir, foliage, "mcla_gpu_impostor_native_size");
  Check(setting_source::DefaultIsTrue(native_size) && native_size.debug_only, "the native bake defaults on and its switch is debug only");
  Check(Has(edram, "side_views::Wanted(resolution_scale_)") && Has(edram, "side_views::FollowOutput(side_scales_, output)"),
        "EdramTargets asks for the classes at the screen's scale, and a change of the render scale moves the followers with it");
  Check(!Has(edram, "{SideView::kShadow, SideView::kEnv, SideView::kReflection}"), "no loop over the classes names three of them any more");
}

}  // namespace

int main(int argc, char** argv) {
  TruthTable();
  LoggedFrame();
  EitherOrder();
  FollowsTheScreenFromOnePointTwoFive();
  QuickMap();
  FoliageImpostors();
  UnknownSizeAndOtherIdentities();
  SecondColourOfTheNightPass();
  if (argc > 1) {
    CheckSource(argv[1]);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
