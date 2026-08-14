#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — render target readback to a file.
// See image_dump.h.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "image_dump.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <dxgiformat.h>

namespace mcla::native_gfx {

namespace {

float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (e == 0) {
    return (s ? -1.0f : 1.0f) * float(m) / 1024.0f * 6.103515625e-5f;
  }
  if (e == 31) {
    return s ? -65504.0f : 65504.0f;
  }
  return (s ? -1.0f : 1.0f) * (1.0f + float(m) / 1024.0f) *
         ((e >= 15) ? float(1u << (e - 15)) : 1.0f / float(1u << (15 - e)));
}

inline uint8_t ToByte(float v) {
  const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return uint8_t(c * 255.0f + 0.5f);
}

// Writes an uncompressed 32-bit TGA. Deliberately dependency-free.
void WriteTga(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint8_t* bgra) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return;
  }
  uint8_t hdr[18] = {};
  hdr[2] = 2;  // uncompressed true-colour
  hdr[12] = uint8_t(w & 0xFF);
  hdr[13] = uint8_t(w >> 8);
  hdr[14] = uint8_t(h & 0xFF);
  hdr[15] = uint8_t(h >> 8);
  hdr[16] = 32;
  hdr[17] = 0x20;  // top-left origin
  std::fwrite(hdr, 1, sizeof(hdr), f);
  std::fwrite(bgra, 1, size_t(w) * h * 4, f);
  std::fclose(f);
}

}  // namespace

void WriteBgraTga(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  const uint8_t* bgra) {
  WriteTga(path, width, height, bgra);
}

ImageCoverage WriteReadbackTga(const std::filesystem::path& path, const uint8_t* rows,
                               uint32_t row_pitch, uint32_t width, uint32_t height,
                               uint32_t rt_format, const float clear_color[4]) {
  ImageCoverage cov;
  cov.min_x = width;
  cov.min_y = height;
  std::vector<uint8_t> bgra(size_t(width) * height * 4);
  const uint8_t clear_bgra[4] = {ToByte(clear_color[2]), ToByte(clear_color[1]),
                                 ToByte(clear_color[0]), ToByte(clear_color[3])};
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = rows + size_t(y) * row_pitch;
    for (uint32_t x = 0; x < width; ++x) {
      float r = 0, g = 0, b = 0, a = 1;
      if (rt_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const auto* px = reinterpret_cast<const uint16_t*>(row) + x * 4;
        r = HalfToFloat(px[0]);
        g = HalfToFloat(px[1]);
        b = HalfToFloat(px[2]);
        a = HalfToFloat(px[3]);
      } else {
        const uint8_t* px = row + x * 4;
        r = px[0] / 255.0f;
        g = px[1] / 255.0f;
        b = px[2] / 255.0f;
        a = px[3] / 255.0f;
      }
      uint8_t* o = bgra.data() + (size_t(y) * width + x) * 4;
      o[0] = ToByte(b);
      o[1] = ToByte(g);
      o[2] = ToByte(r);
      o[3] = ToByte(a);
      if (o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0) {
        ++cov.untouched;
      } else if (std::memcmp(o, clear_bgra, 4) == 0) {
        ++cov.cleared;
      } else {
        ++cov.drawn;
        cov.has_bounds = true;
        if (x < cov.min_x) cov.min_x = x;
        if (x > cov.max_x) cov.max_x = x;
        if (y < cov.min_y) cov.min_y = y;
        if (y > cov.max_y) cov.max_y = y;
      }
    }
  }
  WriteTga(path, width, height, bgra.data());
  return cov;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
