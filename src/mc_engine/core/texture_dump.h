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

// Where dumps go: <mod folder>/textures_dump/ (mc::modloader::ModRootDir), or
// larecomp_texture_folder when it names another folder.
std::filesystem::path TextureDumpDir();

// Writes <dump_dir>/<hash16>_<w>x<h>_<fmt>.dds, untiling and endian-swapping
// first. Compressed formats keep their blocks; everything else is expanded to
// RGBA8. Depth and float render-target formats are skipped -- they are not
// source art. A file that already exists is left alone.
void DumpGuestTexture(const std::filesystem::path& dump_dir, uint64_t content_hash, uint32_t width,
                      uint32_t height, uint32_t pitch_blocks, bool tiled,
                      xenos::TextureFormat format, xenos::Endian endianness,
                      const uint8_t* guest_bytes, uint32_t guest_size);

// The same conversion written to exactly `dest` (an existing file is
// replaced). False when the format is not dumpable or the write failed.
bool DumpGuestTextureTo(const std::filesystem::path& dest, uint32_t width, uint32_t height,
                        uint32_t pitch_blocks, bool tiled, xenos::TextureFormat format,
                        xenos::Endian endianness, const uint8_t* guest_bytes,
                        uint32_t guest_size);

// Whether DumpGuestTextureTo writes anything for `format`: block-compressed
// and the plain colour layouts, not depth or float render targets.
bool IsDumpableTextureFormat(xenos::TextureFormat format);

}  // namespace mcla
