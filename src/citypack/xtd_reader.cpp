#include "citypack/xtd_reader.h"

#include <algorithm>
#include <string_view>

#include "citypack/bytes.h"
#include "citypack/material_class.h"

namespace larecomp::citypack {

namespace {

constexpr uint32_t kRoot = 0x50000000;
constexpr uint32_t kMaxTextures = 100000;
constexpr uint32_t kFetchConstantOffset = 0x1C;  // inside the D3DBaseTexture

bool IsDds(std::string_view name) {
  if (name.size() < 4) return false;
  const std::string_view tail = name.substr(name.size() - 4);
  return (tail[0] == '.') && (tail[1] | 0x20) == 'd' && (tail[2] | 0x20) == 'd' && (tail[3] | 0x20) == 's';
}

}  // namespace

uint32_t TextureNameHash(std::string_view name) {
  if (IsDds(name)) return RageHashText(name);
  std::string with(name);
  with += ".dds";
  return RageHashText(with);
}

void DictionaryStats::Merge(const DictionaryStats& o) {
  entries += o.entries;
  textures += o.textures;
  bad_pointer += o.bad_pointer;
  not_supported += o.not_supported;
  unsupported_format += o.unsupported_format;
  no_base_level += o.no_base_level;
  size_mismatch += o.size_mismatch;
  not_tiled += o.not_tiled;
  odd_swizzle += o.odd_swizzle;
  odd_endian += o.odd_endian;
  for (uint32_t f : o.unsupported_formats) {
    if (std::find(unsupported_formats.begin(), unsupported_formats.end(), f) == unsupported_formats.end()) unsupported_formats.push_back(f);
  }
}

const uint8_t* ResourceBytesAt(const RscImage& image, uint32_t pointer, size_t* size) {
  uint64_t at = pointer & 0x0FFFFFFFu;
  const uint32_t space = pointer >> 28;
  if (space == 5) {
    // the virtual segment starts the buffer
  } else if (space == 6) {
    at += image.virtual_size;
  } else {
    *size = 0;
    return nullptr;
  }
  if (at >= image.data.size()) {
    *size = 0;
    return nullptr;
  }
  *size = image.data.size() - size_t(at);
  return image.data.data() + at;
}

bool ParseDictionaryAt(const RscImage& image, uint32_t dictionary, std::vector<DictionaryTexture>* out, DictionaryStats* stats, std::string* error) {
  out->clear();
  RscCursor c(image);
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (!c.IsPointer(dictionary)) return fail("no dictionary where it should be");
  const uint32_t hashes = c.U32(dictionary + 16);
  const uint32_t count = c.U16(dictionary + 20);
  const uint32_t array = c.U32(dictionary + 24);
  if (c.bad() || !c.IsPointer(hashes) || !c.IsPointer(array) || count == 0 || count > kMaxTextures) return fail("the dictionary has no hashes or no texture array, or a count out of range");
  c.ClearBad();
  DictionaryStats local;
  local.entries = count;
  out->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    DictionaryTexture t;
    t.name_hash = c.U32(hashes + 4 * i);
    t.address = c.U32(array + 4 * i);
    if (c.bad() || !c.IsPointer(t.address)) {
      c.ClearBad();
      ++local.bad_pointer;
      continue;
    }
    const uint32_t d3d = c.U32(t.address + 28);
    t.grc_width = c.U16(t.address + 32);
    t.grc_height = c.U16(t.address + 34);
    uint32_t dwords[6];
    if (c.bad() || !c.IsPointer(d3d)) {
      c.ClearBad();
      ++local.bad_pointer;
      continue;
    }
    for (uint32_t k = 0; k < 6; ++k) dwords[k] = c.U32(d3d + kFetchConstantOffset + 4 * k);
    if (c.bad()) {
      c.ClearBad();
      ++local.bad_pointer;
      continue;
    }
    std::string why;
    if (!xenos::DescribeTexture(dwords, &t.desc, &why)) {
      ++local.not_supported;
      continue;
    }
    if (!xenos::GetFormatInfo(t.desc.format).supported()) {
      ++local.unsupported_format;
      if (std::find(local.unsupported_formats.begin(), local.unsupported_formats.end(), t.desc.format) == local.unsupported_formats.end()) local.unsupported_formats.push_back(t.desc.format);
      continue;
    }
    if (!t.desc.has_base()) {
      ++local.no_base_level;
      continue;
    }
    if ((t.grc_width != 0 || t.grc_height != 0) && (t.grc_width != t.desc.width || t.grc_height != t.desc.height)) ++local.size_mismatch;
    if (!t.desc.tiled) ++local.not_tiled;
    if (t.desc.swizzle != xenos::kIdentitySwizzle) ++local.odd_swizzle;
    if (t.desc.endian != xenos::kEndian8in16) ++local.odd_endian;
    out->push_back(t);
    ++local.textures;
  }
  if (stats) stats->Merge(local);
  return true;
}

bool ParseDictionaryFile(const RscImage& image, std::vector<DictionaryTexture>* out, DictionaryStats* stats, std::string* error) {
  return ParseDictionaryAt(image, kRoot, out, stats, error);
}

bool ParseSectorDictionary(const RscImage& image, std::vector<DictionaryTexture>* out, DictionaryStats* stats, std::string* error) {
  RscCursor c(image);
  const uint32_t dictionary = c.U32(kRoot + 8);
  if (c.bad()) {
    if (error) *error = "the resource is too small for a sector's root";
    return false;
  }
  return ParseDictionaryAt(image, dictionary, out, stats, error);
}

}  // namespace larecomp::citypack
