#pragma once
// ===========================================================================
// LARecomp city pack -- the builder (CITY-RT M1, package CP-A)
// ===========================================================================
// Builds mcla_city.v1.pack from the player's own game files, once, on the player's PC (route C of perf/CITY-RT-SURVEY.md): the launcher's setup or the
// first start runs it after the shader packs, like them (src/shaderpack/builder.h): progress and cancel through a sink, stat-only up-to-date check,
// temporary file then rename, the stamp written last, a cancelled or failed build leaves nothing that reads as a pack.
//
//   inputs   <game>/xarchive_cache.rpf   resources/city/sc/city.xct, <sector>.xcs for each sector of the table, lod.xmd
//            <game>/xarchive_mods.rpf    (optional) the mod loader's overlay: a path found there wins, as it does in the game (CHECK C4: the overlay's order)
//   output   <out_dir>/mcla_city.v1.pack and mcla_city.stamp
//
// The work is one task a sector (read, LZX decompress, parse, weld, encode) on `threads` workers; the file is written in the table's order whatever
// finishes first, so the pack is byte for byte the same with one thread or sixteen. The census (Census) is what the tool prints and what the unit and
// golden checks read.
//
// This library links no decompressor and no archive code of its own: the caller gives the LZX function (mc::modloader::LzxDecompress), and the build
// expects src/mc_engine/modloader/{rpf3,aes256}.cpp in the same program (the shader pack's library expects the same two).
// ===========================================================================

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "citypack/mesh_builder.h"
#include "citypack/pack_format.h"
#include "citypack/rsc_image.h"
#include "citypack/sector_parse.h"
#include "citypack/stamp.h"

namespace larecomp::citypack {

enum class LogLevel : uint8_t { kInfo, kWarning, kError };

// What the build reports to. The app forwards it to the boot progress; the tool prints. Every call comes from the thread that called BuildCityPack.
class ProgressSink {
 public:
  virtual ~ProgressSink() = default;
  virtual void BeginPhase(const std::string& name, int total) { (void)name; (void)total; }
  virtual void Step(const std::string& item) { (void)item; }
  virtual void Log(LogLevel level, const std::string& text) { (void)level; (void)text; }
  // Polled between sectors; true stops the build (a cancelled build leaves nothing behind).
  virtual bool Cancelled() const { return false; }
};

struct BuildRequest {
  std::filesystem::path source_rpf;  // xarchive_cache.rpf
  std::filesystem::path mods_rpf;    // xarchive_mods.rpf; empty or missing: no overlay
  std::filesystem::path out_dir;     // where the pack and the stamp go (made when missing)
  LzxFunction lzx;                   // required
  uint32_t drop_mask = kDefaultDropMask;
  PositionEncoding positions = PositionEncoding::kFloat32;
  uint32_t threads = 0;              // 0: the PC's hardware threads less one, at least one
  bool force = false;                // build even when the stamp says the pack is current
  bool census_only = false;          // read and count, write nothing (the tool's census)
  std::string app_version;
};

// Counts of one shader (group + preset) over the whole city.
struct ShaderCensusRow {
  MaterialClass material = MaterialClass::kSolid;
  uint64_t geometries = 0;
  uint64_t triangles = 0;
};

struct Census {
  uint32_t sectors = 0;
  uint32_t sectors_built = 0;           // with a hi mesh
  uint32_t sectors_without_file = 0;    // a table entry whose .xcs is not in the archive
  uint32_t sectors_failed = 0;          // a file that did not decode or parse
  ParseStats hi;                        // the sectors' sources, summed
  ParseStats lod;                       // lod.xmd's
  WeldStats hi_weld;
  WeldStats lod_weld;
  uint64_t hi_kept_triangles = 0;
  uint64_t hi_kept_vertices = 0;        // welded
  uint64_t hi_kept_alpha_triangles = 0;
  uint64_t lod_kept_triangles = 0;
  uint64_t lod_kept_vertices = 0;
  std::array<uint64_t, size_t(MaterialClass::kCount)> triangles_by_class{};          // the hi sources, every class
  std::array<uint64_t, size_t(MaterialClass::kCount)> dropped_by_class{};            // ... the part the drop mask left out
  std::map<std::string, ShaderCensusRow> shaders;                                    // "group/preset" -> row (hi and lod together)
  std::map<uint32_t, std::array<uint64_t, size_t(MaterialClass::kCount)>> by_bucket; // bucket mask -> triangles by class (CHECK C2)
  std::map<uint32_t, uint64_t> strides;                                              // vertex stride -> geometries
  uint64_t vertices_outside_box = 0;    // vertices more than kBoxMargin outside their sector's box (a different layout would show here)
  uint64_t geometries_outside_box = 0;
  uint32_t lod_models_outside_box = 0;  // lod models whose bounds are not inside their sector's box (C5: the index pairing)
  uint32_t lod_models_checked = 0;
  uint32_t lod_best_is_index = 0;       // lod models whose bounds overlap their own sector's box more than any other sector's (C5: the pairing by index)
  uint32_t max_sector_triangles = 0;
  std::string max_sector_name;
  uint64_t hi_blob_bytes = 0;
  uint64_t lod_blob_bytes = 0;
  double read_seconds = 0.0;            // summed over the workers (C3)
  double decode_seconds = 0.0;
  double parse_seconds = 0.0;
  double encode_seconds = 0.0;
  std::vector<std::string> problems;    // the first few

  void Merge(const Census& other);
  // The text the tool prints: totals, the classes, every shader with its class, the buckets, the golden comparison.
  std::string Report() const;
};

inline constexpr float kBoxMargin = 2.0f;  // metres a vertex may lie outside its sector's box before the census counts it

struct BuildResult {
  bool ok = false;
  bool up_to_date = false;   // the stamp matched: nothing was built
  bool cancelled = false;
  std::string error;
  Census census;
  uint64_t pack_bytes = 0;
  uint64_t pack_hash = 0;
  double seconds = 0.0;
  std::filesystem::path pack_path;
};

// What the stamp is compared with for `request` (stat only).
StampInputs StampInputsFor(const BuildRequest& request);
// Stat only: is the pack in request.out_dir current for these inputs? (BuildCityPack asks this itself unless request.force.)
StampCheck CheckCityPack(const BuildRequest& request);

BuildResult BuildCityPack(const BuildRequest& request, ProgressSink* sink);

// The sector file's path inside the archive: "resources/city/sc/<name>.xcs".
std::string SectorArchivePath(std::string_view sector_name);
inline constexpr const char* kCityTablePath = "resources/city/sc/city.xct";
inline constexpr const char* kLodPath = "resources/city/sc/lod.xmd";

}  // namespace larecomp::citypack
