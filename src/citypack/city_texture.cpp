#include "citypack/city_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "citypack/guest_hash.h"

namespace larecomp::citypack {

namespace {

uint32_t Expand565(uint32_t v, int shift, int bits) {
  const uint32_t c = (v >> shift) & ((1u << bits) - 1u);
  return bits == 5 ? (c << 3) | (c >> 2) : (c << 2) | (c >> 4);
}

// One BC colour block (8 bytes: two 565 endpoints, 2 bits a texel) to 16 RGBA8 texels. `four_colour` forces the 4 colour mode (BC2 and BC3 colour halves).
void DecodeColourBlock(const uint8_t* b, bool four_colour, uint8_t out[16][4]) {
  const uint32_t c0 = uint32_t(b[0]) | uint32_t(b[1]) << 8;
  const uint32_t c1 = uint32_t(b[2]) | uint32_t(b[3]) << 8;
  uint32_t pal[4][4];
  pal[0][0] = Expand565(c0, 11, 5);
  pal[0][1] = Expand565(c0, 5, 6);
  pal[0][2] = Expand565(c0, 0, 5);
  pal[1][0] = Expand565(c1, 11, 5);
  pal[1][1] = Expand565(c1, 5, 6);
  pal[1][2] = Expand565(c1, 0, 5);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || four_colour) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
      pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
      pal[3][k] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;  // the transparent black of the 3 colour mode
  }
  const uint32_t indices = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; ++i) {
    const uint32_t p = (indices >> (2 * i)) & 3u;
    for (int k = 0; k < 4; ++k) out[i][k] = uint8_t(pal[p][k]);
  }
}

void DecodeAlphaBc3(const uint8_t* b, uint8_t out[16]) {
  const int a0 = b[0];
  const int a1 = b[1];
  int pal[8];
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int k = 1; k <= 6; ++k) pal[1 + k] = ((7 - k) * a0 + k * a1) / 7;
  } else {
    for (int k = 1; k <= 4; ++k) pal[1 + k] = ((5 - k) * a0 + k * a1) / 5;
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i] = uint8_t(pal[(bits >> (3 * i)) & 7u]);
}

}  // namespace

float PwlGammaToLinear(float gamma) {
  gamma = std::clamp(gamma, 0.0f, 1.0f);
  float scale;
  float offset;
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else if (gamma >= 64.0f / 255.0f) {
    scale = 2.0f / 1024.0f;
    offset = -64.0f;
  } else {
    scale = 1.0f / 1024.0f;
    offset = 0.0f;
  }
  float lin = gamma * ((255.0f * 1024.0f) * scale) + offset;
  lin += std::trunc(lin * scale);
  return lin * (1.0f / 1023.0f);
}

void DecodeBlocksToRgba8(HitTextureFormat format, const uint8_t* blocks, uint32_t width, uint32_t height, std::vector<uint8_t>* rgba) {
  rgba->assign(size_t(width) * height * 4, 0);
  if (format == HitTextureFormat::kRgba8) {
    std::memcpy(rgba->data(), blocks, rgba->size());
    return;
  }
  const uint32_t bx = (width + 3) / 4;
  const uint32_t by = (height + 3) / 4;
  const uint32_t block_bytes = format == HitTextureFormat::kBc1 ? 8u : 16u;
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      const uint8_t* b = blocks + (size_t(y) * bx + x) * block_bytes;
      uint8_t texels[16][4];
      if (format == HitTextureFormat::kBc1) {
        DecodeColourBlock(b, false, texels);
      } else {
        DecodeColourBlock(b + 8, true, texels);
        if (format == HitTextureFormat::kBc2) {
          for (int i = 0; i < 16; ++i) {
            const uint32_t nibble = (b[i / 2] >> (4 * (i & 1))) & 15u;
            texels[i][3] = uint8_t(nibble * 17u);
          }
        } else {
          uint8_t alpha[16];
          DecodeAlphaBc3(b, alpha);
          for (int i = 0; i < 16; ++i) texels[i][3] = alpha[i];
        }
      }
      for (uint32_t j = 0; j < 4; ++j) {
        const uint32_t py = y * 4 + j;
        if (py >= height) break;
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t px = x * 4 + i;
          if (px >= width) break;
          std::memcpy(rgba->data() + (size_t(py) * width + px) * 4, texels[j * 4 + i], 4);
        }
      }
    }
  }
}

LevelMean MeanOfLevel(HitTextureFormat format, bool gamma, const uint8_t* blocks, uint32_t width, uint32_t height) {
  LevelMean mean;
  std::vector<uint8_t> rgba;
  DecodeBlocksToRgba8(format, blocks, width, height, &rgba);
  // The 256 values of a channel, linearised once.
  float lut[256];
  for (int v = 0; v < 256; ++v) lut[v] = gamma ? PwlGammaToLinear(float(v) / 255.0f) : float(v) / 255.0f;
  double sum[3] = {0, 0, 0};
  double alpha_sum = 0;
  uint64_t counted = 0;
  uint64_t all = 0;
  double all_sum[3] = {0, 0, 0};
  uint32_t amin = 255;
  uint64_t low = 0;
  uint64_t extreme = 0;
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const uint32_t a = rgba[i + 3];
    amin = std::min(amin, a);
    if (a < 128) ++low;
    if (a <= 16 || a >= 239) ++extreme;
    alpha_sum += a;
    ++all;
    for (int k = 0; k < 3; ++k) all_sum[k] += lut[rgba[i + size_t(k)]];
    if (format == HitTextureFormat::kBc1 && a == 0) continue;  // the punch through texel
    for (int k = 0; k < 3; ++k) sum[k] += lut[rgba[i + size_t(k)]];
    ++counted;
  }
  if (all == 0) return mean;
  if (counted == 0) {
    for (int k = 0; k < 3; ++k) mean.rgb[k] = float(all_sum[k] / double(all));  // all transparent: the colours as they are
  } else {
    for (int k = 0; k < 3; ++k) mean.rgb[k] = float(sum[k] / double(counted));
  }
  mean.alpha_mean = float(alpha_sum / (255.0 * double(all)));
  mean.alpha_min = uint8_t(amin);
  mean.alpha_low_share = float(double(low) / double(all));
  mean.alpha_extreme_share = float(double(extreme) / double(all));
  return mean;
}

bool AlphaIsCoverage(const LevelMean& mean) { return mean.alpha_low_share >= kCoverageLowShare && mean.alpha_extreme_share >= kCoverageExtremeShare; }

// CHECK: this is the number the renderer's texture cache keys a texture by (TextureCache::SelectReplacement: XXH3-64 of the base level's extent at its address in guest memory) when the
// guest holds the resource's bytes as the file has them: compare a texture dump's `<name>__<hash>.dds` with the key of the same texture on a run (GB-F2's `hash:` rows).
bool CityTextureKey(const RscImage& image, const DictionaryTexture& texture, uint64_t* key) {
  const xenos::GuestLayout layout = xenos::GetGuestLayout(texture.desc);
  if (!layout.ok) return false;
  const xenos::LevelPlan plan = xenos::PlanLevel(texture.desc, layout, 0);
  if (!plan.ok) return false;
  size_t size = 0;
  const uint8_t* region = ResourceBytesAt(image, plan.region_address, &size);
  if (!region || layout.base.data_extent_bytes > size) return false;
  *key = HashGuestBytes(region, size_t(layout.base.data_extent_bytes));
  return true;
}

DecodedTexture DecodeCityTexture(const RscImage& image, const DictionaryTexture& source) {
  DecodedTexture out;
  const xenos::TextureDesc& desc = source.desc;
  const xenos::FormatInfo info = xenos::GetFormatInfo(desc.format);
  const auto fail = [&](const char* why) {
    out.ok = false;
    out.why = why;
    return out;
  };
  if (!info.supported()) return fail("a format this reader does not decode");
  const xenos::GuestLayout layout = xenos::GetGuestLayout(desc);
  if (!layout.ok) return fail("no layout for the fetch constant");
  out.width = desc.width;
  out.height = desc.height;
  out.stored_levels = layout.max_level + 1;
  out.format = desc.format == xenos::kFormatDxt1 ? HitTextureFormat::kBc1
               : desc.format == xenos::kFormatDxt23 ? HitTextureFormat::kBc2
               : desc.format == xenos::kFormatDxt45 ? HitTextureFormat::kBc3
                                                    : HitTextureFormat::kRgba8;
  if (desc.gamma) out.flags |= kTextureGamma;

  // The key: the base level's storage, as the renderer hashes guest memory (the level's data extent from its address).
  const xenos::LevelPlan base_plan = xenos::PlanLevel(desc, layout, 0);
  if (!base_plan.ok) return fail("no plan for the base level");
  size_t region_size = 0;
  const uint8_t* base_region = ResourceBytesAt(image, base_plan.region_address, &region_size);
  if (!base_region) return fail("the base level's address is outside the resource");
  const uint64_t extent = layout.base.data_extent_bytes;
  out.key_complete = extent <= region_size;
  out.key = out.key_complete ? HashGuestBytes(base_region, size_t(extent)) : 0;

  // The levels.
  std::vector<uint8_t> level;
  std::vector<uint8_t> pick_blocks;  // the level the mean is made from (the first small enough, else the last stored)
  uint32_t pick_level = 0;
  bool settled = false;
  for (uint32_t l = 0; l <= layout.max_level; ++l) {
    const xenos::LevelPlan plan = xenos::PlanLevel(desc, layout, l);
    size_t size = 0;
    const uint8_t* region = plan.ok ? ResourceBytesAt(image, plan.region_address, &size) : nullptr;
    const bool whole = region && xenos::DecodeLevel(desc, plan, region, size, &level);
    if (!whole) {
      if (l == 0) return fail("the base level is cut or outside the resource");
      out.damaged_levels = layout.max_level + 1 - l;
      break;
    }
    // CHECK: no city texture is k_8_8_8_8 (none of the 40,984 entries), so the channel order after the endian swap (Xenia's: x y z w of the texel, then the fetch swizzle) is untested on
    // real data; the synthetic test only checks a swizzle that does what it says.
    if (info.block_bytes == 4 && info.block_width == 1) {
      // k_8_8_8_8: the fetch swizzle picks each channel (0..3 = x y z w of the texel, 4 = 0, 5 = 1).
      uint8_t* p = level.data();
      for (size_t i = 0; i + 3 < level.size(); i += 4) {
        const uint8_t raw[4] = {p[i], p[i + 1], p[i + 2], p[i + 3]};
        for (uint32_t k = 0; k < 4; ++k) {
          const uint32_t sel = (desc.swizzle >> (3 * k)) & 7u;
          p[i + k] = sel < 4 ? raw[sel] : (sel == 5 ? 255 : 0);
        }
      }
    }
    out.data.insert(out.data.end(), level.begin(), level.end());
    if (!settled) {
      pick_level = l;
      if (std::max(std::max(desc.width >> l, 1u), std::max(desc.height >> l, 1u)) <= kAverageLevelMaxSide) {
        pick_blocks = level;
        settled = true;
      } else {
        pick_blocks.swap(level);  // the last level so far
      }
    }
    ++out.mip_count;
  }

  // The mean: from the first level that is small enough, else the smallest there is.
  out.average_level = pick_level;
  const LevelMean mean = MeanOfLevel(out.format, desc.gamma, pick_blocks.data(), std::max(desc.width >> pick_level, 1u), std::max(desc.height >> pick_level, 1u));
  for (int k = 0; k < 3; ++k) out.average[k] = mean.rgb[k];
  out.alpha_mean = mean.alpha_mean;
  out.alpha_min = mean.alpha_min;
  out.alpha_low_share = mean.alpha_low_share;
  out.alpha_extreme_share = mean.alpha_extreme_share;
  if ((out.format == HitTextureFormat::kBc2 || out.format == HitTextureFormat::kBc3 || out.format == HitTextureFormat::kRgba8) && AlphaIsCoverage(mean)) out.flags |= kTextureAlpha;
  out.ok = true;
  return out;
}

}  // namespace larecomp::citypack
