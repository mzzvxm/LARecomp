#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <rex/graphics/xenos.h>
#include <rex/hash.h>

namespace mcla {

namespace xenos = rex::graphics::xenos;

// Content hash of a guest texture. Must stay XXH3-64 over the raw guest bytes:
// it is what names the dump file, and what the SDK's replacement lookup keys
// on, so the two have to agree.
inline uint64_t HashGuestTexture(const uint8_t* data, size_t size) {
  return XXH3_64bits(data, size);
}

// Where dumps go: the SDK's texture_folder cvar when it exists (so the game and
// the SDK write to the same tree), otherwise larecomp_texture_folder.
std::filesystem::path TextureDumpDir();

// Writes <dump_dir>/<hash16>_<w>x<h>_<fmt>.dds, untiling and endian-swapping
// first. Compressed formats keep their blocks; everything else is expanded to
// RGBA8. Depth and float render-target formats are skipped -- they are not
// source art. A file that already exists is left alone.
void DumpGuestTexture(const std::filesystem::path& dump_dir, uint64_t content_hash, uint32_t width,
                      uint32_t height, uint32_t pitch_blocks, bool tiled,
                      xenos::TextureFormat format, xenos::Endian endianness,
                      const uint8_t* guest_bytes, uint32_t guest_size);

}  // namespace mcla
