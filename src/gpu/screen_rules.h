#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen-space products: the host's pure rules (no GPU)
// ===========================================================================
// screen_shared.cpp asks here what the frame does (which products the owner makes, whether the AO's trigger has to run for them), how big the Hi-Z chain is and the C++ twin of
// hlsl/screen_hiz.hlsli (the Hi-Z's two operations); screen_shared_test.cpp checks the rules and compares the twin with the .hlsli compiled as C++. The frozen interface is
// screen_frame.h; this file may change.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "screen_frame.h"

namespace mcla::gpu::screen {

// --- what the owner makes itself ----------------------------------------------------------------------------------------------------------

// The products ScreenShared makes in SS-0 (the others are the AO's, adopted: published when the AO's frame has them). SS-1 adds colour_post and history, SS-2 the rest.
inline constexpr uint32_t kMadeHere = 1u << kHizMin;

// The consumers whose own passes make lin_depth, normal and colour_pre today (ao.cpp): a frame that runs for them runs the depth chain anyway.
constexpr bool IsAoFamily(Consumer consumer) {
  return consumer == Consumer::kAmbientOcclusion || consumer == Consumer::kContactShadows || consumer == Consumer::kBounceLight;
}

// The products that need the depth chain to run in a frame of their own (a consumer outside the AO's family wants them while the AO and the contact shadows are off).
inline constexpr uint32_t kNeedsDepthChain = (1u << kLinDepth) | (1u << kHizMin);

// What a view needs.
constexpr uint32_t ViewNeeds(View view) {
  switch (view) {
    case View::kLinDepth: return 1u << kLinDepth;
    case View::kHizMin: return 1u << kHizMin;
    case View::kHizCheck: return (1u << kLinDepth) | (1u << kHizMin);
    case View::kNormal: return 1u << kNormal;
    case View::kVehicle: return 1u << kLinDepth;
    case View::kOff:
    case View::kCount: break;
  }
  return 0u;
}

inline View ViewFromSetting(int value) {
  return value > 0 && value < int(View::kCount) ? View(value) : View::kOff;
}
inline uint32_t LevelFromSetting(int value) { return uint32_t(std::clamp(value, 0, int(kHizMaxLevels) - 1)); }

// One frame's plan, made at the frame's start from the consumers' wants and the debug view.
struct Plan {
  uint32_t wanted = 0;        // every product some consumer (or the debug view) asked for
  uint32_t external = 0;      // ... the part asked for by consumers outside the AO's family (+ the debug view)
  uint32_t make = 0;          // the products this module makes this frame (wanted & kMadeHere)
  View view = View::kOff;
  uint32_t view_level = 0;    // the Hi-Z level the views show (clamped to the chain's levels when drawn)

  // True when the depth chain has to run for these products although the AO and the contact shadows are off.
  bool NeedsDepthChain() const { return (external & kNeedsDepthChain) != 0; }
};

inline Plan PlanFrame(const Wants& wants, View view, uint32_t view_level) {
  Plan plan;
  plan.view = view;
  plan.view_level = view_level;
  uint32_t wanted = 0;
  uint32_t external = ViewNeeds(view);  // the debug view is a consumer of its own
  for (uint32_t c = 0; c < uint32_t(Consumer::kCount); ++c) {
    const Consumer consumer = Consumer(c);
    if (consumer == Consumer::kDebugView) continue;  // set by the setting only
    wanted |= wants.Of(consumer);
    if (!IsAoFamily(consumer)) external |= wants.Of(consumer);
  }
  wanted |= ViewNeeds(view);
  plan.wanted = wanted & kAllProducts;
  plan.external = external & kAllProducts;
  plan.make = plan.wanted & kMadeHere;
  return plan;
}

// --- the Hi-Z chain ---------------------------------------------------------------------------------------------------------------------

inline constexpr uint32_t kBytesR32 = 4;  // R32_FLOAT

constexpr uint64_t HizBytes(uint32_t width, uint32_t height) {
  uint64_t total = 0;
  const uint32_t levels = HizLevelCount(width, height);
  for (uint32_t level = 0; level < levels; ++level) {
    total += uint64_t(HizLevelSize(width, level)) * uint64_t(HizLevelSize(height, level)) * kBytesR32;
  }
  return total;
}

// --- the published frame ----------------------------------------------------------------------------------------------------------------

// What the owner hands over for one frame, as plain numbers (ScreenShared::Publish turns the images into this; kNoSlot for what the frame does not have), and what makes the
// frame's `valid` and `generation`.
struct FrameInputs {
  uint32_t serial = 0;
  uint32_t flags = 0;                       // kFrame*
  uint32_t traced[2] = {0, 0};
  uint32_t render[2] = {0, 0};
  uint32_t host_samples = 1;
  uint32_t sample0 = 0;
  uint32_t depth[kDepthLevels] = {kNoSlot, kNoSlot, kNoSlot};
  uint32_t depth_full = kNoSlot;
  uint32_t normal = kNoSlot;
  uint32_t colour_pre[kColourPreLevels] = {kNoSlot, kNoSlot, kNoSlot};
  uint32_t hiz_levels = 0;                  // 0: no Hi-Z this frame
  uint32_t hiz[kHizMaxLevels] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint64_t owner_generation = 0;            // the images' owner's own counter (the AO's: bumped when its images go)
  uint64_t hiz_generation = 0;              // ScreenShared's: bumped when the Hi-Z's images are made again
  ScreenCamera camera;
};

// The frame a consumer reads. A product is valid when its level 0 slot is there; the Hi-Z only with all its levels.
inline ScreenFrame MakeFrame(const FrameInputs& in, uint32_t generation) {
  ScreenFrame f;
  f.generation = generation;
  f.frame_serial = in.serial;
  f.flags = in.flags;
  f.traced = {in.traced[0], in.traced[1]};
  f.render = {in.render[0], in.render[1]};
  f.host_samples = in.host_samples;
  f.sample0 = in.sample0;
  if (in.depth[0] != kNoSlot) {
    f.valid |= ProductBit(kLinDepth);
    for (uint32_t i = 0; i < kDepthLevels; ++i) f.lin_depth_slot[i] = in.depth[i];
  }
  if (in.depth_full != kNoSlot) {
    f.valid |= ProductBit(kLinDepthFull);
    f.lin_depth_full_slot = in.depth_full;
  }
  if (in.normal != kNoSlot) {
    f.valid |= ProductBit(kNormal);
    f.normal_slot = in.normal;
  }
  if (in.colour_pre[0] != kNoSlot) {
    f.valid |= ProductBit(kColourPre);
    for (uint32_t i = 0; i < kColourPreLevels; ++i) f.colour_pre_slot[i] = in.colour_pre[i];
  }
  if (in.hiz_levels != 0 && in.hiz_levels <= kHizMaxLevels) {
    bool whole = true;
    for (uint32_t i = 0; i < in.hiz_levels; ++i) whole = whole && in.hiz[i] != kNoSlot;
    if (whole) {
      f.valid |= ProductBit(kHizMin);
      f.hiz_levels = in.hiz_levels;
      for (uint32_t i = 0; i < in.hiz_levels; ++i) f.hiz_min_slot[i] = in.hiz[i];
    }
  }
  f.camera = in.camera;
  return f;
}

// When `generation` moves: the SET of images changed, not which of a ping-pong pair is the current one (the AO swaps its depth image with the history's every frame while the bounce
// light runs, so slot numbers alone would change every frame). The sizes, the samples, the products that exist, the Hi-Z's levels and the two owners' own counters decide.
struct Signature {
  uint32_t traced[2] = {0, 0};
  uint32_t render[2] = {0, 0};
  uint32_t host_samples = 0;
  uint32_t flags = 0;
  uint32_t valid = 0;
  uint32_t hiz_levels = 0;
  uint64_t owner_generation = 0;
  uint64_t hiz_generation = 0;
  bool operator==(const Signature& o) const {
    return traced[0] == o.traced[0] && traced[1] == o.traced[1] && render[0] == o.render[0] && render[1] == o.render[1] && host_samples == o.host_samples &&
           flags == o.flags && valid == o.valid && hiz_levels == o.hiz_levels && owner_generation == o.owner_generation && hiz_generation == o.hiz_generation;
  }
};
inline Signature SignatureOf(const FrameInputs& in, const ScreenFrame& frame) {
  Signature s;
  s.traced[0] = in.traced[0];
  s.traced[1] = in.traced[1];
  s.render[0] = in.render[0];
  s.render[1] = in.render[1];
  s.host_samples = in.host_samples;
  s.flags = in.flags;
  s.valid = frame.valid;
  s.hiz_levels = frame.hiz_levels;
  s.owner_generation = in.owner_generation;
  s.hiz_generation = in.hiz_generation;
  return s;
}
class Generation {
 public:
  // The generation of a frame with `signature`: a new number whenever it differs from the last published one (never 0 once a frame was published).
  uint32_t Of(const Signature& signature) {
    if (!have_ || !(signature == last_)) {
      ++count_;
      last_ = signature;
      have_ = true;
    }
    return count_;
  }

 private:
  Signature last_;
  uint32_t count_ = 0;
  bool have_ = false;
};

// The C++ twin of hlsl/screen_hiz.hlsli.
inline float StripMarks(float level0_depth) {
  uint32_t bits;
  std::memcpy(&bits, &level0_depth, sizeof(bits));
  bits &= ~kDepthMarkMask;
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}
inline float Min4(float a, float b, float c, float d) { return std::min(std::min(a, b), std::min(c, d)); }

// The four texels of the level below that texel (x, y) of a level takes, clamped inside the level below (`last_x`, `last_y`: its last column and row).
struct Footprint {
  int32_t x[4] = {};
  int32_t y[4] = {};
};
constexpr Footprint FootprintOf(int32_t x, int32_t y, int32_t last_x, int32_t last_y) {
  Footprint f;
  for (int32_t k = 0; k < 4; ++k) {
    f.x[k] = std::min(x * 2 + (k & 1), last_x);
    f.y[k] = std::min(y * 2 + (k >> 1), last_y);
  }
  return f;
}

}  // namespace mcla::gpu::screen
