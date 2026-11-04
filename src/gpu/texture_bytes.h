#pragma once
// ===========================================================================
// MCLA GPU renderer -- the size of what the renderer holds in video memory
// ===========================================================================
// What the caches count when they say how much they hold, and what the
// device counts when a creation of that size failed or was given back
// (device.h NoteFailed / NoteReleased): the bytes of the texels with the whole
// mip chain and every sample, at the block size of the format. The driver
// pads and aligns on top of that, so these are lower bounds, good enough to
// compare a request with what a cache released.
//
// TrimResult is what the caches' Trim members answer (vram_budget.h).
// ===========================================================================

#include <algorithm>
#include <cstdint>

#include <plume_render_interface.h>

namespace mcla::gpu {

// `depth` slices at the base level, halved with each level like the other sides.
inline uint64_t MipChainBytes(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels,
                              plume::RenderFormat format, uint32_t samples) {
  if (format == plume::RenderFormat::UNKNOWN) {
    return 0;
  }
  const uint32_t block = std::max<uint32_t>(plume::RenderFormatBlockWidth(format), 1);
  const uint64_t block_bytes = plume::RenderFormatSize(format);
  uint64_t total = 0;
  for (uint32_t level = 0; level < std::max<uint32_t>(levels, 1); ++level) {
    const uint64_t w = std::max<uint32_t>(width >> level, 1);
    const uint64_t h = std::max<uint32_t>(height >> level, 1);
    const uint64_t d = std::max<uint32_t>(depth >> level, 1);
    total += ((w + block - 1) / block) * ((h + block - 1) / block) * d * block_bytes;
  }
  return total * std::max<uint32_t>(samples, 1);
}

// A 2D texture of `levels` mips (the host textures of the caches).
inline uint64_t TextureBytes(uint32_t width, uint32_t height, uint32_t levels,
                             plume::RenderFormat format, uint32_t samples = 1) {
  return MipChainBytes(width, height, 1, levels, format, samples);
}

inline uint64_t TextureBytes(const plume::RenderTextureDesc& desc) {
  return MipChainBytes(desc.width, desc.height, desc.depth, desc.mipLevels, desc.format,
                       desc.multisampling.sampleCount) *
         std::max<uint32_t>(desc.arraySize, 1);
}

// What a Trim released.
struct TrimResult {
  uint32_t items = 0;
  uint64_t bytes = 0;
};

}  // namespace mcla::gpu
