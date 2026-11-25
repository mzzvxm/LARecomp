// Check of the DXIL feature flags reader (dxil_features.h): the Doubles bit of a container's SFI0 part, the way a D3D12 device without double
// precision (the Intel iGPUs) reads it when a pipeline is made. A program of its own: it needs no renderer, no ReXGlue, no compiler and no game
// files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_dxil_features_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_dxil_features_test
//
// The containers are made here: the layout of dxc's own (header, offsets, parts), with the flag words of the two blobs that failed on the laptop's UHD
// (car self-reflection variant 8, flags 0x1: 27 precached pipelines "Shader uses double precision float ops") and of one that was fine (0x0).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "shaderpack/dxil_features.h"

namespace sp = larecomp::shaderpack;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), p, p + 4);
}

struct Part {
  const char* code;
  std::vector<uint8_t> bytes;
};

// "DXBC", a digest, the version (1.0), the size, the count, the offsets, then each part: code, size, bytes.
std::vector<uint8_t> Container(const std::vector<Part>& parts) {
  std::vector<uint8_t> out = {'D', 'X', 'B', 'C'};
  out.insert(out.end(), 16, 0xAB);
  Put32(out, 0x00000001);
  Put32(out, 0);  // size, patched below
  Put32(out, uint32_t(parts.size()));
  size_t at = 32 + 4 * parts.size();
  for (const Part& part : parts) {
    Put32(out, uint32_t(at));
    at += 8 + part.bytes.size();
  }
  for (const Part& part : parts) {
    out.insert(out.end(), part.code, part.code + 4);
    Put32(out, uint32_t(part.bytes.size()));
    out.insert(out.end(), part.bytes.begin(), part.bytes.end());
  }
  const uint32_t size = uint32_t(out.size());
  std::memcpy(out.data() + 24, &size, 4);
  return out;
}

std::vector<uint8_t> Flags(uint64_t flags) {
  std::vector<uint8_t> bytes(8);
  std::memcpy(bytes.data(), &flags, 8);
  return bytes;
}

void RunChecks() {
  const std::vector<uint8_t> other(16, 0x11);

  // The flag of the failing blobs: Doubles alone (what the pack held for the paint's variant words 0x40 and 0x840).
  const std::vector<uint8_t> doubles = Container({{"ISG1", other}, {"SFI0", Flags(0x1)}, {"DXIL", other}});
  uint64_t flags = 7;
  Check(sp::ReadDxilFeatureFlags(doubles, &flags) && flags == 1, "the SFI0 word is read: 0x1");
  Check(sp::DxilUsesDoublePrecision(doubles), "a container with Doubles is found");

  const std::vector<uint8_t> plain = Container({{"ISG1", other}, {"SFI0", Flags(0)}, {"DXIL", other}});
  Check(sp::ReadDxilFeatureFlags(plain, &flags) && flags == 0 && !sp::DxilUsesDoublePrecision(plain), "SFI0 of 0: no doubles");

  // Other bits do not count (bit 1: raw and structured buffers in compute 4.x, 4: minimum precision, 15: 64 bit integers ...).
  const std::vector<uint8_t> others = Container({{"SFI0", Flags((1ull << 1) | (1ull << 4) | (1ull << 15) | (1ull << 32))}});
  Check(!sp::DxilUsesDoublePrecision(others), "feature bits other than Doubles are not doubles");
  const std::vector<uint8_t> both = Container({{"SFI0", Flags((1ull << 15) | 1ull)}});
  Check(sp::DxilUsesDoublePrecision(both), "Doubles among other bits is found");

  // No SFI0 part (a container made by something else): not a double, and the reader says it found nothing.
  const std::vector<uint8_t> none = Container({{"ISG1", other}, {"DXIL", other}});
  flags = 7;
  Check(!sp::ReadDxilFeatureFlags(none, &flags) && flags == 0 && !sp::DxilUsesDoublePrecision(none), "no SFI0 part: nothing read, no doubles");
  const std::vector<uint8_t> empty = Container({});
  Check(!sp::DxilUsesDoublePrecision(empty), "a container with no parts");

  // Not a container, and a cut or damaged one: never read past the end.
  Check(!sp::DxilUsesDoublePrecision({}), "an empty blob");
  const std::vector<uint8_t> spirv = {0x03, 0x02, 0x23, 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  Check(!sp::DxilUsesDoublePrecision(spirv), "a SPIR-V module is not a container");
  for (size_t keep = 0; keep < doubles.size(); ++keep) {
    const std::vector<uint8_t> cut(doubles.begin(), doubles.begin() + keep);
    const bool found = sp::DxilUsesDoublePrecision(cut);
    // Only the whole SFI0 part (it ends 8 bytes before the DXIL part of this fixture, which is 8 + 16 bytes) may be found; the part is complete from there on.
    const size_t sfi_end = doubles.size() - (8 + other.size());
    Check(found == (keep >= sfi_end), "a container cut at " + std::to_string(keep) + " bytes: " + (found ? "found" : "not found"));
  }
  std::vector<uint8_t> many = doubles;
  const uint32_t huge = 0x40000000;
  std::memcpy(many.data() + 28, &huge, 4);
  Check(!sp::DxilUsesDoublePrecision(many), "a part count larger than the blob");
  std::vector<uint8_t> far = doubles;
  const uint32_t beyond = uint32_t(far.size()) + 100;
  std::memcpy(far.data() + 32 + 4, &beyond, 4);  // the second part's offset: past the end
  Check(!sp::DxilUsesDoublePrecision(far), "a part offset past the end");
  std::vector<uint8_t> small = doubles;
  const uint32_t four = 4;  // the SFI0 part claims 4 bytes: no u64 in it
  const size_t sfi_at = 32 + 4 * 3 + 8 + other.size();
  std::memcpy(small.data() + sfi_at + 4, &four, 4);
  Check(!sp::DxilUsesDoublePrecision(small), "an SFI0 part too small for its flags");
}

}  // namespace

int main() {
  RunChecks();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
