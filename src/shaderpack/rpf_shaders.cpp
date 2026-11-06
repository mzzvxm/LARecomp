#include "shaderpack/rpf_shaders.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <utility>

#include "mc_engine/modloader/rpf3.h"
#include "shaderpack/log.h"

namespace larecomp::shaderpack {

namespace {

using mc::modloader::RageHash;
using mc::modloader::Rpf3Entry;
using mc::modloader::Rpf3Reader;

constexpr const char* kArchiveName = "xarchive_cache.rpf";
constexpr const char* kShadersFolder = "shaders";  // RageHash lowercases, so the case does not matter
constexpr int kMaxDepth = 8;                       // the real tree is two folders deep

// Names the table does not keep but the reference extraction prints. Anything
// else is shown as its hash.
const std::vector<std::pair<uint32_t, const char*>>& KnownNames() {
  static const std::vector<std::pair<uint32_t, const char*>> names = [] {
    std::vector<std::pair<uint32_t, const char*>> list;
    for (const char* name : {"ui", "characters", "cars", "effects", "city", "fxl_final"}) {
      list.emplace_back(RageHash(name), name);
    }
    return list;
  }();
  return names;
}

std::string PathUtf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

std::string NameOf(uint32_t hash) {
  for (const auto& [known_hash, name] : KnownNames()) {
    if (known_hash == hash) return name;
  }
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(hash));
  return text;
}

struct PendingFile {
  std::string path;
  Rpf3Entry entry;
};

RpfShadersResult Fail(RpfShadersCode code, std::string message) {
  RpfShadersResult result;
  result.code = code;
  result.message = std::move(message);
  return result;
}

// Depth first walk below `directory`. `budget` is the number of entries the
// whole table has: a table whose folders point back at each other would
// otherwise never end.
bool CollectFiles(const Rpf3Reader& archive, const Rpf3Entry& directory, const std::string& prefix,
                  int depth, size_t& budget, std::vector<PendingFile>& out, std::string& error) {
  if (depth > kMaxDepth) {
    error = "the Shaders folder is nested deeper than any known archive";
    return false;
  }
  std::vector<Rpf3Entry> children;
  if (!archive.Children(directory, children)) {
    error = "a folder in the archive table points outside the table";
    return false;
  }
  for (const Rpf3Entry& child : children) {
    if (budget == 0) {
      error = "the archive table visits more entries than it holds";
      return false;
    }
    --budget;

    const std::string name = NameOf(child.hash);
    if (child.is_directory()) {
      if (!CollectFiles(archive, child, prefix + name + "/", depth + 1, budget, out, error)) {
        return false;
      }
    } else {
      out.push_back(PendingFile{prefix + name, child});
    }
  }
  return true;
}

}  // namespace

std::filesystem::path RpfArchivePath(const std::filesystem::path& game_root) {
  return game_root / kArchiveName;
}

RpfShadersResult ReadRpfShaders(const std::filesystem::path& game_root, RpfShaders& out,
                                const RpfProgress& progress) {
  const std::filesystem::path path = RpfArchivePath(game_root);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return Fail(RpfShadersCode::kArchiveMissing,
                "xarchive_cache.rpf was not found in " + PathUtf8(game_root));
  }
  Rpf3Reader archive;
  if (!archive.Open(path)) {
    return Fail(RpfShadersCode::kArchiveUnreadable,
                "xarchive_cache.rpf in " + PathUtf8(game_root) + " is not a readable RPF3 archive");
  }
  return ReadRpfShadersFromArchive(archive, out, progress);
}

RpfShadersResult ReadRpfShadersFromArchive(const Rpf3Reader& archive, RpfShaders& out,
                                           const RpfProgress& progress) {
  const auto started = std::chrono::steady_clock::now();
  out = RpfShaders{};

  Rpf3Entry root;
  std::vector<Rpf3Entry> top;
  if (!archive.Root(root) || !archive.Children(root, top)) {
    return Fail(RpfShadersCode::kArchiveUnreadable, "the archive has no readable root folder");
  }

  // Exactly one directory named Shaders among the root's children.
  const uint32_t shaders_hash = RageHash(kShadersFolder);
  const Rpf3Entry* shaders = nullptr;
  size_t matches = 0;
  for (const Rpf3Entry& child : top) {
    if (child.is_directory() && child.hash == shaders_hash) {
      shaders = &child;
      ++matches;
    }
  }
  if (matches != 1) {
    return Fail(RpfShadersCode::kNoShadersFolder,
                matches == 0 ? "the archive has no Shaders folder"
                             : "the archive has more than one Shaders folder");
  }

  std::vector<PendingFile> pending;
  std::string error;
  size_t budget = archive.entry_count();
  if (!CollectFiles(archive, *shaders, std::string(), 0, budget, pending, error)) {
    return Fail(RpfShadersCode::kBadTable, error);
  }

  // The table is hash-ordered; sort by path so the result has one order
  // whatever the archive's. Two files with one path would mean two entries
  // with one hash in a folder, which the engine's binary search could not
  // tell apart either.
  std::sort(pending.begin(), pending.end(),
            [](const PendingFile& a, const PendingFile& b) { return a.path < b.path; });
  for (size_t i = 1; i < pending.size(); ++i) {
    if (pending[i - 1].path == pending[i].path) {
      return Fail(RpfShadersCode::kBadTable,
                  "two files in the Shaders folder share the path " + pending[i].path);
    }
  }

  RpfShaders result;
  result.files.reserve(pending.size());
  for (size_t i = 0; i < pending.size(); ++i) {
    const PendingFile& item = pending[i];
    if (progress && !progress(i, pending.size(), item.path)) {
      return Fail(RpfShadersCode::kCancelled, "cancelled while reading the Shaders folder");
    }

    RpfShaderFile file;
    file.path = item.path;
    file.name_hash = item.entry.hash;
    file.was_deflated = (item.entry.flag & 0x40000000u) != 0;
    if (!archive.ReadPlainFile(item.entry, file.bytes)) {
      char detail[128];
      std::snprintf(detail, sizeof(detail), " (size %u, flag 0x%08X, at byte %llu)",
                    static_cast<unsigned>(item.entry.size), static_cast<unsigned>(item.entry.flag),
                    static_cast<unsigned long long>(item.entry.data_offset()));
      return Fail(RpfShadersCode::kReadFailed,
                  "cannot read " + item.path + " from the Shaders folder" + detail);
    }

    result.total_bytes += file.bytes.size();
    (file.was_deflated ? result.deflated_count : result.stored_count)++;
    result.files.push_back(std::move(file));
  }
  if (progress) progress(pending.size(), pending.size(), std::string());

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  SHADERPACK_LOG_INFO("Shaders folder of {}: {} files, {} bytes ({} stored, {} inflated), {:.2f} s",
                      PathUtf8(archive.path()), result.files.size(), result.total_bytes,
                      result.stored_count, result.deflated_count, seconds);
  if (result.files.size() != kExpectedRpfShaderFiles) {
    SHADERPACK_LOG_WARN("the Shaders folder has {} files, the retail game has {}; "
                        "carrying on, the shader contract does not depend on the count",
                        result.files.size(), kExpectedRpfShaderFiles);
  }

  out = std::move(result);
  return RpfShadersResult{};
}

}  // namespace larecomp::shaderpack
