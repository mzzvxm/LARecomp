#pragma once
// ===========================================================================
// MCLA GPU renderer -- the compiled-in texture rows (PBR level 2, package PBR-2; perf/PBR.md section 2)
// ===========================================================================
// The per-texture table of gbuffer_materials.h reads the player's local text files at runtime. The compiled layer uses the same row format and ships empty. A texture without a row uses
// the shader family's material (gbuffer_material.h: class, family defaults and the draw's own exponent).
//
// THE LAYERS (lowest first): the shader family's word; this compiled-in table; the generated table file beside the shader pack; the mods folder's gbuffer_texture_materials.txt (the player's
// own rows); the paths of MCLA_GBUFFER_TEXTURE_MATERIALS. A texture found in a higher layer takes THAT layer's row whole, whatever keys the lower layers know it by (a player's `*/name` row beats
// a compiled-in `hash:` row), so an override has to give every field it wants, as a later file's row always did. Inside the compiled-in layer and inside each file the most specific key wins:
// hash, then dictionary/name, then */name.
//
// THE DATA (gbuffer_texture_rows.inc): three sorted arrays, one per kind of key, each starting with a sentinel of key 0. The keys match the text format (PairKey, NameKey and the XXH3-64
// content hash). emit_rows.py exercises this format with synthetic fixtures. The census produces the local runtime text file beside the player's shader pack.
//
// Cost: a lookup is a binary search of at most a dozen steps over a read-only array, and the renderer makes it once per texture (gbuffer_materials.cpp caches the answer by the texture's
// page), never per draw and never per pixel.
//
// Pure: no renderer, no I/O. Render thread (the arrays are constant).
// ===========================================================================

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gbuffer_materials.h"

namespace mcla::gpu::gbuffer {

// A row of the compiled-in table: the key (by the array it is in) and what the row changes.
struct CompiledRow {
  uint64_t key = 0;
  TextureRow row;
};

#include "gbuffer_texture_rows.inc"

// The three arrays without their sentinel rows.
inline constexpr std::span<const CompiledRow> kCompiledHashRowSpan = std::span<const CompiledRow>(kCompiledHashRows).subspan(1);
inline constexpr std::span<const CompiledRow> kCompiledPairRowSpan = std::span<const CompiledRow>(kCompiledPairRows).subspan(1);
inline constexpr std::span<const CompiledRow> kCompiledNameRowSpan = std::span<const CompiledRow>(kCompiledNameRows).subspan(1);

// A table as the lookups see it (the tests make their own).
struct CompiledView {
  std::span<const CompiledRow> hash;  // keyed by the XXH3-64 content hash
  std::span<const CompiledRow> pair;  // keyed by PairKey(dictionary, name)
  std::span<const CompiledRow> name;  // keyed by NameKey(name)
};
inline constexpr CompiledView kCompiledView = {kCompiledHashRowSpan, kCompiledPairRowSpan, kCompiledNameRowSpan};

// A table is sound when its sentinel is unused (key 0) and its keys strictly increase.
constexpr bool CompiledRowsSorted(std::span<const CompiledRow> table_with_sentinel) {
  if (table_with_sentinel.empty() || table_with_sentinel[0].key != 0) return false;
  for (size_t i = 2; i < table_with_sentinel.size(); ++i) {
    if (!(table_with_sentinel[i - 1].key < table_with_sentinel[i].key)) return false;
  }
  return table_with_sentinel.size() < 2 || table_with_sentinel[1].key != 0;
}
static_assert(CompiledRowsSorted(kCompiledHashRows), "gbuffer_texture_rows.inc: kCompiledHashRows is not sorted by strictly increasing key (or its sentinel is missing)");
static_assert(CompiledRowsSorted(kCompiledPairRows), "gbuffer_texture_rows.inc: kCompiledPairRows is not sorted by strictly increasing key (or its sentinel is missing)");
static_assert(CompiledRowsSorted(kCompiledNameRows), "gbuffer_texture_rows.inc: kCompiledNameRows is not sorted by strictly increasing key (or its sentinel is missing)");

inline constexpr size_t kCompiledRowCount = kCompiledHashRowSpan.size() + kCompiledPairRowSpan.size() + kCompiledNameRowSpan.size();  // after kCompiledView
// Whether a texture's content hash can find anything: when not, the renderer need not hash textures for this layer.
inline constexpr bool kCompiledHasHashRows = !kCompiledHashRowSpan.empty();

// The row of `key` in a table without its sentinel, null when there is none.
inline const TextureRow* SearchCompiledRows(std::span<const CompiledRow> table, uint64_t key) {
  if (key == 0) return nullptr;
  const auto at = std::lower_bound(table.begin(), table.end(), key, [](const CompiledRow& row, uint64_t k) { return row.key < k; });
  return at != table.end() && at->key == key ? &at->row : nullptr;
}

// The compiled-in row of a texture, in the order of the files' tables (gbuffer_materials.h TextureTable::Find): by its content hash (0: not known), else by dictionary and name, else by name
// alone; null when none. An unnamed texture is looked up by hash only.
inline const TextureRow* FindCompiledRow(const CompiledView& view, std::string_view dictionary, std::string_view name, uint64_t content_hash) {
  if (const TextureRow* row = SearchCompiledRows(view.hash, content_hash)) return row;
  if (IsUnnamed(name)) return nullptr;
  if (!dictionary.empty()) {
    if (const TextureRow* row = SearchCompiledRows(view.pair, PairKey(dictionary, name))) return row;
  }
  return SearchCompiledRows(view.name, NameKey(name));
}
inline const TextureRow* FindCompiledRow(std::string_view dictionary, std::string_view name, uint64_t content_hash) {
  return FindCompiledRow(kCompiledView, dictionary, name, content_hash);
}

// Which layers a lookup reads (the setting mcla_gpu_gbuffer_texture_rows, read by gbuffer_materials.cpp): bit 0 the compiled-in table, bit 1 the files.
inline constexpr uint32_t kRowLayerCompiled = 1;
inline constexpr uint32_t kRowLayerFiles = 2;
inline constexpr uint32_t kRowLayersAll = kRowLayerCompiled | kRowLayerFiles;
// The setting's value -> the layers: 0 none (the shader families' own words: the A/B and the census's clean run), 1 both, 2 the compiled-in table only, 3 the files only. Anything else is 1.
constexpr uint32_t RowLayersOfSetting(int value) {
  switch (value) {
    case 0: return 0;
    case 2: return kRowLayerCompiled;
    case 3: return kRowLayerFiles;
    default: return kRowLayersAll;
  }
}

// The row of a texture through the layers: the files' (the player's and the generated table, merged by TextureTable: the later file wins) beat the compiled-in table, and a texture found in
// a higher layer takes that layer's row whole.
inline const TextureRow* FindTextureRow(const CompiledView& compiled, const TextureTable& files, uint32_t layers, std::string_view dictionary, std::string_view name, uint64_t content_hash) {
  if ((layers & kRowLayerFiles) != 0) {
    if (const TextureRow* row = files.Find(dictionary, name, content_hash)) return row;
  }
  if ((layers & kRowLayerCompiled) != 0) return FindCompiledRow(compiled, dictionary, name, content_hash);
  return nullptr;
}
inline const TextureRow* FindTextureRow(const TextureTable& files, uint32_t layers, std::string_view dictionary, std::string_view name, uint64_t content_hash) {
  return FindTextureRow(kCompiledView, files, layers, dictionary, name, content_hash);
}

// A fingerprint of the compiled-in table (FNV-1a 64 over every row's key and fields, the three arrays in order): the city hit data's stamp has it so that a changed table makes the hit data
// build again (the hit data holds words that the rows have changed).
constexpr uint64_t CompiledRowsFingerprint(const CompiledView& view) {
  uint64_t h = kFnvOffset;
  const auto mix = [&h](uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) h = (h ^ ((v >> shift) & 0xFFu)) * kFnvPrime;
  };
  for (const std::span<const CompiledRow> table : {view.hash, view.pair, view.name}) {
    mix(table.size());
    for (const CompiledRow& r : table) {
      mix(r.key);
      mix(uint64_t(r.row.fields) | (uint64_t(r.row.cls) << 8) | (uint64_t(r.row.smooth) << 16) | (uint64_t(r.row.metal) << 24) | (uint64_t(r.row.lit) << 32) | (uint64_t(r.row.ssr ? 1 : 0) << 40));
    }
  }
  return h;
}
inline constexpr uint64_t kCompiledRowsStamp = CompiledRowsFingerprint(kCompiledView);

}  // namespace mcla::gpu::gbuffer
