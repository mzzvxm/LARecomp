#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared screen products' EXTRACTION: the rules of the depth chain, the normal image and the colour chain (SS-2, perf/LIGHT-UNIFY.md section 5.5)
// ===========================================================================
// SS-0 published what the AO makes, SS-1 moved the colour captures; SS-2 moves the passes themselves: the linear depth chain (level 0 from the guest's depth, the two coarser levels, the
// full size depth the contact shadows keep beside a half size trace), the normal image (GB-E, hlsl/ao_normal_ps.hlsl) and the bounce light's colour chain (hlsl/ao_color_ps.hlsl) are recorded
// by ScreenShared (screen_shared.h, screen_extract.cpp) into images it owns, once a frame, and AO, bounce light, contact shadows, SSR and the car passes read them. ao.cpp keeps the visibility,
// the denoise, the history, the contact march and the apply.
//
// WHAT STAYS THE SAME, to the bit. The passes are the same shaders (hlsl/ao_depth_ps.hlsl ... ao_color_ps.hlsl, host_shaders.h AmbientOcclusionPixelShader's passes 0, 1, 2, 11, 12, 13, 27, 28),
// into images of the same sizes, formats and names, recorded in the same order at the same place of the AO's frame, with the same 128 byte push block (ao_push.h): the functions below build
// the block of each pass from the AO's frame block (`base`, ao.cpp FillPush) exactly as ao.cpp's own recording (the old path, mcla_gpu_screen_extract false) does. screen_extract_test.cpp holds
// them to a verbatim copy of the old statements and holds that copy to ao.cpp's text, over a grid of frames (sizes, half and full, MSAA samples, the normal plane on and off, the contact shadows
// at full size).
//
// WHAT IS SAVED. See Cost below: the model of the passes a frame records and the bytes they move, for the products of one frame (depth chain, normals, colour chain, Hi-Z) built once by the
// owner against the same products built by each effect for itself (the arrangement before the owner), at 1080p and at 4K; screen_extract_test.cpp prints the table and holds its invariants.
//
// Pure: no Plume, no renderer. Threading: none.
// ===========================================================================

#include <cstdint>

#include "ao_push.h"
#include "screen_frame.h"
#include "screen_rules.h"

namespace mcla::gpu::screen::extract {

// The bit of the push block's slots2.z flags that says the passes run at half size (ao_common.hlsli AoLow).
inline constexpr uint32_t kLowFlag = 1u << 16;
// The debug view bits of the flags (the colour chain's passes have none).
inline constexpr uint32_t kViewMask = 0xFFu;
// The flag of the colour chain's level 0 pass that says it takes one sample of the colour target's MSAA samples (ao_color_ps.hlsl AoOneSample; ao.cpp kColourOneSampleFlag).
inline constexpr uint32_t kColourOneSampleFlag = 1u << 21;

// What the AO tells the owner about the frame: the passes' sizes (ao_size.h Sizes) and what the frame reads. The AO's Plan, as plain numbers.
struct Frame {
  uint32_t traced[2] = {};       // the depth chain's level 0 (the occlusion's trace: half the picture's size or the whole of it)
  uint32_t level[2][2] = {};     // the coarser levels' sizes (1/2 and 1/4 of `traced`, odd edges rounded up)
  uint32_t render[2] = {};       // the picture's size in host pixels
  bool levels = false;           // the coarser levels are made (the occlusion, the census's probe)
  bool full = false;             // a full size depth beside a half size trace (the contact shadows keep the picture's size: Plan::SplitContact)
};

// --- the depth chain -------------------------------------------------------------------------------------------------------------------------

// Level 0 (hlsl/ao_depth_ps.hlsl): the traced size from the picture's size, reading the depth target (slots.x its sample slot, slots.y the host sample: the AO's frame block already has them) and
// its stencil plane (slots.z, the vehicles' IDs); `source_bits` is slots2.w, which says that the pass also records which sample of each 2x2 block it took (GB-E's half size normals).
inline AoPush DepthPush(const AoPush& base, const Frame& f, uint32_t stencil_slot, bool source_bits) {
  AoPush push = base;
  push.sizes[0] = float(f.traced[0]);
  push.sizes[1] = float(f.traced[1]);
  push.sizes[2] = float(f.render[0]);
  push.sizes[3] = float(f.render[1]);
  push.slots[2] = stencil_slot;
  push.slots2[3] = source_bits ? 1u : 0u;
  return push;
}

// The coarser levels (hlsl/ao_mip_ps.hlsl): level 1 from level 0 (`source_slot` its slot), level 2 from level 1. The block is level 0's with the fields the pass reads replaced: the old path
// kept one block through the three passes, so the fields it does not read (slots2.w) are level 0's.
inline AoPush DepthMipPush(const AoPush& level0_push, const Frame& f, uint32_t level, uint32_t source_slot) {
  AoPush push = level0_push;
  push.slots[2] = source_slot;
  if (level == 1) {
    push.sizes[0] = float(f.level[0][0]);
    push.sizes[1] = float(f.level[0][1]);
    push.sizes[2] = float(f.traced[0]);
    push.sizes[3] = float(f.traced[1]);
  } else {
    push.sizes[0] = float(f.level[1][0]);
    push.sizes[1] = float(f.level[1][1]);
    push.sizes[2] = float(f.level[0][0]);
    push.sizes[3] = float(f.level[0][1]);
  }
  return push;
}

// The full size depth of the contact shadows (the depth pass again without the half size flag; the vehicle bit is in it as in level 0): from the frame's block, not level 0's.
inline AoPush DepthFullPush(const AoPush& base, const Frame& f, uint32_t stencil_slot) {
  AoPush push = base;
  push.slots2[2] &= ~kLowFlag;
  push.slots[2] = stencil_slot;
  push.sizes[0] = push.sizes[2] = float(f.render[0]);
  push.sizes[1] = push.sizes[3] = float(f.render[1]);
  return push;
}

// --- the normal image -------------------------------------------------------------------------------------------------------------------------

// What the normal pass reads besides the depth: the G-buffer's RT1 plane and the camera's rotation (world to view, three rows). The AO decides whether the frame uses the plane (ao_gbuffer.h Decide).
struct NormalRecipe {
  uint32_t plane_slot = 0;       // RT1's bindless slot
  uint32_t plane_samples = 1;    // its host sample count (above 1: Texture2DMS: the other pipeline)
  uint32_t plane_sample0 = 0;    // the host sample that is the guest's sample 0
  uint32_t plane_size[2] = {};   // RT1's size
  float rows_x[4] = {};          // the rotation's rows (ao_gbuffer.h Rows)
  float rows_y[4] = {};
  float rows_z[4] = {};
};

// The traced size image from level 0 (it carries the vehicle mark and, at half size, the source of each pixel). Push fields: hlsl/ao_normal_ps.hlsl.
inline AoPush NormalPush(const AoPush& base, const Frame& f, const NormalRecipe& n, uint32_t depth_slot) {
  AoPush push = base;
  push.slots[0] = n.plane_slot;
  push.slots[1] = n.plane_sample0;
  push.slots[2] = depth_slot;
  push.sizes[0] = float(f.traced[0]);
  push.sizes[1] = float(f.traced[1]);
  push.sizes[2] = float(n.plane_size[0]);
  push.sizes[3] = float(n.plane_size[1]);
  for (int k = 0; k < 4; ++k) {
    push.lin[k] = n.rows_x[k];
    push.shape[k] = n.rows_y[k];
    push.light[k] = n.rows_z[k];
  }
  return push;
}

// The picture's size image, from the contact shadows' own depth (no half size block to choose from): the traced size one's block with the depth slot, the size and the half size flag replaced.
inline AoPush NormalFullPush(const AoPush& traced_push, const Frame& f, uint32_t full_depth_slot) {
  AoPush push = traced_push;
  push.slots[2] = full_depth_slot;
  push.sizes[0] = float(f.render[0]);
  push.sizes[1] = float(f.render[1]);
  push.slots2[2] &= ~kLowFlag;
  return push;
}

// --- the colour chain -------------------------------------------------------------------------------------------------------------------------

// What the colour chain's level 0 needs of the frame (hlsl/ao_color_ps.hlsl): the main colour target's slot and host samples, the cap and the source start of the frame's light (ssgi_light.h; the
// AO computes them from its own light and the setting), and whether one sample of the MSAA samples is taken (Plan::OneSampleColour) with the depth's own host sample.
struct ColourRecipe {
  uint32_t colour_slot = 0;
  uint32_t host_samples = 1;
  float cap = 0.0f;              // shape.x
  float source_start = 0.0f;     // shape.y
  bool one_sample = false;
  uint32_t sample = 0;           // the depth's host sample (base.slots[1]), used when one_sample
};

inline AoPush ColourLevel0Push(const AoPush& base, const Frame& f, const ColourRecipe& c) {
  AoPush push = base;
  push.slots2[2] &= ~kViewMask;  // no debug view
  push.slots[0] = c.colour_slot;
  push.slots[1] = c.host_samples;
  push.sizes[0] = float(f.traced[0]);
  push.sizes[1] = float(f.traced[1]);
  push.sizes[2] = float(f.render[0]);
  push.sizes[3] = float(f.render[1]);
  push.shape[0] = c.cap;
  push.shape[1] = c.source_start;
  if (c.one_sample) {
    // Half size (above 1080 lines): the texel is the mean of the 2x2 pixels' one sample each, the depth's own host sample.
    push.slots2[2] |= kColourOneSampleFlag;
    push.slots2[0] = c.sample;
  }
  return push;
}

// The coarser levels of the colour chain: level 1 from level 0, level 2 from level 1, from the frame's block (not level 0's).
inline AoPush ColourMipPush(const AoPush& base, const Frame& f, uint32_t level, uint32_t source_slot) {
  AoPush push = base;
  push.slots2[2] &= ~kViewMask;
  push.slots[2] = source_slot;
  if (level == 1) {
    push.sizes[0] = float(f.level[0][0]);
    push.sizes[1] = float(f.level[0][1]);
    push.sizes[2] = float(f.traced[0]);
    push.sizes[3] = float(f.traced[1]);
  } else {
    push.sizes[0] = float(f.level[1][0]);
    push.sizes[1] = float(f.level[1][1]);
    push.sizes[2] = float(f.level[0][0]);
    push.sizes[3] = float(f.level[0][1]);
  }
  return push;
}

// --- the passes the owner makes, and their pipelines ----------------------------------------------------------------------------------------

// The pipelines the extraction needs (screen_extract.cpp keeps one of each, made in the background the first time a frame wants it). The pass number is host_shaders.h
// AmbientOcclusionPixelShader's (the AO's own table, ao.h Pass).
enum class Kind : uint32_t {
  kDepth,        // pass 0: from a plain depth plane, R32_FLOAT
  kDepthMs,      // pass 1: from a multisampled one
  kMip,          // pass 2: a coarser level of the depth chain, R16_FLOAT
  kNormal,       // pass 27: the normal image from a plane with one sample, R16G16_UNORM
  kNormalMs,     // pass 28: from the host's MSAA level
  kColour,       // pass 11: the colour chain's level 0 from a plain colour target, RGBA16F
  kColourMs,     // pass 12: from a multisampled one
  kColourMip,    // pass 13: a coarser level of the colour chain
  kCount,
};
constexpr uint32_t PassOf(Kind kind) {
  switch (kind) {
    case Kind::kDepth: return 0;
    case Kind::kDepthMs: return 1;
    case Kind::kMip: return 2;
    case Kind::kNormal: return 27;
    case Kind::kNormalMs: return 28;
    case Kind::kColour: return 11;
    case Kind::kColourMs: return 12;
    case Kind::kColourMip: return 13;
    case Kind::kCount: break;
  }
  return 0;
}
constexpr uint32_t Bit(Kind kind) { return 1u << uint32_t(kind); }
inline constexpr uint32_t kAllKinds = (1u << uint32_t(Kind::kCount)) - 1;

// The pipelines a frame's extraction records with: the depth pass for the depth target's samples (and the coarser levels when `levels`), the normal pass for the plane's, the colour chain
// for the colour target's (and its mip pass). `depth_samples` and `plane_samples` of 1 are the plain passes, above 1 the multisampled ones.
constexpr uint32_t KindsFor(bool depth, bool levels, bool normals, bool colour, bool multisampled, bool plane_multisampled) {
  uint32_t kinds = 0;
  if (depth) kinds |= Bit(multisampled ? Kind::kDepthMs : Kind::kDepth);
  if (depth && levels) kinds |= Bit(Kind::kMip);
  if (normals) kinds |= Bit(plane_multisampled ? Kind::kNormalMs : Kind::kNormal);
  if (colour) kinds |= Bit(multisampled ? Kind::kColourMs : Kind::kColour) | Bit(Kind::kColourMip);
  return kinds;
}

// --- the cost model --------------------------------------------------------------------------------------------------------------------------

namespace cost {

// One pass of a frame: what it is, the texels it writes and the bytes it moves (reads of its inputs through the texture cache counted once per texel fetched; a model, not a measurement).
struct Pass {
  const char* what = "";
  uint64_t write_bytes = 0;
  uint64_t read_bytes = 0;
};

inline constexpr uint32_t kBytesDepthTarget = 4;   // the guest's depth (D24S8's 24 bits read as 4 bytes)
inline constexpr uint32_t kBytesStencil = 1;       // the stencil plane's byte at the chosen source
inline constexpr uint32_t kBytesR16 = 2;
inline constexpr uint32_t kBytesR32 = 4;
inline constexpr uint32_t kBytesRg16 = 4;
inline constexpr uint32_t kBytesRgba16 = 8;
inline constexpr uint32_t kBytesColourTarget = 4;  // the main colour target's 2_10_10_10 float

constexpr uint64_t Texels(uint32_t w, uint32_t h) { return uint64_t(w) * uint64_t(h); }

// The depth chain's level 0: one pixel reads one depth sample (four at half size: the 2x2 block) and one stencil byte.
constexpr Pass DepthLevel0(uint32_t traced_w, uint32_t traced_h, bool half) {
  const uint64_t n = Texels(traced_w, traced_h);
  return {"depth level 0", n * kBytesR32, n * ((half ? 4u : 1u) * kBytesDepthTarget + kBytesStencil)};
}
// A coarser level: four texels of the level below per texel.
constexpr Pass DepthMip(uint32_t w, uint32_t h, uint32_t below_bytes, const char* what) { return {what, Texels(w, h) * kBytesR16, Texels(w, h) * 4ull * below_bytes}; }
// The normal image: the plane's RT1 texel (RG16: 4 bytes) and the depth's mark per pixel.
constexpr Pass Normal(uint32_t w, uint32_t h, const char* what) { return {what, Texels(w, h) * kBytesRg16, Texels(w, h) * (4ull + kBytesR32)}; }
// The colour chain's level 0: `taps` samples of the colour target per texel (one pixel per sample: all the samples of a 2x2 block, or one each), then two coarser levels.
constexpr Pass ColourLevel0(uint32_t w, uint32_t h, uint32_t taps) { return {"colour chain level 0", Texels(w, h) * kBytesRgba16, Texels(w, h) * uint64_t(taps) * kBytesColourTarget}; }
constexpr Pass ColourMip(uint32_t w, uint32_t h, const char* what) { return {what, Texels(w, h) * kBytesRgba16, Texels(w, h) * 4ull * kBytesRgba16}; }
// The Hi-Z chain (screen_shared.cpp RecordHiz): level 0 is the depth with its marks cleared (one read a texel), each level above the min of four of the one below.
constexpr Pass HizLevel(uint32_t w, uint32_t h, bool level0, const char* what) { return {what, Texels(w, h) * kBytesR32, Texels(w, h) * (level0 ? 1ull : 4ull) * kBytesR32}; }

}  // namespace cost

}  // namespace mcla::gpu::screen::extract
