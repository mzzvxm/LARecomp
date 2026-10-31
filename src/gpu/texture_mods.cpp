// ===========================================================================
// MCLA GPU renderer -- texture mods (texture_mods.h)
// ===========================================================================

#include "texture_mods.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "mc_engine/modloader/modloader.h"

namespace mcla::gpu {

namespace {

// The replacements' folder, in the mod folder itself and in each mod.
constexpr const char* kReplaceFolder = "textures_replace";

constexpr uint32_t kDdsMagic = 0x20534444u;  // "DDS "
constexpr uint32_t kDdsdMipMapCount = 0x00020000u;
constexpr uint32_t kDdpfFourCC = 0x00000004u;
constexpr uint32_t kDdpfRgb = 0x00000040u;
constexpr uint32_t kDdsCaps2Cubemap = 0x00000200u;
constexpr uint32_t kDdsCaps2Volume = 0x00200000u;

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
         uint32_t(uint8_t(d)) << 24;
}

// DXGI_FORMAT values the DX10 header may carry.
enum : uint32_t {
  kDxgiR8G8B8A8Unorm = 28,
  kDxgiR8G8B8A8UnormSrgb = 29,
  kDxgiBC1Unorm = 71,
  kDxgiBC1UnormSrgb = 72,
  kDxgiBC2Unorm = 74,
  kDxgiBC2UnormSrgb = 75,
  kDxgiBC3Unorm = 77,
  kDxgiBC3UnormSrgb = 78,
  kDxgiBC4Unorm = 80,
  kDxgiBC5Unorm = 83,
  kDxgiB8G8R8A8Unorm = 87,
  kDxgiB8G8R8X8Unorm = 88,
  kDxgiB8G8R8A8UnormSrgb = 91,
  kDxgiB8G8R8X8UnormSrgb = 93,
};
constexpr uint32_t kDx10DimensionTexture2D = 3;
constexpr uint32_t kDx10MiscTextureCube = 0x4;

#pragma pack(push, 1)
struct DdsPixelFormat {
  uint32_t size, flags, four_cc, rgb_bit_count, r_mask, g_mask, b_mask, a_mask;
};
struct DdsHeader {
  uint32_t magic, size, flags, height, width, pitch_or_linear, depth, mip_count;
  uint32_t reserved1[11];
  DdsPixelFormat pf;
  uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
  uint32_t dxgi_format, dimension, misc_flag, array_size, misc_flags2;
};
#pragma pack(pop)
static_assert(sizeof(DdsHeader) == 128);
static_assert(sizeof(DdsHeaderDx10) == 20);

// How the payload is laid out and what it uploads as.
enum class Payload { kBlocks, kRgba, kBgra, kBgrx };

bool IsHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool ParseHex16(const char* p, uint64_t* out) {
  uint64_t v = 0;
  for (int i = 0; i < 16; ++i) {
    const char c = p[i];
    if (!IsHex(c)) {
      return false;
    }
    v = v << 4 | uint64_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
  }
  *out = v;
  return true;
}

}  // namespace

bool TextureMods::HashFromFileName(const std::string& stem, uint64_t* hash) {
  const size_t n = stem.size();
  // "<anything>__<hash16>"
  if (n > 18 && stem.compare(n - 18, 2, "__") == 0 && ParseHex16(stem.data() + n - 16, hash)) {
    return true;
  }
  // "<hash16>" or "0x<hash16>"
  if (n == 16 && ParseHex16(stem.data(), hash)) {
    return true;
  }
  if (n == 18 && (stem[0] == '0') && (stem[1] == 'x' || stem[1] == 'X') &&
      ParseHex16(stem.data() + 2, hash)) {
    return true;
  }
  // "<hash16>_<anything>" (the SDK's dump names)
  return n > 17 && stem[16] == '_' && ParseHex16(stem.data(), hash);
}

void TextureMods::Initialize() {
  files_.clear();
  const std::filesystem::path mod_root =
      mc::modloader::ModRootDir(rex::filesystem::GetExecutableFolder());
  std::error_code ec;
  if (!std::filesystem::is_directory(mod_root, ec)) {
    REXLOG_INFO("[gpu] texture mods: no {} folder; no replacements", mod_root.string());
    return;
  }
  // The loose folder first, so it wins over every mod; then the mods in name
  // order.
  std::vector<std::filesystem::path> folders;
  if (std::filesystem::is_directory(mod_root / kReplaceFolder, ec)) {
    folders.push_back(mod_root / kReplaceFolder);
  }
  std::vector<std::filesystem::path> mods;
  for (const auto& entry : std::filesystem::directory_iterator(mod_root, ec)) {
    if (entry.is_directory(ec) &&
        !mc::modloader::IsReservedModFolder(entry.path().filename().string()) &&
        std::filesystem::is_directory(entry.path() / kReplaceFolder, ec)) {
      mods.push_back(entry.path() / kReplaceFolder);
    }
  }
  std::sort(mods.begin(), mods.end());
  folders.insert(folders.end(), mods.begin(), mods.end());
  size_t ignored = 0, shadowed = 0;
  for (const std::filesystem::path& root : folders) {
    size_t added = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) {
        continue;
      }
      std::string ext = it->path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(c | 0x20); });
      if (ext != ".dds") {
        continue;
      }
      uint64_t hash = 0;
      if (!HashFromFileName(it->path().stem().string(), &hash)) {
        ++ignored;
        REXLOG_WARN("[gpu] texture mods: {} carries no texture hash in its name (expected "
                    "<name>__<16 hex>.dds); ignored",
                    it->path().string());
        continue;
      }
      const auto [found, inserted] = files_.emplace(hash, it->path());
      if (!inserted) {
        ++shadowed;
        REXLOG_WARN("[gpu] texture mods: {:016x} in {} is also in {}, which wins (the loose "
                    "folder, then mod folders in name order)",
                    hash, it->path().string(), found->second.string());
        continue;
      }
      ++added;
    }
    REXLOG_INFO("[gpu] texture mods: {} replacement(s) from {}", added, root.string());
  }
  REXLOG_INFO("[gpu] texture mods: {} replacement(s) from {} folder(s) ({} names without a hash, "
              "{} shadowed)",
              files_.size(), folders.size(), ignored, shadowed);
}

const std::filesystem::path* TextureMods::Find(uint64_t hash) const {
  const auto it = files_.find(hash);
  return it != files_.end() ? &it->second : nullptr;
}

bool TextureMods::LoadDds(const std::filesystem::path& path, ReplacementImage* out,
                          std::string* why) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    *why = "cannot be opened";
    return false;
  }
  const std::streamoff file_size = file.tellg();
  file.seekg(0);
  DdsHeader h{};
  if (file_size < std::streamoff(sizeof(h)) || !file.read(reinterpret_cast<char*>(&h), sizeof(h)) ||
      h.magic != kDdsMagic || h.size != 124) {
    *why = "not a DDS file";
    return false;
  }
  if (h.width == 0 || h.height == 0 || h.width > 16384 || h.height > 16384) {
    *why = std::format("size {}x{}", h.width, h.height);
    return false;
  }
  if (h.caps2 & (kDdsCaps2Cubemap | kDdsCaps2Volume)) {
    *why = "a cube map or volume (only 2D textures are replaced)";
    return false;
  }
  Payload payload = Payload::kBlocks;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t block_bytes = 0;
  if (h.pf.flags & kDdpfFourCC) {
    switch (h.pf.four_cc) {
      case FourCC('D', 'X', 'T', '1'): format = plume::RenderFormat::BC1_UNORM; break;
      case FourCC('D', 'X', 'T', '2'):
      case FourCC('D', 'X', 'T', '3'): format = plume::RenderFormat::BC2_UNORM; break;
      case FourCC('D', 'X', 'T', '4'):
      case FourCC('D', 'X', 'T', '5'): format = plume::RenderFormat::BC3_UNORM; break;
      case FourCC('A', 'T', 'I', '1'):
      case FourCC('B', 'C', '4', 'U'): format = plume::RenderFormat::BC4_UNORM; break;
      case FourCC('A', 'T', 'I', '2'):
      case FourCC('B', 'C', '5', 'U'): format = plume::RenderFormat::BC5_UNORM; break;
      case FourCC('D', 'X', '1', '0'): {
        DdsHeaderDx10 x{};
        if (!file.read(reinterpret_cast<char*>(&x), sizeof(x))) {
          *why = "truncated DX10 header";
          return false;
        }
        if (x.dimension != kDx10DimensionTexture2D || (x.misc_flag & kDx10MiscTextureCube) ||
            x.array_size > 1) {
          *why = "a DX10 header that is not a single 2D texture";
          return false;
        }
        switch (x.dxgi_format) {
          case kDxgiBC1Unorm: case kDxgiBC1UnormSrgb: format = plume::RenderFormat::BC1_UNORM; break;
          case kDxgiBC2Unorm: case kDxgiBC2UnormSrgb: format = plume::RenderFormat::BC2_UNORM; break;
          case kDxgiBC3Unorm: case kDxgiBC3UnormSrgb: format = plume::RenderFormat::BC3_UNORM; break;
          case kDxgiBC4Unorm: format = plume::RenderFormat::BC4_UNORM; break;
          case kDxgiBC5Unorm: format = plume::RenderFormat::BC5_UNORM; break;
          case kDxgiR8G8B8A8Unorm: case kDxgiR8G8B8A8UnormSrgb: payload = Payload::kRgba; break;
          case kDxgiB8G8R8A8Unorm: case kDxgiB8G8R8A8UnormSrgb: payload = Payload::kBgra; break;
          case kDxgiB8G8R8X8Unorm: case kDxgiB8G8R8X8UnormSrgb: payload = Payload::kBgrx; break;
          default:
            *why = std::format("DXGI format {} (BC1-BC5 or 8-bit RGBA/BGRA only)", x.dxgi_format);
            return false;
        }
        break;
      }
      default:
        *why = std::format("FourCC {:08x} (DXT1/3/5, ATI1/2, BC4U/BC5U or DX10 only)",
                           h.pf.four_cc);
        return false;
    }
  } else if ((h.pf.flags & kDdpfRgb) && h.pf.rgb_bit_count == 32 && h.pf.g_mask == 0x0000FF00u) {
    if (h.pf.r_mask == 0x000000FFu && h.pf.b_mask == 0x00FF0000u) {
      payload = Payload::kRgba;
    } else if (h.pf.r_mask == 0x00FF0000u && h.pf.b_mask == 0x000000FFu) {
      payload = h.pf.a_mask ? Payload::kBgra : Payload::kBgrx;
    } else {
      *why = "32-bit RGB with masks other than RGBA/BGRA";
      return false;
    }
  } else {
    *why = "a pixel format other than BC1-BC5 or 32-bit RGBA/BGRA";
    return false;
  }
  if (payload == Payload::kBlocks) {
    block_bytes = (format == plume::RenderFormat::BC1_UNORM ||
                   format == plume::RenderFormat::BC4_UNORM) ? 8u : 16u;
    if ((h.width & 3) || (h.height & 3)) {
      *why = std::format("block-compressed {}x{}, not a multiple of 4", h.width, h.height);
      return false;
    }
  } else {
    format = plume::RenderFormat::R8G8B8A8_UNORM;
  }

  const uint32_t declared = (h.flags & kDdsdMipMapCount) && h.mip_count ? h.mip_count : 1u;
  out->format = format;
  out->width = h.width;
  out->height = h.height;
  out->levels.clear();
  for (uint32_t level = 0; level < declared && level < 16; ++level) {
    ReplacementImage::Level l;
    l.width = std::max(h.width >> level, 1u);
    l.height = std::max(h.height >> level, 1u);
    if (payload == Payload::kBlocks) {
      l.row_bytes = ((l.width + 3) / 4) * block_bytes;
      l.rows = (l.height + 3) / 4;
    } else {
      l.row_bytes = l.width * 4;
      l.rows = l.height;
    }
    l.data.resize(size_t(l.row_bytes) * l.rows);
    if (!file.read(reinterpret_cast<char*>(l.data.data()), std::streamsize(l.data.size()))) {
      break;  // a shorter chain than declared: the levels that are there
    }
    if (payload == Payload::kBgra || payload == Payload::kBgrx) {
      for (size_t i = 0; i + 3 < l.data.size(); i += 4) {
        std::swap(l.data[i], l.data[i + 2]);
        if (payload == Payload::kBgrx) {
          l.data[i + 3] = 0xFF;
        }
      }
    }
    out->levels.push_back(std::move(l));
    if (out->levels.back().width == 1 && out->levels.back().height == 1) {
      break;
    }
  }
  if (out->levels.empty()) {
    *why = "no complete level in the file";
    return false;
  }
  return true;
}

}  // namespace mcla::gpu
