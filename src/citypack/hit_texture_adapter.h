#pragma once
// ===========================================================================
// LARecomp city pack -- package H2's texture set as the hit builder's texture source (CITY-RT M2, packages H1 and H2)
// ===========================================================================
// H1 (hit_builder.h) and H2 (texture_set.h) were written side by side from the frozen interface (city_hit_format.h) and meet here. The hit file's texture section is H2's table (a
// HitTexture for each picture, offsets into the texture file included), the texture file is H2's (WriteTextureFile, with the hit file's table hash in its header), and a material
// names its texture the way H2's set resolves it:
//
//   * by the NAME the shader instance has ("wall_brick_01.dds"): H2 hashes it (xtd_reader.h TextureNameHash: the rage hash of the lower case name with ".dds"), and the builder never
//     does;
//   * SEEN FROM the dictionary of the sector the shader is in (kDictionaryCommon 0, kDictionaryLod 1, DictionaryOfSector(i) = 2 + i: the same numbers as hit_material.h's
//     kHitDictionaryCommon / kHitDictionaryLod / HitDictionaryOfSector, checked below), with H2's fallbacks (the sector's own, then common, then lod_0; the far level of detail names lod_0
//     first, then common);
//   * to the PICTURE (HitMaterial::texture, an index in the table: a picture is stored once) and the guest KEY OF THE ENTRY the name came from (HitMaterial::key: the XXH3-64 of that
//     entry's base level storage). The key is per entry, not per picture: the same picture is in the dictionaries of several sectors under different keys (14,946 keys for 8,342 pictures
//     in the retail game), and GB-F2's rows (`hash:<key>`) and the texture dumps name the key the renderer sees on the draw, so a row applies to the hits on a texture as to its
//     pixels. Two sectors that use one shader instance and one texture name can therefore have two materials (the key is part of a material's identity, hit_material.h).
//   * the average colour of a hit's texture is H2's (`average`), the mean of the picture.
//
// CHECK: the key is the XXH3-64 of the entry's base level STORAGE bytes as the guest has them; whether the renderer's texture hash (textures.h ContentHashOf, the number
// GB-F2's census logs) is taken over the same bytes is not proven here: the first game run's census and this file's keys must be compared (a census line's hash against a material's key).
//
// Compiled only where texture_set.h is (the two packages are on separate branches until they are merged: __has_include); LARECOMP_HIT_HAS_TEXTURE_SET says so.
// ===========================================================================

#include "citypack/hit_material.h"

#if __has_include("citypack/texture_set.h")
#define LARECOMP_HIT_HAS_TEXTURE_SET 1

#include <string>
#include <string_view>

#include "citypack/texture_set.h"
#include "citypack/xtd_reader.h"

namespace larecomp::citypack {

static_assert(kHitDictionaryCommon == kDictionaryCommon && kHitDictionaryLod == kDictionaryLod && HitDictionaryOfSector(0) == DictionaryOfSector(0) &&
                  HitDictionaryOfSector(438) == DictionaryOfSector(438),
              "the hit builder and the texture set number the dictionaries alike");

class CityTextureSource final : public HitTextureSource {
 public:
  // `set` may be null (a source that only has an identity: the currency check needs nothing else); it is not owned and must outlive the build.
  explicit CityTextureSource(const TextureSet* set = nullptr) : set_(set) {}
  void SetTextureSet(const TextureSet* set) { set_ = set; }

  std::string Identity() const override { return "city-textures:" + std::to_string(kTextureRulesVersion); }
  TextureEntry Resolve(uint32_t dictionary, std::string_view file_name) const override {
    TextureEntry out;
    if (set_ == nullptr) return out;
    const ResolvedTexture r = set_->ResolveEntry(dictionary, TextureNameHash(file_name));
    if (!r.found()) return out;
    out.picture = r.index;
    out.key = r.key;
    return out;
  }
  const std::vector<HitTexture>& Table() const override {
    static const std::vector<HitTexture> none;
    return set_ != nullptr ? set_->textures : none;
  }
  bool Average(uint32_t picture, float rgb[3]) const override {
    if (set_ == nullptr || picture >= set_->average.size()) return false;
    for (int k = 0; k < 3; ++k) rgb[k] = set_->average[picture][size_t(k)];
    return true;
  }
  bool Finish(const std::filesystem::path& texture_file, uint64_t hit_table_hash, std::string* error) override {
    if (set_ == nullptr) return true;
    return WriteTextureFile(texture_file, *set_, hit_table_hash, error);
  }

 private:
  const TextureSet* set_ = nullptr;
};

}  // namespace larecomp::citypack

#endif  // __has_include("citypack/texture_set.h")
