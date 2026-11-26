#include <cstdint>
#include <cstdio>

#include "vram_quality.h"
#include "vram_warning.h"

namespace {
namespace vram = mcla::gpu::vram_quality;
namespace warning = mcla::gpu::vram_warning;

int checks = 0;
int failures = 0;

void Check(bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

void Budget() {
  using enum warning::Change;
  warning::Warning guard;
  Check(guard.Sample(819, 1000, false) == kNone, "below 82 percent is quiet");
  Check(guard.Sample(820, 1000, false) == kShow, "82 percent shows the warning");
  Check(guard.Sample(900, 1000, false) == kNone, "continued pressure does not repeat the warning");
  for (uint32_t n = 0; n < 7; ++n) {
    Check(guard.Sample(680, 1000, false) == kNone, "recovery requires eight low samples");
  }
  Check(guard.Sample(681, 1000, false) == kNone, "a sample above 68 percent interrupts recovery");
  for (uint32_t n = 0; n < 7; ++n) {
    Check(guard.Sample(680, 1000, false) == kNone, "recovery restarts after an interruption");
  }
  Check(guard.Sample(680, 1000, false) == kClear, "the eighth low sample clears the notice");
  Check(guard.Sample(680, 1000, false) == kNone, "recovery only clears once");
  Check(guard.Sample(999, 1000, false) == kShow, "a later pressure episode warns again");

  // Each of these interruptions must force a fresh eight-sample recovery.
  struct Sample { uint64_t used, budget; bool failed; };
  const Sample interruptions[] = {{820, 1000, false}, {0, 0, false}, {0, 1000, true}};
  for (const auto& [used, budget, failed] : interruptions) {
    for (uint32_t n = 0; n < 7; ++n) {
      Check(guard.Sample(680, 1000, false) == kNone, "seven samples before interruption stay active");
    }
    Check(guard.Sample(used, budget, failed) == kNone,
          "high use, missing budget and failure never clear or repeat an active warning");
  }
  Check(guard.Sample(680, 1000, false) == kNone, "a failure resets the recovery counter");

  Check(warning::Warning{}.Sample(0, 0, false) == kNone, "an absent budget alone does not warn");
  Check(warning::Warning{}.Sample(0, 1000, true) == kShow, "an allocation failure warns before 82 percent");
  Check(warning::Warning{}.Sample(0, 0, true) == kShow, "a failed allocation warns without a budget");
  Check(warning::Warning{}.Sample(163, 199, false) == kNone, "entry rounds up a fractional byte");
  warning::Warning fractional;
  Check(fractional.Sample(164, 199, false) == kShow, "the first whole byte at or above 82 percent warns");
  for (uint32_t n = 0; n < 7; ++n) {
    Check(fractional.Sample(135, 199, false) == kNone, "fractional budget, still seven recovery samples");
  }
  Check(fractional.Sample(136, 199, false) == kNone, "136 bytes is above the fractional 68 percent boundary");
  for (uint32_t n = 0; n < 7; ++n) {
    Check(fractional.Sample(135, 199, false) == kNone, "recovery restarts below the fractional boundary");
  }
  Check(fractional.Sample(135, 199, false) == kClear, "135 bytes is below the fractional boundary");
  Check(warning::Warning{}.Sample(15126330140441832324ull, UINT64_MAX, false) == kNone,
        "the threshold does not overflow or round down at the largest budget");
  Check(warning::Warning{}.Sample(15126330140441832325ull, UINT64_MAX, false) == kShow,
        "the largest budget still warns at the boundary");
}

void CostRanking() {
  using enum warning::Setting;
  warning::SettingBytes bytes{};
  bytes[warning::Index(kRtCity)] = 256;
  bytes[warning::Index(kRenderSize)] = 128;
  bytes[warning::Index(kFrameGeneration)] = 128;
  bytes[warning::Index(kNone)] = UINT64_MAX;
  const auto costs = warning::Rank(bytes);
  Check(costs[0].setting == kRtCity && costs[0].bytes == 256, "lead with the largest named cost");
  Check(costs[1].setting == kRenderSize && costs[2].setting == kFrameGeneration, "ties keep table order");
  Check(costs[3].bytes == 0, "zero costs trail all measured costs");
  Check(warning::Rank({}).front().setting == kRenderSize, "with no measured costs, suggest scale and MSAA");
  for (const auto& cost : costs) {
    const auto& labels = warning::kLabels[warning::Index(cost.setting)];
    Check(!labels.hint.empty() && labels.hint.size() <= 8 && !labels.full.empty(),
          "every ranked setting has a full name and a hint that fits the preset row");
  }
}

void Lifetimes() {
  using enum vram::AoPass;
  const vram::Lifetime raw{kBounceTrace, kBounceDenoise};
  const vram::Lifetime contact{kContact, kApply};
  Check(vram::Disjoint(raw, contact) && vram::Disjoint(contact, raw), "disjoint is symmetric");
  Check(!vram::Disjoint({kBounceTrace, kContact}, contact), "last use at first write still overlaps");
  Check(!vram::Disjoint(raw, raw), "an image cannot share with its own lifetime");
  Check(vram::AliasBounceAndContact(true, true, true, true, false), "ordinary SSGI and contact share");
  Check(!vram::AliasBounceAndContact(true, true, true, true, true), "the raw debug view holds the bounce through apply");
  Check(!vram::AliasBounceAndContact(false, true, true, true, false), "switch off uses ordinary targets");
  Check(!vram::AliasBounceAndContact(true, false, true, true, false), "no alias without bounce");
  Check(!vram::AliasBounceAndContact(true, true, false, true, false), "no alias without contact");
  Check(!vram::AliasBounceAndContact(true, true, true, false, false), "a probe has no contact apply");
}
}  // namespace

int main() {
  Budget();
  CostRanking();
  Lifetimes();
  std::printf("vram_quality_test: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
