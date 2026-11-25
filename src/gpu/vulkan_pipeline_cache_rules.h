#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace mcla::gpu::vulkan_pipeline_cache {

constexpr size_t kHeaderSize = 32;
constexpr size_t kMaxBytes = 128 * 1024 * 1024;

struct Identity {
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::array<uint8_t, 16> uuid{};
  bool operator==(const Identity&) const = default;
};

// VkPipelineCacheHeaderVersionOne is 32 bytes, with its integers in little-endian order.
inline std::optional<Identity> ReadIdentity(std::span<const uint8_t> header) {
  if (header.size() < kHeaderSize) {
    return std::nullopt;
  }
  const auto read_u32 = [header](size_t offset) {
    return uint32_t(header[offset]) | (uint32_t(header[offset + 1]) << 8) |
           (uint32_t(header[offset + 2]) << 16) | (uint32_t(header[offset + 3]) << 24);
  };
  if (read_u32(0) != kHeaderSize || read_u32(4) != 1) {
    return std::nullopt;
  }
  Identity identity{read_u32(8), read_u32(12)};
  std::copy_n(header.begin() + 16, identity.uuid.size(), identity.uuid.begin());
  return identity;
}

inline std::string FileName(const Identity& identity) {
  std::string name = std::format("vulkan_pso_{:08x}_{:08x}_", identity.vendor_id, identity.device_id);
  for (const uint8_t byte : identity.uuid) {
    name += std::format("{:02x}", byte);
  }
  return name + ".cache";
}

constexpr bool ValidSize(uint64_t size) {
  return size >= kHeaderSize && size <= kMaxBytes;
}

inline bool ValidBlob(std::span<const uint8_t> data, const Identity& identity) {
  return ValidSize(data.size()) && ReadIdentity(data) == identity;
}

constexpr bool ShouldSave(bool lost, uint64_t created, uint64_t last_saved) {
  return !lost && created > last_saved;
}

constexpr bool Warm(uint64_t loaded_bytes) {
  return loaded_bytes > kHeaderSize;
}

constexpr bool CanUsePrebuildStamp(bool stamp_matches, bool persistent_cache, bool cache_loaded) {
  return stamp_matches && (!persistent_cache || cache_loaded);
}

}  // namespace mcla::gpu::vulkan_pipeline_cache
