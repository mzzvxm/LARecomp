#pragma once
// ===========================================================================
// MCLA GPU renderer -- the resolve copy's conversions, and the pipelines the host builder lists for them
// ===========================================================================
// A resolve into a guest texture (ResolveCache::Execute) is one fullscreen pass: a pixel shader picked
// by (depth or colour source, a source with more than one host sample or not), drawn into a product
// texture in the host format of the destination's guest format. The pipeline is picked by those three
// things alone: there is NO sample count in it, so a source of 2, 4 or 8 samples (the MSAA level, or
// the game's own count) all use the one multisampled pipeline, and a start never makes a pipeline for
// an MSAA level it does not run.
//
// The destinations the pass can convert into are the rows of CopyConversion below (the switch Execute
// had, moved here so that the precache list reads the same table: a destination added there is listed
// at the next start, and cannot be left out again), and their host formats are the ones format.h gives
// the guest format (format::TextureHost). ListedCopies makes the list of pipelines for the host builder
// (host_precache.h) out of both: first the variants a drive was seen to ask for, the likeliest first
// (kLikeliest), then whatever the table gives that is not in it yet.
//
// Pure: it includes only format.h (format.cpp for the table), nothing of the renderer, so
// resolve_variants_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include "format.h"

namespace mcla::gpu::resolve_variants {

// The conversions, as resolve_ps.hlsl numbers them (the shader's numbers: change one, change the other).
enum Mode : uint32_t {
  kModeUnorm8888 = 0,
  kModeUnorm1555 = 1,
  kModeUnorm8 = 2,
  kModeFloat = 3,
  kModeRed32 = 4,
  kModeDepthFloat = 5,
  kModeDepthRaw8888 = 6,
  kModeDepthFloatBits = 7,
  kModeFloat32 = 8,  // k_32_32_FLOAT: X and Y as 32-bit floats
  kModeCount,        // as a result of CopyConversion: no conversion
};

// What the copy into a destination of one guest texture format does.
struct Conversion {
  Mode mode = kModeCount;
  bool dest_float24 = false;  // the destination reads the depth as a 24-bit float (ResolveCache's kFlagDestFloat24)
};

// The conversion for a destination of guest texture format `guest_format` (xenos::TextureFormat's number)
// read from a depth or a colour source; mode kModeCount when the pass has none (a pair the guest
// does not resolve: Execute then fails the copy with Failure::kConversion).
inline Conversion CopyConversion(uint32_t guest_format, bool depth_source) {
  switch (guest_format) {
    case 6:  // k_8_8_8_8
      return {depth_source ? kModeDepthRaw8888 : kModeUnorm8888, false};
    case 3:  // k_1_5_5_5
      return {depth_source ? kModeCount : kModeUnorm1555, false};
    case 2:  // k_8
      return {depth_source ? kModeCount : kModeUnorm8, false};
    case 29:  // k_16_16_16_16_EXPAND
    case 32:  // k_16_16_16_16_FLOAT
      return {depth_source ? kModeCount : kModeFloat, false};
    case 36:  // k_32_FLOAT
      return {depth_source ? kModeDepthFloatBits : kModeRed32, false};
    case 37:  // k_32_32_FLOAT
      return {depth_source ? kModeCount : kModeFloat32, false};
    case 22:  // k_24_8
      return {depth_source ? kModeDepthFloat : kModeCount, false};
    case 23:  // k_24_8_FLOAT
      return {depth_source ? kModeDepthFloat : kModeCount, depth_source};
    default:
      return {};
  }
}

// One pipeline of the copy pass.
struct Variant {
  bool depth;  // a depth source (else colour)
  bool msaa;   // a source with more than one host sample
  plume::RenderFormat format;  // the host format of the product (the pipeline's render target)

  friend constexpr bool operator==(const Variant&, const Variant&) = default;
};

// The shader's UNORM8 load and store round-trip every byte when no conversion flag,
// sample averaging, bias, or half-pixel fill changes it. Limit the copy to mip 0,
// whose extent is the scaled guest extent even at fractional scales.
constexpr bool CanCopyRgba8(Variant variant, plume::RenderFormat source, uint32_t mip,
                            uint32_t mode, float scale, uint32_t fill) {
  constexpr auto rgba8 = plume::RenderFormat::R8G8B8A8_UNORM;
  return !variant.depth && !variant.msaa && source == rgba8 && variant.format == rgba8 &&
         mip == 0 && mode == kModeUnorm8888 && scale == 1.0f && fill == 0;
}

// SV_Position in the shader indexes the destination scissor plus this signed
// offset. At fractional scales rounding can push an edge outside the source.
inline bool Rgba8CopyBox(plume::RenderRect rect, int32_t offset_x, int32_t offset_y,
                            uint32_t source_width, uint32_t source_height, plume::RenderBox& box) {
  const int32_t left = rect.left + offset_x;
  const int32_t top = rect.top + offset_y;
  const int32_t right = rect.right + offset_x;
  const int32_t bottom = rect.bottom + offset_y;
  if (left < 0 || top < 0 || right <= left || bottom <= top ||
      uint32_t(right) > source_width || uint32_t(bottom) > source_height) {
    return false;
  }
  box = plume::RenderBox(left, top, right, bottom);
  return true;
}

// A copy pipeline's key in ResolveCache::pipelines_ and in the host precache (its variant word). The
// format fits in 8 bits (resolve_variants_test.cpp checks it for every listed one).
constexpr uint32_t PipelineKeyOf(bool depth, bool msaa, plume::RenderFormat format) {
  return (uint32_t(depth) << 9) | (uint32_t(msaa) << 8) | uint32_t(format);
}
constexpr uint32_t PipelineKeyOf(const Variant& v) {
  return PipelineKeyOf(v.depth, v.msaa, v.format);
}

// The guest texture formats are 6 bits (xenos::TextureFormat).
inline constexpr uint32_t kGuestFormatCount = 64;

// Every variant the copy pass can be asked for: each (source kind, host format) the table gives, with
// the multisampled variant first and then the plain one, in guest format order (colour before depth),
// no repeats (k_16_16_16_16_EXPAND and _FLOAT are one host format, k_24_8 and k_24_8_FLOAT too).
inline std::vector<Variant> TableVariants() {
  std::vector<Variant> list;
  for (const bool depth : {false, true}) {
    for (uint32_t guest = 0; guest < kGuestFormatCount; ++guest) {
      if (CopyConversion(guest, depth).mode == kModeCount) {
        continue;
      }
      const format::TextureHostFormat host = format::TextureHost(guest);
      if (host.status != format::Status::kOk) {
        continue;  // Execute fails such a copy too
      }
      for (const bool msaa : {true, false}) {
        const Variant v{depth, msaa, host.format};
        if (std::find(list.begin(), list.end(), v) == list.end()) {
          list.push_back(v);
        }
      }
    }
  }
  return list;
}

// The variants listed first, the likeliest first. The first six are the labels the logs of the measured
// drives show (MCLA, 1280x720 products); the seventh is the copy into k_1_5_5_5, which two runs of the
// starter verify round (2026-11-17: the High circuit race, the High menus at night) made on the render
// thread when it was not listed, once, as the game view's first scene begins; the rest are the same
// conversions at the other source kind and the formats of the rarer targets. Nothing is here that the
// table does not give (the test checks both ways), and ListedCopies adds what is not here.
inline constexpr plume::RenderFormat kRgba8 = plume::RenderFormat::R8G8B8A8_UNORM;
inline constexpr plume::RenderFormat kRgba16F = plume::RenderFormat::R16G16B16A16_FLOAT;
inline constexpr plume::RenderFormat kR32F = plume::RenderFormat::R32_FLOAT;
inline constexpr plume::RenderFormat kRg32F = plume::RenderFormat::R32G32_FLOAT;
inline constexpr plume::RenderFormat kR8 = plume::RenderFormat::R8_UNORM;
inline constexpr plume::RenderFormat kBgr5A1 = plume::RenderFormat::B5G5R5A1_UNORM;
inline constexpr Variant kLikeliest[] = {
    {false, true, kRgba8},
    {true, true, kR32F},
    {false, true, kRgba16F},
    {false, false, kRgba8},
    {false, false, kRgba16F},
    {true, true, kRgba8},
    {false, true, kBgr5A1},
    {false, false, kR32F},
    {false, true, kR32F},
    {true, false, kR32F},
    {true, false, kRgba8},
    {false, false, kRg32F},
    {false, true, kRg32F},
    {false, false, kR8},
    {false, true, kR8},
    {false, false, kBgr5A1},
};

// The copy pipelines to list for the host builder: kLikeliest in its order, then every variant of the
// table it does not hold, in the table's order.
inline std::vector<Variant> ListedCopies() {
  std::vector<Variant> list(std::begin(kLikeliest), std::end(kLikeliest));
  for (const Variant& v : TableVariants()) {
    if (std::find(list.begin(), list.end(), v) == list.end()) {
      list.push_back(v);
    }
  }
  return list;
}

}  // namespace mcla::gpu::resolve_variants
