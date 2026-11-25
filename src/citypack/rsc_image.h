#pragma once
// ===========================================================================
// LARecomp city pack -- a game resource in memory, and bounds checked reads of its pointers
// ===========================================================================
// A resource of the game (.xcs sector, city.xct, lod.xmd) is the RSC5 container (src/mc_engine/modloader/rsc5.h) around one buffer, [virtual
// segment][physical segment], whose pointers are absolute guest addresses: 0x5xxxxxxx is an offset into the virtual segment, 0x6xxxxxxx one into the
// physical segment (which starts after the virtual one in the buffer). The data is big endian, as the console.
//
//   +0 'RSC5' (big endian 0x05435352), +4 type, +8 flag (segment sizes, bit 30 LZX compressed, bit 31 resource), then
//   compressed: +12 0x0FF512EF, +16 the stream's length, +20 the XCompress chunks;  not compressed: +12 the payload
//
// DecodeResourceFile takes the container and the LZX function as a parameter (the mod loader's LzxDecompress in the game's build and the tool,
// a stand-in in the tests), so this library does not link libmspack.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace larecomp::citypack {

struct RscImage {
  std::vector<uint8_t> data;  // [virtual][physical]
  uint32_t virtual_size = 0;
  uint32_t physical_size = 0;
  uint32_t type = 0;
  uint32_t flag = 0;
};

// Decompresses `src_len` bytes of XCompress chunks into exactly `dst_len` bytes (mc::modloader::LzxDecompress).
using LzxFunction = std::function<bool(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst, size_t dst_len)>;

// The segment sizes the flag word encodes: mantissa << (shift + 8).
inline uint32_t VirtualSizeOf(uint32_t flag) { return (flag & 0x7FFu) << (((flag >> 11) & 0xFu) + 8); }
inline uint32_t PhysicalSizeOf(uint32_t flag) { return ((flag >> 15) & 0x7FFu) << (((flag >> 26) & 0xFu) + 8); }

// The file's bytes (as Rpf3Reader::ReadFile gives them) to a resource image. False (with `error`) for a file that is not an RSC5 resource or whose
// stream does not decompress to the size its flag declares.
bool DecodeResourceFile(const std::vector<uint8_t>& file, const LzxFunction& lzx, RscImage* out, std::string* error);

// Reads through the image's pointers. A read outside the image returns 0 and sets bad(); a parser reads a whole structure and asks once.
class RscCursor {
 public:
  explicit RscCursor(const RscImage& image) : image_(image) {}
  // True when `value` is a pointer into the image (0x5... within the virtual segment, 0x6... within the physical one), not a null or a fill word.
  bool IsPointer(uint32_t value) const;
  uint32_t U32(uint32_t pointer);
  uint16_t U16(uint32_t pointer);
  uint8_t U8(uint32_t pointer);
  float F32(uint32_t pointer);
  // `length` bytes at the pointer, or null when they are not all in the image (bad() is set).
  const uint8_t* Span(uint32_t pointer, uint64_t length);
  // A zero terminated string of at most `limit` characters (the rest is cut), empty with bad() set when the pointer is not in the image.
  std::string CString(uint32_t pointer, size_t limit = 96);
  bool bad() const { return bad_; }
  void ClearBad() { bad_ = false; }

 private:
  bool Offset(uint32_t pointer, uint64_t length, size_t* offset);
  const RscImage& image_;
  bool bad_ = false;
};

}  // namespace larecomp::citypack
