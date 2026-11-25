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
#include <utility>

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
  // The table of GFX-UPGRADES-DESIGN.md section 9: auto is game / pcss / pcss (decided by the owner on 2026-11-19).
  const Mode auto_mode[] = {Mode::kGame, Mode::kPcss, Mode::kPcss};
  for (int i = 0; i < 3; ++i) {
    const std::string at = std::string(" on ") + kClassNames[i];
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kAuto) == auto_mode[i], "auto" + at);
    // game is never overridden (the variant is not selected: the picture is the game's); soft is the variant on
    // every class; pcss is the variant with the blocker search where the class has it (G1c) and soft on Low.
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kGame) == Mode::kGame, "game" + at);
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kSoft) == Mode::kSoft, "soft" + at);
    Check(shadow_filter::ModeFor(kClasses[i], Choice::kPcss) == (i == 0 ? Mode::kSoft : Mode::kPcss),
          "pcss has its search on Mid and High, none on Low" + at);
    Check(shadow_filter::PcssAvailable(kClasses[i]) == (i != 0), "the search is there on Mid and High only" + at);
  }
  Check(shadow_filter::ModeOfCell("game") == Mode::kGame && shadow_filter::ModeOfCell("soft") == Mode::kSoft &&
            shadow_filter::ModeOfCell("pcss") == Mode::kPcss,
        "a cell of the table");
  // The table is game / pcss / pcss (owner, 2026-11-19): the contact hardening on Mid and High, the game's own on Low (a cell that said pcss
  // there would be soft, no search, whatever it said).
  Check(gpu_class::Cell(gpu_class::Effect::kShadowFilter, GpuClass::kHigh) == "pcss" &&
            gpu_class::Cell(gpu_class::Effect::kShadowFilter, GpuClass::kMid) == "pcss" &&
            gpu_class::Cell(gpu_class::Effect::kShadowFilter, GpuClass::kLow) == "game",
        "the table's cells are game on Low, pcss on Mid and High");
  // A handheld (the Steam Deck: kLow with gpu_class.h's kHandheldCells): auto is soft, 8 taps (the Low class's), no blocker search; the other choices
  // are as on Low, and the flag means nothing on Mid or High.
  Check(shadow_filter::ModeFor(GpuClass::kLow, Choice::kAuto, true) == Mode::kSoft, "auto is soft on a handheld");
  Check(shadow_filter::ModeFor(GpuClass::kLow, Choice::kAuto, false) == Mode::kGame, "auto is the game's on a plain Low class");
  Check(shadow_filter::ModeFor(GpuClass::kLow, Choice::kAuto) == Mode::kGame, "the two argument ModeFor is the plain Low class");
  Check(shadow_filter::ModeFor(GpuClass::kLow, Choice::kGame, true) == Mode::kGame, "game is never overridden on a handheld");
  Check(shadow_filter::ModeFor(GpuClass::kLow, Choice::kSoft, true) == Mode::kSoft, "soft on a handheld");
  Check(shadow_filter::ModeFor(GpuClass::kLow, Choice::kPcss, true) == Mode::kSoft, "pcss on a handheld is soft: no blocker search on a Low class");
  Check(shadow_filter::ModeFor(GpuClass::kMid, Choice::kAuto, true) == Mode::kPcss, "the handheld flag changes nothing on Mid");
  Check(shadow_filter::ModeFor(GpuClass::kHigh, Choice::kAuto, true) == Mode::kPcss, "the handheld flag changes nothing on High");
  Check(shadow_filter::TapsFor(GpuClass::kLow, 0) == 8, "a handheld keeps Low's 8 taps");
  // The mode that is selected as a variant: soft and pcss, not game (pipelines.cpp: one shader for both).
  Check(shadow_filter::Mode::kGame != shadow_filter::Mode::kSoft && shadow_filter::Mode::kSoft != shadow_filter::Mode::kPcss,
        "three modes");
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

  // The noise (the owner's "dithering", 2026-11-19): a per pixel turn of the disk is a grain on every edge unless a temporal
  // upscaler's history averages it, so only a frame that has one gets the bit; without it the disk keeps its turn.
  CheckNum(static_cast<uint32_t>(c.mode + 0.5f) & shadow_filter::kModeNoise, 0, "no upscaler: no noise bit (the disk is the same on every pixel)");
  CheckNum(shadow_filter::kModeNoise, 4, "kModeNoise is 4 (the HLSL reads `& 4u`)");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, true, 77);
  CheckNum(c.mode, shadow_filter::kModeNoise, "an upscaler: the noise bit");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, true, true, 77, true, 4.0);
  CheckNum(c.mode, 1 + 2 + 4 + 64 * 256, "an upscaler with pcss and Gather taps: bits 0, 1 and 2 and 64 sixteenths");

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
  CheckNum(shadow_filter::kModePcss, 2, "kModePcss is 2 (the HLSL reads `& 2u`)");
  CheckNum(shadow_filter::kSunShift, 8, "the sun's size is from bit 8 (the HLSL reads `mode >> 8u`)");
  c = shadow_filter::MakeConstants(GpuClass::kLow, 0, 1.0, 1.0f, false, false, 0);
  CheckNum(c.taps, 8, "Low: 8 taps");
  CheckNum(c.mode, 0, "Low without pcss: no mode bit");

  // Contact hardening (G1c): the bit and the sun's size in sixteenths of the real sun, from bit 8; Gather keeps bit 0.
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, false, 0, true, 1.0);
  CheckNum(c.mode, 2 + 16 * 256, "pcss, the real sun: bit 1 and 16 sixteenths");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, true, false, 0, true, 4.0);
  CheckNum(c.mode, 1 + 2 + 64 * 256, "pcss with Gather taps, a sun of 4: bits 0 and 1 and 64 sixteenths");
  c = shadow_filter::MakeConstants(GpuClass::kMid, 0, 1.0, 1.0f, false, false, 0, true);
  CheckNum(c.mode, 2 + 64 * 256, "pcss on Mid: the default sun is 4 times the real one");
  CheckNum(shadow_filter::kDefaultSunTuning, 4.0, "the default sun (to be decided on pictures)");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, false, 0, false, 4.0);
  CheckNum(c.mode, 0, "soft: no pcss bit and no sun, whatever the sun cvar says");
  c = shadow_filter::MakeConstants(GpuClass::kLow, 0, 1.0, 1.0f, false, false, 0, true, 4.0);
  CheckNum(c.mode, 0, "Low never gets the search, even when the caller asks");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, false, 0, true, 100.0);
  CheckNum(c.mode, 2 + 240 * 256, "the sun is limited to 15 times (240 sixteenths, inside the 8 bits)");
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, false, false, 0, true, 0.0);
  CheckNum(c.mode, 2 + 4 * 256, "the sun is limited to a quarter (4 sixteenths: never 0, no sun is no penumbra)");
  CheckNum(shadow_filter::SunBits(1.0), 16, "1 times the real sun is 16");
  CheckNum(shadow_filter::SunBits(2.53), 40, "2.53 rounds to 40 sixteenths");
  // The mode word is an exact integer in a float for every value it can take (the shader adds 0.5 and truncates).
  for (const bool gather : {false, true}) {
    for (const double sun : {0.25, 1.0, 4.0, 15.0}) {
      const shadow_filter::ShadowFilterConstants k =
          shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 1.0f, gather, false, 0, true, sun);
      const uint32_t word = static_cast<uint32_t>(k.mode + 0.5f);
      Check((word & 1u) == (gather ? 1u : 0u) && (word & 2u) == 2u && (word >> 8) == shadow_filter::SunBits(sun) &&
                (word & 0xF8u) == 0,
            "the mode word of pcss with sun " + std::to_string(sun) + " reads back as its bits");
    }
  }
  // The radius scale and the taps do not depend on the contact hardening (the PCF below it is the soft filter's).
  c = shadow_filter::MakeConstants(GpuClass::kHigh, 0, 1.0, 0.5f, false, false, 0, true, 4.0);
  CheckNum(c.radius_scale, 0.5, "pcss at a 2x atlas: the soft radius's scale is the same (the cap and the floor follow it)");
  CheckNum(c.taps, 16, "pcss: 16 taps for the kernel on High");

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

// G1a: the radius follows the atlas's scale (side_view_shadow.cpp): 1 / scale, so the filter keeps its width in
// atlas texels. The scales are the class's: 1x, 2x, 3x (high, ultra) and the grid's steps in between.
void CheckAtlasRadius() {
  CheckNum(shadow_filter::AtlasRadiusScale(Scale{}), 1.0, "the game's 1280 atlas: the game's radius");
  CheckNum(shadow_filter::AtlasRadiusScale(Scale::Whole(2)), 0.5, "high (2x, 2560): half the radius");
  Check(shadow_filter::AtlasRadiusScale(Scale::Whole(3)) > 0.3333f &&
            shadow_filter::AtlasRadiusScale(Scale::Whole(3)) < 0.3334f,
        "ultra (3x, 3840): a third of the radius");
  CheckNum(shadow_filter::AtlasRadiusScale(Scale{30}), 20.0f / 30.0f, "1.5x: two thirds");
  // Never above the game's radius, and a missing scale is the game's: a coarser atlas is not made.
  CheckNum(shadow_filter::AtlasRadiusScale(Scale{10}), 1.0, "0.5x is clamped to the game's radius");
  CheckNum(shadow_filter::AtlasRadiusScale(Scale::None()), 1.0, "no scale is the game's radius");
  // Through the constants: the soft filter's radius at each setting, tuning 1.
  for (const auto& [scale, want] : {std::pair{Scale{}, 1.0}, {Scale::Whole(2), 0.5}, {Scale::Whole(3), 1.0 / 3.0}}) {
    const shadow_filter::ShadowFilterConstants c = shadow_filter::MakeConstants(
        GpuClass::kHigh, 0, 1.0, shadow_filter::AtlasRadiusScale(scale), false, false, 0);
    Check(c.radius_scale > want - 1e-6 && c.radius_scale < want + 1e-6,
          "the constants' radius at " + scale.Text() + "x");
  }
  // The width in atlas texels is the same at every scale: the game's radius is count / 1280 UV, an atlas
  // of 1280 x scale texels sees it as count x scale texels, and the radius scale takes the scale out again.
  for (const Scale scale : {Scale{}, Scale::Whole(2), Scale::Whole(3)}) {
    const double texels = (1.0 / 1280.0) * (1280.0 * scale.value()) * shadow_filter::AtlasRadiusScale(scale);
    Check(texels > 1.0 - 1e-6 && texels < 1.0 + 1e-6, "one game texel of radius is one atlas texel at " + scale.Text() + "x");
  }
}

}  // namespace

int main() {
  CheckChoice();
  CheckModes();
  CheckTaps();
  CheckConstants();
  CheckAtlasRadius();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
