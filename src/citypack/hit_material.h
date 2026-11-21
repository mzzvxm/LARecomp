#pragma once
// ===========================================================================
// LARecomp city pack -- the materials of the hit data (CITY-RT M2, package H1)
// ===========================================================================
// A sector's shader group has one instance of an effect for each material; the hit data makes ONE GLOBAL TABLE of them (material 0 is the default surface), so that a run of
// any sector names its material by one number. A material is (shader, texture, parameters): the same wall texture under two presets is two materials, two sectors that use one
// instance's values share one. What a material says is made from the instance's parameters (hit_params.h) through the raster G-buffer's own rules (src/gpu/gbuffer_material.h, part
// 1 of the material channel), so that a reflected wall has the class and gloss the wall has on screen:
//
//   word        the G-buffer's material word + the metal byte (city_hit_format.h): the CLASS and the SSR flag from the group's row (kGroupRows: "x" + the group's name), the smoothness
//               from SpecularExponent through the same formula and the same bank (the day city shaders are lit per vertex: the vertex shader's exponent, c73 ...; the others by their
//               PSMultiLight's: GlossBank, GlossSmoothness), the class default where there is no exponent, the lit weight from the class (0 for what glows), kWordFlagAlphaCovered
//               for the alpha tested ones (the pack's class), the metal byte 0 (only a texture row gives one)
//   texture     the DIFFUSE texture's file name: DiffuseSampler, or DiffuseSamplerA (the terrain and the grass have no DiffuseSampler); resolved, seen from the dictionary of the sector
//               the shader is in, to a picture (an index in the texture table) and the guest KEY of the dictionary entry it came from, by the HitTextureSource (below), which package
//               H2's texture set implements (texture_set.h; hit_texture_adapter.h joins them). The key is per ENTRY (the same picture sits in several sectors' dictionaries under
//               different guest keys), so two sectors that use one shader instance with one texture name can have two materials: the key is part of a material's identity.
//   tint        the flat colour of the materials that have one: DiffuseColor (CityGrimeMetalTint ...), TintColor, WindowTintColor, and for a glowing tint NightColor, else DayColor;
//               (1, 1, 1) otherwise
//   emissive    for the glowing class only: NeonIntensity, NightIntensity (CityNeonTint, CityStoreSign), 1 where it has neither
//   average     tint x the diffuse texture's mean colour where the texture resolved, tint x 0.5 where it has a texture that did not resolve, the tint alone where it has none
//
// The per-texture rows of GB-F2 (gbuffer_materials.h: class=, rough=, metal=, ssr=, lit= by `hash:<XXH3-64 of the base level>`) are applied by the same ApplyTextureRow the raster side
// uses, when the builder is given the table files: by the texture's key, which the texture source gives. The records written WITHOUT rows are the F1 words; the runtime may apply the rows
// itself when the files change (the key is in the record).
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "citypack/city_hit_format.h"
#include "citypack/pack_format.h"
#include "citypack/sector_parse.h"
#include "gpu/gbuffer_material.h"
#include "gpu/gbuffer_materials.h"

namespace larecomp::citypack {

// What makes a material (the key of the global table).
struct MaterialDef {
  uint32_t word = PackMaterialWord(38, 1, 0, 255, 0);  // HitMaterial's default: smoothness 0.15, class world, lit
  float tint[3] = {1.0f, 1.0f, 1.0f};
  float emissive = 0.0f;
  std::string texture;  // the diffuse texture's file name as the resource has it ("wall_brick_01.dds"), "" none
  uint32_t picture = kNoTexture;  // the texture source's answer for `texture`: the picture (an index in its table); kNoTexture: none, or not found
  uint64_t texture_key = 0;       // ... and the guest key of the dictionary entry (HitMaterial::key)
  bool flat_colour = false;  // no texture and the tint is the colour of the surface
  uint32_t shader_hash = 0;  // rage hash of "<group>/<preset>" (the pack's runs carry the same number)

  // The canonical text of every field above: two defs are one material when their keys are equal.
  std::string Key() const;
};

// How a def was made (the census counts these).
struct MaterialFacts {
  bool group_known = false;   // the group has a row in the class table
  bool gloss_pixel = false;   // the smoothness is from the pixel shader's SpecularExponent
  bool gloss_vertex = false;  // ... from the vertex shader's
  bool has_exponent = false;  // the instance has a SpecularExponent
  bool no_params = false;     // the instance's parameters could not be read
};

// The def of a shader instance: `names` has the group, the preset and the parameters (ParseSector with read_shader_params); `base_class` is the pack's class of the geometry
// (ClassifyShader), which decides the alpha flag and the interior class. A null `names` or an instance without parameters gives the class's words and no texture.
MaterialDef MaterialFromShader(const ShaderNames* names, MaterialClass base_class, MaterialFacts* facts);

// The global table: material 0 is the default; Add returns the number of an equal material that is there, or makes a new one.
class MaterialSet {
 public:
  MaterialSet();
  uint32_t Add(const MaterialDef& def);
  const std::vector<MaterialDef>& defs() const { return defs_; }
  size_t size() const { return defs_.size(); }

 private:
  std::vector<MaterialDef> defs_;
  std::unordered_map<std::string, uint32_t> index_;
};

// ---------------------------------------------------------------------------
// the texture source (package H2's texture set is one: hit_texture_adapter.h)
// ---------------------------------------------------------------------------

// The dictionaries a texture name is seen from (texture_set.h of package H2 numbers them the same way; hit_texture_adapter.h checks it at compile time): the common dictionary,
// the far level of detail's, and each sector's own, in the city table's order.
inline constexpr uint32_t kHitDictionaryCommon = 0;
inline constexpr uint32_t kHitDictionaryLod = 1;
constexpr uint32_t HitDictionaryOfSector(uint32_t sector_index) { return 2u + sector_index; }

// What a texture name is in one dictionary: the picture (an index in the source's table) and the guest key of the entry the name resolved to (the XXH3-64 of the base level's storage:
// the number GB-F2's rows and the texture dumps name a texture by). kNoTexture: no dictionary has the name.
struct TextureEntry {
  uint32_t picture = kNoTexture;
  uint64_t key = 0;
  bool found() const { return picture != kNoTexture; }
};

class HitTextureSource {
 public:
  virtual ~HitTextureSource() = default;
  // What the stamp keeps of the source's rules (its version): another identity makes the hit pack stale. "none" for no source.
  virtual std::string Identity() const { return "none"; }
  // The name `file_name` ("wall_brick_01.dds", as the shader instance has it) as seen from `dictionary`, with the source's fallbacks (the sector's own dictionary, then the common one).
  virtual TextureEntry Resolve(uint32_t dictionary, std::string_view file_name) const {
    (void)dictionary;
    (void)file_name;
    return TextureEntry();
  }
  // The HitTexture records the hit file's texture section holds (a TextureEntry::picture is a position in it), and the linear mean colour of a picture (false: not known).
  virtual const std::vector<HitTexture>& Table() const {
    static const std::vector<HitTexture> none;
    return none;
  }
  virtual bool Average(uint32_t picture, float rgb[3]) const {
    (void)picture;
    (void)rgb;
    return false;
  }
  // Called once the hit file is in place: write the texture file (kTexFileName) with `hit_table_hash` (HitHeader::table_hash) in its header.
  virtual bool Finish(const std::filesystem::path& texture_file, uint64_t hit_table_hash, std::string* error) {
    (void)texture_file;
    (void)hit_table_hash;
    (void)error;
    return true;
  }
};

// ---------------------------------------------------------------------------
// the finished table
// ---------------------------------------------------------------------------

struct FinalizeStats {
  uint32_t texture_names = 0;        // distinct diffuse texture names
  uint32_t names_resolved = 0;       // ... some material's name resolved (in the dictionary of the sector it is used in)
  uint32_t materials_textured = 0;   // materials that have a texture name
  uint32_t materials_unresolved = 0; // ... whose texture did not resolve (the average stands)
  uint32_t rows_applied = 0;         // materials a GB-F2 row changed
  std::vector<std::string> unresolved_names;  // the first few
};

// The records: `set`'s defs turned into HitMaterial (texture index, key and average from the defs' resolved pictures and the source's means; the rows applied when `rows` is given) and
// the texture records (the source's table; empty without a source).
bool FinalizeMaterials(const MaterialSet& set, const HitTextureSource* textures, const mcla::gpu::gbuffer::TextureTable* rows, std::vector<HitMaterial>* materials, std::vector<HitTexture>* table,
                       FinalizeStats* stats, std::string* error);

// The text of the GB-F2 table files (the hit builder's `texture_rows`), read in order into one table; `hash` is a hash of the files' bytes (0: none), for the stamp. Missing files are
// skipped, bad rows are counted in `bad_rows`.
bool LoadTextureRows(const std::vector<std::filesystem::path>& files, mcla::gpu::gbuffer::TextureTable* table, uint64_t* hash, size_t* bad_rows);

}  // namespace larecomp::citypack
