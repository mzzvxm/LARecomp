// The Shaders folder of xarchive_cache.rpf, read into memory.
//
// Half of the shader containers the pack builder translates live here: the
// archive's `Shaders` folder holds 407 small files (about 3.1 MB once inflated)
// for the UI, characters, cars, effects and city, and the generator's container
// walk finds 2214 shader containers inside them. The other half is inside
// default.xex (image_scan.h).
//
// Nothing is written to disk. The archive is opened read-only through
// mc::modloader::Rpf3Reader (same TOC key as the mod loader), the folder is
// found by the hash of its name, and every file below it comes back as bytes.
//
// The table keeps hashes, not names, so a file's path is made of the names this
// reader knows (ui, characters, cars, effects, city, fxl_final) and `0x%08X` of
// the hash for the rest, which is the form the reference extraction
// (native\tools\extract_rpf_shaders.py) uses:
//   cars/fxl_final/0x31CC20E9   one of the containers' bundles
//   cars/0x6D1F986C/0x008DBBA2  a small side file
// A file directly in a folder (cars/0x0DB1C876) is a file in that folder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mc::modloader {
class Rpf3Reader;
}

namespace larecomp::shaderpack {

// The retail Complete Edition has 407 files under Shaders. A different count is
// logged as a warning, never an error: the contract does not depend on it.
constexpr size_t kExpectedRpfShaderFiles = 407;

struct RpfShaderFile {
  std::string path;       // below the Shaders folder, '/' separators, never starts with '/'
  uint32_t name_hash = 0; // the file's own hash in the table
  bool was_deflated = false;
  std::vector<uint8_t> bytes;  // the file as the engine sees it (inflated)
};

struct RpfShaders {
  std::vector<RpfShaderFile> files;  // ordered by path (byte order), so the result never depends on the table order
  uint64_t total_bytes = 0;          // sum of the inflated sizes
  size_t stored_count = 0;           // files stored as they are
  size_t deflated_count = 0;         // files stored as raw deflate
};

enum class RpfShadersCode {
  kOk,
  kCancelled,          // the progress callback asked to stop
  kArchiveMissing,     // no xarchive_cache.rpf in the game folder
  kArchiveUnreadable,  // not an RPF3 archive, or the table could not be read
  kNoShadersFolder,    // the root has no (or more than one) `Shaders` folder
  kBadTable,           // a folder range outside the table, or nesting that never ends
  kReadFailed,         // a file could not be read, inflated, or has the wrong size
};

struct RpfShadersResult {
  RpfShadersCode code = RpfShadersCode::kOk;
  std::string message;  // plain English, names the file when one is at fault
  bool ok() const { return code == RpfShadersCode::kOk; }
};

// Called before each file is read; `done` of `total` files are finished and
// `path` is the next one. Return false to stop (the result is kCancelled).
using RpfProgress = std::function<bool(size_t done, size_t total, const std::string& path)>;

// `game_root` is the folder that holds xarchive_cache.rpf.
std::filesystem::path RpfArchivePath(const std::filesystem::path& game_root);

// Reads the Shaders folder of `<game_root>/xarchive_cache.rpf`. `out` is only
// filled when the result is ok.
RpfShadersResult ReadRpfShaders(const std::filesystem::path& game_root, RpfShaders& out,
                                const RpfProgress& progress = {});

// The same for an archive that is already open (another archive, or a test).
RpfShadersResult ReadRpfShadersFromArchive(const mc::modloader::Rpf3Reader& archive, RpfShaders& out,
                                           const RpfProgress& progress = {});

}  // namespace larecomp::shaderpack
