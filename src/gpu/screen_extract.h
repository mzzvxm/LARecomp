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
// SS-3 (FUSED Hi-Z, see "the fused Hi-Z" below): while the shared screen frame makes the Hi-Z (the reflections want it), the depth passes write its first levels beside their own images in the same draw (a
// second render target), so that the Hi-Z's own passes for them are not recorded: level 0 with the depth pass, levels 1 and 2 with the two coarser depth levels. The images are the ones the separate passes
// make, to the bit (hlsl/screen_hiz.hlsli, screen_hiz_fuse_test.cpp); mcla_gpu_screen_hiz_fused false records the separate passes (the A/B).
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
#include "ssgi_lights.h"

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

// --- the fused Hi-Z (SS-3) --------------------------------------------------------------------------------------------------------------------

// The Hi-Z's levels 0, 1 and 2 are written by the depth chain's own passes (kDepthHiz / kDepthMsHiz for level 0, kMipHiz for levels 1 and 2) when the frame makes the Hi-Z and the pipelines are
// there; the levels above them by the Hi-Z's own pass (screen_shared.cpp RecordHiz's mode 1), one a level, as before. Three because those are the levels whose size is a depth level's: one draw has
// one target size.
inline constexpr uint32_t kHizFusedMaxLevels = 3;

// How many of the Hi-Z's levels (`hiz_levels` = screen::HizLevelCount(traced size), at most) the depth chain's passes write this frame: level 0 always (the depth pass runs in every frame that has a
// Hi-Z), levels 1 and 2 beside the coarser depth levels while the frame makes them (`f.levels`) and while the Hi-Z level's size is that depth level's (it is, by the same halving rule: ao_size.h Half and
// screen_frame.h HizLevelSize; the test holds the two over every size, and a size that broke it would simply fuse less).
constexpr uint32_t FusedHizLevels(const Frame& f, uint32_t hiz_levels) {
  if (hiz_levels == 0 || f.traced[0] == 0 || f.traced[1] == 0) {
    return 0;
  }
  uint32_t fused = 1;
  if (f.levels && hiz_levels > 1 && screen::HizLevelSize(f.traced[0], 1) == f.level[0][0] && screen::HizLevelSize(f.traced[1], 1) == f.level[0][1]) {
    fused = 2;
    if (hiz_levels > 2 && screen::HizLevelSize(f.traced[0], 2) == f.level[1][0] && screen::HizLevelSize(f.traced[1], 2) == f.level[1][1]) {
      fused = 3;
    }
  }
  return fused;
}

// The fused depth pass's block: the plain depth pass's (DepthPush) unchanged; the Hi-Z's level 0 is the second target of the draw and the shader reads nothing more for it.
inline AoPush DepthHizPush(const AoPush& base, const Frame& f, uint32_t stencil_slot, bool source_bits) { return DepthPush(base, f, stencil_slot, source_bits); }

// The fused coarser level's block: the plain pass's (DepthMipPush) with slots.w the slot of the image the Hi-Z's level takes its four texels from: for level 1 the depth chain's level 0 itself (the same
// slot as the mean's source: the shader shares its four loads and takes the marks off each), for level 2 the Hi-Z's level 1 (the depth chain's level 1 is a mean and cannot stand for them).
inline AoPush DepthMipHizPush(const AoPush& level0_push, const Frame& f, uint32_t level, uint32_t source_slot, uint32_t hiz_source_slot) {
  AoPush push = DepthMipPush(level0_push, f, level, source_slot);
  push.slots[3] = hiz_source_slot;
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
  // GI-BOUNCE-SS (ssgi_lights.h, hlsl/ao_color_ps.hlsl LightsHistory): the last frame's light, added to the level 0 texels. Off unless the frame has a usable light history; then the push block carries what the pass
  // reprojects with and the flag, and nothing else of the block changes.
  struct Lights {
    bool on = false;
    uint32_t post_slot = 0;       // the last frame's colour after the deferred light pass (RGBA16F at the traced size) -> slots.z
    uint32_t pre_slot = 0;        // ... and before it -> slots.w
    uint32_t depth_slot = 0;      // this frame's linear depth, level 0 -> slots2.y
    uint32_t depth_prev_slot = 0; // the last frame's (the depth history) -> slots2.w
    uint32_t normal_slot = 0;     // the normal image's (0: none) -> the high half of slots.y
    float motion[12] = {};        // the 3x4 that moves this frame's view space into the last frame's, row by row -> lin, light, extra
    float tan_prev[2] = {};       // the last frame's tan half field of view -> lens.zw
    float gain = 1.0f;            // shape.z
    float cap_floor = 0.0f;       // shape.w: the lights' own cap floor (ssgi_lights.h LightsCap; 0 = the chain's cap alone)
    // GI-BOUNCE-JOIN: the chain runs LIGHTS-ONLY (the probe field stands: ssgi_lights.h kFlagOnly). Independent of `on`: with no usable history the lights-only chain is black.
    bool only = false;
  };
  Lights lights;
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
  if (c.lights.only) {
    push.slots2[2] |= ssgi_lights::kFlagOnly;
  }
  if (c.lights.on) {
    // GI-BOUNCE-SS: the fields the lights' share reads (the pass reads none of them otherwise: ssgi_lights.h kFlag, hlsl/ao_color_ps.hlsl).
    push.slots2[2] |= ssgi_lights::kFlag;
    push.slots[1] = (c.host_samples & ssgi_lights::kHostSamplesMask) | (c.lights.normal_slot << ssgi_lights::kNormalShift);
    push.slots[2] = c.lights.post_slot;
    push.slots[3] = c.lights.pre_slot;
    push.slots2[1] = c.lights.depth_slot;
    push.slots2[3] = c.lights.depth_prev_slot;
    push.lens[2] = c.lights.tan_prev[0];
    push.lens[3] = c.lights.tan_prev[1];
    for (int k = 0; k < 4; ++k) {
      push.lin[k] = c.lights.motion[k];
      push.light[k] = c.lights.motion[4 + k];
      push.extra[k] = c.lights.motion[8 + k];
    }
    push.shape[2] = c.lights.gain;
    push.shape[3] = c.lights.cap_floor;
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
  // The FUSED passes (SS-3): the depth chain's passes with the Hi-Z's level of the same size written beside their image (two render targets, a pipeline of two formats). Not passes of the AO's table:
  // their shaders are host_shaders.h ScreenFusedDepthPixelShader's (FusedPassOf).
  kDepthHiz,     // kDepth + the Hi-Z's level 0 (R32_FLOAT, the depth's marks off)
  kDepthMsHiz,   // kDepthMs + the same
  kMipHiz,       // kMip (R16_FLOAT) + the Hi-Z's level 1 or 2 (R32_FLOAT)
  kCount,
};
inline constexpr uint32_t kFirstFusedKind = uint32_t(Kind::kDepthHiz);
constexpr bool IsFused(Kind kind) { return uint32_t(kind) >= kFirstFusedKind && kind != Kind::kCount; }
// The AO table's pass number of a kind that has one; the fused kinds have none (they are not the AO's passes: FusedPassOf).
inline constexpr uint32_t kNoAoPass = ~0u;
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
    case Kind::kDepthHiz:
    case Kind::kDepthMsHiz:
    case Kind::kMipHiz: return kNoAoPass;
    case Kind::kCount: break;
  }
  return 0;
}
// host_shaders.h ScreenFusedDepthPixelShader's number of a fused kind.
constexpr uint32_t FusedPassOf(Kind kind) {
  switch (kind) {
    case Kind::kDepthHiz: return 0;
    case Kind::kDepthMsHiz: return 1;
    case Kind::kMipHiz: return 2;
    default: break;
  }
  return 0;
}
constexpr uint32_t Bit(Kind kind) { return 1u << uint32_t(kind); }
inline constexpr uint32_t kAllKinds = (1u << uint32_t(Kind::kCount)) - 1;
// The kinds that are the AO's own passes (what the AO's frame holds itself off for until they are made) and the fused ones.
inline constexpr uint32_t kBaseKinds = (1u << kFirstFusedKind) - 1;
inline constexpr uint32_t kFusedKinds = kAllKinds & ~kBaseKinds;

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

// The pipelines the START's choices use (SS-PRECACHE), listed with the boot precache: what the AO's own table listed for them before SS-2 moved the passes here (ao.cpp VariantsFor, the old path's
// list). The depth pass for both sample counts (the host's MSAA level changes while the game runs, msaa_level.h: a switch finds the other made), the coarser level pass, and, where the bounce light
// is on at the start, the colour chain's three. The fused ones (SS-3) and the normal passes are not here: they were never in that list (a frame goes without a pipeline that is not made yet, and
// ChainReady lists them the first frame that wants them).
constexpr uint32_t StartKinds(bool depth, bool mip, bool colour) {
  uint32_t kinds = 0;
  if (depth) kinds |= Bit(Kind::kDepth) | Bit(Kind::kDepthMs);
  if (mip) kinds |= Bit(Kind::kMip);
  if (colour) kinds |= Bit(Kind::kColour) | Bit(Kind::kColourMs) | Bit(Kind::kColourMip);
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

// THE FUSED passes (SS-3): the depth pass and the coarser levels with the Hi-Z's level written beside their image. What a fused pass moves is the sum of the two passes it replaces less what they shared:
// the depth chain's reads are the same, the Hi-Z's own reads of its level below are gone where that level is the depth chain's level 0 (level 1: the four texels the mean reads) or the pass's own output
// (level 0: the texel written), and what it writes is both images.
constexpr Pass DepthLevel0Hiz(uint32_t traced_w, uint32_t traced_h, bool half) {
  Pass p = DepthLevel0(traced_w, traced_h, half);
  p.what = "depth level 0 + Hi-Z level 0";
  p.write_bytes += Texels(traced_w, traced_h) * kBytesR32;
  return p;
}
// level 1: the mean reads the four R32 texels of level 0 and the Hi-Z takes the same four; level 2: the mean reads four R16 texels of level 1 and the Hi-Z four R32 texels of its own level 1.
constexpr Pass DepthMipHiz(uint32_t w, uint32_t h, uint32_t below_bytes, bool hiz_reads_its_own, const char* what) {
  Pass p = DepthMip(w, h, below_bytes, what);
  p.write_bytes += Texels(w, h) * kBytesR32;
  if (hiz_reads_its_own) {
    p.read_bytes += Texels(w, h) * 4ull * kBytesR32;
  }
  return p;
}

// The passes the owner records for the Hi-Z of one frame whose depth chain has the traced size w x h (`levels`: the coarser depth levels are made), into `out` (room for kHizMaxLevels + 2); the count.
// `fused` false: the separate passes (one a level, the first copying the depth chain's level 0 with its marks off, as SS-0 made them) and the depth chain's own passes as they were; true: the fused ones.
// The depth chain's own passes are listed too, so that both arrangements are the whole of what the depth and the Hi-Z cost together.
inline uint32_t DepthAndHizPasses(uint32_t w, uint32_t h, bool half, bool levels, bool hiz, bool fused, Pass* out) {
  uint32_t n = 0;
  const uint32_t hiz_levels = hiz ? screen::HizLevelCount(w, h) : 0u;
  Frame f;
  f.traced[0] = w;
  f.traced[1] = h;
  f.level[0][0] = (w + 1) / 2;
  f.level[0][1] = (h + 1) / 2;
  f.level[1][0] = (w + 3) / 4;
  f.level[1][1] = (h + 3) / 4;
  f.levels = levels;
  const uint32_t made = fused ? FusedHizLevels(f, hiz_levels) : 0u;
  out[n++] = made >= 1 ? DepthLevel0Hiz(w, h, half) : DepthLevel0(w, h, half);
  if (levels) {
    out[n++] = made >= 2 ? DepthMipHiz(f.level[0][0], f.level[0][1], kBytesR32, false, "depth level 1 + Hi-Z level 1") : DepthMip(f.level[0][0], f.level[0][1], kBytesR32, "depth level 1");
    out[n++] = made >= 3 ? DepthMipHiz(f.level[1][0], f.level[1][1], kBytesR16, true, "depth level 2 + Hi-Z level 2") : DepthMip(f.level[1][0], f.level[1][1], kBytesR16, "depth level 2");
  }
  for (uint32_t level = made; level < hiz_levels; ++level) {
    out[n++] = HizLevel(screen::HizLevelSize(w, level), screen::HizLevelSize(h, level), level == 0, "Hi-Z level");
  }
  return n;
}

}  // namespace cost

}  // namespace mcla::gpu::screen::extract
