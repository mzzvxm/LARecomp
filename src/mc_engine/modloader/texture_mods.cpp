#include "texture_mods.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include "mc_engine/logging.h"

namespace mc::modloader {

namespace {

std::atomic<bool> g_scene_dump_requested{false};
std::mutex g_state_mutex;
std::vector<TextureModInfo> g_active_mods;
size_t g_total_replacements = 0;

// Mapping of base_address -> asset_name
std::mutex g_name_registry_mutex;
std::unordered_map<uint32_t, std::string> g_texture_names_by_base;

std::string ToLower(std::string_view str) {
  std::string lower(str);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower;
}

bool Parse16HexHash(std::string_view stem, uint64_t& out_hash) {
  if (stem.size() < 16) {
    return false;
  }
  uint64_t hash = 0;
  for (size_t i = 0; i < 16; ++i) {
    char c = stem[i];
    uint64_t nibble = 0;
    if (c >= '0' && c <= '9') {
      nibble = static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      nibble = static_cast<uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      nibble = static_cast<uint64_t>(c - 'A' + 10);
    } else {
      return false;
    }
    hash = (hash << 4) | nibble;
  }
  out_hash = hash;
  return true;
}

}  // namespace

std::string SanitizeTextureAssetName(std::string_view name) {
  std::string safe;
  safe.reserve(name.size());
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
      safe.push_back(c);
    } else if (c == ' ' || c == '/' || c == '\\') {
      safe.push_back('_');
    }
  }
  // Trim leading/trailing underscores
  while (!safe.empty() && safe.front() == '_') safe.erase(safe.begin());
  while (!safe.empty() && safe.back() == '_') safe.pop_back();
  return safe;
}

void RegisterTextureIdentity(uint32_t base_address, std::string_view name) {
  if (base_address == 0 || name.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_name_registry_mutex);
  g_texture_names_by_base[base_address] = std::string(name);
}

std::string GetTextureNameByBase(uint32_t base_address) {
  if (base_address == 0) {
    return {};
  }
  std::lock_guard<std::mutex> lock(g_name_registry_mutex);
  auto it = g_texture_names_by_base.find(base_address);
  if (it != g_texture_names_by_base.end()) {
    return it->second;
  }
  return {};
}

void TriggerSceneTextureDump() {
  g_scene_dump_requested.store(true, std::memory_order_release);
  MC_INFO("[texture-mods] Scene texture dump requested for next frame");
}

bool IsSceneTextureDumpRequested() {
  return g_scene_dump_requested.load(std::memory_order_acquire);
}

void ClearSceneTextureDumpRequest() {
  g_scene_dump_requested.store(false, std::memory_order_release);
}

size_t RescanTextureMods(const std::filesystem::path& modloader_dir) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_active_mods.clear();
  g_total_replacements = 0;

  std::error_code ec;
  if (!std::filesystem::is_directory(modloader_dir, ec)) {
    MC_WARN("[texture-mods] modloader directory does not exist: {}", modloader_dir.string());
    return 0;
  }

  const std::filesystem::path dump_dir = modloader_dir / "dump";
  const std::filesystem::path replace_dir = modloader_dir / "replace";

  std::filesystem::create_directories(dump_dir, ec);
  std::filesystem::create_directories(replace_dir, ec);

  // Map of unique 64-bit hash -> source file path (priority: first mod found keeps it)
  struct ReplacementFile {
    uint64_t hash;
    std::filesystem::path source_path;
    std::string mod_name;
  };
  std::unordered_map<uint64_t, ReplacementFile> replacements;

  // Enumerate mod directories in modloader_dir
  std::vector<std::filesystem::path> mod_dirs;
  for (const auto& entry : std::filesystem::directory_iterator(modloader_dir, ec)) {
    if (ec) break;
    if (!entry.is_directory()) continue;

    const std::string dir_name = entry.path().filename().string();
    if (dir_name == "dump" || dir_name == "replace" || dir_name == ".cache" ||
        dir_name.starts_with('.')) {
      continue;
    }
    mod_dirs.push_back(entry.path());
  }

  // Sort mod directories alphabetically for stable load order
  std::sort(mod_dirs.begin(), mod_dirs.end());

  for (const auto& mod_dir : mod_dirs) {
    const std::string mod_name = mod_dir.filename().string();
    const std::filesystem::path inject_dir = mod_dir / "inject";

    if (!std::filesystem::is_directory(inject_dir, ec)) {
      continue;
    }

    size_t mod_texture_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(inject_dir, ec)) {
      if (ec) break;
      if (!entry.is_regular_file()) continue;

      const auto ext = ToLower(entry.path().extension().string());
      if (ext != ".dds" && ext != ".png") continue;

      const std::string stem = entry.path().stem().string();
      uint64_t hash = 0;
      if (!Parse16HexHash(stem, hash)) {
        MC_WARN("[texture-mods] [{}] skipping invalid texture filename (needs 16-hex prefix): {}",
                mod_name, entry.path().filename().string());
        continue;
      }

      if (replacements.find(hash) == replacements.end()) {
        replacements.emplace(hash, ReplacementFile{hash, entry.path(), mod_name});
      } else {
        MC_DEBUG("[texture-mods] [{}] hash {:016x} shadowed by higher priority mod",
                 mod_name, hash);
      }
      ++mod_texture_count;
    }

    if (mod_texture_count > 0) {
      g_active_mods.push_back(TextureModInfo{mod_name, mod_dir, mod_texture_count});
      MC_INFO("[texture-mods] Loaded mod '{}' with {} injected texture(s)",
              mod_name, mod_texture_count);
    }
  }

  // Clear previous staging in modloader/replace to avoid orphan files
  for (const auto& entry : std::filesystem::directory_iterator(replace_dir, ec)) {
    if (ec) break;
    if (entry.is_regular_file()) {
      std::filesystem::remove(entry.path(), ec);
    }
  }

  // Stage resolved replacement files into modloader/replace
  for (const auto& [hash, repl] : replacements) {
    const std::filesystem::path target_file = replace_dir / repl.source_path.filename();

    // Prefer fast NTFS hard links (zero disk copy, instantaneous, auto-updates on source edits)
    std::filesystem::create_hard_link(repl.source_path, target_file, ec);
    if (ec) {
      // Fallback to copy if cross-volume or filesystem doesn't support hard links
      ec.clear();
      std::filesystem::copy_file(repl.source_path, target_file,
                                 std::filesystem::copy_options::overwrite_existing, ec);
    }

    if (!ec) {
      ++g_total_replacements;
    } else {
      MC_WARN("[texture-mods] Failed to stage texture {}: {}",
              repl.source_path.filename().string(), ec.message());
    }
  }

  MC_INFO("[texture-mods] Staged {} unique texture replacement(s) across {} mod(s) in {}",
          g_total_replacements, g_active_mods.size(), replace_dir.string());

  return g_total_replacements;
}

void InitTextureModding(const std::filesystem::path& modloader_dir) {
  // Ensure ReXGlue texture_folder cvar points to our modloader directory
  const std::string modloader_str = modloader_dir.string();
  rex::cvar::SetFlagByName("texture_folder", modloader_str.c_str());
  rex::cvar::SetFlagByName("texture_replace_enabled", "true");

  RescanTextureMods(modloader_dir);
}

std::vector<TextureModInfo> GetActiveTextureMods() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return g_active_mods;
}

size_t GetTotalActiveTextureReplacements() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return g_total_replacements;
}

}  // namespace mc::modloader
