#pragma once

namespace mcla::gpu::vram_quality {

enum class AoPass { kBounceTrace, kBounceDenoise, kBounceHistory, kContact, kApply };
struct Lifetime {
  AoPass first;
  AoPass last;
};

constexpr bool Disjoint(Lifetime a, Lifetime b) {
  return a.last < b.first || b.last < a.first;
}

constexpr bool AliasBounceAndContact(bool enabled, bool bounce, bool contact, bool apply, bool bounce_view) {
  constexpr Lifetime scratch{AoPass::kContact, AoPass::kApply};
  const Lifetime raw{AoPass::kBounceTrace, bounce_view ? AoPass::kApply : AoPass::kBounceDenoise};
  return enabled && bounce && contact && apply && Disjoint(raw, scratch);
}

}  // namespace mcla::gpu::vram_quality
