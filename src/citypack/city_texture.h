#pragma once
// ===========================================================================
// LARecomp city pack -- one city texture decoded to the host's formats (CITY-RT M2 package H2)
// ===========================================================================
// A DictionaryTexture (xtd_reader.h) becomes what the GPU takes: its mip chain as linear BC1 / BC2 / BC3 blocks (RGBA8 for a raw k_8_8_8_8 texture), the levels one after the other, the
// full size first, tightly packed (HitTexture::MipOffset / ChainBytes). The levels are the ones the game stored (the fetch constant's mip range: a texture of 512 pixels may have 3 of
// its 10 levels, one of lod_0.xtd has only the base): a texture is never made bigger or filled in; a sampler reads the last stored level for what is smaller (GpuTextureRow::mips).
//
// Also here: the texture's KEY (XXH3-64 of the base level's storage, guest_hash.h), its AVERAGE colour (what a far hit and M3's probes use: the linear mean of a small level, the game's
// piecewise linear gamma undone where the fetch constant asks), and the BC block decoder those means are made with.
// ===========================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"
#include "citypack/rsc_image.h"
#include "citypack/xtd_reader.h"

namespace larecomp::citypack {

struct DecodedTexture {
  bool ok = false;
  std::string why;                // when !ok
  uint32_t width = 0;
  uint32_t height = 0;
  HitTextureFormat format = HitTextureFormat::kBc1;
  uint32_t flags = 0;             // kTextureGamma, kTextureAlpha
  uint32_t mip_count = 0;         // levels in `data`
  uint32_t stored_levels = 0;     // the fetch constant's
  uint32_t damaged_levels = 0;    // stored levels dropped from the end of the chain (a block beyond the resource)
  std::vector<uint8_t> data;      // the levels' blocks, the full size first
  uint64_t key = 0;               // XXH3-64 of the base level's guest storage (0 when the resource is cut before its end: see key_complete)
  bool key_complete = false;
  float average[3] = {0.5f, 0.5f, 0.5f};  // linear, 0..1
  float alpha_mean = 1.0f;
  uint8_t alpha_min = 255;
  float alpha_low_share = 0.0f;       // the share of texels of the mean's level with alpha below 128
  float alpha_extreme_share = 1.0f;   // ... with alpha at most 16 or at least 239 (a cut-out is nearly all extreme, a gloss map in the alpha is not)
  uint32_t average_level = 0;     // the level the means were made from
};

// The texture of `image` (the resource `texture` was read from).
DecodedTexture DecodeCityTexture(const RscImage& image, const DictionaryTexture& texture);
// Its key without decoding it (XXH3-64 of the base level's storage). False when the base level is not wholly inside the resource.
bool CityTextureKey(const RscImage& image, const DictionaryTexture& texture, uint64_t* key);

// ---------------------------------------------------------------------------
// the pieces (public for the tests)
// ---------------------------------------------------------------------------

// The game's piecewise linear gamma to linear (the translated shaders' pwlGammaToLinearScalar, as the car hit shading's CarMatPwl), 0..1 to 0..1.
float PwlGammaToLinear(float gamma);

// A level's blocks (width x height texels, whole blocks) as RGBA8 texels, `width * height * 4` bytes. BC1: the transparent colour of the 3 colour mode has alpha 0; BC2 alpha is the
// 4 bit explicit; BC3 alpha is the 8 value interpolated one; RGBA8 is copied.
void DecodeBlocksToRgba8(HitTextureFormat format, const uint8_t* blocks, uint32_t width, uint32_t height, std::vector<uint8_t>* rgba);

struct LevelMean {
  float rgb[3] = {0.5f, 0.5f, 0.5f};  // linear
  float alpha_mean = 1.0f;
  uint8_t alpha_min = 255;
  float alpha_low_share = 0.0f;
  float alpha_extreme_share = 1.0f;
};

// kTextureAlpha: the alpha is COVERAGE (a cut-out: fences, railings, foliage cards, neon) and not a data channel (the retail city has 8,500 BC3 pictures and nearly all carry a smooth
// specular or gloss map in the alpha: only about one in ten is a cut-out). At least 2 percent of the texels below 128 and at least 85 percent at either extreme. CHECK: a heuristic on the
// pixels, not the game's own flag; H1 knows the shader class (alpha tested, blended) and overrides it where it knows.
inline constexpr float kCoverageLowShare = 0.02f;
inline constexpr float kCoverageExtremeShare = 0.85f;
bool AlphaIsCoverage(const LevelMean& mean);
// The mean colour of a level. Texels the BC1 punch through mode made transparent do not count; every other texel does (the alpha of a BC2 or BC3 texture may be a data channel).
LevelMean MeanOfLevel(HitTextureFormat format, bool gamma, const uint8_t* blocks, uint32_t width, uint32_t height);

// The largest level that is at most this many texels on its longer side is the one the mean is made from (the base when it is small enough, the smallest stored level otherwise).
inline constexpr uint32_t kAverageLevelMaxSide = 64;

}  // namespace larecomp::citypack
