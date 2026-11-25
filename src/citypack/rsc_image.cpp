#include "citypack/rsc_image.h"

#include <algorithm>
#include <cstring>

#include "citypack/bytes.h"

namespace larecomp::citypack {

namespace {
constexpr uint32_t kRsc5Magic = 0x05435352;      // 'RSC' 5, big endian
constexpr uint32_t kXCompressMagic = 0x0FF512EF;  // after the header of a compressed resource
}  // namespace

bool DecodeResourceFile(const std::vector<uint8_t>& file, const LzxFunction& lzx, RscImage* out, std::string* error) {
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (file.size() < 12) return fail("a file too small for an RSC5 header");
  if (LoadBE32(file.data()) != kRsc5Magic) return fail("not an RSC5 resource");
  RscImage image;
  image.type = LoadBE32(file.data() + 4);
  image.flag = LoadBE32(file.data() + 8);
  image.virtual_size = VirtualSizeOf(image.flag);
  image.physical_size = PhysicalSizeOf(image.flag);
  const uint64_t total = uint64_t(image.virtual_size) + image.physical_size;
  if (image.virtual_size == 0) return fail("a resource without a virtual segment");
  if (total > (1ull << 30)) return fail("a resource of more than a gigabyte");

  if ((image.flag & 0x40000000u) != 0) {
    // compressed: marker, stream length, XCompress chunks of LZX
    if (file.size() < 20) return fail("a compressed resource cut before its stream");
    if (LoadBE32(file.data() + 12) != kXCompressMagic) return fail("a compressed resource without the XCompress marker");
    size_t payload = LoadBE32(file.data() + 16);
    if (20 + payload > file.size()) payload = file.size() - 20;
    if (!lzx) return fail("a compressed resource and no LZX decoder");
    if (!lzx(file.data() + 20, payload, image.data, size_t(total)) || image.data.size() != total) return fail("the LZX stream did not decompress to the size the resource declares");
  } else {
    // stored: the payload follows the 12 byte header
    if (file.size() < 12 + total) return fail("a stored resource shorter than its segments");
    image.data.assign(file.begin() + 12, file.begin() + 12 + std::ptrdiff_t(total));
  }
  *out = std::move(image);
  return true;
}

bool RscCursor::IsPointer(uint32_t value) const {
  const uint32_t space = value >> 28;
  const uint32_t offset = value & 0x0FFFFFFFu;
  if (space == 5) return offset < image_.virtual_size;
  if (space == 6) return offset < image_.physical_size;
  return false;
}

bool RscCursor::Offset(uint32_t pointer, uint64_t length, size_t* offset) {
  const uint32_t space = pointer >> 28;
  uint64_t at = pointer & 0x0FFFFFFFu;
  if (space == 5) {
    at += 0;
  } else if (space == 6) {
    at += image_.virtual_size;
  } else {
    bad_ = true;
    return false;
  }
  if (at > image_.data.size() || length > image_.data.size() - at) {
    bad_ = true;
    return false;
  }
  *offset = size_t(at);
  return true;
}

uint32_t RscCursor::U32(uint32_t pointer) {
  size_t at = 0;
  return Offset(pointer, 4, &at) ? LoadBE32(image_.data.data() + at) : 0;
}

uint16_t RscCursor::U16(uint32_t pointer) {
  size_t at = 0;
  return Offset(pointer, 2, &at) ? LoadBE16(image_.data.data() + at) : uint16_t(0);
}

uint8_t RscCursor::U8(uint32_t pointer) {
  size_t at = 0;
  return Offset(pointer, 1, &at) ? image_.data[at] : uint8_t(0);
}

float RscCursor::F32(uint32_t pointer) {
  size_t at = 0;
  return Offset(pointer, 4, &at) ? LoadBEFloat(image_.data.data() + at) : 0.0f;
}

const uint8_t* RscCursor::Span(uint32_t pointer, uint64_t length) {
  size_t at = 0;
  return Offset(pointer, length, &at) ? image_.data.data() + at : nullptr;
}

std::string RscCursor::CString(uint32_t pointer, size_t limit) {
  size_t at = 0;
  if (!Offset(pointer, 1, &at)) return {};
  const size_t room = std::min(limit, image_.data.size() - at);
  const auto* begin = reinterpret_cast<const char*>(image_.data.data() + at);
  size_t length = 0;
  while (length < room && begin[length] != '\0') ++length;
  return std::string(begin, length);
}

}  // namespace larecomp::citypack
