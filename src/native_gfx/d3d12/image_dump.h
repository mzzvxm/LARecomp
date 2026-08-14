#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — render target readback to a file
// ===========================================================================
// Shared by first_draw (one draw) and frame_capture (many). Kept in one place
// because both have to decode the same guest render target formats: two
// copies of a half-float decoder would drift and then the two diagnostics
// would disagree about what the GPU produced.
// ===========================================================================

#include <cstdint>
#include <filesystem>

namespace mcla::native_gfx {

// Coverage of a readback image, in pixels.
struct ImageCoverage {
  uint64_t drawn = 0;       // differs from the clear colour
  uint64_t cleared = 0;     // exactly the clear colour
  uint64_t untouched = 0;   // all-zero, i.e. the clear itself never ran
  uint32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  bool has_bounds = false;
};

// Converts a mapped readback buffer to BGRA and writes a TGA, returning what
// the image contains. `rows` is the mapped copy destination, `row_pitch` the
// footprint pitch, `rt_format` the DXGI format the target was rendered in.
ImageCoverage WriteReadbackTga(const std::filesystem::path& path, const uint8_t* rows,
                               uint32_t row_pitch, uint32_t width, uint32_t height,
                               uint32_t rt_format, const float clear_color[4]);

// Writes already-BGRA pixels straight out, for callers that decoded
// themselves (the resolve destinations carry depth, not a colour format).
void WriteBgraTga(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  const uint8_t* bgra);

}  // namespace mcla::native_gfx
