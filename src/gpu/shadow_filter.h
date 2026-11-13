#pragma once
// ===========================================================================
// MCLA GPU renderer -- the sun shadow filter (mcla_gpu_shadow_filter)
// ===========================================================================
// The game filters the sun's shadows with four taps at an angle that changes from pixel to pixel: five
// levels of grey along an edge and a grain that the render scale blows up. The shader pack's variant
// "shadow_filter" (src/shaderpack/variants/shadow_filter.h) has the collector's lookup with 8 to 16 taps on
// a disk, turned by noise of the host pixel; this is its host half.
//
//   mcla_gpu_shadow_filter   auto | game | soft | pcss (live; the pause menu's SHADOW FILTER row)
//     game   the game's own shaders: nothing of the variant is selected, the picture is the game's, bit for bit
//     soft   the variant: Vogel taps of the game's radius
//     pcss   contact hardening (package G1c); it is the same as soft until that package is in
//     auto   gpu_class.h's table: game on Low, soft on Mid and High
//
// Pipelines::ChoosePackedVariant asks SoftSelected() per draw; Scene::Prepare uploads the 16 bytes the
// variant reads after the shared constants (ShadowFilterConstants, byte 608 of the draw's SharedConstants
// block: `float4 g_ShadowFilter`) for the draws that use it.
//
// Everything that decides something is a pure inline function here (ParseChoice, ModeFor, TapsFor,
// MakeConstants) and this header includes nothing of the renderer: shadow_filter_test.cpp builds on its own.
// Current(), SoftSelected() and CurrentConstants() (shadow_filter.cpp) add the cvars and the GPU class.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "gpu_class.h"

namespace mcla::gpu::shadow_filter {

enum class Mode : uint8_t { kGame, kSoft };

// What the cvar says. A text the list does not know (a hand edited file) counts as auto.
enum class Choice : uint8_t { kAuto, kGame, kSoft, kPcss };

constexpr Choice ParseChoice(std::string_view text) {
  if (text == "game") return Choice::kGame;
  if (text == "soft") return Choice::kSoft;
  if (text == "pcss") return Choice::kPcss;
  return Choice::kAuto;
}

// What a cell of gpu_class.h's table stands for.
constexpr Mode ModeOfCell(std::string_view cell) {
  return cell == "game" ? Mode::kGame : Mode::kSoft;
}

// The mode a choice comes to on a class: `auto` is the table's cell, `pcss` is soft until package G1c.
constexpr Mode ModeFor(gpu_class::GpuClass cls, Choice choice) {
  switch (choice) {
    case Choice::kGame:
      return Mode::kGame;
    case Choice::kSoft:
    case Choice::kPcss:
      return Mode::kSoft;
    default:
      return ModeOfCell(gpu_class::Cell(gpu_class::Effect::kShadowFilter, cls));
  }
}

// How many taps: 8 on a Low class (an integrated GPU that was given `soft` by hand), 16 on the others (the
// game has 4); mcla_gpu_shadow_filter_taps (debug) forces any number from 4 to 32.
inline constexpr int kTapsLow = 8;
inline constexpr int kTapsOther = 16;
inline constexpr int kMinTaps = 4;
inline constexpr int kMaxTaps = 32;
constexpr int TapsFor(gpu_class::GpuClass cls, int forced_taps) {
  if (forced_taps > 0) return std::clamp(forced_taps, kMinTaps, kMaxTaps);
  return cls == gpu_class::GpuClass::kLow ? kTapsLow : kTapsOther;
}

// The `mode` bits of the constants (hlsl: `uint(g_ShadowFilter.w + 0.5)`).
inline constexpr uint32_t kModeGather = 1u;  // Gather taps (a bilinear weighted compare of 4 texels) instead of the sampler's
inline constexpr uint32_t kModePcss = 2u;    // contact hardening (G1c: nothing reads it yet, never set)

// The 16 bytes after the SharedConstants (constants.h kShadowFilterConstantsOffset), `float4 g_ShadowFilter` in
// the variant's HLSL (shaderpack/variants/shadow_filter.cpp).
struct ShadowFilterConstants {
  float taps;          // x: how many
  float radius_scale;  // y: times the game's radius (1; G1a gives 1 / the atlas scale)
  float noise_frame;   // z: the frame the noise is shifted by (R2, as the occlusion's), 0 for a fixed pattern
  float mode;          // w: kMode* bits
};
static_assert(sizeof(ShadowFilterConstants) == 16);

inline constexpr double kMinRadiusTuning = 0.25;
inline constexpr double kMaxRadiusTuning = 4.0;

// `atlas_scale_inverse` is what the cascade resolution package gives (1 at the game's 1280, 0.5 at 2x); the
// tuning cvar multiplies it. `temporal_frame_valid`: a temporal upscaler works on the frame, so the noise
// moves from frame to frame and the upscaler's history averages it; otherwise the pattern stays where it is.
constexpr ShadowFilterConstants MakeConstants(gpu_class::GpuClass cls, int forced_taps, double radius_tuning,
                                              float atlas_scale_inverse, bool gather, bool temporal_frame_valid,
                                              uint64_t frame) {
  ShadowFilterConstants constants{};
  constants.taps = static_cast<float>(TapsFor(cls, forced_taps));
  constants.radius_scale =
      static_cast<float>(std::clamp(radius_tuning, kMinRadiusTuning, kMaxRadiusTuning)) * atlas_scale_inverse;
  constants.noise_frame = temporal_frame_valid ? static_cast<float>(frame & 1023u) : 0.0f;
  constants.mode = static_cast<float>(gather ? kModeGather : 0u);
  return constants;
}

// --- the renderer's side (shadow_filter.cpp) ---------------------------------------------------

// The mode in force: the cvar's choice with `auto` resolved for the GPU class (the weakest class until the
// class is known: gpu_class::Known()). Any thread, cheap; a change of the cvar is seen at once.
Mode CurrentMode();
inline bool SoftSelected() { return CurrentMode() == Mode::kSoft; }

// The constants for a draw that uses the variant. `temporal_upscaler`: one works on this frame (Scene::UpscalerRunning);
// `frame` a number that changes every frame (the scene's frame serial).
ShadowFilterConstants CurrentConstants(bool temporal_upscaler, uint64_t frame);

// The cascade resolution package (G1a) says how much finer the atlas is than the game's: 1 / its scale, so the
// filter keeps its width in texels (and is that much narrower in the world). 1 until it is set. Any thread.
void SetAtlasRadiusScale(float scale_inverse);

}  // namespace mcla::gpu::shadow_filter
