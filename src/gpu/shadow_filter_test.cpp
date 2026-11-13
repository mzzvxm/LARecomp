// Check of the sun shadow filter's rules (shadow_filter.h). A program of its own: it needs no renderer, no
// ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_shadow_filter_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_shadow_filter_rules_test
//
// The expected values are written out here again on purpose: GFX-UPGRADES-DESIGN.md's G1b row is the other
// side of the check (8 taps on Low, 16 otherwise; `game` selects nothing; `auto` follows the class table).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "shadow_filter.h"

using namespace mcla::gpu;
using gpu_class::GpuClass;
using shadow_filter::Choice;
using shadow_filter::Mode;

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

const char* const kClassNames[] = {"low", "mid", "high"};
const GpuClass kClasses[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};

void CheckChoice() {
  Check(shadow_filter::ParseChoice("auto") == Choice::kAuto, "auto");
  Check(shadow_filter::ParseChoice("game") == Choice::kGame, "game");
  Check(shadow_filter::ParseChoice("soft") == Choice::kSoft, "soft");
  Check(shadow_filter::ParseChoice("pcss") == Choice::kPcss, "pcss");
  // What the list does not know (a hand edited file) counts as auto; the match is exact (the caller lower-cases).
  for (const char* text : {"", "Auto", "SOFT", "off", "high", "soft ", "pcss2"}) {
    Check(shadow_filter::ParseChoice(text) == Choice::kAuto, std::string("'") + text + "' counts as auto");
  }
}

void CheckModes() {
  // The table of GFX-UPGRADES-DESIGN.md section 9: auto is game / soft / soft.
  const Mode auto_mode[] = {Mode::kGame, Mode::kSoft, Mode::kSoft};
  for (int i = 0; i < 3; ++i) {
    const std::string at = std::string(" on ") + kClassNames[i];
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kAuto) == auto_mode[i], "auto" + at);
    // game is never overridden (the variant is not selected: the picture is the game's); soft and pcss are the
    // variant on every class (pcss is soft until G1c).
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kGame) == Mode::kGame, "game" + at);
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kSoft) == Mode::kSoft, "soft" + at);
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kPcss) == Mode::kSoft, "pcss is soft" + at);
  }
  Check(shadow_filter::ModeOfCell("game") == Mode::kGame && shadow_filter::ModeOfCell("soft") == Mode::kSoft,
        "a cell of the table");
}

void CheckTaps() {
  CheckNum(shadow_filter::TapsFor(GpuClass::kLow, 0), 8, "8 taps on Low");
  CheckNum(shadow_filter::TapsFor(GpuClass::kMid, 0), 16, "16 taps on Mid");
  CheckNum(shadow_filter::TapsFor(GpuClass::kHigh, 0), 16, "16 taps on High");
  CheckNum(shadow_filter::TapsFor(GpuClass::kHigh, 12), 12, "forced 12");
  CheckNum(shadow_filter::TapsFor(GpuClass::kLow, 32), 32, "forced 32 on Low");
  CheckNum(shadow_filter::TapsFor(GpuClass::kHigh, 2), 4, "forced 2 is raised to 4");
  CheckNum(shadow_filter::TapsFor(GpuClass::kHigh, 99), 32, "forced 99 is lowered to 32");
  CheckNum(shadow_filter::TapsFor(GpuClass::kHigh, -3), 16, "a negative number is no force");
}

void CheckConstants() {
  static_assert(sizeof(shadow_filter::ShadowFilterConstants) == 16);
  static_assert(offsetof(shadow_filter::ShadowFilterConstants, taps) == 0);
  static_assert(offsetof(shadow_filter::ShadowFilterConstants, radius_scale) == 4);
  static_assert(offsetof(shadow_filter::ShadowFilterConstants, noise_frame) == 8);
  static_assert(offsetof(shadow_filter::ShadowFilterConstants, mode) == 12);

  // The defaults: the game's radius, a fixed pattern, the sampler's own taps.
  shadow_filter::ShadowFilterConstants c =
      shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, false, 77);
  CheckNum(c.taps, 16, "defaults: 16 taps");
  CheckNum(c.radius_scale, 1.0, "defaults: the game's radius");
  CheckNum(c.noise_frame, 0, "defaults: no upscaler: the frame is not used");
  CheckNum(c.mode, 0, "defaults: bilinear taps");

  // A temporal upscaler: the frame number, wrapped at 1024 (exact in a float).
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, true, 77);
  CheckNum(c.noise_frame, 77, "an upscaler: the frame");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, true, 1024 + 5);
  CheckNum(c.noise_frame, 5, "an upscaler: the frame wraps at 1024");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, true, 0xFFFFFFFFFFFFFFFFull);
  CheckNum(c.noise_frame, 1023, "an upscaler: the largest frame");

  // The mode bits.
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, true, false, 0);
  CheckNum(c.mode, shadow_filter::kModeGather, "Gather taps set the mode's bit 0");
  CheckNum(shadow_filter::kModeGather, 1, "kModeGather is 1 (the HLSL reads `& 1u`)");
  CheckNum(shadow_filter::kModePcss, 2, "kModePcss is 2 (reserved for G1c)");
  c = shadow_filter::MakeConstants(GpuClass::kLow, 0, 1.0, 1.0f, false, false, 0);
  CheckNum(c.taps, 8, "Low: 8 taps");

  // The radius: the tuning cvar times the atlas's 1 / scale (G1a), the tuning limited to 0.25..4.
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 2.0, 0.5f, false, false, 0);
  CheckNum(c.radius_scale, 1.0, "tuning 2 at a 2x atlas: back to the game's width in texels");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 100.0, 1.0f, false, false, 0);
  CheckNum(c.radius_scale, 4.0, "tuning is limited to 4");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 0.0, 1.0f, false, false, 0);
  CheckNum(c.radius_scale, 0.25, "tuning is limited to 0.25 (never 0: the helper divides by nothing, but a zero disk is no filter)");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 6, 1.0, 1.0f / 3.0f, false, false, 0);
  CheckNum(c.taps, 6, "forced taps");
  Check(c.radius_scale > 0.333f && c.radius_scale < 0.3334f, "a 3x atlas: a third of the radius");

  // The block is bytes the shader reads as float4: no padding.
  std::uint8_t bytes[16];
  std::memcpy(bytes, &c, sizeof(c));
  float back[4];
  std::memcpy(back, bytes, sizeof(back));
  CheckNum(back[0], 6, "the block read as four floats: taps");
}

}  // namespace

int main() {
  CheckChoice();
  CheckModes();
  CheckTaps();
  CheckConstants();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
