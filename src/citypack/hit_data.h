#pragma once
// ===========================================================================
// LARecomp city pack -- the hit data as ONE operation (CITY-HIT-ACTION)
// ===========================================================================
// The hit data is two files beside the city pack: the hit file (mcla_city_hit.v1.pack, hit_builder.h) and the texture file (mcla_city_tex.v1.pack, texture_set.h) that its texture table points
// into, with ONE stamp (mcla_city_hit.stamp: it holds the texture file's size and the texture set's identity, and the texture file's header carries the hit file's table hash). Building them
// is "the textures first, then the hit file over the city pack", checking them is "the stamp, and the files read back whole". Three programs do exactly that and share this code:
//
//   citypack_tool build-hit / check-hit / check-tex    the dev tool (tools/citypack_tool)
//   larecomp.exe --build_city_hit / --check_city_hit   the launcher's setup and a hand check (bootprogress/larecomp_city_pack.h)
//   the game itself                                    in the background, when the hit data is wanted and missing or stale (the same file)
//
// Nothing here knows a window, a renderer or a thread of its own; the caller gives the LZX function, the folder and the sink, as for the city pack (builder.h). Reads the player's own game
// files; what it writes is game-derived and is never shipped or committed.
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/hit_builder.h"
#include "citypack/hit_texture_adapter.h"
#include "citypack/texture_set.h"

#if !defined(LARECOMP_HIT_HAS_TEXTURE_SET)
#error "citypack/hit_data.h needs the texture set (texture_set.h): the hit data is the hit file and the texture file together"
#endif

namespace larecomp::citypack {

struct HitDataRequest {
  // As HitBuildRequest::base: source_rpf, mods_rpf, out_dir (the CITY PACK's folder: the hit data goes there too), lzx, threads, worker_start (also the texture set's workers), force,
  // census_only (everything is read and counted, nothing is written), app_version. drop_mask and positions are ignored: the city pack's header decides.
  BuildRequest base;
  // GB-F2 per-texture table files (hit_builder.h HitBuildRequest::texture_rows).
  std::vector<std::filesystem::path> texture_rows;
  HitRules rules;
  // zstd level of the hit file's corner streams and the texture file's chains (city_zstd.h; 0 = stored as they are).
  int zstd_level = kZstdLevelDefault;
  // The texture file with the hit file (the default). False builds the hit file alone (the development tool's --no-textures): its stamp then says "none" for the texture set.
  bool textures = true;
};

// The hit builder's own request for `request` (the one place that fills it: the stamp check and the build must see the same rules). `source` is the texture source the hit file's stamp
// names (null with request.textures false).
HitBuildRequest MakeHitBuildRequest(const HitDataRequest& request, HitTextureSource* source);

// Stat only: is the hit data in request.base.out_dir current for the game files, the city pack beside it and these rules? (CheckCityHitPack: a few file stats, the city pack's table, the
// stamp; the texture file's size is in the stamp, so a texture file that is gone or cut makes it not current.) `why` says the first line that did not match, in plain words.
StampCheck CheckHitData(const HitDataRequest& request);

struct HitDataResult {
  bool ok = false;
  bool up_to_date = false;     // the stamp matched: nothing was built
  bool cancelled = false;
  bool no_archive = false;     // the game archive (base.source_rpf) is not there
  bool no_pack = false;        // the city pack the hit data is built over is not there or is damaged: build the city pack first
  std::string error;
  HitBuildResult hit;          // the census, the file's size, the table hash (the hit builder's own result; census.* is what the tool prints)
  bool textures_built = false;  // the texture set was built in this run (not when the stamp was current)
  TextureSetStats texture_stats;
  uint64_t texture_bytes = 0;  // the texture file's size on disk after the build (0: none, or a census)
  double seconds = 0.0;        // the whole run
};

// The textures first (the texture set: its dictionaries and pictures, "city textures" and "city texture files" in the sink), then the hit file over the city pack ("far level of detail" and
// "city hit sectors"), which writes the texture file and the stamp LAST. A run whose stamp is current (and not forced) does nothing and says so (up_to_date). A cancelled or failed run
// leaves nothing that reads as hit data (the builders write under temporary names). Every call of `sink` comes from the calling thread.
HitDataResult BuildHitData(const HitDataRequest& request, ProgressSink* sink);

// ---------------------------------------------------------------------------
// the deep check: the files read back whole
// ---------------------------------------------------------------------------

struct HitDataCheckParts {
  bool pack = true;      // the city pack: every blob decoded and compared with its table (ValidatePack: the lamps section too)
  bool hit = true;       // the hit file: every blob decoded and compared with the city pack (ValidateHitPack)
  bool textures = true;  // the texture file: every picture read and unpacked against the hit file's texture table (ValidateTextureFile)
};

struct HitDataCheck {
  bool ok = false;               // every asked part is whole
  bool unreadable = false;       // a file could not even be opened (`error` says which): not a count of problems
  std::string error;
  uint32_t pack_problems = 0;
  uint32_t hit_problems = 0;
  uint32_t texture_problems = 0;
  uint32_t pictures = 0;         // the texture table's size
  bool textures_skipped = false;  // the hit file has no textures and there is no texture file (built with textures off): nothing to read back
  double pack_seconds = 0.0;
  double hit_seconds = 0.0;
  double texture_seconds = 0.0;
  std::vector<std::string> problems;  // the first few of every part, each starting with the part's name
};

// Reads the files in `folder` (the city pack's) through. One phase per asked part in the sink ("checking the city pack", "checking the city hit file", "checking the city textures"; one
// step each), so that a progress bar moves. `sink` may be null. Not a stat check: seconds, not milliseconds.
HitDataCheck DeepCheckHitData(const std::filesystem::path& folder, const HitDataCheckParts& parts, ProgressSink* sink);

}  // namespace larecomp::citypack
