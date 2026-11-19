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
  const ui_sharp::UiSharpConstants off = ui_sharp::ConstantsFor(0.0);
  CheckNum(off.colour, 0.0f, "off: no Catmull-Rom");
  CheckNum(off.edge_gain, 0.0f, "off: no alpha edge");
  const ui_sharp::UiSharpConstants on = ui_sharp::ConstantsFor(1.0);
  CheckNum(on.colour, 1.0f, "on: all Catmull-Rom");
  CheckNum(on.edge_gain, 1.0f, "on: the edge gain follows the magnification once");
  CheckNum(on.min_magnification, 1.25f, "the magnification the helper starts at");
  const ui_sharp::UiSharpConstants half = ui_sharp::ConstantsFor(0.5);
  CheckNum(half.colour, 0.5f, "0.5: half Catmull-Rom");
  CheckNum(half.edge_gain, 0.5f, "0.5: half the edge gain");
  const ui_sharp::UiSharpConstants hard = ui_sharp::ConstantsFor(2.0);
  CheckNum(hard.colour, 1.0f, "2: the colour stays at full Catmull-Rom");
  CheckNum(hard.edge_gain, 2.0f, "2: the edge gain doubles");
  Check(on == ui_sharp::ConstantsFor(1.0) && !(on == half), "equality");
  Check(sizeof(ui_sharp::UiSharpConstants) == 16, "16 bytes after the shared constants");
}

ui_sharp::DrawFacts Wanted() {
  ui_sharp::DrawFacts f;
  f.ui_phase = true;
  f.color_count = 1;
  f.eight_bit_colour = true;
  f.target_scale = 3.0f;
  f.has_variant = true;
  f.ps_variant = 0;
  f.reads_textures = true;
  f.all_linear = true;
  f.hidden = false;
  return f;
}

void CheckWants() {
  Check(ui_sharp::Wants(1.0, Wanted()), "a UI draw of a textured shader at 4K takes it");
  Check(!ui_sharp::Wants(0.0, Wanted()), "off: no draw takes it");
  {
    ui_sharp::DrawFacts f = Wanted();
    f.ui_phase = false;
    Check(!ui_sharp::Wants(1.0, f), "a scene draw (before the composite marker) never does");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.color_count = 0;
    Check(!ui_sharp::Wants(1.0, f), "a depth-only draw does not");
    f.color_count = 2;
    Check(!ui_sharp::Wants(1.0, f), "nor one into two targets");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.eight_bit_colour = false;
    Check(!ui_sharp::Wants(1.0, f), "a float target (the scene, the HDR images) does not");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.target_scale = 1.0f;
    Check(!ui_sharp::Wants(1.0, f), "at the console's size (720p) nothing is magnified");
    f.target_scale = 1.2f;
    Check(!ui_sharp::Wants(1.0, f), "a render scale of 1.2 stays below 1.25");
    f.target_scale = 1.25f;
    Check(ui_sharp::Wants(1.0, f), "1.25 takes it");
    f.target_scale = 1.5f;
    Check(ui_sharp::Wants(1.0, f), "1080p (1.5) takes it");
    f.target_scale = 3.0f;
    Check(ui_sharp::Wants(1.0, f), "4K (3) takes it");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.has_variant = false;
    Check(!ui_sharp::Wants(1.0, f), "a shader the pack has no variant for keeps the game's");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.ps_variant = 1;
    Check(!ui_sharp::Wants(1.0, f), "an alpha tested draw (spec variant 1) keeps the game's shader: the variants are for spec 0");
    f.ps_variant = 3;
    Check(!ui_sharp::Wants(1.0, f), "a draw that already has another variant keeps it");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.reads_textures = false;
    Check(!ui_sharp::Wants(1.0, f), "a shader with no 2D fetch has nothing to sharpen");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.all_linear = false;
    Check(!ui_sharp::Wants(1.0, f), "a point filtered bitmap stays hard-edged");
  }
  {
    ui_sharp::DrawFacts f = Wanted();
    f.hidden = true;
    Check(!ui_sharp::Wants(1.0, f), "a draw with an empty scissor is not worth a pipeline");
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
  Check(lines.size() == 4, "a header and three (shader, texture) pairs: " + std::to_string(lines.size()));
  if (lines.size() == 4) {
    Check(lines[0].find("3 (shader, texture) pairs") != std::string::npos, "the header counts the pairs: " + lines[0]);
    Check(lines[0].find("9 texture reads") != std::string::npos, "the header counts the reads: " + lines[0]);
    Check(lines[0].find("8 at a target scale of at least 1.25") != std::string::npos, "and the magnified ones: " + lines[0]);
    Check(lines[0].find("7 of them took the variant") != std::string::npos, "and the draws that took the variant: " + lines[0]);
    Check(lines[1].find("64x64") != std::string::npos && lines[1].find("6 reads") != std::string::npos, "the most read pair first: " + lines[1]);
    Check(lines[1].find("5 magnified") != std::string::npos && lines[1].find("5 with the variant") != std::string::npos,
          "one of its six reads was at scale 1: " + lines[1]);
    Check(lines[2].find("128x128") != std::string::npos, "then the 128x128");
    Check(lines[3].find("k_8 1024x1024") != std::string::npos && lines[3].find("unnamed") != std::string::npos, "an unnamed one says so: " + lines[3]);
  }
  Check(census.empty() && !census.Due() && census.frames() == 0, "the window is reset after the report");

  // A texture the game's hooks named is printed with its dictionary.
  ui_sharp::CensusSample named = Sample(0x22ull, "k_DXT4_5", 64, 64, 3.0f, true);
  named.name = "shared_24.dds";
  named.dictionary = "shared_latin";
  census.Note(named);
  const std::vector<std::string> one = census.TakeReport();
  Check(one.size() == 2 && one[1].find("named shared_24.dds in shared_latin") != std::string::npos, "a named texture: " + (one.size() > 1 ? one[1] : std::string()));

  // The report is cut at `limit` and says how many pairs it left out.
  for (uint32_t i = 0; i < 10; ++i) census.Note(Sample(0x100 + i, "k_DXT1", 32 + i, 32, 3.0f, false));
  const std::vector<std::string> cut = census.TakeReport(4);
  Check(cut.size() == 6, "a header, four pairs and the rest's line: " + std::to_string(cut.size()));
  Check(cut.back().find("6 more pairs") != std::string::npos, "the rest is counted: " + cut.back());
}

}  // namespace

int main() {
  CheckStrength();
  CheckConstants();
  CheckWants();
  CheckCensus();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
