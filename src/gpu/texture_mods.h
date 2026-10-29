#pragma once
// ===========================================================================
// MCLA GPU renderer -- texture mods: the replacement files and their pixels
// ===========================================================================
// A mod replaces a game texture with a DDS in models/<Mod>/textures/ (any
// depth below it). The file is found by the content hash of the texture it
// replaces -- XXH3-64 of the guest bytes of its base level, the SDK's and
// Xenia-path dumps' key, so their files work unchanged -- written as the last
// 16 hex digits of the file name after "__" ("capo__3f2a91c04d7e55b1.dds"),
// the whole name ("3f2a91c04d7e55b1.dds", "0x3f2a91c04d7e55b1.dds") or its
// first 16 ("3f2a91c04d7e55b1_512x512_k_DXT1.dds", the SDK's dumps). Two mods
// with the same hash: the mod folder first in name order wins, logged.
//
// The pixels stay as authored: BC1-BC5 (DXT1/DXT3/DXT5/ATI1/ATI2/BC4U/BC5U
// FourCC, or DX10 headers) go to the GPU compressed, RGBA8/BGRA8/BGRX8 as
// R8G8B8A8. Every mip level the file holds is used; none are made. A format
// outside these, a cube, an array, or a compressed base not a multiple of 4
// is refused, logged once per file, and the game's texture stays.
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

struct ReplacementImage {
  struct Level {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_bytes = 0;  // one row of blocks (or texels), tightly packed
    uint32_t rows = 0;       // rows of blocks (or texels)
    std::vector<uint8_t> data;
  };
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<Level> levels;
};

class TextureMods {
 public:
  // Indexes models/*/textures/ next to the executable.
  void Initialize();
  bool empty() const { return files_.empty(); }
  size_t size() const { return files_.size(); }

  // The file replacing the texture whose base level hashes to `hash`; null
  // when no mod has one.
  const std::filesystem::path* Find(uint64_t hash) const;

  // Reads `path` into `out`. False with `why` on any refusal.
  static bool LoadDds(const std::filesystem::path& path, ReplacementImage* out, std::string* why);

  // The hash a replacement file name carries; false when it carries none.
  static bool HashFromFileName(const std::string& stem, uint64_t* hash);

 private:
  std::unordered_map<uint64_t, std::filesystem::path> files_;
};

}  // namespace mcla::gpu
