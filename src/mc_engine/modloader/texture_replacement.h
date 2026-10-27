#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::modloader {

// A single decoded mip level (tightly packed RGBA8)
struct ReplacementMipLevel {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
};

// Complete decoded replacement texture data
struct TextureReplacementData {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_levels = 1;
  std::vector<ReplacementMipLevel> mips;
};

class TextureReplacement {
 public:
  static TextureReplacement& Instance();

  // Initialize and index replacement files from modloader/textures_replace/
  void Initialize(const std::filesystem::path& replace_dir);

  // Rescans the replacement directory for newly added or modified DDS files
  void Rescan();

  // Clears the in-memory decoded pixel cache
  void FlushCache();

  // Finds and returns the decoded replacement texture for a guest 64-bit content hash.
  // Returns nullptr if no replacement exists for this hash.
  // Loads and decodes on demand with thread-safe caching.
  const TextureReplacementData* FindReplacement(uint64_t content_hash);

  // Fast existence check without loading/decoding the DDS payload
  bool HasReplacement(uint64_t content_hash) const;

  size_t IndexedCount() const;

 private:
  TextureReplacement() = default;

  bool ReadDDS(const std::filesystem::path& path, TextureReplacementData& out);

  mutable std::mutex mutex_;
  std::filesystem::path replace_dir_;
  std::unordered_map<uint64_t, std::filesystem::path> indexed_files_;
  std::unordered_map<uint64_t, TextureReplacementData> pixel_cache_;
  std::unordered_map<uint64_t, bool> failed_cache_;
};

inline const TextureReplacementData* FindTextureReplacement(uint64_t content_hash) {
  return TextureReplacement::Instance().FindReplacement(content_hash);
}

inline bool HasTextureReplacement(uint64_t content_hash) {
  return TextureReplacement::Instance().HasReplacement(content_hash);
}

}  // namespace mc::modloader
