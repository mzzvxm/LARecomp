#include "shaderpack/image_scan.h"

#include <cstdint>
#include <fstream>
#include <system_error>
#include <utility>

#include "XenosRecomp/container_scan.h"
#include "shaderpack/log.h"

namespace larecomp::shaderpack {

// The container walk is the generator's own (LAR-XenosRecomp's forEachShaderContainer in
// container_scan.h), so the scan here and the command line tool find the same containers.
ImageScan ScanImage(const uint8_t* image, size_t size, uint32_t base_address) {
  ImageScan scan;
  if (!image || size == 0) return scan;
  scan.scanned_bytes = size;

  forEachShaderContainer(image, size, [&](size_t offset, size_t data_size) {
    ImageContainer container;
    container.offset = offset;
    container.guest_address = base_address + static_cast<uint32_t>(offset);
    container.bytes.assign(image + offset, image + offset + data_size);
    scan.containers.push_back(std::move(container));
  });

  size_t total = 0;
  for (const ImageContainer& container : scan.containers) total += container.bytes.size();
  SHADERPACK_LOG_INFO("image scan: {} shader containers ({} bytes) in {} bytes from 0x{:08X}",
                      scan.containers.size(), total, size, base_address);
  if (scan.containers.size() != kExpectedImageContainers) {
    SHADERPACK_LOG_WARN("the image holds {} shader containers, the retail game has {}; "
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
