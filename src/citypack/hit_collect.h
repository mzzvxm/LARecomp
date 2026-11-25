#pragma once
// ===========================================================================
// LARecomp city pack -- the corner attributes of a mesh and its runs (CITY-RT M2, package H1)
// ===========================================================================
// The pack's mesh (mesh_builder.h) welds the source vertices by position, so a triangle's corner has no attributes of its own there. The hit data keeps them PER TRIANGLE CORNER
// (city_hit_format.h): the texture coordinates of UV set 0 and the vertex colour, read from the source vertices of exactly the triangles the weld kept, in the mesh's own order (the
// opaque triangles first, then the alpha tested ones). HitCollector is told, geometry by geometry, which source triangles the MeshBuilder kept (its `kept_triangles`), reads their
// corners, and at the end makes the HitBlob: the corner words (uv as unorm16 over the run's range, the colour as RGBA8) and the RUNS (triangles of one material with one UV range).
//
// THE SOURCE VERTEX (settled from the retail game's 14 vertex declarations and the translated vertex shaders; perf/CITY-RT-M2.md CHECK C2 and C3, section 3):
//   * the declaration (SourceGeometry::fvf, ::fvf_sizes) is a rage FVF: bit 0 position, bit 3 normal, bit 4 COLOR0, bit 5 COLOR1, bits 6..13 TEXCOORD0..7, bit 14 tangent;
//     a 64 bit table of channel formats says what each channel is (4 bits a channel: 6 = float3 at 12 bytes for the position, 1 = half2 for a texture coordinate, 9 = a 4 byte colour,
//     a = a packed normal or tangent, 4 bytes). The city has 14 different declarations, strides 16 to 40; the channels sit in bit order, tightly packed, so the offset of a channel is
//     the sum of the sizes of the channels before it (VertexLayout checks that the sum is the stride).
//   * TEXCOORD0 is the DIFFUSE coordinate of every city shader that has a diffuse texture (read from CityBumpSpec, CityDefault, CityGrime, CityRoad ...: DiffuseSampler is fetched at
//     the interpolated TEXCOORD0). TEXCOORD1 exists in 12 groups (the roads, the sidewalks, the garage floor, the decal groups, the ocean shore); it is the coordinate of their second
//     LAYER (the lane markings, the decal), which version 1 does not keep (the second layer's texture is never written: city_hit_format.h HitMaterial::layer). The census counts
//     the triangles that have a second set (CHECK C3).
//   * COLOR1 is the baked vertex lighting: ALPHA = the ambient occlusion the lit result is multiplied by (the vertex shader mixes it with the frame's occlusion strength:
//     oColor1.w = (1 - a) OcclusionInfo.w + a), RGB = a small added tint (the retail values average 0.05) that the pixel shader scales by a per-draw constant. COLOR0 (the grime
//     shaders only) is a per-vertex mask of the grime layer. The corner keeps COLOR1 (CHECK C2). The word is a D3DCOLOR (A in the top byte, then R, G, B): the corner's RGBA8
//     is made from it.
// ===========================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"
#include "citypack/sector_parse.h"

namespace larecomp::citypack {

// The channel formats of the 4 bit codes the retail declarations use (rage's FVF data sizes on the Xbox 360).
inline constexpr uint32_t kFvfPosition = 1u << 0;
inline constexpr uint32_t kFvfNormal = 1u << 3;
inline constexpr uint32_t kFvfColour0 = 1u << 4;
inline constexpr uint32_t kFvfColour1 = 1u << 5;
inline constexpr uint32_t kFvfTexcoord0 = 1u << 6;
inline constexpr uint32_t kFvfTexcoord1 = 1u << 7;
inline constexpr uint32_t kChannelPosition = 6;  // float3
inline constexpr uint32_t kChannelHalf2 = 1;     // a texture coordinate, two big endian float16
inline constexpr uint32_t kChannelColour = 9;    // a D3DCOLOR, four bytes
inline constexpr uint32_t kChannelPacked = 10;   // a normal or a tangent, four bytes

struct VertexLayout {
  bool valid = false;  // the declaration is one this reads (the sizes add up to the stride)
  uint32_t stride = 0;
  int32_t colour0 = -1;  // byte offsets of the channels, -1 when the vertex has none (or not in the format the hit data reads)
  int32_t colour1 = -1;
  int32_t uv0 = -1;
  int32_t uv1 = -1;
};
// The layout of a declaration: `fvf` its channel mask, `channel_sizes` the 64 bit table. valid is false for a declaration whose sizes are unknown or do not add up to the stride
// (a synthetic test resource, another game build): such a geometry has no corner attributes.
VertexLayout LayoutOfDeclaration(uint32_t fvf, uint64_t channel_sizes, uint32_t stride);

// A D3DCOLOR word (A in the top byte) as the corner's RGBA8 (r in bits 0..7, a in 24..31: city_hit_format.h PackCornerColour).
constexpr uint32_t CornerColourOfD3dColor(uint32_t word) { return PackCornerColour((word >> 16) & 0xFFu, (word >> 8) & 0xFFu, word & 0xFFu, word >> 24); }
inline constexpr uint32_t kWhiteCorner = 0xFFFFFFFFu;  // the colour of a corner the vertex has no colour for: white, ambient occlusion 1

// What the builder may throw away to keep the hit pack inside the owner's 200 to 300 MB (with the textures): the corners are made lossy here, before they are coded (the corner
// stream itself, hit_corner_codec.h, is lossless). The numbers are in the stamp. Measured on the retail game (hit file, 67.9 M corners; uv step / AO bits / tint bits): the indexed
// stream alone, lossless (0 / 8 / 8) = 187.5 MiB; 12 / 8 / 8 = 179.4 MiB; 12 / 7 / 6 (these defaults) = 166.1 MiB; 11 / 6 / 4 = 140.6 MiB; 10 / 6 / 4 = 137.7 MiB. The defaults lose
// next to nothing a reflection could show (1/16 texel at 256 pixels, a 0.8 percent step of ambient occlusion) and leave the textures (30 to 67 MB) room inside the budget.
struct HitRules {
  // The texture coordinate's step is never finer than 2^-uv_step_log2 of a texture repeat: a run whose range is narrower than 65535 x that step is given a transform of that
  // range, so its quantised coordinates are small numbers (cheap to code). 12 = 1/4096 of a repeat: a sixteenth of a texel at 256 pixels, an eighth at 512; the textures are read
  // at a mip level of the reflection's cone, never finer than the cone. 0 = the full 16 bits of every run (no loss beyond the unorm16).
  uint32_t uv_step_log2 = 12;
  // The vertex colour's alpha (the baked ambient occlusion) keeps this many bits, linear (8 = all).
  uint32_t ao_bits = 7;
  // The colour's rgb (a small baked tint: the retail values average 0.05) keeps this many bits on a square root scale, so that the small values stay apart (8 = all).
  uint32_t tint_bits = 6;

  // A number for the stamp.
  uint64_t Pack() const { return uint64_t(uv_step_log2) | (uint64_t(ao_bits) << 8) | (uint64_t(tint_bits) << 16); }
  // The corner colour word after the loss.
  uint32_t QuantiseColour(uint32_t corner_colour) const;
};

// What the collector counted (summed over the sectors by the builder; the census prints them).
struct CollectStats {
  uint64_t geometries = 0;
  uint64_t triangles = 0;
  uint64_t corners = 0;
  uint64_t geometries_without_uv = 0;      // no TEXCOORD0 in a format the hit data reads
  uint64_t geometries_without_colour = 0;  // no COLOR1
  uint64_t geometries_layout_unknown = 0;  // the declaration did not read (no attributes at all)
  uint64_t geometries_second_uv = 0;       // TEXCOORD1 present (CHECK C3)
  uint64_t triangles_second_uv = 0;
  uint64_t geometries_colour0 = 0;         // COLOR0 present (the grime mask)
  uint64_t uv_not_finite = 0;              // corners whose texture coordinate was a NaN or an infinity (kept as 0)
  uint64_t runs = 0;
  uint64_t runs_split = 0;                 // runs made by cutting a geometry whose UV range was wider than kMaxRunUvRange
  uint64_t triangles_uv_clamped = 0;       // triangles whose range does not fit a transform at all (clamped into one)
  void Add(const CollectStats& o);
};

class HitCollector {
 public:
  explicit HitCollector(const HitRules& rules = HitRules()) : rules_(rules) {}
  // One source geometry that was added to the MeshBuilder: `kept` is MeshBuilder::AddGeometry's kept_triangles (source triangle numbers in the order they were appended), `alpha` says
  // which list they went to (!IsOpaqueClass(class)), `material` is the local material number the runs carry.
  void AddGeometry(const SourceGeometry& g, bool alpha, const std::vector<uint32_t>& kept, uint32_t material);
  // The blob: runs (cut at every geometry, and inside one where the UV range would be wider than kMaxRunUvRange) and the corner words, the opaque list first. `opaque_triangles` and
  // `triangle_count` are the finished mesh's: false (with `error`) when they are not what was collected (the builder's two passes disagree).
  bool Finish(uint32_t opaque_triangles, uint32_t triangle_count, HitBlob* out, std::string* error);
  const CollectStats& stats() const { return stats_; }

 private:
  struct Corner {
    float u = 0.0f, v = 0.0f;
    uint32_t colour = kWhiteCorner;
  };
  struct Rec {
    uint32_t first = 0;  // first triangle within its list
    uint32_t count = 0;
    uint32_t material = 0;
    bool has_uv = false;
    bool has_colour = false;
  };
  struct List {
    std::vector<Corner> corners;  // 3 a triangle
    std::vector<Rec> recs;
    uint32_t triangles = 0;
  };
  bool Build(bool split, uint32_t opaque_triangles, HitBlob* out, CollectStats* stats) const;

  HitRules rules_;
  List lists_[2];  // 0 opaque, 1 alpha tested
  CollectStats stats_;
};

}  // namespace larecomp::citypack
