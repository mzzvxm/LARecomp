// Check of the UI sharpening's rules (ui_sharp.h). A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0
// when every check holds.
//
//   cmake --build <build dir> --target larecomp_ui_sharp_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ui_sharp_rules_test
//
// The expected values are written out here again on purpose: the strength's range, the constants the shader reads, the draws that take
// the variant (and each reason one does not), and the census's counts and report.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "ui_sharp.h"

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

void CheckNum(double got, double want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %g, want %g\n", what.c_str(), got, want);
  }
}

void CheckStrength() {
  CheckNum(ui_sharp::ClampStrength(0.0), 0.0, "0 is off");
  CheckNum(ui_sharp::ClampStrength(1.0), 1.0, "1 is on");
  CheckNum(ui_sharp::ClampStrength(0.5), 0.5, "0.5 stays");
  CheckNum(ui_sharp::ClampStrength(2.0), 2.0, "2 stays");
  CheckNum(ui_sharp::ClampStrength(7.0), 2.0, "above the range: 2");
  CheckNum(ui_sharp::ClampStrength(-1.0), 0.0, "below 0: off");
  CheckNum(ui_sharp::ClampStrength(std::numeric_limits<double>::quiet_NaN()), 0.0, "NaN: off");
  CheckNum(ui_sharp::ClampStrength(std::numeric_limits<double>::infinity()), 2.0, "infinity: the top of the range");
}

void CheckConstants() {
  const ui_sharp::UiSharpConstants off = ui_sharp::ConstantsFor(0.0, 0x1);
  CheckNum(off.colour, 0.0f, "off: no Catmull-Rom");
  CheckNum(off.edge_gain, 0.0f, "off: no alpha edge");
  const ui_sharp::UiSharpConstants on = ui_sharp::ConstantsFor(1.0, 0x1);
  CheckNum(on.colour, 1.0f, "on: all Catmull-Rom");
  CheckNum(on.edge_gain, 1.0f, "on: the edge gain follows the magnification once");
  CheckNum(on.min_magnification, 1.25f, "the magnification the helper starts at");
  CheckNum(on.slots, 1.0f, "slot 0 is bit 0");
  const ui_sharp::UiSharpConstants half = ui_sharp::ConstantsFor(0.5, 0x1);
  CheckNum(half.colour, 0.5f, "0.5: half Catmull-Rom");
  CheckNum(half.edge_gain, 0.5f, "0.5: half the edge gain");
  const ui_sharp::UiSharpConstants hard = ui_sharp::ConstantsFor(2.0, 0x1);
  CheckNum(hard.colour, 1.0f, "2: the colour stays at full Catmull-Rom");
  CheckNum(hard.edge_gain, 2.0f, "2: the edge gain doubles");
  Check(on == ui_sharp::ConstantsFor(1.0, 0x1) && !(on == half), "equality");
  Check(!(on == ui_sharp::ConstantsFor(1.0, 0x2)), "another slot mask is another block");
  Check(sizeof(ui_sharp::UiSharpConstants) == 16, "16 bytes after the shared constants");
  // The slot mask is an integer held in a float, as the helper reads it (uint(settings.w + 0.5) >> slot): every mask of 16 bits survives.
  bool exact = true;
  for (uint32_t mask = 0; mask < 65536; ++mask) {
    const float w = ui_sharp::ConstantsFor(1.0, mask).slots;
    exact &= uint32_t(w + 0.5f) == mask;
  }
  Check(exact, "all 65536 slot masks round-trip through the float the shader reads");
  CheckNum(ui_sharp::ConstantsFor(1.0, 0x1FFFF).slots, 65535.0f, "slots past 15 are dropped (a pixel shader has 16 fetch constants)");
  const uint32_t gloss = (1u << 1);  // xGloss: the diffuse is slot 1, the environment map slot 0
  CheckNum(uint32_t(ui_sharp::ConstantsFor(1.0, gloss).slots + 0.5f) >> 1 & 1u, 1.0, "the shader's test for slot 1 sees the bit");
  CheckNum(uint32_t(ui_sharp::ConstantsFor(1.0, gloss).slots + 0.5f) >> 0 & 1u, 0.0, "and not slot 0's");
}

void CheckBitmapRule() {
  // The formats the helper may reconstruct, by Xenos number (xenos.h TextureFormat).
  for (const uint32_t format : {2u, 3u, 4u, 6u, 8u, 9u, 10u, 14u, 15u, 18u, 19u, 20u}) {
    Check(ui_sharp::IsBitmapFormat(format), "format " + std::to_string(format) + " is a colour bitmap");
  }
  // k_24_8 (22) and k_24_8_FLOAT (23) the scene's depth, k_16_16_16_16_EXPAND (29) the environment map, k_DXN (49) a normal map, k_32_FLOAT (36).
  for (const uint32_t format : {0u, 1u, 7u, 22u, 23u, 26u, 29u, 32u, 36u, 49u, 59u}) {
    Check(!ui_sharp::IsBitmapFormat(format), "format " + std::to_string(format) + " is not");
  }
  Check(ui_sharp::IsBitmapFetch(true, true, true, 20), "a linear 2D DXT5 is a bitmap");
  Check(!ui_sharp::IsBitmapFetch(false, true, true, 20), "not a texture fetch constant");
  Check(!ui_sharp::IsBitmapFetch(true, false, true, 20), "a cube or a volume or a stacked one");
  Check(!ui_sharp::IsBitmapFetch(true, true, false, 20), "a point filtered bitmap stays hard-edged");
  Check(!ui_sharp::IsBitmapFetch(true, true, true, 23), "the scene's depth is never sharpened, even filtered linearly");
}

ui_sharp::DrawFacts Facts() {
  ui_sharp::DrawFacts f;
  f.context = ui_sharp::Context::kPhase;
  f.eight_bit_colour = true;
  f.target_scale = 3.0f;
  f.has_variant = true;
  f.has_alpha_variant = true;
  f.ps_variant = 0;
  f.bitmap_slots = 0x1;
  f.hidden = false;
  return f;
}

void CheckDecide() {
  using ui_sharp::Why;
  {
    const ui_sharp::Decision d = ui_sharp::Decide(Facts());
    Check(d.taken() && !d.alpha_test && d.why == Why::kTaken, "a UI draw of a textured shader at 4K takes it");
  }
  {
    ui_sharp::DrawFacts f = Facts();
    f.context = ui_sharp::Context::kNone;
    Check(ui_sharp::Decide(f).why == Why::kNotUi, "a scene draw (before the composite marker, not in a menu's view) never does");
    // Round 3: the menus' render-to-texture views and the frames with no scene are UI draws too, and are told apart in the census.
    f.context = ui_sharp::Context::kMovie;
    Check(ui_sharp::Decide(f).taken(), "a draw into one of the menus' views (the pause menu's 960x640 page, the minimap) takes it");
    f.context = ui_sharp::Context::kNoScene;
    Check(ui_sharp::Decide(f).taken(), "an inline draw of a frame with no scene (a loading screen) takes it");
    f.target_scale = 1.0f;
    Check(ui_sharp::Decide(f).why == Why::kTargetScale, "... and at 720p it never does, whatever the context");
    f.context = ui_sharp::Context::kMovie;
    Check(ui_sharp::Decide(f).why == Why::kTargetScale, "... a menu's view at the guest's size (the class off or below 1.25x) is the game's own");
  }
  {
    ui_sharp::DrawFacts f = Facts();
    f.eight_bit_colour = false;
    Check(ui_sharp::Decide(f).why == Why::kColourNot8Bit, "a float target (the scene, the HDR images) does not");
  }
  {
    // Round 1 refused a draw into two colour targets: the variant writes colour 0 as the game's shader does, so the count is no reason now (the facts have none).
    ui_sharp::DrawFacts f = Facts();
    Check(ui_sharp::Decide(f).taken(), "no gate on the number of colour targets");
  }
  {
    ui_sharp::DrawFacts f = Facts();
    f.target_scale = 1.0f;
    Check(ui_sharp::Decide(f).why == Why::kTargetScale, "at the console's size (720p, --scale 1) nothing is magnified");
    f.target_scale = 1.2f;
    Check(!ui_sharp::Decide(f).taken(), "a render scale of 1.2 stays below 1.25");
    f.target_scale = 1.25f;
    Check(ui_sharp::Decide(f).taken(), "1.25 takes it");
    f.target_scale = 1.5f;
    Check(ui_sharp::Decide(f).taken(), "1080p (1.5) takes it");
    f.target_scale = 3.0f;
    Check(ui_sharp::Decide(f).taken(), "4K (3) takes it");
  }
  {
    ui_sharp::DrawFacts f = Facts();
    f.has_variant = false;
    Check(ui_sharp::Decide(f).why == Why::kNoVariant, "a shader the pack has no variant for keeps the game's");
  }
  {
    ui_sharp::DrawFacts f = Facts();
    f.ps_variant = 1;
    const ui_sharp::Decision d = ui_sharp::Decide(f);
    Check(d.taken() && d.alpha_test, "an alpha tested draw (spec variant 1) takes the twin (round 1 left it the game's: the first suspect of the 0)");
    f.has_alpha_variant = false;
    Check(ui_sharp::Decide(f).why == Why::kAlphaTestNoTwin, "with a pack that has no twin it keeps the game's and says why");
    f.ps_variant = 3;
    Check(ui_sharp::Decide(f).why == Why::kOtherVariant, "a draw that already has another variant keeps it");
  }
  {
    ui_sharp::DrawFacts f = Facts();
    f.bitmap_slots = 0;
    Check(ui_sharp::Decide(f).why == Why::kNoBitmap, "a draw with no linear colour bitmap (only a depth fetch) has nothing to sharpen");
  }
  {
    ui_sharp::DrawFacts f = Facts();
    f.hidden = true;
    Check(ui_sharp::Decide(f).why == Why::kHidden, "a draw with an empty scissor is not worth a pipeline");
  }
  {
    // The order of the reasons: the first that fails is the one told.
    ui_sharp::DrawFacts f = Facts();
    f.context = ui_sharp::Context::kNone;
    f.eight_bit_colour = false;
    f.target_scale = 1.0f;
    f.has_variant = false;
    f.bitmap_slots = 0;
    Check(ui_sharp::Decide(f).why == Why::kNotUi, "whether it is a UI draw is asked first");
    f.context = ui_sharp::Context::kPhase;
    Check(ui_sharp::Decide(f).why == Why::kColourNot8Bit, "then the colour");
    f.eight_bit_colour = true;
    Check(ui_sharp::Decide(f).why == Why::kTargetScale, "then the scale");
    f.target_scale = 3.0f;
    Check(ui_sharp::Decide(f).why == Why::kNoVariant, "then the pack's variant");
    f.has_variant = true;
    Check(ui_sharp::Decide(f).why == Why::kNoBitmap, "then the bitmap");
  }
  for (size_t i = 0; i < size_t(Why::kCount); ++i) {
    Check(std::string(ui_sharp::WhyText(Why(i))) != "?", "every reason has a name: " + std::to_string(i));
  }
}

ui_sharp::CensusSample Sample(uint64_t ps, const char* format, uint32_t w, uint32_t h, float scale, bool took) {
  ui_sharp::CensusSample s;
  s.ps = ps;
  s.format = format;
  s.width = w;
  s.height = h;
  s.levels = 1;
  s.base_page = 0x6e65;
  s.mag_filter = 1;
  s.min_filter = 1;
  s.mip_filter = 1;
  s.target_scale = scale;
  s.took_variant = took;
  s.slot = 0;
  s.bitmap = true;
  s.ps_variant = 0;
  s.why = took ? ui_sharp::Why::kTaken : ui_sharp::Why::kTargetScale;
  s.context = ui_sharp::Context::kPhase;
  return s;
}

void CheckCensus() {
  ui_sharp::Census census;
  Check(census.empty() && !census.Due(), "a new census is empty and not due");
  for (int i = 0; i < 5; ++i) census.Note(Sample(0x5CA2EECD341441FFull, "k_DXT4_5", 64, 64, 3.0f, true));
  census.Note(Sample(0x5CA2EECD341441FFull, "k_DXT4_5", 64, 64, 1.0f, false));
  for (int i = 0; i < 2; ++i) census.Note(Sample(0x5CA2EECD341441FFull, "k_DXT4_5", 128, 128, 3.0f, true));
  census.Note(Sample(0x1111111111111111ull, "k_8", 1024, 1024, 3.0f, false));
  Check(!census.empty(), "samples were noted");
  for (uint32_t i = 0; i < ui_sharp::kCensusFrames - 1; ++i) census.OnFrame();
  Check(!census.Due(), "not due one frame early");
  census.OnFrame();
  Check(census.Due(), "due after kCensusFrames frames");
  const std::vector<std::string> lines = census.TakeReport();
  Check(lines.size() == 5, "a header, the outcomes line and three (shader, texture) pairs: " + std::to_string(lines.size()));
  if (lines.size() == 5) {
    Check(lines[0].find("3 (shader, texture) pairs") != std::string::npos, "the header counts the pairs: " + lines[0]);
    Check(lines[0].find("9 texture reads") != std::string::npos, "the header counts the reads: " + lines[0]);
    Check(lines[0].find("8 at a target scale of at least 1.25") != std::string::npos, "and the magnified ones: " + lines[0]);
    Check(lines[0].find("7 of them took the variant") != std::string::npos, "and the draws that took the variant: " + lines[0]);
    Check(lines[1].find("outcomes") != std::string::npos && lines[1].find("taken 7") != std::string::npos &&
              lines[1].find("target below the minimum scale 2") != std::string::npos,
          "the second line counts the outcomes of the reads: " + lines[1]);
    Check(lines[2].find("64x64") != std::string::npos && lines[2].find("6 reads") != std::string::npos, "the most read pair first: " + lines[2]);
    Check(lines[2].find("5 magnified") != std::string::npos && lines[2].find("5 with the variant") != std::string::npos,
          "one of its six reads was at scale 1: " + lines[2]);
    Check(lines[2].find("slot 0 bitmap") != std::string::npos && lines[2].find("spec variant 0") != std::string::npos, "the slot and the spec variant: " + lines[2]);
    Check(lines[2].find("outcomes: taken 5, target below the minimum scale 1") != std::string::npos, "the pair's outcomes: " + lines[2]);
    Check(lines[3].find("128x128") != std::string::npos, "then the 128x128");
    Check(lines[4].find("k_8 1024x1024") != std::string::npos && lines[4].find("unnamed") != std::string::npos, "an unnamed one says so: " + lines[4]);
  }
  Check(census.empty() && !census.Due() && census.frames() == 0, "the window is reset after the report");
  {
    // One shader, two textures (a bitmap in slot 0, the scene's depth in slot 1), and the same bitmap drawn with the alpha test: three rows.
    ui_sharp::CensusSample bitmap = Sample(0x805FD1A239E5E5D2ull, "k_DXT4_5", 128, 128, 3.0f, true);
    ui_sharp::CensusSample depth = Sample(0x805FD1A239E5E5D2ull, "k_24_8_FLOAT", 1280, 720, 3.0f, true);
    depth.slot = 1;
    depth.bitmap = false;
    depth.mag_filter = 0;
    ui_sharp::CensusSample at = bitmap;
    at.ps_variant = 1;
    census.Note(bitmap);
    census.Note(depth);
    census.Note(at);
    const std::vector<std::string> rows = census.TakeReport();
    Check(rows.size() == 5, "two slots and two spec variants make three rows: " + std::to_string(rows.size()));
    bool depth_row = false;
    bool at_row = false;
    for (const std::string& row : rows) {
      depth_row |= row.find("slot 1 not a bitmap") != std::string::npos;
      at_row |= row.find("spec variant 1") != std::string::npos;
    }
    Check(depth_row && at_row, "the depth slot says it is not a bitmap and the alpha test draw says its spec variant");
  }

  {
    // Round 3: the same bitmap read after the marker, in one of the menus' views and in a frame with no scene is three rows, each naming where it stood.
    ui_sharp::CensusSample after = Sample(0x5CA2EECD341441FFull, "k_DXT4_5", 128, 128, 3.0f, true);
    ui_sharp::CensusSample view = after;
    view.context = ui_sharp::Context::kMovie;
    ui_sharp::CensusSample none = after;
    none.context = ui_sharp::Context::kNoScene;
    census.Note(after);
    census.Note(view);
    census.Note(none);
    const std::vector<std::string> rows = census.TakeReport();
    Check(rows.size() == 5, "three contexts make three rows: " + std::to_string(rows.size()));
    bool a = false, m = false, n = false;
    for (const std::string& row : rows) {
      a |= row.find("spec variant 0, after the marker;") != std::string::npos;
      m |= row.find("spec variant 0, menu view;") != std::string::npos;
      n |= row.find("spec variant 0, no scene;") != std::string::npos;
    }
    Check(a && m && n, "each row names its context");
  }

  // A texture the game's hooks named is printed with its dictionary.
  ui_sharp::CensusSample named = Sample(0x22ull, "k_DXT4_5", 64, 64, 3.0f, true);
  named.name = "shared_24.dds";
  named.dictionary = "shared_latin";
  census.Note(named);
  const std::vector<std::string> one = census.TakeReport();
  Check(one.size() == 3 && one[2].find("named shared_24.dds in shared_latin") != std::string::npos, "a named texture: " + (one.size() > 2 ? one[2] : std::string()));

  // The report is cut at `limit` and says how many pairs it left out.
  for (uint32_t i = 0; i < 10; ++i) census.Note(Sample(0x100 + i, "k_DXT1", 32 + i, 32, 3.0f, false));
  const std::vector<std::string> cut = census.TakeReport(4);
  Check(cut.size() == 7, "a header, the outcomes, four pairs and the rest's line: " + std::to_string(cut.size()));
  Check(cut.back().find("6 more pairs") != std::string::npos, "the rest is counted: " + cut.back());
}

}  // namespace

int main() {
  CheckStrength();
  CheckConstants();
  CheckBitmapRule();
  CheckDecide();
  CheckCensus();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
