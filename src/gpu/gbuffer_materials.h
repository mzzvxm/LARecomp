#pragma once
// ===========================================================================
// MCLA GPU renderer -- the G-buffer's material channel, part 2: the per-texture table (package GB-F2 of perf/LIGHT-UNIFY.md 11.2; perf/GBUFFER-SURVEY.md 5)
// ===========================================================================
// gbuffer_material.h gives a draw its material word from its SHADERS (the class of the shader group, the game's SpecularExponent). That is the family's word. This file lets a TEXTURE
// refine it: a table of rows keyed by the texture, each changing some of the word's fields (class, smoothness, metalness, the reflection flag, the lit weight), read from text files
// that are edited by hand or written by tools/gbuffer_materials/author_materials.py and reloaded while the game runs. A texture with no row keeps the family's word: the table is an
// override, never a requirement (an empty or missing table changes nothing).
//
// WHICH KEY. The game gives a texture two identities (texture_names.h; textures.h ContentHashOf):
//   dictionary/name   what the game's resource constructor was given. MOST CITY TEXTURES HAVE NO NAME: in a dump of the retail game the dictionaries `common` and `lod_0` and 88 percent of
//                     `Unknown_file_in_an_archive` are all "unnamed" (the console builds strip the names), so a name keys the cars, the pedestrians, the sky and effect textures, the
//                     runtime ones and the named props (and mods' textures), not the city's walls and roads.
//   hash:<16 hex>     the XXH3-64 of the base level's guest bytes: the number the texture dumps (`unnamed__<hash>.dds`) and the texture mods name their files by. The key of every unnamed texture.
//   */name            any dictionary: the same texture name found in more than one.
// A row's key is checked in the order hash, dictionary/name, */name: the most specific wins. A name of "unnamed" is refused as a key (it would hit thousands of textures).
//
// THE TEXT FORMAT (version 1; the census and the authoring tool use the same):
//   # a comment, also after a row
//   @version 1                                  (optional; a file of another version is refused whole)
//   <key> field=value field=value ...           one row per line, fields in any order, each at most once
//   fields   class=<none|world|road|wetroad|sidewalk|terrain|lod|foliage|paint|chrome|metal|glass|skin|emissive|interior|water | 0..15>
//            rough=<0..1>   perceptual roughness (1 = chalk, 0 = mirror; smoothness = 1 - rough; the word holds the smoothness, gbuffer_rules.h part 3)      smooth=<0..1> the same, as smoothness
//            metal=<0..1>   the word's metalness byte (bits 24..31, gbuffer_rules.h kMaterialMetalShift): no plane carries it yet, so give a metal surface class=metal or class=chrome as well
//            ssr=<0|1>      screen-space reflections eligible (the flag)
//            lit=<0..1>     the lit weight (RT2.a): 0 = lights itself, the deferred lights add nothing
// A field a row does not give keeps the draw's own value, with ONE exception: a row with class= and no rough= (or smooth=) takes the new class's default smoothness, and with no lit= its default
// lit weight (a texture that says "this is glass" gets glass, not the shader family's number). A later row for the same key replaces the earlier (the files are read in order, the last wins).
// A draw whose family says class 0 ("no data": the groups the G-buffer never writes) is never changed by a row.
//
// THE FILES (TextureMaterials::Resolve): <shader pack folder>/mcla_gbuffer_texture_materials.txt (the generated table), <mods folder>/gbuffer_texture_materials.txt (the owner's overrides),
// and the paths in the environment variable MCLA_GBUFFER_TEXTURE_MATERIALS (separated by ';'; the value "off" reads no file), read in that order. Each is looked at again every 30 frames; a changed file is read again
// once it has stayed the same for two looks (an editor that is still writing it is not read), so editing and saving is the whole loop. A bad row is skipped and logged, the rest is used.
//
// THE CENSUS. With MCLA_GBUFFER_CENSUS=1 the renderer logs, every 300 frames, one line per (texture, pixel shader) the G-buffer's draws used: how many draws, how many indices (the draw's size,
// the proxy for the surface it covers), the word it got and whether a row changed it, with the texture's name, dictionary, content hash and size (TextureCensus below; the lines are read by
// tools/gbuffer_materials/census_merge.py). It is what the rules of the authoring tool and the review are made from.
//
// Pure: no Plume, no renderer, no ReXGlue (the glue to the texture names, the texture cache and the log is gbuffer_materials.cpp), so that the tests build alone. Render thread for
// TextureMaterials.
// WHICH TEXTURE OF A DRAW. The one its pixel shader takes its colour from: the DiffuseSampler where it reads one, else the lowest sampler that is not a reflection map, in the fetch slot
// that gbuffer_material.h's diffuse table gives (almost every lit shader: slot 0; the windows, tiles and metal props put their reflection maps first; the prop glass reads no other).
// CHECK (nothing here was run in the game): the key priority, the poll period, the census window, that a texture's name and hash found this way are the ones the dumps use, and that
// the first sampler is what the eye calls the surface's texture for the shaders with several (the tinted ones read a grime, decal or channel map first) are first cuts; the census and
// the pictures decide.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gbuffer_material.h"

namespace mcla::gpu {
class TextureCache;
struct TextureKey;
}  // namespace mcla::gpu

namespace mcla::gpu::gbuffer {

inline constexpr uint32_t kTextureTableVersion = 1;
inline constexpr const char* kTextureTableFileName = "mcla_gbuffer_texture_materials.txt";    // beside the shader pack: the generated table
inline constexpr const char* kTextureOverridesFileName = "gbuffer_texture_materials.txt";      // in the mods folder: the owner's rows
inline constexpr const char* kTextureTableEnvironment = "MCLA_GBUFFER_TEXTURE_MATERIALS";
inline constexpr const char* kCensusEnvironment = "MCLA_GBUFFER_CENSUS";
inline constexpr const char* kShaderNamesFileName = "mcla_gbuffer_shader_names.v1.tsv";        // what the pack build writes beside the material table, for the census tool

// ---------------------------------------------------------------------------
// The class names of the text format
// ---------------------------------------------------------------------------

inline constexpr std::string_view kSurfaceClassNames[] = {"none",  "world", "road", "wetroad", "sidewalk", "terrain",  "lod",   "foliage",
                                                          "paint", "chrome", "metal", "glass",  "skin",     "emissive", "interior", "water", "garagefloor"};
static_assert(std::size(kSurfaceClassNames) == size_t(SurfaceClass::kCount), "a name for every class");

constexpr std::string_view SurfaceClassName(SurfaceClass cls) {
  return size_t(cls) < std::size(kSurfaceClassNames) ? kSurfaceClassNames[size_t(cls)] : std::string_view("none");
}

constexpr char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

constexpr bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
  }
  return true;
}

// A class by name (any case) or by its number 0..15.
inline bool ParseSurfaceClass(std::string_view text, SurfaceClass* out) {
  for (size_t i = 0; i < std::size(kSurfaceClassNames); ++i) {
    if (EqualsNoCase(text, kSurfaceClassNames[i])) {
      *out = SurfaceClass(i);
      return true;
    }
  }
  if (!text.empty() && text.size() <= 2 && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    const unsigned number = unsigned(std::atoi(std::string(text).c_str()));
    if (number < unsigned(SurfaceClass::kCount)) {
      *out = SurfaceClass(number);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// The keys: FNV-1a 64 over the lower case text
// ---------------------------------------------------------------------------

inline constexpr uint64_t kFnvOffset = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr uint64_t Fnv1aLower(std::string_view text, uint64_t hash = kFnvOffset) {
  for (const char c : text) {
    hash = (hash ^ uint64_t(uint8_t(LowerAscii(c)))) * kFnvPrime;
  }
  return hash;
}
// The key of "dictionary/name" (any case), and of a name in any dictionary.
constexpr uint64_t PairKey(std::string_view dictionary, std::string_view name) { return Fnv1aLower(name, Fnv1aLower("/", Fnv1aLower(dictionary))); }
constexpr uint64_t NameKey(std::string_view name) { return Fnv1aLower(name); }

// What the game calls a texture it gave no name (texture_names.cpp FileSafe's fallback): no key.
constexpr bool IsUnnamed(std::string_view name) { return name.empty() || EqualsNoCase(name, "unnamed"); }

// ---------------------------------------------------------------------------
// A row: what a texture changes in a draw's material word
// ---------------------------------------------------------------------------

inline constexpr uint8_t kRowClass = 1;
inline constexpr uint8_t kRowSmooth = 2;
inline constexpr uint8_t kRowMetal = 4;
inline constexpr uint8_t kRowSsr = 8;
inline constexpr uint8_t kRowLit = 16;

struct TextureRow {
  uint8_t fields = 0;  // kRow*: which of the values below the row gives
  uint8_t cls = 0;     // SurfaceClass
  uint8_t smooth = 0;  // unorm8 smoothness
  uint8_t metal = 0;   // unorm8 metalness
  uint8_t lit = 255;   // unorm8 lit weight
  bool ssr = false;
  bool operator==(const TextureRow& o) const { return fields == o.fields && cls == o.cls && smooth == o.smooth && metal == o.metal && lit == o.lit && ssr == o.ssr; }
};

// The word of a draw with the row's fields set (the rules at the top). A draw of class 0 is left alone.
inline uint32_t ApplyTextureRow(uint32_t word, const TextureRow& row) {
  if (row.fields == 0 || MaterialClassOf(word) == SurfaceClass::kNone) return word;
  uint32_t cls = uint32_t(MaterialClassOf(word));
  uint32_t flags = MaterialFlagsOf(word);
  uint32_t smooth = MaterialRt3Smoothness(word);
  uint32_t lit = MaterialLitByte(word);
  uint32_t metal = MaterialMetalByte(word);
  if ((row.fields & kRowClass) != 0) {
    cls = row.cls & kClassMask;
    if ((row.fields & kRowSmooth) == 0) smooth = ToByte(DefaultSmoothness(SurfaceClass(cls)));
    if ((row.fields & kRowLit) == 0) lit = ToByte(DefaultLit(SurfaceClass(cls)));
  }
  if ((row.fields & kRowSmooth) != 0) smooth = row.smooth;
  if ((row.fields & kRowLit) != 0) lit = row.lit;
  if ((row.fields & kRowMetal) != 0) metal = row.metal;
  if ((row.fields & kRowSsr) != 0) flags = row.ssr ? (flags | kMaterialFlagSsr) : (flags & ~kMaterialFlagSsr);
  return (smooth & 255u) | (cls << kMaterialClassShift) | ((flags & 7u) << kMaterialFlagsShift) | ((lit & 255u) << kMaterialLitShift) | ((metal & 255u) << kMaterialMetalShift);
}

// ---------------------------------------------------------------------------
// The table: rows by key
// ---------------------------------------------------------------------------

enum class KeyKind : uint8_t { kPair, kAnyDictionary, kContentHash };

struct TextureKeyRef {
  KeyKind kind = KeyKind::kPair;
  std::string dictionary;  // kPair
  std::string name;        // kPair, kAnyDictionary
  uint64_t hash = 0;       // kContentHash
};

// A key's text: `hash:<1..16 hex digits>` (an optional 0x), `dictionary/name`, `*/name`. False with *why.
inline bool ParseTextureKey(std::string_view text, TextureKeyRef* out, std::string* why) {
  *out = {};
  if (text.size() > 5 && EqualsNoCase(text.substr(0, 5), "hash:")) {
    std::string_view digits = text.substr(5);
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.remove_prefix(2);
    if (digits.empty() || digits.size() > 16) {
      *why = "a hash is 1 to 16 hexadecimal digits";
      return false;
    }
    uint64_t value = 0;
    for (const char c : digits) {
      const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0) {
        *why = "a hash is 1 to 16 hexadecimal digits";
        return false;
      }
      value = (value << 4) | uint64_t(d);
    }
    if (value == 0) {
      *why = "the hash 0 is no texture";
      return false;
    }
    out->kind = KeyKind::kContentHash;
    out->hash = value;
    return true;
  }
  const size_t slash = text.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= text.size() || text.find('/', slash + 1) != std::string_view::npos) {
    *why = "a key is dictionary/name, */name or hash:<hex>";
    return false;
  }
  const std::string_view dictionary = text.substr(0, slash);
  const std::string_view name = text.substr(slash + 1);
  if (IsUnnamed(name)) {
    *why = "an unnamed texture has no name to key by: use hash:<hex> (the census and the dumps give it)";
    return false;
  }
  out->name = std::string(name);
  if (dictionary == "*") {
    out->kind = KeyKind::kAnyDictionary;
  } else {
    out->kind = KeyKind::kPair;
    out->dictionary = std::string(dictionary);
  }
  return true;
}

class TextureTable {
 public:
  void Clear() {
    rows_.clear();
    by_hash_.clear();
    by_pair_.clear();
    by_name_.clear();
  }
  bool empty() const { return rows_.empty(); }
  size_t size() const { return rows_.size(); }
  bool has_hash_rows() const { return !by_hash_.empty(); }

  // Adds a row, or replaces the row of the same key. True when it replaced one.
  bool Set(const TextureKeyRef& key, const TextureRow& row) {
    std::unordered_map<uint64_t, uint32_t>& map = key.kind == KeyKind::kContentHash ? by_hash_ : key.kind == KeyKind::kPair ? by_pair_ : by_name_;
    const uint64_t k = key.kind == KeyKind::kContentHash ? key.hash : key.kind == KeyKind::kPair ? PairKey(key.dictionary, key.name) : NameKey(key.name);
    const auto it = map.find(k);
    if (it != map.end()) {
      rows_[it->second] = row;
      return true;
    }
    map.emplace(k, uint32_t(rows_.size()));
    rows_.push_back(row);
    return false;
  }

  // The row of a texture: by its content hash (0: not known), else by dictionary and name, else by name alone; null when none. An unnamed texture is looked up by hash only.
  // The pointer is good until the table changes.
  const TextureRow* Find(std::string_view dictionary, std::string_view name, uint64_t content_hash) const {
    if (content_hash != 0 && !by_hash_.empty()) {
      const auto it = by_hash_.find(content_hash);
      if (it != by_hash_.end()) return &rows_[it->second];
    }
    if (!IsUnnamed(name)) {
      if (!by_pair_.empty() && !dictionary.empty()) {
        const auto it = by_pair_.find(PairKey(dictionary, name));
        if (it != by_pair_.end()) return &rows_[it->second];
      }
      if (!by_name_.empty()) {
        const auto it = by_name_.find(NameKey(name));
        if (it != by_name_.end()) return &rows_[it->second];
      }
    }
    return nullptr;
  }

 private:
  std::vector<TextureRow> rows_;
  std::unordered_map<uint64_t, uint32_t> by_hash_;
  std::unordered_map<uint64_t, uint32_t> by_pair_;
  std::unordered_map<uint64_t, uint32_t> by_name_;
};

struct TableDiagnostic {
  uint32_t line = 0;
  std::string text;
};
struct TableParse {
  size_t rows = 0;      // rows read (each key's later row counts too)
  size_t replaced = 0;  // ... of which replaced an earlier row of the same key
  std::vector<TableDiagnostic> errors;
};

namespace table_detail {
// A number 0..1 as text ("0.35"); false for anything else (no trailing text, no nan or inf).
inline bool ParseUnit(std::string_view text, float* out) {
  if (text.empty() || text.size() > 24) return false;
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end == copy.c_str() || *end != '\0' || !std::isfinite(value) || value < 0.0 || value > 1.0) return false;
  *out = float(value);
  return true;
}
inline bool ParseBool(std::string_view text, bool* out) {
  if (text == "1" || EqualsNoCase(text, "on") || EqualsNoCase(text, "true") || EqualsNoCase(text, "yes")) {
    *out = true;
    return true;
  }
  if (text == "0" || EqualsNoCase(text, "off") || EqualsNoCase(text, "false") || EqualsNoCase(text, "no")) {
    *out = false;
    return true;
  }
  return false;
}
// The tokens of a line, up to a token that starts with '#'.
inline std::vector<std::string_view> Tokens(std::string_view line) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    if (i >= line.size() || line[i] == '#') break;
    const size_t begin = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
    tokens.push_back(line.substr(begin, i - begin));
  }
  return tokens;
}
// The row of the field tokens; false with *why.
inline bool ParseRowFields(std::span<const std::string_view> tokens, TextureRow* row, std::string* why) {
  *row = {};
  if (tokens.empty()) {
    *why = "a row with no field changes nothing (class= rough= smooth= metal= ssr= lit=)";
    return false;
  }
  for (const std::string_view token : tokens) {
    const size_t eq = token.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 >= token.size()) {
      *why = "'" + std::string(token) + "' is not field=value";
      return false;
    }
    const std::string_view field = token.substr(0, eq);
    const std::string_view value = token.substr(eq + 1);
    const auto once = [&](uint8_t bit) {
      if ((row->fields & bit) != 0) {
        *why = "the field '" + std::string(field) + "' is given twice";
        return false;
      }
      row->fields |= bit;
      return true;
    };
    float unit = 0.0f;
    if (EqualsNoCase(field, "class")) {
      SurfaceClass cls = SurfaceClass::kNone;
      if (!ParseSurfaceClass(value, &cls)) {
        *why = "'" + std::string(value) + "' is not a class (none world road wetroad sidewalk terrain lod foliage paint chrome metal glass skin emissive interior water, or 0 to 15)";
        return false;
      }
      if (!once(kRowClass)) return false;
      row->cls = uint8_t(cls);
    } else if (EqualsNoCase(field, "rough") || EqualsNoCase(field, "smooth")) {
      if (!ParseUnit(value, &unit)) {
        *why = "'" + std::string(value) + "' is not a number from 0 to 1";
        return false;
      }
      if (!once(kRowSmooth)) return false;
      row->smooth = ToByte(EqualsNoCase(field, "rough") ? 1.0f - unit : unit);
    } else if (EqualsNoCase(field, "metal")) {
      if (!ParseUnit(value, &unit)) {
        *why = "'" + std::string(value) + "' is not a number from 0 to 1";
        return false;
      }
      if (!once(kRowMetal)) return false;
      row->metal = ToByte(unit);
    } else if (EqualsNoCase(field, "lit")) {
      if (!ParseUnit(value, &unit)) {
        *why = "'" + std::string(value) + "' is not a number from 0 to 1";
        return false;
      }
      if (!once(kRowLit)) return false;
      row->lit = ToByte(unit);
    } else if (EqualsNoCase(field, "ssr")) {
      bool on = false;
      if (!ParseBool(value, &on)) {
        *why = "'" + std::string(value) + "' is not 0 or 1";
        return false;
      }
      if (!once(kRowSsr)) return false;
      row->ssr = on;
    } else {
      *why = "'" + std::string(field) + "' is not a field (class rough smooth metal ssr lit)";
      return false;
    }
  }
  return true;
}
}  // namespace table_detail

// Reads the text of one table file into `table` (rows of an earlier file are kept, a later row of the same key replaces them). Bad lines are in report->errors and skipped; a file of
// another version gives no rows at all.
inline void ParseTextureTable(std::string_view text, TextureTable* table, TableParse* report) {
  uint32_t line_number = 0;
  size_t at = 0;
  std::vector<std::pair<TextureKeyRef, TextureRow>> rows;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view line = text.substr(at, end - at);
    at = end + 1;
    ++line_number;
    const std::vector<std::string_view> tokens = table_detail::Tokens(line);
    if (tokens.empty()) continue;
    const auto fail = [&](const std::string& why) { report->errors.push_back({line_number, why}); };
    if (tokens[0][0] == '@') {
      if (EqualsNoCase(tokens[0], "@version") && tokens.size() == 2) {
        if (tokens[1] != std::to_string(kTextureTableVersion)) {
          fail("a table of version " + std::string(tokens[1]) + " (this build reads " + std::to_string(kTextureTableVersion) + "): the file is not used");
          return;
        }
      } else {
        fail("'" + std::string(tokens[0]) + "' is not a directive (@version 1)");
      }
      continue;
    }
    TextureKeyRef key;
    std::string why;
    if (!ParseTextureKey(tokens[0], &key, &why)) {
      fail(why);
      continue;
    }
    TextureRow row;
    if (!table_detail::ParseRowFields(std::span<const std::string_view>(tokens).subspan(1), &row, &why)) {
      fail(why);
      continue;
    }
    rows.emplace_back(std::move(key), row);
  }
  for (const auto& [key, row] : rows) {
    ++report->rows;
    if (table->Set(key, row)) ++report->replaced;
  }
}

// ---------------------------------------------------------------------------
// The files, read again while the game runs
// ---------------------------------------------------------------------------

// The table files, lowest priority first: the generated table beside the shader pack, the owner's rows in the mods folder, then the paths of the environment variable
// MCLA_GBUFFER_TEXTURE_MATERIALS (separated by ';'). The value "off" (any case) reads no file at all (a census must see the families' own words, not a table's).
inline std::vector<std::filesystem::path> TextureTableSources(const std::filesystem::path& pack_folder, const std::filesystem::path& mods_folder, std::string_view environment) {
  std::vector<std::filesystem::path> sources;
  if (EqualsNoCase(environment, "off")) return sources;
  if (!pack_folder.empty()) sources.push_back(pack_folder / kTextureTableFileName);
  if (!mods_folder.empty()) sources.push_back(mods_folder / kTextureOverridesFileName);
  size_t at = 0;
  while (at < environment.size()) {
    size_t end = environment.find(';', at);
    if (end == std::string_view::npos) end = environment.size();
    if (end > at) sources.emplace_back(std::string(environment.substr(at, end - at)));
    at = end + 1;
  }
  return sources;
}

class TextureTableFiles {
 public:
  static constexpr uint32_t kPollFrames = 30;

  struct Report {
    size_t files = 0;   // the files that exist
    size_t rows = 0;
    size_t errors = 0;
    std::vector<std::string> lines;  // what to log
  };

  // The files, lowest priority first (a later file's row replaces an earlier one's). Forgets what was read: the next Poll reads them.
  void SetSources(std::vector<std::filesystem::path> files) {
    sources_ = std::move(files);
    loaded_.clear();
    pending_.clear();
    pending_set_ = false;
    first_ = true;
    next_poll_ = 0;
  }
  const std::vector<std::filesystem::path>& sources() const { return sources_; }

  // Once per frame (the first call, and then every kPollFrames frames, looks at the files). True when the table was made again (the first call always does, even from no file):
  // everything that kept a row (a pointer, a cache) is out of date. `report` is filled then.
  bool Poll(uint64_t frame, Report* report) {
    if (!first_ && frame < next_poll_) return false;
    next_poll_ = frame + kPollFrames;
    std::vector<Stamp> stamps;
    stamps.reserve(sources_.size());
    for (const std::filesystem::path& source : sources_) stamps.push_back(StampOf(source));
    if (first_) {
      first_ = false;
      Load(stamps, report);
      return true;
    }
    if (stamps == loaded_) {
      pending_set_ = false;
      return false;
    }
    if (!pending_set_ || stamps != pending_) {
      // Changed since the last read, but maybe not finished being written: it has to stay as it is for one more look.
      pending_ = stamps;
      pending_set_ = true;
      return false;
    }
    pending_set_ = false;
    Load(stamps, report);
    return true;
  }

  const TextureTable& table() const { return table_; }
  // Bumped whenever the table is made again.
  uint32_t epoch() const { return epoch_; }

 private:
  struct Stamp {
    bool exists = false;
    uint64_t size = 0;
    int64_t time = 0;
    bool operator==(const Stamp& o) const { return exists == o.exists && size == o.size && time == o.time; }
  };
  static Stamp StampOf(const std::filesystem::path& file) {
    Stamp stamp;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return stamp;
    const uint64_t size = std::filesystem::file_size(file, ec);
    if (ec) return stamp;
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec) return stamp;
    stamp.exists = true;
    stamp.size = size;
    stamp.time = int64_t(time.time_since_epoch().count());
    return stamp;
  }

  void Load(const std::vector<Stamp>& stamps, Report* report) {
    TextureTable table;
    size_t files = 0;
    size_t errors = 0;
    size_t shown = 0;
    std::vector<std::string> lines;
    TableParse total;
    for (size_t i = 0; i < sources_.size(); ++i) {
      if (!stamps[i].exists) continue;
      std::ifstream in(sources_[i], std::ios::binary);
      if (!in) continue;
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      ++files;
      TableParse parse;
      ParseTextureTable(text, &table, &parse);
      total.rows += parse.rows;
      errors += parse.errors.size();
      for (const TableDiagnostic& error : parse.errors) {
        if (shown < 8) {
          lines.push_back(sources_[i].string() + ":" + std::to_string(error.line) + ": " + error.text);
          ++shown;
        }
      }
    }
    if (errors > shown) lines.push_back("... and " + std::to_string(errors - shown) + " more bad rows");
    table_ = std::move(table);
    loaded_ = stamps;
    ++epoch_;
    if (report != nullptr) {
      report->files = files;
      report->rows = table_.size();
      report->errors = errors;
      report->lines = std::move(lines);
    }
  }

  std::vector<std::filesystem::path> sources_;
  std::vector<Stamp> loaded_;
  std::vector<Stamp> pending_;
  bool pending_set_ = false;
  bool first_ = true;
  uint64_t next_poll_ = 0;
  TextureTable table_;
  uint32_t epoch_ = 0;
};

// ---------------------------------------------------------------------------
// The census
// ---------------------------------------------------------------------------

struct CensusTexture {
  uint32_t page = 0;
  std::string dictionary;
  std::string name;
  uint64_t hash = 0;  // 0 until the texture cache has it
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;  // xenos::TextureFormat
};

namespace census_detail {
// A field of a census line: no separator, no blank, no control character.
inline std::string Clean(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) out.push_back((c == '|' || c == ' ' || c == '\t' || uint8_t(c) < 32) ? '_' : c);
  return out.empty() ? std::string("_") : out;
}
inline std::string Hex16(uint64_t v) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(v));
  return buffer;
}
}  // namespace census_detail

class TextureCensus {
 public:
  static constexpr uint32_t kWindowFrames = 300;
  static constexpr size_t kMaxLines = 8000;
  static constexpr const char* kLinePrefix = "texcensus ";  // the census tool looks for this in the log

  struct Entry {
    uint64_t draws = 0;
    uint64_t indices = 0;
    uint32_t first_word = 0;
    uint64_t words_changed = 0;  // draws whose word was not the first one's
    uint64_t rows = 0;           // draws a table row changed
  };

  // The id of a texture (the same page, dictionary and name give the same id; the page is how the cache tells textures apart, the names how the log does).
  uint32_t Intern(uint32_t page, std::string_view dictionary, std::string_view name) {
    const std::string key = std::to_string(page) + "|" + std::string(dictionary) + "|" + std::string(name);
    const auto it = ids_.find(key);
    if (it != ids_.end()) return it->second;
    CensusTexture texture;
    texture.page = page;
    texture.dictionary = std::string(dictionary);
    texture.name = std::string(name);
    textures_.push_back(std::move(texture));
    ids_.emplace(key, uint32_t(textures_.size() - 1));
    return uint32_t(textures_.size() - 1);
  }
  // What the cache knows of the texture by now (the hash is known from the texture's second draw on).
  void Describe(uint32_t id, uint64_t hash, uint32_t width, uint32_t height, uint32_t format) {
    if (id >= textures_.size()) return;
    CensusTexture& t = textures_[id];
    if (hash != 0) t.hash = hash;
    t.width = width;
    t.height = height;
    t.format = format;
  }
  const CensusTexture* Texture(uint32_t id) const { return id < textures_.size() ? &textures_[id] : nullptr; }

  // One draw of the window. `word` is the material word the draw got, after the table.
  void Note(uint32_t id, uint64_t ps_key, uint32_t word, uint32_t indices, bool row_applied) {
    if (id >= textures_.size()) return;
    Entry& e = entries_[{id, ps_key}];
    if (e.draws == 0) {
      e.first_word = word;
    } else if (word != e.first_word) {
      ++e.words_changed;
    }
    ++e.draws;
    e.indices += indices;
    if (row_applied) ++e.rows;
  }
  bool empty() const { return entries_.empty(); }
  size_t size() const { return entries_.size(); }

  // The window ends: its lines (a header, then a row per (texture, pixel shader), most draws first, at most kMaxLines) and the counters start again. `frame` is the frame it ended in.
  std::vector<std::string> TakeLines(uint64_t frame) {
    using census_detail::Clean;
    using census_detail::Hex16;
    struct Row {
      uint32_t id;
      uint64_t ps;
      const Entry* entry;
    };
    std::vector<Row> rows;
    rows.reserve(entries_.size());
    uint64_t draws = 0;
    uint64_t indices = 0;
    for (const auto& [key, entry] : entries_) {
      rows.push_back({key.first, key.second, &entry});
      draws += entry.draws;
      indices += entry.indices;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      if (a.entry->draws != b.entry->draws) return a.entry->draws > b.entry->draws;
      return a.id != b.id ? a.id < b.id : a.ps < b.ps;
    });
    const uint64_t frames = frame >= window_start_ ? frame - window_start_ : 0;
    const size_t shown = std::min(rows.size(), kMaxLines);
    std::vector<std::string> lines;
    lines.reserve(shown + 1);
    lines.push_back(std::string(kLinePrefix) + "tc1-window|" + std::to_string(frames) + "|" + std::to_string(textures_.size()) + "|" + std::to_string(rows.size()) + "|" +
                    std::to_string(draws) + "|" + std::to_string(indices) + "|" + std::to_string(rows.size() - shown));
    for (size_t i = 0; i < shown; ++i) {
      const CensusTexture& t = textures_[rows[i].id];
      const Entry& e = *rows[i].entry;
      char word[16];
      std::snprintf(word, sizeof(word), "%08X", e.first_word);
      lines.push_back(std::string(kLinePrefix) + "tc1|" + std::to_string(frames) + "|" + Clean(t.dictionary) + "|" + Clean(t.name) + "|" + Hex16(t.hash) + "|" + Hex16(rows[i].ps) + "|" +
                      std::to_string(t.width) + "|" + std::to_string(t.height) + "|" + std::to_string(t.format) + "|" + std::to_string(e.draws) + "|" + std::to_string(e.indices) + "|" +
                      word + "|" + std::to_string(e.words_changed) + "|" + std::to_string(e.rows));
    }
    entries_.clear();
    window_start_ = frame;
    return lines;
  }
  uint64_t window_start() const { return window_start_; }
  // The first frame of the window the next draws belong to (the runtime sets it at the first draw of a run).
  void StartWindow(uint64_t frame) { window_start_ = frame; }
  bool WindowDue(uint64_t frame) const { return !entries_.empty() && frame >= window_start_ + kWindowFrames; }

 private:
  std::vector<CensusTexture> textures_;
  std::unordered_map<std::string, uint32_t> ids_;
  std::map<std::pair<uint32_t, uint64_t>, Entry> entries_;
  uint64_t window_start_ = 0;
};

// ---------------------------------------------------------------------------
// The runtime: the files, a cache of what each texture resolved to, the census (gbuffer_materials.cpp)
// ---------------------------------------------------------------------------

class TextureMaterials {
 public:
  static constexpr uint32_t kNoCensus = 0xFFFFFFFFu;

  // What Scene::Prepare found for a draw's diffuse texture and keeps until the draw's constants are made.
  struct Hit {
    TextureRow row;
    uint32_t census = kNoCensus;
  };

  // Prepare: the row of the texture `key` the draw reads in its diffuse slot (empty when none or when there is no table and no census). `pack_folder` is the shader pack's folder
  // (the files are looked for once it is known). `frame` is the Scene's frame serial. Reads the files again when they changed.
  Hit Resolve(TextureCache& textures, const std::filesystem::path& pack_folder, const TextureKey& key, uint64_t frame);
  // FillVariantBlock: the draw's material word with the row applied, and the draw noted in the census. `indices`: the draw's index (or vertex) count.
  uint32_t Finish(uint32_t word, const Hit& hit, uint64_t ps_key, uint32_t indices);

  size_t rows() const { return files_.table().size(); }
  bool census_on() const { return census_on_; }

 private:
  void Prepare(const std::filesystem::path& pack_folder, uint64_t frame);
  struct CacheEntry {
    TextureRow row;
    uint32_t census = kNoCensus;
    uint64_t frame = 0;       // the frame it was worked out in
    uint64_t generation = 0;  // the registry of texture names it was worked out against (texture_names.h Generation)
    bool settled = false;     // the hash (when needed) was known: the answer stays until the table changes, and past the names' changes for kRefreshFrames
  };
  TextureTableFiles files_;
  TextureCensus census_;
  std::unordered_map<uint32_t, CacheEntry> cache_;
  uint64_t cache_generation_ = ~0ull;
  uint32_t cache_epoch_ = ~0u;
  uint64_t last_frame_ = ~0ull;
  bool initialized_ = false;
  bool census_on_ = false;
};

// ---------------------------------------------------------------------------
// The shader names file (the census tool's join of a pixel shader's identity to its group)
// ---------------------------------------------------------------------------

// One line per shader, sorted by identity, the first of two with one identity winning: key (16 hex digits, the identity the census logs), stage (p or v), group, entry, the class and
// flags and default smoothness and lit weight of the pixel shaders of known groups ('-' otherwise), and the gloss bank and register where the shader has one. Tab separated, with a header line.
inline std::string FormatShaderNamesTsv(std::span<const NamedShader> shaders) {
  std::vector<const NamedShader*> sorted;
  sorted.reserve(shaders.size());
  for (const NamedShader& shader : shaders) sorted.push_back(&shader);
  std::stable_sort(sorted.begin(), sorted.end(), [](const NamedShader* a, const NamedShader* b) { return a->key < b->key; });
  std::string out = "key\tstage\tgroup\tentry\tclass\tflags\tsmooth\tlit\tgloss_bank\tgloss_reg\n";
  uint64_t previous = 0;
  bool have_previous = false;
  for (const NamedShader* shader : sorted) {
    if (have_previous && shader->key == previous) continue;
    have_previous = true;
    previous = shader->key;
    const ShaderName split = SplitShaderName(shader->name);
    std::string class_text = "-";
    std::string flags_text = "-";
    std::string smooth_text = "-";
    std::string lit_text = "-";
    if (shader->pixel) {
      const ShaderMaterial m = MaterialOfShaderName(shader->name);
      if (m.known) {
        class_text = std::string(SurfaceClassName(m.cls));
        flags_text = std::to_string(m.flags & 7u);
        smooth_text = std::to_string(unsigned(ToByte(m.smoothness)));
        lit_text = std::to_string(unsigned(ToByte(m.lit)));
      }
    }
    std::string bank_text = "-";
    std::string reg_text = "-";
    if (const GlossRow* gloss = FindGlossRow(shader->name); gloss != nullptr && gloss->bank == (shader->pixel ? GlossBank::kPixel : GlossBank::kVertex)) {
      bank_text = gloss->bank == GlossBank::kPixel ? "pixel" : "vertex";
      reg_text = std::to_string(gloss->reg);
    }
    out += census_detail::Hex16(shader->key) + "\t" + (shader->pixel ? "p" : "v") + "\t" + std::string(split.group) + "\t" + std::string(split.entry.empty() ? std::string_view("-") : split.entry) + "\t" +
           class_text + "\t" + flags_text + "\t" + smooth_text + "\t" + lit_text + "\t" + bank_text + "\t" + reg_text + "\n";
  }
  return out;
}

}  // namespace mcla::gpu::gbuffer
