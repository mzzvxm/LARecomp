// MCLA GPU renderer -- the shadow lookup in HLSL: THE TWIN of src/gpu/light_shadow_lookup.h (perf/LIGHT-UNIFY.md 4.2 "one lookup source", package LU-5).
//
// For a pass that reads a light's shadow from the atlas by the light's ShadowRef (GpuLight packed1: shadow kind, first view, view count; shadow_weight): the deferred light pass GB-H1/H2 and
// any later reader. It is NOT included by the lit shaders' headlight variant or by the volume's inject, which keep their own copies of the lookup (a text change moves the variant pack; the
// volume's shader is compiled): shadow_atlas_test.cpp holds those two to the same C++ reference this file is held to, so that when a run has shown the pictures equal they can include this
// file in place of the copy and nothing changes (CHECK: the main session decides when).
//
// THE SEAMS. The file DECLARES these five (below, after LslView) and the including shader DEFINES them, anywhere after the include (the file reads the atlas and the view table through them, so
// that it does not depend on how the pass binds them):
//     float LslTexel(uint slot, int ix, int iy);   one texel of the atlas's depth plane, as a float; the index clamped into the atlas (Load with a clamp, or a clamp sampler)
//     uint  LslAtlasWidth(uint slot);              the atlas's size in texels
//     uint  LslAtlasHeight(uint slot);
//     LslView LslLoadView(uint entry);             entry `entry` of the frame's view table (GpuShadowBuffer: a 32 byte header, then the 96 byte rows of LocalShadowView)
//     uint  LslLayerOf(uint entry);                its layer: LslLayerBits(header.layers, entry)
// src/gpu/light_shadow_lookup_test (shadow_atlas_test.cpp) compiles THIS FILE as C++ through a shim of the HLSL types, so the text keeps to a subset both languages read: scalar arithmetic,
// .x .y .z .w, no swizzles of more than one component, floor / min / max / lerp only, no file-scope constants of the static kind (every number is a #define).
//
// THE MODES. LSL_MODE_SURFACE is the lit shaders' kernel (a box of LSL_KERNEL texels square, taps not clamped to the tile, a depth in [0, 1] is looked up), LSL_MODE_AIR the volume's (bilinear
// inside the tile, a depth in (0, 1)). A deferred pass on surfaces takes LSL_MODE_SURFACE.
//
// THE TEXT must not use the names of the game's registers (r0..r31, c0..c255, p0, ps, a0, aL): every name here starts with Lsl or LSL_.

#ifndef LARECOMP_LIGHT_SHADOW_LOOKUP_HLSLI
#define LARECOMP_LIGHT_SHADOW_LOOKUP_HLSLI

#define LSL_MODE_SURFACE 0
#define LSL_MODE_AIR 1
#define LSL_KERNEL 3.0f
// the most entries a light may name: the headlights' table (8). The deferred light pass raises it to its combined entry space (the street lamps' table after the headlights': local_light_ps.hlsl).
#ifndef LSL_MAX_VIEWS
#define LSL_MAX_VIEWS 8
#endif

#define LSL_LAYER_SINGLE 0
#define LSL_LAYER_STATIC 1
#define LSL_LAYER_LIVE 2

#define LSL_SHADOW_NONE 0
#define LSL_SHADOW_ATLAS 1
#define LSL_SHADOW_RT_MASK 2

// the table's layout (light_shadow_lookup.h GpuShadowBuffer)
#define LSL_HEADER_BYTES 32
#define LSL_VIEW_BYTES 96

// One row of the view table: LocalShadowView (local_shadow_types.h), 96 bytes. m0..m3 = world_to_shadow's rows (out = M * (p, 1), then / w: atlas uv in xy, depth in z); rect = u0 v0 u1 v1
// of the view's tile; bias = the constant depth bias; texel = one texel as a share of the tile; the last two are the rig's light index and the valid flag.
struct LslView {
  float4 m0;
  float4 m1;
  float4 m2;
  float4 m3;
  float4 rect;
  float bias;
  float texel;
  uint light_index;
  uint valid;
};

// The seams (the including shader defines them).
float LslTexel(uint slot, int ix, int iy);
uint LslAtlasWidth(uint slot);
uint LslAtlasHeight(uint slot);
LslView LslLoadView(uint entry);
uint LslLayerOf(uint entry);

// The two bits of entry `entry` in the header's `layers`.
uint LslLayerBits(uint layers, uint entry) {
  return (layers >> (entry * 2u)) & 3u;
}

// A depth tap against the receiver's depth: 1 lit (the game's `>=`), 0 in shadow.
float LslTap(uint slot, int ix, int iy, float depth) {
  return LslTexel(slot, ix, iy) >= depth ? 1.0f : 0.0f;
}

int LslClampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// The lit shaders' kernel: the share of a box LSL_KERNEL texels square, centred on the receiver's place in the atlas (tux, tuy = one texel in uv on each axis), whose texels are lit, each
// counted by the part of it the box covers. The four outer taps of the 4 x 4 footprint go first: when they agree the answer is that and the other twelve are not taken.
float LslBox(uint slot, float su, float sv, float tux, float tuy, float depth) {
  float lx = su / tux - 0.5f * LSL_KERNEL;
  float ly = sv / tuy - 0.5f * LSL_KERNEL;
  float bx = floor(lx);
  float by = floor(ly);
  float fx = lx - bx;
  float fy = ly - by;
  int ix = int(bx);
  int iy = int(by);
  float corners = LslTap(slot, ix, iy, depth) + LslTap(slot, ix + 3, iy, depth) + LslTap(slot, ix, iy + 3, depth) + LslTap(slot, ix + 3, iy + 3, depth);
  if (corners == 0.0f || corners == 4.0f) {
    return corners * 0.25f;
  }
  float lit = 0.0f;
  for (int j = 0; j < 4; ++j) {
    float wy = j == 0 ? 1.0f - fy : (j == 3 ? fy : 1.0f);
    float row = 0.0f;
    for (int i = 0; i < 4; ++i) {
      float wx = i == 0 ? 1.0f - fx : (i == 3 ? fx : 1.0f);
      row += wx * LslTap(slot, ix + i, iy + j, depth);
    }
    lit += wy * row;
  }
  return lit * (1.0f / 9.0f);
}

// The volume's kernel: the bilinear weights of the 2 x 2 texels around the receiver's place, every tap clamped into the view's tile, a stored 1 lit whatever the bias.
float LslBilinear(uint slot, float su, float sv, float4 rect, float ref_depth) {
  float sx = float(LslAtlasWidth(slot));
  float sy = float(LslAtlasHeight(slot));
  int lox = int(floor(rect.x * sx + 0.5f));
  int loy = int(floor(rect.y * sy + 0.5f));
  int hix = max(int(floor(rect.z * sx + 0.5f)) - 1, lox);
  int hiy = max(int(floor(rect.w * sy + 0.5f)) - 1, loy);
  float tcx = su * sx - 0.5f;
  float tcy = sv * sy - 0.5f;
  int i0x = int(floor(tcx));
  int i0y = int(floor(tcy));
  float frx = tcx - float(i0x);
  float fry = tcy - float(i0y);
  float a = LslTexel(slot, LslClampInt(i0x, lox, hix), LslClampInt(i0y, loy, hiy));
  float b = LslTexel(slot, LslClampInt(i0x + 1, lox, hix), LslClampInt(i0y, loy, hiy));
  float c = LslTexel(slot, LslClampInt(i0x, lox, hix), LslClampInt(i0y + 1, loy, hiy));
  float d = LslTexel(slot, LslClampInt(i0x + 1, lox, hix), LslClampInt(i0y + 1, loy, hiy));
  float s00 = (a >= 1.0f || a >= ref_depth) ? 1.0f : 0.0f;
  float s10 = (b >= 1.0f || b >= ref_depth) ? 1.0f : 0.0f;
  float s01 = (c >= 1.0f || c >= ref_depth) ? 1.0f : 0.0f;
  float s11 = (d >= 1.0f || d >= ref_depth) ? 1.0f : 0.0f;
  return lerp(lerp(s00, s10, frx), lerp(s01, s11, frx), fry);
}

// One view at the world point (px, py, pz): 1 lit .. 0 in shadow. A view that is not valid, a point behind it or outside its rectangle or depth range is lit.
float LslViewLit(LslView v, float px, float py, float pz, uint slot, uint mode) {
  if (v.valid == 0u) {
    return 1.0f;
  }
  float cx = v.m0.x * px + v.m0.y * py + v.m0.z * pz + v.m0.w;
  float cy = v.m1.x * px + v.m1.y * py + v.m1.z * pz + v.m1.w;
  float cz = v.m2.x * px + v.m2.y * py + v.m2.z * pz + v.m2.w;
  float cw = v.m3.x * px + v.m3.y * py + v.m3.z * pz + v.m3.w;
  if (!(cw > 0.0f)) {
    return 1.0f;
  }
  float su = cx / cw;
  float sv = cy / cw;
  float sz = cz / cw;
  bool depth_out = mode == LSL_MODE_AIR ? !(sz > 0.0f && sz < 1.0f) : (sz < 0.0f || sz > 1.0f);
  if (depth_out || su < v.rect.x || sv < v.rect.y || su > v.rect.z || sv > v.rect.w) {
    return 1.0f;
  }
  float depth = sz - v.bias;
  if (mode == LSL_MODE_SURFACE) {
    float tux = (v.rect.z - v.rect.x) * v.texel;
    float tuy = (v.rect.w - v.rect.y) * v.texel;
    return LslBox(slot, su, sv, tux, tuy, depth);
  }
  return LslBilinear(slot, su, sv, v.rect, depth);
}

// A light's MEAN visibility from its entries (GpuLight: shadow kind, first view, view count): the one combine rule without the weight (light_laws.h CombineShadow). The valid views are
// averaged and a LSL_LAYER_STATIC entry followed by its LSL_LAYER_LIVE twin is one view whose visibility is their product. A light with no atlas views is lit (1).
float LslLightMean(uint shadow_kind, uint first_view, uint view_count, float px, float py, float pz, uint slot, uint mode) {
  if (shadow_kind != LSL_SHADOW_ATLAS || view_count == 0u) {
    return 1.0f;
  }
  float sum = 0.0f;
  uint n = 0u;
  uint end = min(first_view + view_count, uint(LSL_MAX_VIEWS));
  for (uint e = first_view; e < end; ++e) {
    LslView v = LslLoadView(e);
    float vis = LslViewLit(v, px, py, pz, slot, mode);
    if (LslLayerOf(e) == LSL_LAYER_STATIC && e + 1u < end && LslLayerOf(e + 1u) == LSL_LAYER_LIVE) {
      vis *= LslViewLit(LslLoadView(e + 1u), px, py, pz, slot, mode);
      ++e;
    }
    if (v.valid != 0u) {
      sum += vis;
      ++n;
    }
  }
  if (n == 0u) {
    return 1.0f;
  }
  return sum / float(n);
}

// The mean blended by the light's weight: lit = lerp(1, mean, weight), exact at weight 1.
float LslBlend(float mean, float weight) {
  return weight >= 1.0f ? mean : 1.0f + (mean - 1.0f) * weight;
}

// A light's visibility from its ShadowRef (GpuLight: shadow kind, first view, view count; shadow_weight): the one combine rule (light_laws.h CombineShadow). The valid views are averaged,
// a LSL_LAYER_STATIC entry followed by its LSL_LAYER_LIVE twin is one view whose visibility is their product, and the share is blended by the weight. A light with no atlas views is lit.
float LslLightLit(uint shadow_kind, uint first_view, uint view_count, float weight, float px, float py, float pz, uint slot, uint mode) {
  if (shadow_kind != LSL_SHADOW_ATLAS || view_count == 0u) {
    return 1.0f;
  }
  return LslBlend(LslLightMean(shadow_kind, first_view, view_count, px, py, pz, slot, mode), weight);
}

#endif  // LARECOMP_LIGHT_SHADOW_LOOKUP_HLSLI
