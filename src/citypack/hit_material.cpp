#include "citypack/hit_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>

#include "citypack/hit_params.h"
#include "citypack/material_class.h"

namespace larecomp::citypack {

namespace gb = mcla::gpu::gbuffer;

namespace {

uint32_t FloatBits(float v) { return std::bit_cast<uint32_t>(v); }

const ShaderParam* FindFloat(const ShaderParams* params, uint32_t hash) {
  if (!params) return nullptr;
  const ShaderParam* p = params->Find(hash);
  return p && !p->empty && p->type == kParamTypeFloat4 ? p : nullptr;
}

bool ReadColour(const ShaderParams* params, uint32_t hash, float out[3]) {
  const ShaderParam* p = FindFloat(params, hash);
  if (!p) return false;
  for (int k = 0; k < 3; ++k) {
    if (!std::isfinite(p->value[k])) return false;
  }
  for (int k = 0; k < 3; ++k) out[k] = std::clamp(p->value[k], 0.0f, kHitRgb10Max);
  return true;
}

// The vertex shader gloss row of a group ("xCityDefault__VSCityDefaultDay"): the day city shaders are lit per vertex.
bool HasVertexGloss(const std::string& group) {
  const std::string prefix = "x" + group + "__V";
  for (const gb::GlossRow& row : gb::kGlossRows) {
    if (row.bank == gb::GlossBank::kVertex && row.name.substr(0, prefix.size()) == prefix) return true;
  }
  return false;
}

}  // namespace

uint32_t GlowKindOfGroup(std::string_view group) {
  std::string lower(group);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const auto has = [&](const char* word) { return lower.find(word) != std::string::npos; };
  if (has("lampglass")) return kGlowLens;
  if (has("trafficsignal")) return kGlowAlways;
  return kGlowNight;
}

RowGlow GlowAfterRow(uint32_t word, float emissive_before, uint8_t kind_before) {
  RowGlow g;
  const uint32_t cls = WordClass(word);
  const bool had = std::isfinite(emissive_before) && emissive_before > 0.0f;
  const float unlit = 1.0f - float(WordLit(word)) * (1.0f / 255.0f);
  if (cls == uint32_t(gb::SurfaceClass::kEmissive)) {
    g.emissive = had ? emissive_before : 1.0f;
    g.kind = had ? kind_before : uint8_t(kGlowNight);
  } else if (cls != uint32_t(gb::SurfaceClass::kNone) && unlit > 0.0f) {
    g.emissive = had ? std::max(emissive_before, unlit) : unlit;
    g.kind = had ? kind_before : uint8_t(kGlowNight);
  }
  return g;
}

std::string MaterialDef::Key() const {
  char head[176];
  std::snprintf(head, sizeof(head), "w%08x|t%08x,%08x,%08x|e%08x,%u|s%08x|f%d|", word, FloatBits(tint[0]), FloatBits(tint[1]), FloatBits(tint[2]), FloatBits(emissive), unsigned(glow_kind), shader_hash,
                flat_colour ? 1 : 0);
  char picked[40];
  std::snprintf(picked, sizeof(picked), "p%08x,%08x,%08x|", picture, uint32_t(texture_key), uint32_t(texture_key >> 32));
  return std::string(head) + picked + texture;
}

MaterialDef MaterialFromShader(const ShaderNames* names, MaterialClass base_class, MaterialFacts* facts) {
  MaterialFacts local;
  if (!facts) facts = &local;
  *facts = MaterialFacts();
  MaterialDef def;
  if (!names) return def;
  def.shader_hash = ShaderHash(names->group, names->preset);
  const std::string pixel_name = "x" + names->group + "__PSMultiLight";
  const gb::ShaderMaterial m = gb::MaterialOfShaderName(pixel_name);
  facts->group_known = m.known;
  const ShaderParams* params = names->params.get();
  facts->no_params = params == nullptr;
  if (!m.known) return def;  // another game build or a mod's shader: the default surface, the shader's own hash

  gb::SurfaceClass cls = m.cls;
  if (base_class == MaterialClass::kInterior && cls == gb::SurfaceClass::kWorld) cls = gb::SurfaceClass::kInterior;
  float smoothness = gb::DefaultSmoothness(cls);
  const float lit = gb::DefaultLit(cls);

  const ShaderParam* exponent_param = FindFloat(params, kParamSpecularExponent);
  if (exponent_param && std::isfinite(exponent_param->value[0]) && exponent_param->value[0] >= 0.0f) {
    facts->has_exponent = true;
    const float exponent = exponent_param->value[0];
    // The bank the raster side reads (gbuffer_material.h MaterialTable::WordFor): the pixel shader's when its lit entry has a row, else the vertex shader's.
    const gb::GlossRow* pixel_row = gb::FindGlossRow(pixel_name);
    if (pixel_row != nullptr && pixel_row->bank == gb::GlossBank::kPixel) {
      smoothness = gb::GlossSmoothness(gb::GlossBank::kPixel, exponent, smoothness);
      facts->gloss_pixel = true;
    } else if (HasVertexGloss(names->group)) {
      smoothness = gb::GlossSmoothness(gb::GlossBank::kVertex, exponent, smoothness);
      facts->gloss_vertex = true;
    }
  }
  uint32_t flags = m.flags & 7u;
  if (base_class == MaterialClass::kAlphaTested) flags |= kWordFlagAlphaCovered;
  def.word = PackMaterialWord(gb::ToByte(smoothness), uint32_t(cls), flags, gb::ToByte(lit), 0);

  if (params) {
    const ShaderParam* texture = params->Find(kParamDiffuseSampler);
    if (!texture || texture->empty) texture = params->Find(kParamDiffuseSamplerA);
    if (texture && !texture->empty && texture->type == kParamTypeTexture) def.texture = texture->texture;
  }
  const bool found_tint = ReadColour(params, kParamDiffuseColor, def.tint) || ReadColour(params, kParamTintColor, def.tint) || ReadColour(params, kParamWindowTintColor, def.tint) ||
                          ReadColour(params, kParamNightColor, def.tint) || ReadColour(params, kParamDayColor, def.tint);
  def.flat_colour = found_tint && def.texture.empty();
  if (cls == gb::SurfaceClass::kEmissive) {
    def.emissive = 1.0f;
    def.glow_kind = uint8_t(GlowKindOfGroup(names->group));
    const ShaderParam* glow = FindFloat(params, kParamNeonIntensity);
    if (!glow) glow = FindFloat(params, kParamNightIntensity);
    if (glow && std::isfinite(glow->value[0]) && glow->value[0] > 0.0f) def.emissive = glow->value[0];
  }
  return def;
}

MaterialSet::MaterialSet() {
  MaterialDef def;  // material 0: the default surface
  index_.emplace(def.Key(), 0u);
  defs_.push_back(std::move(def));
}

uint32_t MaterialSet::Add(const MaterialDef& def) {
  const std::string key = def.Key();
  const auto [it, inserted] = index_.try_emplace(key, uint32_t(defs_.size()));
  if (inserted) defs_.push_back(def);
  return it->second;
}

bool FinalizeMaterials(const MaterialSet& set, const HitTextureSource* textures, const gb::TextureTable* rows, std::vector<HitMaterial>* materials, std::vector<HitTexture>* table,
                       FinalizeStats* stats, std::string* error) {
  FinalizeStats local;
  if (!stats) stats = &local;
  *stats = FinalizeStats();
  table->clear();
  if (textures != nullptr) *table = textures->Table();
  if (table->size() > kMaxTextures) {
    if (error) *error = "the texture source has more textures than a 16 bit index holds";
    return false;
  }
  std::unordered_map<std::string, bool> names;  // distinct diffuse names -> some material resolved it
  materials->clear();
  materials->reserve(set.size());
  for (size_t index = 0; index < set.defs().size(); ++index) {
    const MaterialDef& def = set.defs()[index];
    HitMaterial m;  // the defaults are the default surface's: a mid grey
    m.word = def.word;
    m.emissive = def.emissive;
    m.glow_kind = def.glow_kind;
    m.shader_hash = def.shader_hash;
    if (index != 0) {
      for (int k = 0; k < 3; ++k) {
        m.tint[k] = def.tint[k];
        // the tint alone where the tint IS the colour, the class grey tinted where there is no texture
        m.average[k] = def.flat_colour ? def.tint[k] : def.tint[k] * 0.5f;
      }
    }
    if (!def.texture.empty()) {
      ++stats->materials_textured;
      bool& resolved = names.try_emplace(def.texture, false).first->second;
      if (def.picture != kNoTexture) {
        if (def.picture >= table->size()) {
          if (error) *error = "a material names a picture that is not in the texture source's table";
          return false;
        }
        resolved = true;
        m.texture = uint16_t(def.picture);
        m.key = def.texture_key;
        float mean[3];
        if (textures != nullptr && textures->Average(def.picture, mean)) {
          for (int k = 0; k < 3; ++k) m.average[k] = std::min(def.tint[k] * mean[k], kHitRgb10Max);
        }
      } else {
        ++stats->materials_unresolved;
        if (stats->unresolved_names.size() < 12) stats->unresolved_names.push_back(def.texture);
      }
      if (rows != nullptr) {
        const gb::TextureRow* row = rows->Find(std::string_view(), TextureStem(def.texture), m.key);
        if (row != nullptr) {
          const uint32_t changed = gb::ApplyTextureRow(m.word, *row);
          if (changed != m.word) {
            ++stats->rows_applied;
            // a row that says "emissive" or "lights itself" makes a lit window glow; one that takes the class away takes the glow (R7)
            const RowGlow glow = GlowAfterRow(changed, m.emissive, m.glow_kind);
            if (glow.emissive > 0.0f && !(m.emissive > 0.0f)) ++stats->rows_glow_on;
            if (!(glow.emissive > 0.0f) && m.emissive > 0.0f) ++stats->rows_glow_off;
            m.emissive = glow.emissive;
            m.glow_kind = glow.kind;
          }
          m.word = changed;
        }
      }
    }
    materials->push_back(m);
  }
  stats->texture_names = uint32_t(names.size());
  for (const auto& entry : names) {
    if (entry.second) ++stats->names_resolved;
  }
  return true;
}

bool LoadTextureRows(const std::vector<std::filesystem::path>& files, gb::TextureTable* table, uint64_t* hash, size_t* bad_rows) {
  uint64_t h = 0;
  size_t bad = 0;
  bool any = false;
  for (const std::filesystem::path& file : files) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) continue;
    std::ifstream in(file, std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    any = true;
    h = Fnv1a64(text.data(), text.size(), h == 0 ? 0xCBF29CE484222325ull : h);
    gb::TableParse parse;
    gb::ParseTextureTable(text, table, &parse);
    bad += parse.errors.size();
  }
  if (hash) *hash = any ? (h == 0 ? 1 : h) : 0;
  if (bad_rows) *bad_rows = bad;
  return true;
}

}  // namespace larecomp::citypack
