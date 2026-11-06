#include "shaderpack/image_scan.h"

#include <cstdint>
#include <fstream>
#include <system_error>
#include <utility>

#include "larecomp_log.h"

namespace larecomp::shaderpack {

namespace {

// ---------------------------------------------------------------------------
// The generator's container walk.
//
// A port of forEachShaderContainer from LAR-XenosRecomp's main.cpp (6177a03),
// kept line for line so both find the same containers: the same signature test,
// the same one byte step, the same skip over a container that was found, the
// same 32 bit wrap of virtualSize + physicalSize.
//
// SWITCH TO THE SHARED HEADER: agent W1 is moving the original into
// container_scan.h in the LAR-XenosRecomp clone (plan 3.2 item 3). When that
// lands in this tree, delete this block, include "container_scan.h" and call
// its forEachShaderContainer(data, size, callback) with the same callback
// (offset, dataSize). Nothing else in this file depends on the block.
// ---------------------------------------------------------------------------

// sizeof(ShaderContainer) in shader.h: nine big endian dwords.
constexpr size_t kContainerHeaderSize = 36;

uint32_t LoadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

template <typename F>
void ForEachShaderContainer(const uint8_t* data, size_t size, F&& callback) {
  for (size_t i = 0; size > kContainerHeaderSize && i < size - kContainerHeaderSize - 1;) {
    const uint8_t* header = data + i;

    // flags & 0xFFFFFF00 == 0x102A1100, tested on the bytes first because
    // nearly every position fails here.
    bool hit = header[0] == 0x10 && header[1] == 0x2A && header[2] == 0x11;
    size_t data_size = 0;
    if (hit) {
      const uint32_t virtual_size = LoadBe32(header + 4);
      const uint32_t physical_size = LoadBe32(header + 8);
      const uint32_t constant_table_offset = LoadBe32(header + 16);
      const uint32_t shader_offset = LoadBe32(header + 24);
      const uint32_t field_1c = LoadBe32(header + 28);
      const uint32_t field_20 = LoadBe32(header + 32);

      // Both sizes are 32 bit values and the generator adds them as such.
      data_size = static_cast<uint32_t>(virtual_size + physical_size);
      hit = data_size != 0 && data_size <= (size - i) && constant_table_offset != 0 &&
            constant_table_offset < data_size && shader_offset < data_size && field_1c == 0 &&
            field_20 == 0;
    }

    if (hit) {
      callback(i, data_size);
      i += data_size;
    } else {
      i += 1;
    }
  }
}

}  // namespace

ImageScan ScanImage(const uint8_t* image, size_t size, uint32_t base_address) {
  ImageScan scan;
  if (!image || size == 0) return scan;
  scan.scanned_bytes = size;

  ForEachShaderContainer(image, size, [&](size_t offset, size_t data_size) {
    ImageContainer container;
    container.offset = offset;
    container.guest_address = base_address + static_cast<uint32_t>(offset);
    container.bytes.assign(image + offset, image + offset + data_size);
    scan.containers.push_back(std::move(container));
  });

  size_t total = 0;
  for (const ImageContainer& container : scan.containers) total += container.bytes.size();
  LARECOMP_APP_INFO("[shaderpack] image scan: {} shader containers ({} bytes) in {} bytes from 0x{:08X}",
                    scan.containers.size(), total, size, base_address);
  if (scan.containers.size() != kExpectedImageContainers) {
    LARECOMP_APP_WARN("[shaderpack] the image holds {} shader containers, the retail game has {}; "
                      "carrying on, the shader contract does not depend on the count",
                      scan.containers.size(), kExpectedImageContainers);
  }
  return scan;
}

bool LoadFlatImage(const std::filesystem::path& file, std::vector<uint8_t>& out) {
  out.clear();
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(file, ec);
  if (ec || size == 0 || size > (std::uintmax_t{1} << 31)) return false;

  std::ifstream input(file, std::ios::binary);
  if (!input) return false;
  out.resize(static_cast<size_t>(size));
  input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (static_cast<size_t>(input.gcount()) != out.size()) {
    out.clear();
    return false;
  }
  return true;
}

}  // namespace larecomp::shaderpack
