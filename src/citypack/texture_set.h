#pragma once
// ===========================================================================
// LARecomp city pack -- the city texture set and the texture file (CITY-RT M2 package H2)
// ===========================================================================
// What H2 builds, on the player's PC, from the player's own game files (never shipped, never committed): every texture the city's shaders can name, once each.
//
//   SOURCES   common.xtd (508 textures), lod_0.xtd (1,240) and the sector's own dictionary of each of the 439 .xcs (39,236 entries): 40,984 entries in all, 8,421 distinct names. The retail
//             files measured (xtd_reader.h): every texture is 2D, tiled, 8in16 swapped, DXT1 or DXT4/5.
//   PICTURES  a picture is one set of decoded levels (the blocks, the size, the format, the level count, the gamma bit): 8,342 of them. The same picture is in the dictionaries of several
//             sectors, and in most of them its guest bytes differ (the padding of the tiles is whatever the game's packer left there), so the guest KEY (XXH3 of the base level's storage, the
//             number GB-F2's rows and the texture dumps name a texture by) is per ENTRY: 14,946 distinct keys name 8,342 pictures. A picture is stored once (232 MB at full detail instead of
//             264 MB); every entry keeps its own key (ResolvedTexture::key: H1 puts it in the material record so that a GB-F2 row for a texture applies to the hits on it as to the pixels).
//   TABLE     the pictures in the order of first appearance (common, lod_0, then the sectors in the city table's order, each in its dictionary's order): the HitTexture table that goes in
//             the hit file (H1 writes it; city_hit_format.h), with data_offset and data_size set for the texture file; HitTexture::key is the key of the picture's first entry. The order does
//             not depend on the number of threads.
//   FILE      mcla_city_tex.v1.pack: TexHeader, then each picture's levels, the stored mip chain (the full size first, each level tightly packed, BC1 / BC2 / BC3 blocks) on 16 bytes. FULL
//             detail, whatever the graphics class: the class's cap (city_hit_format.h HitBudget::texture_cap_px) is applied when a texture is made resident, from the first mip it keeps
//             (texture_residency.h), reading only the levels it needs from the file. The owner's answer of 2026-11-21: 200 to 300 MB on disk for the hit data pack is fine.
//   NAMES     a shader names a texture by the hash of its file name (xtd_reader.h TextureNameHash); ResolveEntry(dictionary, hash) says which picture that is, and its key: the dictionary of
//             the sector the shader is in first (the sector's own textures), then common.xtd, then lod_0.xtd (the far level of detail names lod_0 first, then common). H1 calls it for each
//             material. In the retail files all but a few dozen of the names are the same picture in every dictionary that has them; those few are different pictures under one name, each
//             resolving in its own dictionary.
//   AVERAGE   the linear mean colour of every picture (city_texture.h), for the material records' average (what a hit shows until the texture is loaded, and what M3's probes read).
//
// The texture file belongs to a hit file: TexHeader::hit_table_hash is the hit file's table hash (H1 knows it once the hit file's tables are laid out; the texture table's offsets are
// fixed here before that), so a texture file of another build is never read with it.
// ===========================================================================

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "citypack/builder.h"
#include "citypack/city_hit_format.h"
#include "citypack/city_texture.h"
#include "citypack/rsc_image.h"
#include "citypack/xtd_reader.h"

namespace larecomp::citypack {

// What the texture set is built by (the stamp of the hit file holds it): a change of how a texture is read, averaged, deduplicated or ordered bumps it.
inline constexpr uint32_t kTextureRulesVersion = 1;

// The dictionaries a name is resolved in.
inline constexpr uint32_t kDictionaryCommon = 0;
inline constexpr uint32_t kDictionaryLod = 1;
inline constexpr uint32_t kDictionarySectorFirst = 2;
constexpr uint32_t DictionaryOfSector(uint32_t sector_index) { return kDictionarySectorFirst + sector_index; }

// A texture file's data starts after the 64 byte header; every texture starts on 16 bytes.
inline constexpr uint32_t kTextureDataAlignment = 16;

struct TextureSetStats {
  uint32_t sources = 0;                   // dictionaries read: common, lod_0 and a sector's each
  uint32_t sources_failed = 0;            // a file that was not in the archive, did not decode or had no dictionary
  uint32_t sectors = 0;
  DictionaryStats dictionary;             // summed over the sources
  uint64_t entries = 0;                   // dictionary entries read (every source)
  uint32_t distinct = 0;                  // the table's size: the pictures
  uint32_t key_variants = 0;              // distinct guest keys (with their size, format and levels) over the entries: what was decoded
  uint32_t distinct_names = 0;            // different name hashes over every source
  uint32_t decode_failed = 0;             // entries (not pictures) whose texture did not decode: they resolve to nothing
  uint32_t damaged = 0;                   // pictures with levels dropped from the end of their chain
  uint32_t name_conflicts = 0;            // a name with two different pictures in one dictionary (the first is kept)
  uint32_t cross_conflicts = 0;           // names met again in another dictionary as a different picture (each resolves in its own)
  uint64_t duplicate_bytes = 0;           // what the entries that decoded to a picture already there would have taken
  std::array<uint32_t, size_t(HitTextureFormat::kCount)> by_format{};
  uint32_t single_level = 0;              // pictures with only a base level
  uint32_t with_alpha = 0;                // kTextureAlpha (the alpha is coverage: city_texture.h AlphaIsCoverage)
  uint32_t gamma = 0;                     // kTextureGamma
  uint64_t data_bytes = 0;                // the data section: every picture's levels
  double seconds = 0.0;
};

// What a name is: the picture (a table index), and the guest key of the dictionary entry it came from.
struct ResolvedTexture {
  uint32_t index = kNoTexture;           // kNoTexture (0xFFFF): not found
  uint64_t key = 0;                      // the entry's XXH3-64 of the base level's storage (HitMaterial::key)
  uint32_t dictionary = 0xFFFFFFFFu;     // the dictionary that had it
  bool found() const { return index != kNoTexture; }
};

struct TextureSet {
  std::vector<HitTexture> textures;                 // the table, offsets into the texture file included
  std::vector<std::array<float, 3>> average;        // linear mean colour, 0..1
  std::vector<float> alpha_mean;
  std::vector<std::vector<uint8_t>> blobs;          // each picture's levels (textures[i].data_size bytes), in the table's order
  uint64_t data_bytes = 0;                          // the file's data section: the blobs, each on kTextureDataAlignment
  uint32_t sector_count = 0;

  // The picture a name is, as seen from `dictionary` (kDictionaryCommon, kDictionaryLod, DictionaryOfSector(i)), with the fallbacks above; not found where no dictionary has it.
  ResolvedTexture ResolveEntry(uint32_t dictionary, uint32_t name_hash) const;
  uint32_t Resolve(uint32_t dictionary, uint32_t name_hash) const { return ResolveEntry(dictionary, name_hash).index; }
  // ... in that dictionary alone.
  ResolvedTexture ResolveExactEntry(uint32_t dictionary, uint32_t name_hash) const;
  uint32_t ResolveExact(uint32_t dictionary, uint32_t name_hash) const { return ResolveExactEntry(dictionary, name_hash).index; }
  // The pictures a dictionary names, each once, in its order (what a sector can reach without the fallbacks).
  std::vector<uint32_t> PicturesOf(uint32_t dictionary) const;

  uint64_t file_bytes() const { return kTexHeaderBytes + data_bytes; }

  // Internal: dictionary << 32 | name hash -> the entry.
  struct Entry {
    uint32_t index = kNoTexture;
    uint64_t key = 0;
    bool operator==(const Entry&) const = default;
  };
  std::unordered_map<uint64_t, Entry> names;
  std::vector<std::pair<uint32_t, uint32_t>> dictionary_order;  // (dictionary, name hash) of every entry in reading order, for PicturesOf
};

struct TextureSetRequest {
  std::filesystem::path source_rpf;  // xarchive_cache.rpf
  std::filesystem::path mods_rpf;    // xarchive_mods.rpf: a path found there wins (the same overlay as the city pack)
  LzxFunction lzx;                   // required
  uint32_t threads = 0;              // 0: the PC's hardware threads less one, at least one
  std::function<void()> worker_start;
};

struct TextureSetResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  TextureSet set;
  TextureSetStats stats;
};

// Reads the dictionaries and makes the set. A source that is missing or damaged is counted (stats.sources_failed) and skipped, not an error: the materials that name its textures take
// their average colour; only a missing city table or no texture at all fails the build. About the size of the set again in memory at the peak (the entries' data, then the table's).
TextureSetResult BuildTextureSet(const TextureSetRequest& request, ProgressSink* sink);

// Writes the texture file (temporary file, then rename): the header with `hit_table_hash`, then the blobs at their offsets. False (with `error`) when the disk refuses.
bool WriteTextureFile(const std::filesystem::path& path, const TextureSet& set, uint64_t hit_table_hash, std::string* error);

// The reader of the runtime: Open checks the header against the hit file's texture table and its table hash; ReadMips reads the levels [first_mip, mip_count) of a texture, tightly packed
// (the bytes a texture of `mip_count - first_mip` levels is uploaded from). One reader is used by one thread at a time (it keeps the file open).
class TextureFileReader {
 public:
  bool Open(const std::filesystem::path& path, const std::vector<HitTexture>& table, uint64_t expected_hit_table_hash, std::string* error);
  bool is_open() const { return open_; }
  uint64_t file_size() const { return file_size_; }
  bool ReadMips(uint32_t index, uint32_t first_mip, std::vector<uint8_t>* out, std::string* error);

 private:
  std::filesystem::path path_;
  std::vector<HitTexture> table_;
  uint64_t file_size_ = 0;
  bool open_ = false;
  std::unique_ptr<std::ifstream> in_;
};

// Every texture of the file read and checked (sizes against the table, the chain's bytes, the file's end); the tool's validate. The number of problems (the first few in `problems`).
uint32_t ValidateTextureFile(const std::filesystem::path& path, const std::vector<HitTexture>& table, uint64_t expected_hit_table_hash, std::vector<std::string>* problems);

}  // namespace larecomp::citypack
