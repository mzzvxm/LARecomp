#pragma once
// ===========================================================================
// LARecomp city pack -- the hit data's builder (CITY-RT M2, package H1)
// ===========================================================================
// Builds mcla_city_hit.v1.pack (city_hit_format.h) next to the city pack (mcla_city.v1.pack, builder.h) from the player's own game files: the corner attributes (texture
// coordinates of UV set 0, the vertex colour) of the triangles of the pack, in the pack's own order, the runs (triangles of one material with one UV range), the GLOBAL
// MATERIAL TABLE made from the shader instances' parameters through the raster G-buffer's rules (hit_material.h), and the texture table the texture set gives (H2).
//
//   inputs   the base pack in <out_dir> (its header says what was dropped, its records say how many triangles every sector has: the hit pass checks its own meshes against them,
//            triangle for triangle), <game>/xarchive_cache.rpf and the mod overlay (the same files the base pack was made from)
//   output   <out_dir>/mcla_city_hit.v1.pack and mcla_city_hit.stamp; the texture set writes <out_dir>/mcla_city_tex.v1.pack when it has one
//
// The pass parses every sector again (the shader instances' parameters and the vertex attributes are not in the pack): one task a sector on `threads` workers, the file written in
// the table's order whatever finishes first, so the pack is byte for byte the same with one thread or sixteen. The blobs go to a temporary body file as they come and the file is
// assembled at the end (the head's size is known only when every sector has said which materials it uses). A cancelled or failed build leaves nothing that reads as a pack; the stamp
// is written last.
//
// The stamp (mcla_city_hit.stamp) keeps what the pack was made from: the base pack's hash and size, the game archive's stat and the mods' city files (as the base stamp),
// the rule versions (this builder, the file format, the G-buffer's material rules), the texture set's identity and the GB-F2 table files' hash. The check reads a few
// small files.
// ===========================================================================

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/city_hit_format.h"
#include "citypack/hit_collect.h"
#include "citypack/hit_corner_codec.h"
#include "citypack/hit_material.h"
#include "citypack/pack_reader.h"

namespace larecomp::citypack {

inline constexpr uint32_t kHitStampFormat = 1;
// The hit builder's own revision (the rules of the runs, the materials and the corner coding below city_hit_format.h's frozen builder version): bump it by hand when what the builder
// writes for the same inputs changes. It is in the stamp.
inline constexpr uint32_t kHitBuilderRevision = 1;

struct HitBuildRequest {
  // What the base pack was built with and from: source_rpf, mods_rpf, out_dir (the base pack's folder: the hit pack goes there too), lzx, threads, worker_start, force,
  // census_only (everything is read, parsed and encoded, nothing is written: the golden counts), app_version. drop_mask and positions are ignored: the base pack's header decides.
  BuildRequest base;
  // The textures of the materials, already built (package H2's texture set through hit_texture_adapter.h); null: none (no texture file, every texture name is counted unresolved).
  HitTextureSource* textures = nullptr;
  // GB-F2 per-texture table files (gbuffer_materials.h), read in order; their rows are applied to the materials by the textures' keys. Empty: the F1 words stand.
  std::vector<std::filesystem::path> texture_rows;
  // How much the corners may lose to stay inside the disk budget (hit_collect.h); in the stamp.
  HitRules rules;
};

struct HitCensus {
  uint32_t sectors = 0;
  uint32_t hi_blobs = 0;
  uint32_t lod_blobs = 0;
  uint32_t sectors_failed = 0;
  uint64_t hi_triangles = 0;
  uint64_t lod_triangles = 0;
  uint64_t hi_runs = 0;
  uint64_t lod_runs = 0;
  uint64_t corners = 0;               // all blobs
  uint64_t hi_blob_bytes = 0;
  uint64_t lod_blob_bytes = 0;
  uint64_t raw_equivalent_bytes = 0;  // what the blobs would be with the raw corner encoding (8 bytes a corner + the runs): the indexed stream's gain
  uint64_t corner_stream_bytes = 0;   // the indexed corner streams' bytes (the rest of the blobs is the runs and the containers)
  uint32_t blobs_without_corners = 0;
  uint32_t materials = 0;             // the table's size, the default included
  uint32_t textures = 0;              // the texture table's size
  std::array<uint32_t, 16> materials_by_class{};  // the G-buffer's classes
  uint32_t materials_group_unknown = 0;
  uint32_t materials_without_params = 0;
  uint32_t materials_without_exponent = 0;
  uint32_t materials_gloss_pixel = 0;
  uint32_t materials_gloss_vertex = 0;
  uint32_t materials_flat_colour = 0;
  uint32_t materials_emissive = 0;
  uint32_t shader_instances = 0;      // the sectors' and the lod's shader tables, summed
  FinalizeStats finalize;
  CollectStats collect;
  uint64_t head_bytes = 0;            // header, tables, padding
  uint64_t file_bytes = 0;
  double read_seconds = 0.0;          // summed over the workers
  double parse_seconds = 0.0;
  double encode_seconds = 0.0;        // the writer's
  std::vector<std::string> problems;  // the first few

  // The text the tool prints: totals, the sizes, the materials, the golden comparison.
  std::string Report() const;
};

struct HitBuildResult {
  bool ok = false;
  bool up_to_date = false;
  bool cancelled = false;
  std::string error;
  HitCensus census;
  uint64_t hit_bytes = 0;
  uint64_t table_hash = 0;  // HitHeader::table_hash
  double seconds = 0.0;
  std::filesystem::path hit_path;
};

HitBuildResult BuildCityHitPack(const HitBuildRequest& request, ProgressSink* sink);

// Is the hit pack in request.base.out_dir current for these inputs? A few file stats, the base pack's table and the stamp (and the GB-F2 files' bytes).
StampCheck CheckCityHitPack(const HitBuildRequest& request);

// ---------------------------------------------------------------------------
// the reader (the tool's, the validator's; H3's pool filler reads blobs through the same)
// ---------------------------------------------------------------------------

class HitPackReader {
 public:
  // False (with `error`) for a file that is missing, not a hit pack, another format version, cut short, or whose tables do not match their hash.
  bool Open(const std::filesystem::path& file, std::string* error);
  bool is_open() const { return open_; }
  const std::filesystem::path& path() const { return path_; }
  const HitHeader& header() const { return header_; }
  uint32_t sector_count() const { return uint32_t(sectors_.size()); }
  const HitSectorRecord& sector(uint32_t i) const { return sectors_[i]; }
  const std::vector<HitSectorRecord>& sectors() const { return sectors_; }
  const std::vector<HitMaterial>& materials() const { return materials_; }
  const std::vector<HitTexture>& textures() const { return textures_; }
  // The raw bytes of a blob (one read), and the decoded blob. A sector without that blob: false with an empty `error`.
  bool ReadBlob(uint32_t sector, MeshKind kind, std::vector<uint8_t>* out, std::string* error) const;
  bool ReadHitBlob(uint32_t sector, MeshKind kind, HitBlob* out, std::string* error) const;

 private:
  bool open_ = false;
  std::filesystem::path path_;
  HitHeader header_;
  std::vector<HitSectorRecord> sectors_;
  std::vector<HitMaterial> materials_;
  std::vector<HitTexture> textures_;
};

// Every blob decoded and compared with the base pack's records (names, triangle counts, opaque counts, runs that partition the triangles, material numbers inside the table, the
// corner counts): the tool's `check-hit --deep`. Returns the number of problems (the first few are in `problems`); 0 = the hit pack fits the base pack.
uint32_t ValidateHitPack(const HitPackReader& hit, const PackReader& base, std::vector<std::string>* problems);

}  // namespace larecomp::citypack
