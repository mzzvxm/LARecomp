#include "texture_mods.h"
#include "texture_replacement.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include "mc_engine/logging.h"

namespace mc::modloader {

namespace {

std::atomic<bool> g_scene_dump_requested{false};
std::mutex g_state_mutex;
std::vector<TextureModInfo> g_active_mods;
size_t g_total_replacements = 0;


std::string ToLower(std::string_view str) {
  std::string lower(str);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower;
}

bool ParseDdsHash(std::string_view stem, uint64_t& out_hash, std::string& out_clean_stem,
                  bool& out_prefixed) {
  std::string_view s = stem;
  out_prefixed = (s.starts_with("0x") || s.starts_with("0X"));
  if (out_prefixed) {
    s.remove_prefix(2);
  }
  if (s.size() != 16) {
    return false;
  }
  uint64_t hash = 0;
  for (size_t i = 0; i < 16; ++i) {
    char c = s[i];
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
  out_clean_stem = ToLower(s);
  return true;
}

}  // namespace

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

  const std::filesystem::path dump_dir = modloader_dir / "textures_dump";
  const std::filesystem::path replace_dir = modloader_dir / "textures_replace";

  std::filesystem::create_directories(dump_dir, ec);
  std::filesystem::create_directories(replace_dir, ec);

  // Map of unique 64-bit hash -> source file path (priority: first mod found keeps it)
  struct ReplacementFile {
    uint64_t hash = 0;
    std::filesystem::path source_path;
    std::string mod_name;
    std::string clean_stem;
    bool prefixed = false;
  };
  std::unordered_map<uint64_t, ReplacementFile> replacements;

  auto process_inject_dir = [&](const std::string& mod_name,
                                const std::filesystem::path& mod_dir,
                                const std::filesystem::path& inject_dir) {
    std::error_code dir_ec;
    if (!std::filesystem::is_directory(inject_dir, dir_ec)) {
      return;
    }

    size_t mod_texture_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(inject_dir, dir_ec)) {
      if (dir_ec) break;
      if (!entry.is_regular_file()) continue;

      const auto ext = ToLower(entry.path().extension().string());
      if (ext != ".dds") {
        // Strict DDS-only enforcement for high-resolution texture injection
        continue;
      }

      const std::string stem = entry.path().stem().string();
      uint64_t hash = 0;
      std::string clean_stem;
      bool prefixed = false;
      if (!ParseDdsHash(stem, hash, clean_stem, prefixed)) {
        MC_WARN("[texture-mods] [{}] skipping invalid texture filename (needs 16-hex hash prefix): {}",
                mod_name, entry.path().filename().string());
        continue;
      }

      auto existing = replacements.find(hash);
      if (existing == replacements.end()) {
        replacements.emplace(hash, ReplacementFile{hash, entry.path(), mod_name, clean_stem, prefixed});
      } else if (!prefixed && existing->second.prefixed && existing->second.mod_name == mod_name) {
        // Within the same mod, unprefixed spelling wins (Texture Toolkit rule)
        existing->second = ReplacementFile{hash, entry.path(), mod_name, clean_stem, prefixed};
      } else {
        MC_DEBUG("[texture-mods] [{}] hash {:016x} shadowed by mod '{}'",
                 mod_name, hash, existing->second.mod_name);
      }
      ++mod_texture_count;
    }

    if (mod_texture_count > 0) {
      g_active_mods.push_back(TextureModInfo{mod_name, mod_dir, mod_texture_count});
      MC_INFO("[texture-mods] Loaded mod '{}' with {} injected texture(s)",
              mod_name, mod_texture_count);
    }
  };

  // Enumerate mod directories in modloader_dir: modloader/<ModName>/textures_replace/
  std::vector<std::filesystem::path> mod_dirs;
  for (const auto& entry : std::filesystem::directory_iterator(modloader_dir, ec)) {
    if (ec) break;
    if (!entry.is_directory()) continue;

    const std::string dir_name = entry.path().filename().string();
    if (dir_name == "textures_dump" || dir_name == "textures_replace" ||
        dir_name == "music" || dir_name == ".cache" || dir_name.starts_with('.')) {
      continue;
    }
    mod_dirs.push_back(entry.path());
  }

  // Sort mod directories alphabetically for stable load order
  std::sort(mod_dirs.begin(), mod_dirs.end());

  for (const auto& mod_dir : mod_dirs) {
    const std::string mod_name = mod_dir.filename().string();
    const auto tr_dir = mod_dir / "textures_replace";
    if (std::filesystem::is_directory(tr_dir, ec)) {
      process_inject_dir(mod_name, mod_dir, tr_dir);
    }
  }

  // Clear previous staging in modloader/textures_replace to avoid orphan files
  for (const auto& entry : std::filesystem::directory_iterator(replace_dir, ec)) {
    if (ec) break;
    if (entry.is_regular_file()) {
      std::filesystem::remove(entry.path(), ec);
    }
  }

  // Stage resolved replacement files into modloader/replace without 0x prefix
  for (const auto& [hash, repl] : replacements) {
    const std::filesystem::path target_file = replace_dir / (repl.clean_stem + ".dds");

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

  // Update LARecomp native texture replacement engine
  TextureReplacement::Instance().Rescan();

  return g_total_replacements;
}

void InitTextureModding(const std::filesystem::path& modloader_dir) {
  // Ensure ReXGlue texture_folder cvar points to our modloader directory
  const std::string modloader_str = modloader_dir.string();
  rex::cvar::SetFlagByName("texture_folder", modloader_str.c_str());
  rex::cvar::SetFlagByName("texture_replace_enabled", "true");

  TextureReplacement::Instance().Initialize(modloader_dir / "textures_replace");

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
