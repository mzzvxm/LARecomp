#include "texture_replacement.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

#include "mc_engine/logging.h"

namespace mc::modloader {

namespace {

static constexpr uint32_t kDdsMagic = 0x20534444u;  // "DDS "
static constexpr uint32_t kDdsdMipmapCount = 0x00020000u;
static constexpr uint32_t kDdsPfFourCC = 0x00000004u;
static constexpr uint32_t kDdsPfRgb = 0x00000040u;

static constexpr uint32_t kFourCC_DXT1 = 0x31545844u;
static constexpr uint32_t kFourCC_DXT2 = 0x32545844u;
static constexpr uint32_t kFourCC_DXT3 = 0x33545844u;
static constexpr uint32_t kFourCC_DXT4 = 0x34545844u;
static constexpr uint32_t kFourCC_DXT5 = 0x35545844u;
static constexpr uint32_t kFourCC_DX10 = 0x30315844u;
static constexpr uint32_t kFourCC_ATI1 = 0x31495441u;  // "ATI1" / BC4
static constexpr uint32_t kFourCC_BC4U = 0x55344342u;  // "BC4U"
static constexpr uint32_t kFourCC_BC4S = 0x53344342u;  // "BC4S"
static constexpr uint32_t kFourCC_ATI2 = 0x32495441u;  // "ATI2" / BC5
static constexpr uint32_t kFourCC_BC5U = 0x55354342u;  // "BC5U"
static constexpr uint32_t kFourCC_BC5S = 0x53354342u;  // "BC5S"

static constexpr uint32_t kDxgiFormatR8G8B8A8UNorm = 28;
static constexpr uint32_t kDxgiFormatR8G8B8A8UNormSRGB = 29;
static constexpr uint32_t kDxgiFormatBC1UNorm = 71;
static constexpr uint32_t kDxgiFormatBC1UNormSRGB = 72;
static constexpr uint32_t kDxgiFormatBC2UNorm = 74;
static constexpr uint32_t kDxgiFormatBC2UNormSRGB = 75;
static constexpr uint32_t kDxgiFormatBC3UNorm = 77;
static constexpr uint32_t kDxgiFormatBC3UNormSRGB = 78;
static constexpr uint32_t kDxgiFormatBC4UNorm = 80;
static constexpr uint32_t kDxgiFormatBC4SNorm = 81;
static constexpr uint32_t kDxgiFormatBC5UNorm = 83;
static constexpr uint32_t kDxgiFormatBC5SNorm = 84;
static constexpr uint32_t kDxgiFormatB8G8R8A8UNorm = 87;
static constexpr uint32_t kDxgiFormatB8G8R8X8UNorm = 88;
static constexpr uint32_t kDxgiFormatB8G8R8A8UNormSRGB = 91;
static constexpr uint32_t kDxgiFormatB8G8R8X8UNormSRGB = 93;

#pragma pack(push, 1)
struct DdsPixelFormat {
  uint32_t size = 32;
  uint32_t flags = 0;
  uint32_t four_cc = 0;
  uint32_t rgb_bit_count = 0;
  uint32_t r_bit_mask = 0;
  uint32_t g_bit_mask = 0;
  uint32_t b_bit_mask = 0;
  uint32_t a_bit_mask = 0;
};

struct DdsHeader {
  uint32_t magic = kDdsMagic;
  uint32_t size = 124;
  uint32_t flags = 0;
  uint32_t height = 0;
  uint32_t width = 0;
  uint32_t pitch_or_linear = 0;
  uint32_t depth = 0;
  uint32_t mip_map_count = 1;
  uint32_t reserved1[11] = {};
  DdsPixelFormat ddspf;
  uint32_t caps = 0;
  uint32_t caps2 = 0;
  uint32_t caps3 = 0;
  uint32_t caps4 = 0;
  uint32_t reserved2 = 0;
};

struct DdsHeaderDX10 {
  uint32_t dxgi_format = 0;
  uint32_t resource_dimension = 0;
  uint32_t misc_flag = 0;
  uint32_t array_size = 0;
  uint32_t misc_flags2 = 0;
};
#pragma pack(pop)

static_assert(sizeof(DdsHeader) == 128);
static_assert(sizeof(DdsHeaderDX10) == 20);

enum class DdsDecodeFormat {
  kBC1,
  kBC2,
  kBC3,
  kBC4,
  kBC5,
  kRGBA8,
  kBGRA8,
  kBGRX8,
};

inline uint32_t ReadLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t ReadLE16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

inline void Unpack565(uint16_t c, uint8_t out[3]) {
  const uint8_t r = static_cast<uint8_t>((c >> 11) & 0x1F);
  const uint8_t g = static_cast<uint8_t>((c >> 5) & 0x3F);
  const uint8_t b = static_cast<uint8_t>(c & 0x1F);
  out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
  out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
  out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

void DecodeBC1Block(const uint8_t* block, uint8_t out[16][4]) {
  const uint16_t c0 = ReadLE16(block);
  const uint16_t c1 = ReadLE16(block + 2);
  uint8_t colour[4][4] = {};
  Unpack565(c0, colour[0]);
  Unpack565(c1, colour[1]);
  colour[0][3] = colour[1][3] = 255;

  if (c0 > c1) {
    for (int i = 0; i < 3; ++i) {
      colour[2][i] = static_cast<uint8_t>((2 * colour[0][i] + colour[1][i]) / 3);
      colour[3][i] = static_cast<uint8_t>((colour[0][i] + 2 * colour[1][i]) / 3);
    }
    colour[2][3] = colour[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      colour[2][i] = static_cast<uint8_t>((colour[0][i] + colour[1][i]) / 2);
      colour[3][i] = 0;
    }
    colour[2][3] = 255;
    colour[3][3] = 0;
  }

  const uint32_t bits = ReadLE32(block + 4);
  for (int texel = 0; texel < 16; ++texel) {
    const uint8_t* src = colour[(bits >> (texel * 2)) & 3];
    for (int i = 0; i < 4; ++i) out[texel][i] = src[i];
  }
}

void DecodeBC2Alpha(const uint8_t* block, uint8_t out[16]) {
  for (int i = 0; i < 8; ++i) {
    const uint8_t b = block[i];
    out[i * 2 + 0] = static_cast<uint8_t>((b & 0x0F) * 17);
    out[i * 2 + 1] = static_cast<uint8_t>(((b >> 4) & 0x0F) * 17);
  }
}

void DecodeBC3Alpha(const uint8_t* block, uint8_t out[16]) {
  uint8_t alpha[8] = {block[0], block[1]};
  if (alpha[0] > alpha[1]) {
    for (int i = 0; i < 6; ++i) {
      alpha[2 + i] = static_cast<uint8_t>(((6 - i) * alpha[0] + (1 + i) * alpha[1]) / 7);
    }
  } else {
    for (int i = 0; i < 4; ++i) {
      alpha[2 + i] = static_cast<uint8_t>(((4 - i) * alpha[0] + (1 + i) * alpha[1]) / 5);
    }
    alpha[6] = 0;
    alpha[7] = 255;
  }

  for (int half = 0; half < 2; ++half) {
    const uint8_t* p = block + 2 + half * 3;
    const uint32_t bits = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                          (static_cast<uint32_t>(p[2]) << 16);
    for (int i = 0; i < 8; ++i) out[half * 8 + i] = alpha[(bits >> (i * 3)) & 7];
  }
}

void DecodeBC4Block(const uint8_t* block, uint8_t out[16]) {
  DecodeBC3Alpha(block, out);
}

void DecodeBC5Block(const uint8_t* block, uint8_t out_r[16], uint8_t out_g[16]) {
  DecodeBC4Block(block, out_r);
  DecodeBC4Block(block + 8, out_g);
}

bool DecodeBCToRGBA8(const uint8_t* src, size_t src_size, uint32_t width, uint32_t height,
                     DdsDecodeFormat format, std::vector<uint8_t>& out_rgba) {
  const uint32_t bx = (width + 3) / 4;
  const uint32_t by = (height + 3) / 4;
  const uint32_t block_bytes = (format == DdsDecodeFormat::kBC1 || format == DdsDecodeFormat::kBC4) ? 8 : 16;
  if (src_size < static_cast<size_t>(bx) * by * block_bytes) {
    return false;
  }

  out_rgba.resize(static_cast<size_t>(width) * height * 4);
  const uint32_t row_pitch = width * 4;

  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      const uint8_t* blk = src + (y * bx + x) * block_bytes;
      uint8_t colors[16][4] = {};

      if (format == DdsDecodeFormat::kBC1) {
        DecodeBC1Block(blk, colors);
      } else if (format == DdsDecodeFormat::kBC2) {
        DecodeBC1Block(blk + 8, colors);
        uint8_t alpha[16];
        DecodeBC2Alpha(blk, alpha);
        for (int i = 0; i < 16; ++i) colors[i][3] = alpha[i];
      } else if (format == DdsDecodeFormat::kBC3) {
        DecodeBC1Block(blk + 8, colors);
        uint8_t alpha[16];
        DecodeBC3Alpha(blk, alpha);
        for (int i = 0; i < 16; ++i) colors[i][3] = alpha[i];
      } else if (format == DdsDecodeFormat::kBC4) {
        uint8_t red[16];
        DecodeBC4Block(blk, red);
        for (int i = 0; i < 16; ++i) {
          colors[i][0] = red[i];
          colors[i][1] = red[i];
          colors[i][2] = red[i];
          colors[i][3] = 255;
        }
      } else if (format == DdsDecodeFormat::kBC5) {
        uint8_t red[16], green[16];
        DecodeBC5Block(blk, red, green);
        for (int i = 0; i < 16; ++i) {
          colors[i][0] = red[i];
          colors[i][1] = green[i];
          colors[i][2] = red[i];  // Mirror R to B for neutral dual-channel display
          colors[i][3] = 255;
        }
      }

      for (uint32_t py = 0; py < 4; ++py) {
        const uint32_t ty = y * 4 + py;
        if (ty >= height) break;
        for (uint32_t px = 0; px < 4; ++px) {
          const uint32_t tx = x * 4 + px;
          if (tx >= width) break;
          const auto* c = colors[py * 4 + px];
          uint8_t* dst = out_rgba.data() + ty * row_pitch + tx * 4;
          dst[0] = c[0];
          dst[1] = c[1];
          dst[2] = c[2];
          dst[3] = c[3];
        }
      }
    }
  }
  return true;
}

bool DecodeLinear32ToRGBA8(const uint8_t* src, size_t src_size, uint32_t width, uint32_t height,
                           uint32_t src_row_pitch, DdsDecodeFormat format,
                           std::vector<uint8_t>& out_rgba) {
  if (src_size < static_cast<size_t>(src_row_pitch) * height) {
    return false;
  }
  out_rgba.resize(static_cast<size_t>(width) * height * 4);
  const uint32_t dst_row_pitch = width * 4;

  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* s = src + y * src_row_pitch;
    uint8_t* d = out_rgba.data() + y * dst_row_pitch;
    if (format == DdsDecodeFormat::kRGBA8) {
      std::memcpy(d, s, dst_row_pitch);
    } else if (format == DdsDecodeFormat::kBGRA8) {
      for (uint32_t x = 0; x < width; ++x) {
        d[x * 4 + 0] = s[x * 4 + 2];
        d[x * 4 + 1] = s[x * 4 + 1];
        d[x * 4 + 2] = s[x * 4 + 0];
        d[x * 4 + 3] = s[x * 4 + 3];
      }
    } else if (format == DdsDecodeFormat::kBGRX8) {
      for (uint32_t x = 0; x < width; ++x) {
        d[x * 4 + 0] = s[x * 4 + 2];
        d[x * 4 + 1] = s[x * 4 + 1];
        d[x * 4 + 2] = s[x * 4 + 0];
        d[x * 4 + 3] = 255;
      }
    }
  }
  return true;
}

}  // namespace

TextureReplacement& TextureReplacement::Instance() {
  static TextureReplacement inst;
  return inst;
}

void TextureReplacement::Initialize(const std::filesystem::path& replace_dir) {
  std::lock_guard<std::mutex> lock(mutex_);
  replace_dir_ = replace_dir;
  Rescan();
}

void TextureReplacement::FlushCache() {
  std::lock_guard<std::mutex> lock(mutex_);
  pixel_cache_.clear();
  failed_cache_.clear();
}

size_t TextureReplacement::IndexedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return indexed_files_.size();
}

bool TextureReplacement::HasReplacement(uint64_t content_hash) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return indexed_files_.contains(content_hash);
}

void TextureReplacement::Rescan() {
  indexed_files_.clear();
  pixel_cache_.clear();
  failed_cache_.clear();

  std::error_code ec;
  if (!std::filesystem::is_directory(replace_dir_, ec)) {
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(replace_dir_, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;

    const auto ext = entry.path().extension().string();
    if (ext != ".dds" && ext != ".DDS") continue;

    std::string stem = entry.path().stem().string();
    if (stem.starts_with("0x") || stem.starts_with("0X")) {
      stem = stem.substr(2);
    }
    if (stem.size() != 16) continue;

    uint64_t hash = 0;
    bool ok = true;
    for (char c : stem) {
      uint64_t nibble = 0;
      if (c >= '0' && c <= '9') nibble = static_cast<uint64_t>(c - '0');
      else if (c >= 'a' && c <= 'f') nibble = static_cast<uint64_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') nibble = static_cast<uint64_t>(c - 'A' + 10);
      else { ok = false; break; }
      hash = (hash << 4) | nibble;
    }
    if (ok) {
      indexed_files_[hash] = entry.path();
    }
  }

  MC_INFO("[texture-replacement] Indexed {} replacement textures from {}",
          indexed_files_.size(), replace_dir_.string());
}

const TextureReplacementData* TextureReplacement::FindReplacement(uint64_t content_hash) {
  if (content_hash == 0) return nullptr;

  std::lock_guard<std::mutex> lock(mutex_);

  auto cached = pixel_cache_.find(content_hash);
  if (cached != pixel_cache_.end()) {
    return &cached->second;
  }

  if (failed_cache_.contains(content_hash)) {
    return nullptr;
  }

  auto file_it = indexed_files_.find(content_hash);
  if (file_it == indexed_files_.end()) {
    return nullptr;
  }

  TextureReplacementData data;
  if (!ReadDDS(file_it->second, data)) {
    MC_WARN("[texture-replacement] Failed to decode replacement: {}", file_it->second.string());
    failed_cache_[content_hash] = true;
    return nullptr;
  }

  auto [it, _] = pixel_cache_.emplace(content_hash, std::move(data));
  return &it->second;
}

bool TextureReplacement::ReadDDS(const std::filesystem::path& path, TextureReplacementData& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return false;

  DdsHeader hdr;
  if (!f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) return false;

  if (hdr.magic != kDdsMagic || hdr.size != 124) return false;
  if (hdr.width == 0 || hdr.height == 0) return false;

  out.width = hdr.width;
  out.height = hdr.height;

  bool compressed = false;
  DdsDecodeFormat decode_format = DdsDecodeFormat::kRGBA8;

  if (hdr.ddspf.flags & kDdsPfFourCC) {
    compressed = true;
    switch (hdr.ddspf.four_cc) {
      case kFourCC_DXT1:
        decode_format = DdsDecodeFormat::kBC1;
        break;
      case kFourCC_DXT2:
      case kFourCC_DXT3:
        decode_format = DdsDecodeFormat::kBC2;
        break;
      case kFourCC_DXT4:
      case kFourCC_DXT5:
        decode_format = DdsDecodeFormat::kBC3;
        break;
      case kFourCC_ATI1:
      case kFourCC_BC4U:
      case kFourCC_BC4S:
        decode_format = DdsDecodeFormat::kBC4;
        break;
      case kFourCC_ATI2:
      case kFourCC_BC5U:
      case kFourCC_BC5S:
        decode_format = DdsDecodeFormat::kBC5;
        break;
      case kFourCC_DX10: {
        DdsHeaderDX10 dx10;
        if (!f.read(reinterpret_cast<char*>(&dx10), sizeof(dx10))) return false;
        switch (dx10.dxgi_format) {
          case kDxgiFormatBC1UNorm:
          case kDxgiFormatBC1UNormSRGB:
            decode_format = DdsDecodeFormat::kBC1;
            break;
          case kDxgiFormatBC2UNorm:
          case kDxgiFormatBC2UNormSRGB:
            decode_format = DdsDecodeFormat::kBC2;
            break;
          case kDxgiFormatBC3UNorm:
          case kDxgiFormatBC3UNormSRGB:
            decode_format = DdsDecodeFormat::kBC3;
            break;
          case kDxgiFormatBC4UNorm:
          case kDxgiFormatBC4SNorm:
            decode_format = DdsDecodeFormat::kBC4;
            break;
          case kDxgiFormatBC5UNorm:
          case kDxgiFormatBC5SNorm:
            decode_format = DdsDecodeFormat::kBC5;
            break;
          case kDxgiFormatR8G8B8A8UNorm:
          case kDxgiFormatR8G8B8A8UNormSRGB:
            compressed = false;
            decode_format = DdsDecodeFormat::kRGBA8;
            break;
          case kDxgiFormatB8G8R8A8UNorm:
          case kDxgiFormatB8G8R8A8UNormSRGB:
            compressed = false;
            decode_format = DdsDecodeFormat::kBGRA8;
            break;
          case kDxgiFormatB8G8R8X8UNorm:
          case kDxgiFormatB8G8R8X8UNormSRGB:
            compressed = false;
            decode_format = DdsDecodeFormat::kBGRX8;
            break;
          default:
            MC_WARN("[texture-replacement] Unsupported DX10 format {} in {}", dx10.dxgi_format, path.string());
            return false;
        }
        break;
      }
      default:
        MC_WARN("[texture-replacement] Unsupported FourCC {:08X} in {}", hdr.ddspf.four_cc, path.string());
        return false;
    }
  } else if (hdr.ddspf.flags & kDdsPfRgb) {
    if (hdr.ddspf.rgb_bit_count != 32 || hdr.ddspf.g_bit_mask != 0x0000FF00u) {
      MC_WARN("[texture-replacement] Unsupported RGB DDS layout in {}", path.string());
      return false;
    }
    if (hdr.ddspf.r_bit_mask == 0x000000FFu && hdr.ddspf.b_bit_mask == 0x00FF0000u) {
      decode_format = DdsDecodeFormat::kRGBA8;
    } else if (hdr.ddspf.r_bit_mask == 0x00FF0000u && hdr.ddspf.b_bit_mask == 0x000000FFu) {
      decode_format = (hdr.ddspf.a_bit_mask == 0) ? DdsDecodeFormat::kBGRX8 : DdsDecodeFormat::kBGRA8;
    } else {
      MC_WARN("[texture-replacement] Unsupported channel masks in {}", path.string());
      return false;
    }
  } else {
    MC_WARN("[texture-replacement] Neither FourCC nor RGB in {}", path.string());
    return false;
  }

  // Exact authored mip count adherence:
  // If the author explicitly specified mip_map_count, strictly load that many levels.
  // 1 level = 0 extra mips. 3 levels = 3 mips. 4 levels = 4 mips. 12 levels = 12 mips.
  const uint32_t dds_mips =
      ((hdr.flags & kDdsdMipmapCount) && hdr.mip_map_count > 0) ? hdr.mip_map_count : 1;

  // Read all remaining bytes from the file
  const auto current_pos = f.tellg();
  f.seekg(0, std::ios::end);
  const size_t file_remaining = static_cast<size_t>(f.tellg() - current_pos);
  f.seekg(current_pos, std::ios::beg);

  std::vector<uint8_t> payload(file_remaining);
  if (!f.read(reinterpret_cast<char*>(payload.data()), file_remaining)) {
    return false;
  }

  out.mips.clear();
  size_t offset = 0;

  for (uint32_t i = 0; i < dds_mips; ++i) {
    const uint32_t level_w = std::max(1u, out.width >> i);
    const uint32_t level_h = std::max(1u, out.height >> i);

    ReplacementMipLevel mip;
    mip.width = level_w;
    mip.height = level_h;

    if (compressed) {
      const uint32_t bx = (level_w + 3) / 4;
      const uint32_t by = (level_h + 3) / 4;
      const uint32_t bytes_per_block =
          (decode_format == DdsDecodeFormat::kBC1 || decode_format == DdsDecodeFormat::kBC4) ? 8 : 16;
      const size_t level_bytes = static_cast<size_t>(bx) * by * bytes_per_block;
      if (offset + level_bytes > payload.size()) break;
      if (!DecodeBCToRGBA8(payload.data() + offset, level_bytes, level_w, level_h, decode_format, mip.pixels)) {
        break;
      }
      offset += level_bytes;
    } else {
      const uint32_t row_pitch = level_w * 4;
      const size_t level_bytes = static_cast<size_t>(row_pitch) * level_h;
      if (offset + level_bytes > payload.size()) break;
      if (!DecodeLinear32ToRGBA8(payload.data() + offset, level_bytes, level_w, level_h, row_pitch,
                                 decode_format, mip.pixels)) {
        break;
      }
      offset += level_bytes;
    }

    out.mips.push_back(std::move(mip));
  }

  if (out.mips.empty()) return false;

  // Strict author adherence: DO NOT synthesize extra box-filtered mips.
  out.mip_levels = static_cast<uint32_t>(out.mips.size());
  MC_DEBUG("[texture-replacement] Successfully decoded {} ({}x{}, {} mip level(s))",
           path.filename().string(), out.width, out.height, out.mip_levels);
  return true;
}

}  // namespace mc::modloader
