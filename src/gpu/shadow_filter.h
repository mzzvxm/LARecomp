#pragma once
// ===========================================================================
// MCLA GPU renderer -- the sun shadow filter (mcla_gpu_shadow_filter)
// ===========================================================================
// The game filters the sun's shadows with four taps at an angle that changes from pixel to pixel: five
// levels of grey along an edge and a grain that the render scale blows up. The shader pack's variant
// "shadow_filter" (src/shaderpack/variants/shadow_filter.h) has the collector's lookup with 16 taps on
// a disk, turned by noise of the host pixel while a temporal upscaler runs (and not at all otherwise); this is its host half.
//
//   mcla_gpu_shadow_filter   auto | game | soft | pcss (live; the pause menu's SHADOW FILTER row)
//     game   the game's own shaders: nothing of the variant is selected, the picture is the game's, bit for bit
//     soft   the variant: Vogel taps of the game's radius
//     pcss   contact hardening (package G1c): the same variant and pipeline with a blocker search in front (8
//            taps) and the kernel that the sun's size gives at the distance of the blockers: sharp where a
//            shadow touches its caster, softer with distance. Unanimous probes can skip the remaining taps.
//     auto   pcss on all devices
//
// pipelines.cpp WantedFeatures asks VariantSelected() per draw; Scene::Prepare uploads the 16 bytes the
// variant reads after the shared constants (ShadowFilterConstants, byte 608 of the draw's SharedConstants
// block: `float4 g_ShadowFilter`) for the draws that use it.
//
// Everything that decides something is a pure inline function here (ParseChoice, ModeFor, TapsFor,
// MakeConstants) and this header includes nothing of the renderer: shadow_filter_test.cpp builds on its own.
// CurrentMode(), VariantSelected() and CurrentConstants() (shadow_filter.cpp) read the settings.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "scale.h"

namespace mcla::gpu::shadow_filter {

enum class Mode : uint8_t { kGame, kSoft, kPcss };

// What the cvar says. A text the list does not know (a hand edited file) counts as auto.
enum class Choice : uint8_t { kAuto, kGame, kSoft, kPcss };

constexpr Choice ParseChoice(std::string_view text) {
  if (text == "game") return Choice::kGame;
  if (text == "soft") return Choice::kSoft;
  if (text == "pcss") return Choice::kPcss;
  return Choice::kAuto;
}

constexpr Mode ModeFor(Choice choice) {
  switch (choice) {
    case Choice::kGame: return Mode::kGame;
    case Choice::kSoft: return Mode::kSoft;
    default: return Mode::kPcss;
  }
}

// The debug setting can override the sample count for either filter.
inline constexpr int kTaps = 16;
inline constexpr int kMinTaps = 4;
inline constexpr int kMaxTaps = 32;
constexpr int TapsFor(int forced_taps) {
  return forced_taps > 0 ? std::clamp(forced_taps, kMinTaps, kMaxTaps) : kTaps;
}

// The `mode` bits of the constants (hlsl: `uint(g_ShadowFilter.w + 0.5)`).
inline constexpr uint32_t kModeGather = 1u;  // true PCF taps (a bilinear weighted compare of 4 texels, each at the receiver's plane) instead of the sampler's
inline constexpr uint32_t kModePcss = 2u;    // contact hardening (G1c): the blocker search and the penumbra's kernel
inline constexpr uint32_t kModeNoise = 4u;   // the disk is turned per pixel by noise (interleaved gradient noise + the R2 shift of noise_frame)
inline constexpr uint32_t kModeEarlyOut = 8u;  // unanimous PCSS blocker and filter probes
inline constexpr uint32_t kModeBlueNoise = 32u;  // with kModeNoise: that noise is the blue noise tile stepped along the R2 sequence by noise_frame (mcla_gpu_noise_blue), not the first version's gradient noise, a lattice
// With kModePcss, bits 8 to 15 hold the sun's size, in sixteenths of its real angular radius (0.27 degrees). The real
// sun's penumbra is under one texel of the nearest cascade for every caster lower than about 3 m (6 m at the second
// cascade), so a sun that shows at a car's height is bigger than the real one: 4 times by default (to be decided
// on pictures: mcla_gpu_shadow_filter_sun).
inline constexpr uint32_t kSunShift = 8u;
inline constexpr double kSunUnit = 1.0 / 16.0;
inline constexpr double kMinSunTuning = 0.25;
inline constexpr double kMaxSunTuning = 15.0;
inline constexpr double kDefaultSunTuning = 4.0;
// The sun's size as the bits the shader reads: sixteenths, 4 (a quarter of the real sun) to 240 (15 times).
constexpr uint32_t SunBits(double sun_tuning) {
  const double clamped = std::clamp(sun_tuning, kMinSunTuning, kMaxSunTuning);
  return static_cast<uint32_t>(clamped / kSunUnit + 0.5);
}

// The 16 bytes after the SharedConstants (constants.h kShadowFilterConstantsOffset), `float4 g_ShadowFilter` in
// the variant's HLSL (shaderpack/variants/shadow_filter.cpp).
struct ShadowFilterConstants {
  float taps;          // x: how many
  float radius_scale;  // y: times the game's radius (1; G1a gives 1 / the atlas scale)
  float noise_frame;   // z: the frame the noise is shifted by (R2, as the occlusion's), 0 for a fixed pattern
  float mode;          // w: kMode* bits, and with kModePcss the sun's size from bit kSunShift
};
static_assert(sizeof(ShadowFilterConstants) == 16);

inline constexpr double kMinRadiusTuning = 0.25;
inline constexpr double kMaxRadiusTuning = 4.0;

// What the cascade resolution (G1a, side_view_shadow.cpp) gives the radius: 1 / the scale the cascade atlas is made
// at (1 at the game's 1280x1280, 0.5 at 2x, 1/3 at 3x), so the filter keeps its width in ATLAS TEXELS and its edge is
// that much narrower in the world. The game's radius is count / 1280 in UV, which a finer atlas reads as it is: the
// same width in the world, so without this the soft filter at 2x would be 2x as wide in texels and no sharper than
// the game's. Never above 1: no setting makes an atlas coarser than the game's.
constexpr float AtlasRadiusScale(Scale atlas_scale) {
  return atlas_scale.n <= Scale::kDenominator ? 1.0f
                                              : float(Scale::kDenominator) / float(atlas_scale.n);
}

// `atlas_scale_inverse` is what the cascade resolution package gives (AtlasRadiusScale: 1 at the game's 1280, 0.5 at
// 2x); the tuning cvar multiplies it. `temporal_frame_valid`: a temporal upscaler works on the frame, so the noise
// moves from frame to frame and the upscaler's history averages it (kModeNoise); otherwise there is NO noise: a
// per pixel turn of the disk that nothing averages is a grain on every shadow edge (the owner's "dithering", 2026-11-19),
// so every pixel takes the same disk and the edge is smooth.
// `pcss`: contact hardening with the sun `sun_tuning` times its real size.
constexpr ShadowFilterConstants MakeConstants(int forced_taps, double radius_tuning,
                                              float atlas_scale_inverse, bool gather, bool temporal_frame_valid,
                                              uint64_t frame, bool pcss = false,
                                              double sun_tuning = kDefaultSunTuning, bool blue_noise = false,
                                              bool early_out = false) {
  ShadowFilterConstants constants{};
  constants.taps = static_cast<float>(TapsFor(forced_taps));
  constants.radius_scale =
      static_cast<float>(std::clamp(radius_tuning, kMinRadiusTuning, kMaxRadiusTuning)) * atlas_scale_inverse;
  constants.noise_frame = temporal_frame_valid ? static_cast<float>(frame & 1023u) : 0.0f;
  uint32_t mode = gather ? kModeGather : 0u;
  if (temporal_frame_valid) mode |= kModeNoise | (blue_noise ? kModeBlueNoise : 0u);
  if (pcss) mode |= kModePcss | (SunBits(sun_tuning) << kSunShift) | (early_out ? kModeEarlyOut : 0u);
  constants.mode = static_cast<float>(mode);
  return constants;
}

// --- the renderer's side (shadow_filter.cpp) ---------------------------------------------------

// The mode in force. Any thread, cheap; a setting change is seen at once.
Mode CurrentMode();
// The variant is drawn (soft or pcss: one shader, the constants tell them apart); `game` selects nothing.
inline bool VariantSelected() { return CurrentMode() != Mode::kGame; }

// The constants for a draw that uses the variant. `temporal_upscaler`: one works on this frame (Scene::UpscalerRunning);
// `frame` a number that changes every frame (the scene's frame serial).
ShadowFilterConstants CurrentConstants(bool temporal_upscaler, uint64_t frame);

// The cascade resolution package (G1a) says how much finer the atlas is than the game's: 1 / its scale
// (AtlasRadiusScale), so the filter keeps its width in texels (and is that much narrower in the world). 1 until it
// is set; Scene::FillVariantBlock sets it from the scale the cascades are made at now. Any thread.
void SetAtlasRadiusScale(float scale_inverse);

}  // namespace mcla::gpu::shadow_filter
