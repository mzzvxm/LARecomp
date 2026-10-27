#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::modloader {

struct TextureModInfo {
  std::string name;
  std::filesystem::path dir;
  size_t texture_count = 0;
};

// Initializes the texture modding pipeline.
// Scans modloader/*/inject/ and stages them for the texture cache.
void InitTextureModding(const std::filesystem::path& modloader_dir);

// Rescans all mod directories under modloader_dir for new or modified inject files.
// Rebuilds the staged replace/ directory and notifies the texture cache.
size_t RescanTextureMods(const std::filesystem::path& modloader_dir);

// Gets a list of all detected texture mods and their texture counts.
std::vector<TextureModInfo> GetActiveTextureMods();

// Returns total number of active replacement textures currently indexed.
size_t GetTotalActiveTextureReplacements();

// Triggers a snapshot dump of the active scene's textures on the next frame.
void TriggerSceneTextureDump();

// Checks if a scene texture dump is pending.
bool IsSceneTextureDumpRequested();

// Clears the pending scene texture dump flag.
void ClearSceneTextureDumpRequest();

}  // namespace mc::modloader
