// The shader containers inside default.xex, found in the guest image.
//
// The 119 containers the executable carries (post-processing, tone map, pass
// through vertex shaders...) sit in its data, which the guest has already
// loaded at 0x82000000 by the time LARecomp builds its shader packs. So they
// are scanned straight out of guest memory: no XEX decryption, no AES, and it
// works for whatever default.xex the runtime could boot.
//
// The scan is the generator's own container walk (LAR-XenosRecomp's
// forEachShaderContainer), applied to one flat byte range, which finds the same
// 119 containers as carving the image (checked on this PC's image).
//
// Typical use inside larecomp.exe, once the runtime has built the guest memory:
//   const uint8_t* membase = rex::Runtime::instance()->virtual_membase();
//   ImageScan scan = ScanImage(membase + kGuestImageBase, kGuestImageSize);
// and in a tool, from a flat dump of the image (native\work\image.bin):
//   std::vector<uint8_t> image; LoadFlatImage(path, image);
//   ImageScan scan = ScanImage(image.data(), image.size());
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace larecomp::shaderpack {

// Where default.xex lands in guest memory, and how far the loaded image goes
// (the same bounds larecomp_app.h uses for its own scans of the image).
constexpr uint32_t kGuestImageBase = 0x82000000u;
constexpr uint32_t kGuestImageEnd = 0x829E0000u;
constexpr size_t kGuestImageSize = static_cast<size_t>(kGuestImageEnd - kGuestImageBase);

// The retail Complete Edition's executable has 119. A different count is
// logged as a warning, never an error: other regions or a title update may
// carry another number, and nothing downstream depends on it.
constexpr size_t kExpectedImageContainers = 119;

struct ImageContainer {
  size_t offset = 0;            // from the start of the scanned range
  uint32_t guest_address = 0;   // base_address + offset: the name the reference carving gives (827D2E1E.xsc)
  std::vector<uint8_t> bytes;   // the container as found: virtual size + physical size
};

struct ImageScan {
  std::vector<ImageContainer> containers;  // ascending offset, which is the walk's own order
  size_t scanned_bytes = 0;
};

// Walks `size` bytes at `image` (a flat copy of the guest image, or the guest
// memory itself) and copies out every shader container found. `base_address`
// is the guest address of image[0]; it only labels the results. The containers
// are copied, so the result stays valid when the range is not. Identical
// containers found at two addresses are both returned: telling them apart is
// the builder's job (it hashes them).
ImageScan ScanImage(const uint8_t* image, size_t size, uint32_t base_address = kGuestImageBase);

// Reads a flat image file whole (for tools and tests).
bool LoadFlatImage(const std::filesystem::path& file, std::vector<uint8_t>& out);

}  // namespace larecomp::shaderpack
