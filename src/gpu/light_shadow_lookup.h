#pragma once
// ===========================================================================
// MCLA GPU renderer -- ONE lookup of a light's shadow, for every reader of the atlas (perf/LIGHT-UNIFY.md 4.2 "one lookup source", package LU-5)
// ===========================================================================
// Three readers look at the atlas's depth plane with a light's views: the lit shaders' headlight variant (shaderpack/variants/headlight.cpp HeadlightShadow: four gathers of the 4 x 4 footprint
// compared at the receiver's own plane, a contact hardening kernel, a fade at the tile's border: light_shadow_filter.h is its CPU twin), the volumetric light's inject
// (hlsl/volumetrics_inject_cs.hlsl HeadlightLit: a BILINEAR compare inside the tile, in the air) and the deferred light pass (GB-H1/H2). They agree on everything but the filter: the view's
// matrix, rectangle, bias and texel size (the 96 byte LocalShadowView), what is lit (stored depth >= receiver depth - bias, a stored 1 is nothing drawn), what is outside (lit), and what a
// light's views make (the views of a light are AVERAGED, the layers of a view are MULTIPLIED, the share is blended by the light's weight: light_laws.h CombineShadow). This file is that one
// source:
//
//   ViewLit      one view at a world point, by a Mode: kSurface (the variant's lookup: it needs the surface's unit normal, the receiver's plane) or kAir (the volume's).
//   LightLit     a light's visibility from its ShadowRef (kind, first_view, view_count, weight) in the frame's ShadowFrame: the one combine rule. 1 = lit.
//   GpuShadow*   the layout of the view table a GPU pass reads (a 32 byte header and kMaxLightShadowViews rows of the 96 byte view): the data of the lookup for GB-H, with its builder.
//
// IT IS THE REFERENCE the other copies are held to. hlsl/light_shadow_lookup.hlsli is the HLSL twin written for a NEW reader (GB-H1/H2 includes it); shadow_atlas_test.cpp compiles it as
// C++ and compares it with this on random views and atlases, and holds this file to light_shadow_filter.h ViewLit (the variant's CPU twin, itself held to the variant's text) and to a port of
// the volume's HeadlightLit. The volume's inject is NOT changed by HL-SHADOW-QUALITY; it changes when a run has shown the pictures equal, and the goldens say what it must still equal (CHECK:
// the main session decides when).
//
// WHAT A MODE FIXES:
//   kSurface  light_shadow_filter.h ViewLit: four gathers (here: sixteen texel reads) of the 4 x 4 footprint, each texel compared with the receiver's depth AT ITS OWN PLANE (the slope from the
//             unit normal `n` and the view's matrix, so a road seen at a grazing angle needs no bias margin), a box of 1 to 3 texels by contact hardening, a fade over the last 5 percent of
//             the tile; the taps are NOT clamped to the tile (the sampler clamps at the atlas's edge), a receiver whose depth is outside [0, 1] (inclusive) is lit. The overloads without a
//             normal look at a receiver that faces the light (the view's axis, reversed): the plane is parallel to the map and the slope is 0.
//   kAir      bilinear weights of the 2 x 2 texels around the place, every tap clamped INTO THE VIEW'S TILE (a neighbour tile is another light's view), a receiver whose depth is not in
//             (0, 1) (exclusive) is lit, a stored 1 is lit whatever the bias.
//
// Pure: no renderer, no guest. Namespace larecomp::gpu::shadow_lookup.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "light_frame.h"
#include "light_shadow_filter.h"
#include "local_shadow_types.h"

namespace larecomp::gpu::shadow_lookup {

enum class Mode : uint32_t { kSurface = 0, kAir = 1 };

// One view at the world point `p` whose surface has the unit normal `normal` (null: facing the light): 1 lit .. 0 in shadow. `depth_at(ix, iy)` = the atlas's stored depth of texel (ix, iy), the
// index clamped into the atlas by the caller (the sampler's or Load's clamp). `atlas_w` / `atlas_h` = the atlas's size in texels. A view that is not valid, a point behind it or outside its
// rectangle or depth range is lit.
template <class Depth>
inline float ViewLitImpl(const LocalShadowView& v, const float p[3], const float* normal, uint32_t atlas_w, uint32_t atlas_h, Mode mode, Depth&& depth_at) {
  if (v.valid == 0) {
    return 1.0f;
  }
  if (mode == Mode::kSurface) {
    float facing[3];
    if (normal == nullptr) {
      // a receiver that faces the light: the reverse of the view's axis (row 3 of the matrix, the unit direction the view looks along)
      const float* m = v.world_to_shadow;
      const float len = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
      const float k = len > 0.0f ? -1.0f / len : 0.0f;
      facing[0] = m[12] * k;
      facing[1] = m[13] * k;
      facing[2] = m[14] * k;
      normal = facing;
    }
    // the four texels one Gather returns, in the order the hardware does: x = (0, 1), y = (1, 1), z = (1, 0), w = (0, 0) of the quad whose lowest texel is (ix, iy)
    const auto gather = [&](int ix, int iy) {
      shadow_filter::Quad q;
      q.x = depth_at(ix, iy + 1);
      q.y = depth_at(ix + 1, iy + 1);
      q.z = depth_at(ix + 1, iy);
      q.w = depth_at(ix, iy);
      return q;
    };
    return shadow_filter::ViewLit(v, p, normal, gather);
  }
  float c[4];
  for (int r = 0; r < 4; ++r) {
    const float* row = v.world_to_shadow + 4 * r;
    c[r] = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
  }
  if (!(c[3] > 0.0f)) {
    return 1.0f;
  }
  const float su = c[0] / c[3];
  const float sv = c[1] / c[3];
  const float sz = c[2] / c[3];
  const float* rect = v.atlas_rect;
  const bool depth_out = !(sz > 0.0f && sz < 1.0f);
  if (depth_out || su < rect[0] || sv < rect[1] || su > rect[2] || sv > rect[3]) {
    return 1.0f;
  }
  const float sx = float(atlas_w);
  const float sy = float(atlas_h);
  const int lox = int(std::floor(rect[0] * sx + 0.5f));
  const int loy = int(std::floor(rect[1] * sy + 0.5f));
  const int hix = std::max(int(std::floor(rect[2] * sx + 0.5f)) - 1, lox);
  const int hiy = std::max(int(std::floor(rect[3] * sy + 0.5f)) - 1, loy);
  const float tcx = su * sx - 0.5f;
  const float tcy = sv * sy - 0.5f;
  const int i0x = int(std::floor(tcx));
  const int i0y = int(std::floor(tcy));
  const float frx = tcx - float(i0x);
  const float fry = tcy - float(i0y);
  const float ref = sz - v.depth_bias;
  const auto clamp_to = [](int x, int lo, int hi) { return std::min(std::max(x, lo), hi); };
  const auto at = [&](int dx, int dy) { return depth_at(clamp_to(i0x + dx, lox, hix), clamp_to(i0y + dy, loy, hiy)); };
  const float a = at(0, 0), b = at(1, 0), cc = at(0, 1), d = at(1, 1);
  const float s00 = (a >= 1.0f || a >= ref) ? 1.0f : 0.0f;
  const float s10 = (b >= 1.0f || b >= ref) ? 1.0f : 0.0f;
  const float s01 = (cc >= 1.0f || cc >= ref) ? 1.0f : 0.0f;
  const float s11 = (d >= 1.0f || d >= ref) ? 1.0f : 0.0f;
  const auto lerp = [](float x, float y, float t) { return x + (y - x) * t; };
  return lerp(lerp(s00, s10, frx), lerp(s01, s11, frx), fry);
}

// A receiver that faces the light (no normal): the plane is parallel to the map. The air mode never reads a normal.
template <class Depth>
inline float ViewLit(const LocalShadowView& v, const float p[3], uint32_t atlas_w, uint32_t atlas_h, Mode mode, Depth&& depth_at) {
  return ViewLitImpl(v, p, nullptr, atlas_w, atlas_h, mode, depth_at);
}

// A receiver of the unit normal `normal` (the surface's own, turned to the camera: the pass takes the depth plane's quad normal, the lit shaders' variant the derivatives of the world position).
template <class Depth>
inline float ViewLit(const LocalShadowView& v, const float p[3], const float normal[3], uint32_t atlas_w, uint32_t atlas_h, Mode mode, Depth&& depth_at) {
  return ViewLitImpl(v, p, normal, atlas_w, atlas_h, mode, depth_at);
}

// The blend of a light's mean visibility by its weight: lit = lerp(1, visibility, weight). EXACT at weight 1 (the answer is the mean itself: the live readers' `seen / views`), unlike 1 + (m - 1)
// that can differ in the last place.
inline float Blend(float mean, float weight) { return weight >= 1.0f ? mean : 1.0f + (mean - 1.0f) * weight; }

// A light's visibility at `p` (whose surface has the unit normal `normal`, null: facing the light) from its ShadowRef: the views [first_view, first_view + view_count) of the frame, the valid ones
// averaged (a kStatic entry followed by its kLive twin is ONE view whose visibility is their product), blended by the weight. A light with no atlas views (kNone, or kRtMask: the mask's own
// channel is read by its consumer) or none valid is lit. This is light_laws.h CombineShadow with each entry's lookup made here.
template <class Depth>
inline float LightLitImpl(const ShadowFrame& frame, const ShadowRef& ref, const float p[3], const float* normal, Mode mode, Depth&& depth_at) {
  if (ref.kind != ShadowKind::kAtlas || ref.view_count == 0) {
    return 1.0f;
  }
  float sum = 0.0f;
  uint32_t n = 0;
  const uint32_t end = std::min<uint32_t>(uint32_t(ref.first_view) + ref.view_count, kMaxLightShadowViews);
  for (uint32_t e = ref.first_view; e < end; ++e) {
    const bool valid = frame.views[e].valid != 0;
    float vis = ViewLitImpl(frame.views[e], p, normal, frame.atlas.width, frame.atlas.height, mode, depth_at);
    if (frame.view_layer[e] == ShadowLayer::kStatic && e + 1 < end && frame.view_layer[e + 1] == ShadowLayer::kLive) {
      vis *= ViewLitImpl(frame.views[e + 1], p, normal, frame.atlas.width, frame.atlas.height, mode, depth_at);
      ++e;
    }
    if (valid) {
      sum += vis;
      ++n;
    }
  }
  if (n == 0) {
    return 1.0f;
  }
  return Blend(sum / float(n), ref.weight);
}

template <class Depth>
inline float LightLit(const ShadowFrame& frame, const ShadowRef& ref, const float p[3], Mode mode, Depth&& depth_at) {
  return LightLitImpl(frame, ref, p, nullptr, mode, depth_at);
}

template <class Depth>
inline float LightLit(const ShadowFrame& frame, const ShadowRef& ref, const float p[3], const float normal[3], Mode mode, Depth&& depth_at) {
  return LightLitImpl(frame, ref, p, normal, mode, depth_at);
}

// ---------------------------------------------------------------------------
// the view table a GPU pass reads (GB-H)
// ---------------------------------------------------------------------------

// A 32 byte header then kMaxLightShadowViews rows of the 96 byte LocalShadowView (field for field: the forward block's and the volume's views are the same bytes). `layers` = two bits for
// each entry (ShadowLayer: 0 single, 1 static, 2 live) so that a pass applies the combine rule; `atlas_slot` is the bindless depth texture, the sizes are its texels. A light's ShadowRef
// (GpuLight packed1: first view, view count, kind) and weight (GpuLight::shadow_weight) index this table.
struct alignas(16) GpuShadowHeader {
  uint32_t count = 0;        // entries of the frame (ShadowFrame::count)
  uint32_t atlas_slot = kNoLightSlot;
  uint32_t atlas_width = 0;
  uint32_t atlas_height = 0;
  uint32_t layers = 0;       // two bits an entry
  uint32_t tile_small = 0;
  uint32_t tile_large = 0;
  uint32_t reserved = 0;
};
struct GpuShadowBuffer {
  GpuShadowHeader header;
  std::array<LocalShadowView, kMaxLightShadowViews> views{};
};
static_assert(sizeof(GpuShadowHeader) == 32 && alignof(GpuShadowHeader) == 16, "the header is two float4");
static_assert(sizeof(LocalShadowView) == 96, "a view row is 96 bytes");
static_assert(sizeof(GpuShadowBuffer) == 32 + 96 * kMaxLightShadowViews && offsetof(GpuShadowBuffer, views) == 32, "header, then the rows");
static_assert(std::is_trivially_copyable_v<GpuShadowBuffer>);
inline constexpr uint32_t kGpuShadowHeaderBytes = 32;
inline constexpr uint32_t kGpuShadowViewBytes = 96;

inline uint32_t LayerBits(ShadowLayer layer) { return uint32_t(layer) & 3u; }

// The table of a frame's views. With no valid atlas the slot is kNoLightSlot and the count 0 (a pass that finds no atlas skips the lookup: every light is lit).
inline void BuildGpuShadowBuffer(const ShadowFrame& frame, GpuShadowBuffer* out) {
  *out = GpuShadowBuffer{};
  if (!frame.atlas.valid) {
    return;
  }
  const uint32_t n = std::min<uint32_t>(frame.count, kMaxLightShadowViews);
  out->header.count = n;
  out->header.atlas_slot = frame.atlas.slot;
  out->header.atlas_width = frame.atlas.width;
  out->header.atlas_height = frame.atlas.height;
  out->header.tile_small = frame.atlas.tile_small;
  out->header.tile_large = frame.atlas.tile_large;
  for (uint32_t e = 0; e < n; ++e) {
    out->views[e] = frame.views[e];
    out->header.layers |= LayerBits(frame.view_layer[e]) << (2u * e);
  }
}

}  // namespace larecomp::gpu::shadow_lookup
