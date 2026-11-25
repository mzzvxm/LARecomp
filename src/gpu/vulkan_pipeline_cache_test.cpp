#include "vulkan_pipeline_cache_rules.h"

#include <cstdio>

namespace rules = mcla::gpu::vulkan_pipeline_cache;

namespace {

constexpr std::array<uint8_t, 35> kBlob{
    32, 0, 0, 0, 1, 0, 0, 0,
    0xde, 0x10, 0, 0, 0x78, 0x56, 0x34, 0x12,
    0x00, 0x04, 0x30, 0x56, 0x78, 0x9a, 0xbc, 0xde,
    0xf0, 0xc2, 0xb4, 0xa6, 0x98, 0x8a, 0x7c, 0x6e,
    0x21, 0x44, 0x05,
};
constexpr rules::Identity kIdentity{
    0x10de, 0x12345678,
    {0x00, 0x04, 0x30, 0x56, 0x78, 0x9a, 0xbc, 0xde,
     0xf0, 0xc2, 0xb4, 0xa6, 0x98, 0x8a, 0x7c, 0x6e},
};

struct Checks {
  int total = 0;
  int failed = 0;

  void Run(bool pass, const char* label) {
    ++total;
    if (!pass) {
      ++failed;
      std::fprintf(stderr, "FAIL: %s\n", label);
    }
  }
};

void CheckBlob(Checks& checks) {
  const std::span bytes(kBlob);
  checks.Run(rules::ReadIdentity(bytes) == kIdentity, "decode both IDs and every UUID byte");
  checks.Run(rules::ValidBlob(bytes, kIdentity), "accept a matching cache blob");
  checks.Run(rules::ValidBlob(bytes.first(32), kIdentity), "accept a header-only driver cache");
  for (size_t size = 0; size < 32; ++size) {
    checks.Run(!rules::ValidBlob(bytes.first(size), kIdentity), "reject a truncated header");
  }
  for (size_t offset = 0; offset < 32; ++offset) {
    auto bad = kBlob;
    bad[offset] ^= 1;
    checks.Run(!rules::ValidBlob(bad, kIdentity), "reject a bad header or mismatched device key");
  }
  checks.Run(!rules::ValidSize(31), "reject files smaller than the header before allocating");
  checks.Run(rules::ValidSize(32), "permit the minimum cache file size");
  checks.Run(rules::ValidSize(128ull << 20), "permit a file at the size limit");
  checks.Run(!rules::ValidSize((128ull << 20) + 1), "reject an oversized cache file");
  checks.Run(!rules::ValidSize(UINT64_MAX), "reject a size that would wrap a small integer");

  const auto name = rules::FileName(kIdentity);
  checks.Run(name == "vulkan_pso_000010de_12345678_00043056789abcdef0c2b4a6988a7c6e.cache",
             "stable cache filename with fixed-width hexadecimal fields");
  auto other = kIdentity;
  ++other.vendor_id;
  checks.Run(rules::FileName(other) != name, "another vendor gets another file");
  other = kIdentity;
  ++other.device_id;
  checks.Run(rules::FileName(other) != name, "another GPU model gets another file");
  other = kIdentity;
  ++other.uuid.back();
  checks.Run(rules::FileName(other) != name, "a cache UUID change gets another file");
}

void CheckLifecycle(Checks& checks) {
  checks.Run(!rules::ShouldSave(false, 0, 0), "a cold device with no creations need not save");
  checks.Run(rules::ShouldSave(false, 1, 0), "save after a successful pipeline creation");
  checks.Run(rules::ShouldSave(false, 5, 4), "save again after more creations or a failed save");
  checks.Run(!rules::ShouldSave(false, 5, 5), "skip the exit write after an unchanged snapshot");
  checks.Run(!rules::ShouldSave(false, 4, 5), "require newer creations to save");
  checks.Run(!rules::ShouldSave(true, 5, 4), "skip snapshots on a lost device");
  checks.Run(!rules::Warm(0), "no loaded bytes means cold");
  checks.Run(!rules::Warm(32), "a header-only load has no compiled data");
  checks.Run(rules::Warm(33), "a loaded payload counts as warm");

  checks.Run(rules::CanUsePrebuildStamp(true, true, true), "reuse a stamp with a loaded cache");
  checks.Run(!rules::CanUsePrebuildStamp(true, true, false), "rebuild a missing or rejected cache");
  checks.Run(rules::CanUsePrebuildStamp(true, false, false), "reuse a stamp when persistence is off");
  checks.Run(!rules::CanUsePrebuildStamp(false, false, false), "rebuild an outdated ordinary stamp");
  checks.Run(!rules::CanUsePrebuildStamp(false, true, true), "loaded data cannot validate a stale stamp");
}

}  // namespace

int main() {
  Checks checks;
  CheckBlob(checks);
  CheckLifecycle(checks);
  std::printf("Vulkan pipeline cache: %d checks, %d failed\n", checks.total, checks.failed);
  return checks.failed == 0 ? 0 : 1;
}
