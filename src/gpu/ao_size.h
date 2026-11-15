#pragma once
// ===========================================================================
// MCLA GPU renderer -- ambient occlusion and bounce light: how big their images are and what they hold (pure maths, no GPU)
// ===========================================================================
// ao.cpp (OnResolve, EnsureImages, EnsureGiImages) asks here which size the occlusion's and the bounce light's passes run at and which
// images a frame needs; ao_size_test.cpp and host_plan_test.cpp check the rules and the memory they come to without a renderer.
//
// WHY THE SIZE IS A RULE OF THE HEIGHT. The occlusion's main pass takes the same 18 samples a pixel (3 slices of 3 steps, a side each) at
// every picture size, so the signal it makes is a ~720 line signal whatever the picture has (ao_stable.h: the blur's radius grows with the
// image's height to keep its reach in the picture's terms). A 1440p or 4K picture therefore pays 4 times and 9 times the 720p cost for
// what the blur then throws away. Above 1080 lines the occlusion's (and the bounce light's) images are traced at half the picture's size,
// 720 lines for 1440p, 1080 for 4K, and the apply pass's depth-aware 2x upsample (ao_apply_ps.hlsl VisibilityAt, GiAt) puts them back:
// 1080 lines and below are unchanged. The bounce light is the lowest frequency signal of all (it gathers 5 to 16 m and is filtered over
// up to 30 pixels), so it follows the same rule.
//
// THE TWO SIZES. Before, one flag (Plan::low) made every image of the frame half size, and the contact shadows pinned it to the picture's
// size whenever they were on `high` (the owner asked for sharp contact shadows: their march reads the full size depth). Now the
// occlusion's and the bounce light's images (the trace) can be half size while the contact shadows keep exactly the size they had: the
// depth pass then also writes a full size linear depth for the contact march and the apply's contact filter (the `contact_full` choice).
//
// THE OLD RULE (legacy_low) is kept: the passes are at half size when nothing that is on asks for the whole of it (the occlusion on high,
// the contact shadows on high, the bounce light on high), and always at half size for the bounce light's low with mcla_gpu_ssgi_low 1 or 2
// (unless the contact shadows are on high). The new rule only adds to it, above 1080 lines.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::ao_size {

// The picture's height above which the traced images are half size (the rule's threshold: 1080 lines and below are as they were).
inline constexpr uint32_t kHalfAboveLines = 1080;

constexpr uint32_t Half(uint32_t n) { return (n + 1) / 2; }

// What the rule looks at.
struct Inputs {
  uint32_t render_height = 0;    // the main view's height in host pixels (the render size, an upscaler's render size)
  bool occlusion = false;        // the occlusion's term is computed (mcla_gpu_ao is not off)
  bool occlusion_high = false;
  bool contact = false;          // the contact shadows run this frame
  bool contact_high = false;
  bool bounce = false;           // the bounce light runs this frame (its images are the traced ones)
  bool bounce_high = false;
  bool bounce_half = false;      // mcla_gpu_ssgi_low 1 or 2 at the bounce light's low: half size by the old setting
  bool half_above_ao = true;     // mcla_gpu_ao_half_above_1080
  bool half_above_bounce = true; // mcla_gpu_ssgi_half
};

struct Choice {
  bool legacy_low = false;    // the old rule's answer (every image of the frame half size)
  bool low = false;           // the occlusion's and the bounce light's images are half size (Plan::low)
  bool by_height = false;     // low only because of the height rule (above 1080 lines), not the old rule
  bool contact_full = false;  // the contact shadows keep the picture's size while `low` (they had it before)
};

constexpr Choice Decide(const Inputs& in) {
  Choice c;
  const bool contact_high = in.contact && in.contact_high;
  c.legacy_low = !(in.occlusion && in.occlusion_high) && !contact_high && !(in.bounce && in.bounce_high);
  if (in.bounce && in.bounce_half && !contact_high) {
    c.legacy_low = true;  // the bounce light's low at half the size, whatever the occlusion's own quality
  }
  // With the bounce light on its images give the occlusion's visibility too, so its setting decides.
  const bool enabled = in.occlusion && (in.bounce ? in.half_above_bounce : in.half_above_ao);
  c.by_height = !c.legacy_low && enabled && in.render_height > kHalfAboveLines;
  c.low = c.legacy_low || c.by_height;
  c.contact_full = c.by_height && in.contact;
  return c;
}

// ---------------------------------------------------------------------------
// the half size depth (hlsl/ao_depth_ps.hlsl, AoLow): the twin of its choice
// ---------------------------------------------------------------------------
// Each pixel of a half size image stands for a 2x2 block of the depth plane. Where the four samples are one surface (their spread under
// kHalfSurfaceShare of the mean plus kHalfSurfaceFloor, linear depth in world units) the pixel takes their MEAN: a surface's depth at the
// block's centre, where the pixel's position is. Across an edge it takes the sample nearest to the mean (a depth that exists in the block).
// The first version took that sample always: on a surface at a grazing angle (a road, a wall along the street) it is a corner of the block,
// half a pixel off the centre, the surface becomes a staircase in the half size image and the occlusion is darker by 0.04 on average (software
// rasteriser, 3 slices of 3 steps, a road, a wall and a kerb at 1280x720 traced at 640x360: 0.0024 with the mean). The same constants are in the shader.
inline constexpr float kHalfSurfaceShare = 0.04f;
inline constexpr float kHalfSurfaceFloor = 0.02f;

struct HalfDepth {
  float depth = 0.0f;
  int source = 0;           // the sample whose stencil and vehicle mark the pixel takes
  bool averaged = false;    // the four were one surface: `depth` is their mean
};

inline HalfDepth HalfSizeDepth(const float d[4]) {
  const float mean = (d[0] + d[1] + d[2] + d[3]) * 0.25f;
  HalfDepth out;
  out.depth = d[0];
  out.source = 0;
  float best = std::fabs(d[0] - mean);
  for (int k = 1; k < 4; ++k) {
    if (std::fabs(d[k] - mean) < best) {
      out.depth = d[k];
      out.source = k;
      best = std::fabs(d[k] - mean);
    }
  }
  const float spread = std::max(std::max(d[0], d[1]), std::max(d[2], d[3])) - std::min(std::min(d[0], d[1]), std::min(d[2], d[3]));
  if (spread <= kHalfSurfaceShare * mean + kHalfSurfaceFloor) {
    out.depth = mean;
    out.averaged = true;
  }
  return out;
}

// ---------------------------------------------------------------------------
// the images a frame holds
// ---------------------------------------------------------------------------

inline constexpr uint32_t kBytesR32 = 4;     // R32_FLOAT: the linear depth
inline constexpr uint32_t kBytesR16 = 2;     // R16_FLOAT: a visibility, a coarser depth level
inline constexpr uint32_t kBytesRG16 = 4;    // R16G16_FLOAT: the occlusion's history
inline constexpr uint32_t kBytesRGBA16 = 8;  // RGBA16F: the bounce light's images

constexpr uint64_t ImageBytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel) {
  return uint64_t(width) * uint64_t(height) * bytes_per_pixel;
}

// What a frame asks for (ao.cpp EnsureImages and EnsureGiImages).
struct Frame {
  uint32_t render[2] = {};
  Choice choice;
  bool occlusion = true;
  bool apply = true;          // false: the census's probe only
  bool probe = false;
  bool contact = false;
  bool contact_fast = true;   // mcla_gpu_contact_shadows_fast: the apply pass does the contact filter (at the picture's size)
  bool bounce = false;
  bool debug_weight = false;  // the picture of debug view 6 / 9 (not counted)
};

struct Held {
  uint64_t depth = 0;      // the traced depth, level 0
  uint64_t levels = 0;     // the two coarser levels
  uint64_t scratch = 0;    // the visibility passes' two images (raw_, denoised_)
  uint64_t history = 0;    // the occlusion's own history (RG16F, not made while the bounce light runs)
  uint64_t contact = 0;    // the contact shadows' own full size depth and images, when the sizes differ
  uint64_t bounce = 0;     // the bounce light's colour chain, passes' images, history and depth history
  uint64_t Total() const { return depth + levels + scratch + history + contact + bounce; }
};

// The traced size (the depth chain's level 0), and the two coarser levels, as OnResolve sets them.
struct Sizes {
  uint32_t ao[2] = {};
  uint32_t level0[2] = {};
  uint32_t level1[2] = {};
  uint32_t contact[2] = {};  // the contact shadows' images (and, when it differs from `ao`, their own depth)
};

constexpr Sizes SizesOf(const uint32_t render[2], const Choice& choice) {
  Sizes s;
  for (uint32_t axis = 0; axis < 2; ++axis) {
    s.ao[axis] = choice.low ? Half(render[axis]) : render[axis];
    s.level0[axis] = (s.ao[axis] + 1) / 2;
    s.level1[axis] = (s.ao[axis] + 3) / 4;
    s.contact[axis] = (choice.low && !choice.contact_full) ? Half(render[axis]) : render[axis];
  }
  return s;
}

// The apply pass does the contact filter itself (no filter pass, no second contact image) at the picture's size only.
constexpr bool ContactFilterInApply(bool contact_fast, const Choice& choice) {
  return contact_fast && (!choice.low || choice.contact_full);
}

// The new rule's images (this version). Mirrors EnsureImages: the visibility passes' scratch images only where something reads them (the
// bounce light has its own, the contact shadows' own images are separate while their size differs from the traced one).
constexpr Held Bytes(const Frame& f) {
  Held h;
  const Sizes s = SizesOf(f.render, f.choice);
  const bool split = f.contact && f.choice.contact_full;  // the contact shadows keep the picture's size beside a half size trace
  const bool levels = f.occlusion || f.probe;
  h.depth = ImageBytes(s.ao[0], s.ao[1], kBytesR32);
  if (levels) {
    h.levels = ImageBytes(s.level0[0], s.level0[1], kBytesR16) + ImageBytes(s.level1[0], s.level1[1], kBytesR16);
  }
  const bool ao_scratch = f.occlusion && !f.bounce;
  const bool contact_scratch = f.contact && !split;
  const bool in_apply = ContactFilterInApply(f.contact_fast, f.choice);
  if (f.apply) {
    if (ao_scratch || contact_scratch) {
      h.scratch += ImageBytes(s.ao[0], s.ao[1], kBytesR16);
    }
    if (ao_scratch || (contact_scratch && !in_apply)) {
      h.scratch += ImageBytes(s.ao[0], s.ao[1], kBytesR16);
    }
  }
  if (f.occlusion && !f.bounce) {
    h.history = 2 * ImageBytes(s.ao[0], s.ao[1], kBytesRG16);
  }
  if (split) {
    h.contact = ImageBytes(f.render[0], f.render[1], kBytesR32) + ImageBytes(f.render[0], f.render[1], kBytesR16);
    if (!in_apply) {
      h.contact += ImageBytes(f.render[0], f.render[1], kBytesR16);
    }
  }
  if (f.bounce) {
    h.bounce = ImageBytes(s.ao[0], s.ao[1], kBytesRGBA16) + ImageBytes(s.level0[0], s.level0[1], kBytesRGBA16) +
               ImageBytes(s.level1[0], s.level1[1], kBytesRGBA16) +
               4 * ImageBytes(s.ao[0], s.ao[1], kBytesRGBA16) +  // gi_, gi_dn_, the two history images
               ImageBytes(s.ao[0], s.ao[1], kBytesR32);          // the depth history
  }
  return h;
}

// The images the version before this one held for the same frame: the old rule's size, raw_ and denoised_ whenever the apply runs.
constexpr Held BytesBefore(Frame f) {
  Choice c;
  c.legacy_low = f.choice.legacy_low;
  c.low = f.choice.legacy_low;
  f.choice = c;
  Held h;
  const Sizes s = SizesOf(f.render, f.choice);
  const bool levels = f.occlusion || f.probe;
  h.depth = ImageBytes(s.ao[0], s.ao[1], kBytesR32);
  if (levels) {
    h.levels = ImageBytes(s.level0[0], s.level0[1], kBytesR16) + ImageBytes(s.level1[0], s.level1[1], kBytesR16);
  }
  if (f.apply) {
    h.scratch = 2 * ImageBytes(s.ao[0], s.ao[1], kBytesR16);
  }
  if (f.occlusion && !f.bounce) {
    h.history = 2 * ImageBytes(s.ao[0], s.ao[1], kBytesRG16);
  }
  if (f.bounce) {
    h.bounce = ImageBytes(s.ao[0], s.ao[1], kBytesRGBA16) + ImageBytes(s.level0[0], s.level0[1], kBytesRGBA16) +
               ImageBytes(s.level1[0], s.level1[1], kBytesRGBA16) + 4 * ImageBytes(s.ao[0], s.ao[1], kBytesRGBA16) +
               ImageBytes(s.ao[0], s.ao[1], kBytesR32);
  }
  return h;
}

constexpr double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// ---------------------------------------------------------------------------
// the cost model (an RTX 5080's, scaled; the numbers at the traced size are NOT measured)
// ---------------------------------------------------------------------------
// Measured at 3840x2160, full size, 3 slices of 3 steps, the blur 7x7 (GFXW1.txt K-DAY: visibility 0.81 ms, temporal 0.17, denoise 0.147,
// apply 0.14; the depth chain, not in the log lines, about 0.1 by an estimate from its reads). The passes scale with the pixels they
// cover, the main pass with the taps (slices x steps), the blur with its (2r+1)^2 taps. The apply covers the picture, always.
inline constexpr double kMeasuredPixels = 3840.0 * 2160.0;
inline constexpr double kMainMs = 0.81;
inline constexpr double kTemporalMs = 0.17;
inline constexpr double kDenoiseMs = 0.147;
inline constexpr double kApplyMs = 0.14;
inline constexpr double kDepthMs = 0.10;  // an estimate: the full size depth pass and the two levels
inline constexpr double kDepthFullMs = 0.06;  // an estimate: the extra full size depth pass of the contact shadows, one read of the plane

struct AoCost {
  double main_ms = 0.0, denoise_ms = 0.0, temporal_ms = 0.0, depth_ms = 0.0, apply_ms = 0.0;
  double Total() const { return main_ms + denoise_ms + temporal_ms + depth_ms + apply_ms; }
};

// `taps`: slices x steps of the main pass (9 at high, 4 at low); `radius`: the blur's radius for the traced height (ao_stable.h).
// `extra_depth`: the contact shadows' own full size depth pass is also made.
constexpr AoCost Cost(const Sizes& s, const uint32_t render[2], uint32_t taps, uint32_t radius, bool extra_depth) {
  AoCost c;
  const double traced = double(s.ao[0]) * double(s.ao[1]) / kMeasuredPixels;
  const double picture = double(render[0]) * double(render[1]) / kMeasuredPixels;
  const double taps_share = double(taps) / 9.0;
  const double blur_taps = double((2 * radius + 1) * (2 * radius + 1)) / 49.0;
  c.main_ms = kMainMs * traced * taps_share;
  c.denoise_ms = kDenoiseMs * traced * blur_taps;
  c.temporal_ms = kTemporalMs * traced;
  c.depth_ms = kDepthMs * traced + (extra_depth ? kDepthFullMs * picture : 0.0);
  c.apply_ms = kApplyMs * picture;
  return c;
}

}  // namespace mcla::gpu::ao_size
