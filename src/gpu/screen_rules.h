#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen-space products: the host's pure rules (no GPU)
// ===========================================================================
// screen_shared.cpp asks here what the frame does (which products the owner makes, whether the AO's trigger has to run for them), how big the Hi-Z chain and the colour captures are, when the
// history of the last frame can be used, and the C++ twin of hlsl/screen_hiz.hlsli (the Hi-Z's two operations); screen_shared_test.cpp checks the rules and compares the twin with the .hlsli
// compiled as C++. The frozen interface is screen_frame.h; this file may change.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "screen_frame.h"

namespace mcla::gpu::screen {

// --- what the owner makes itself ----------------------------------------------------------------------------------------------------------

// The products ScreenShared makes itself: the Hi-Z (SS-0), colour_post and its history (SS-1). The others are the AO's passes, adopted: the depth chain, the normal and the full size depth
// are published when the AO's frame has them, colour_pre's images belong to the owner and its pass is recorded for it by the AO (ScreenShared::CapturePre); SS-2 moves the passes.
inline constexpr uint32_t kMadeHere = (1u << kHizMin) | (1u << kColourPost) | (1u << kHistory);

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
    case View::kColourPost: return 1u << kColourPost;
    case View::kHistory: return 1u << kHistory;
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
  uint32_t make = 0;          // the products this module makes this frame (wanted & kMadeHere, and colour_post wherever the history is wanted)
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
  if ((plan.make & (1u << kHistory)) != 0) {
    plan.make |= 1u << kColourPost;  // the history is the last frame's colour_post: it is kept, so it is made
    plan.wanted |= 1u << kColourPost;
  }
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

// --- the colour captures --------------------------------------------------------------------------------------------------------------------

// colour_post's size: the picture's up to kColourPostHalfAboveLines of render height and half of it (rounded up, ao_size.h Half) above: the AO's HEIGHT rule alone. The AO's trace is half size also
// at its high quality at any height (ao_size.h legacy_low), so the depth's traced size is often smaller than this; a reflection's colour at 1080p is the picture's own, and the car history,
// which reads it pixel for pixel, can take it whole there. (ao_size::kHalfAboveLines is the same number: the test says so.)
inline constexpr uint32_t kColourPostHalfAboveLines = 1080;
constexpr uint32_t ColourPostEdge(uint32_t render_edge, bool half) { return half ? (render_edge + 1) / 2 : render_edge; }
constexpr bool ColourPostIsHalf(uint32_t render_height) { return render_height > kColourPostHalfAboveLines; }

// The mip chain a rough reflection reads (GB-I's: levels above 0 are made only while the reflections consumer wants colour_post): up to this many halvings, while the smaller side has one left.
inline constexpr uint32_t kColourPostMipHalvings = 4;
constexpr uint32_t ColourPostLevelCount(uint32_t width, uint32_t height, bool mips) {
  if (!mips) {
    return 1;
  }
  uint32_t smaller = width < height ? width : height;
  uint32_t levels = 1;
  while (levels <= kColourPostMipHalvings && smaller > 1) {
    smaller = (smaller + 1) / 2;
    ++levels;
  }
  return levels;
}
inline constexpr uint32_t kBytesRgba16f = 8;  // RGBA16_FLOAT
constexpr uint64_t ColourPostBytes(uint32_t width, uint32_t height, uint32_t levels) {
  uint64_t total = 0;
  for (uint32_t level = 0; level < levels; ++level) {
    total += uint64_t(HizLevelSize(width, level)) * uint64_t(HizLevelSize(height, level)) * kBytesRgba16f;
  }
  return total;
}
// Whether the colour_post chain is made with its mips: while the reflections consumer wants colour_post, or the ray traced reflections of the car paint (CITY-RT M2: a hit on the city is read at the mip of
// its footprint) do (nothing else reads a level above 0), or the debug view shows a level above 0.
constexpr bool ColourPostWithMips(const Wants& wants, View view, uint32_t view_level) {
  return (wants.Of(Consumer::kReflections) & (1u << kColourPost)) != 0 || (wants.Of(Consumer::kCarRtRefl) & (1u << kColourPost)) != 0 || (view == View::kColourPost && view_level > 0);
}

// A trigger frame whose depth has no main colour target at its sample count. The capture finds the colour by the depth's guest samples; the game's first frames draw a 1-sample depth (a loading scene) before the
// game view's 2-sample colour and depth exist, so the first trigger of a run that wants colour_post from the start (the reflections on at launch) has a depth and no colour partner. That is a frame with no
// colour_post, not a failure: it was logged as an ERROR once in every such run (24 run logs of 2026-11-21 and 22: the RR chain's live, row, frame generation, validation and race runs and its SSR cost runs, the Super Resolution and the native arms too) and made the run count as failed. Quiet (one note) while it
// lasts no more than kColourMissingGraceFrames trigger frames in a row, an error when it lasts longer (a colour target that never comes is a real problem). The run counts the frames in a row.
inline constexpr uint32_t kColourMissingGraceFrames = 300;
enum class ColourPartner : uint8_t { kFound, kEarly, kMissingTooLong };
constexpr ColourPartner JudgeColourPartner(bool found, uint32_t missing_in_a_row) {
  if (found) return ColourPartner::kFound;
  return missing_in_a_row > kColourMissingGraceFrames ? ColourPartner::kMissingTooLong : ColourPartner::kEarly;
}

// The exposure bias as the camera carries it: the factor between the shaders' output and what the main colour target (and so colour_post) holds, 2^exp_bias (the main target's bias is 4: 16).
// 1 where no draw has told it yet.
inline float ExposureOf(int32_t exp_bias, bool known) {
  if (!known) {
    return 1.0f;
  }
  int32_t e = exp_bias < -24 ? -24 : (exp_bias > 24 ? 24 : exp_bias);
  float f = 1.0f;
  for (; e > 0; --e) f *= 2.0f;
  for (; e < 0; ++e) f *= 0.5f;
  return f;
}

// --- the history ----------------------------------------------------------------------------------------------------------------------------

// What identifies a captured colour_post for the next frame: the frame it was made in (the caller's count of guest presents), the kind of view and its size.
struct CaptureKey {
  bool valid = false;
  uint64_t frame = 0;
  bool pause = false;
  uint32_t size[2] = {0, 0};
};
// The last frame's capture is the history of this frame's only when it was made in the frame before, in the same kind of view (the pause menu's, the garage's and the map's are other worlds), at
// the same size (a resize or a render scale change remakes the images).
constexpr bool HistoryUsable(const CaptureKey& last, const CaptureKey& now) {
  return last.valid && now.valid && last.frame + 1 == now.frame && last.pause == now.pause && last.size[0] == now.size[0] && last.size[1] == now.size[1];
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
  uint32_t colour_post_levels = 0;          // 0: no colour_post this frame
  uint32_t colour_post[kColourPostMaxLevels] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint32_t colour_post_size[2] = {0, 0};    // level 0's size (not in the frozen frame: the image's own width and height say it; the generation counts on it)
  uint32_t history_colour = kNoSlot;        // the last frame's colour_post level 0, only when it is usable (HistoryUsable)
  uint32_t history_depth = kNoSlot;         // the last frame's lin_depth level 0: SS-2
  uint64_t colour_generation = 0;           // ScreenShared's: bumped when the colour_post / history images are made again
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
  if (in.colour_post_levels != 0 && in.colour_post_levels <= kColourPostMaxLevels) {
    bool whole = true;
    for (uint32_t i = 0; i < in.colour_post_levels; ++i) whole = whole && in.colour_post[i] != kNoSlot;
    if (whole) {
      f.valid |= ProductBit(kColourPost);
      f.colour_post_levels = in.colour_post_levels;
      for (uint32_t i = 0; i < in.colour_post_levels; ++i) f.colour_post_slot[i] = in.colour_post[i];
      // The history is the last frame's colour_post: valid with this frame's own (a consumer of the history reads both), and with a colour slot.
      if (in.history_colour != kNoSlot) {
        f.valid |= ProductBit(kHistory);
        f.history_colour_slot = in.history_colour;
        f.history_depth_slot = in.history_depth;
      }
    }
  }
  f.camera = in.camera;
  return f;
}

// When `generation` moves: the SET of images changed, not which of a ping-pong pair is the current one (the AO swaps its depth image with the history's every frame while the bounce
// light runs, and the owner's colour_post trades with its history the same way, so slot numbers alone would change every frame). The sizes, the samples, the products that exist, the chains'
// levels and the owners' own counters decide.
struct Signature {
  uint32_t traced[2] = {0, 0};
  uint32_t render[2] = {0, 0};
  uint32_t host_samples = 0;
  uint32_t flags = 0;
  uint32_t valid = 0;
  uint32_t hiz_levels = 0;
  uint32_t colour_post_levels = 0;
  uint32_t colour_post_size[2] = {0, 0};
  uint64_t owner_generation = 0;
  uint64_t hiz_generation = 0;
  uint64_t colour_generation = 0;
  bool operator==(const Signature& o) const {
    return traced[0] == o.traced[0] && traced[1] == o.traced[1] && render[0] == o.render[0] && render[1] == o.render[1] && host_samples == o.host_samples &&
           flags == o.flags && valid == o.valid && hiz_levels == o.hiz_levels && colour_post_levels == o.colour_post_levels && colour_post_size[0] == o.colour_post_size[0] &&
           colour_post_size[1] == o.colour_post_size[1] && owner_generation == o.owner_generation && hiz_generation == o.hiz_generation && colour_generation == o.colour_generation;
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
  s.colour_post_levels = frame.colour_post_levels;
  s.colour_post_size[0] = in.colour_post_size[0];
  s.colour_post_size[1] = in.colour_post_size[1];
  s.owner_generation = in.owner_generation;
  s.hiz_generation = in.hiz_generation;
  s.colour_generation = in.colour_generation;
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
