#include "hlsl/cabin_shadow_rules.hlsli"

#include <cstdio>
#include <initializer_list>

namespace cs = mcla::gpu::cabin_shadow;

int main() {
  int checks = 0;
  int failures = 0;
  const auto check = [&](bool ok, const char* rule) {
    ++checks;
    if (!ok) {
      ++failures;
      std::fprintf(stderr, "%s\n", rule);
    }
  };

  for (int mode = -1; mode <= 5; ++mode) {
    check(cs::CabinMode(mode) == unsigned(mode < 0 || mode > 4 ? 2 : mode), "mode range and fallback");
    if (mode >= 0) {
      check(cs::CabinSourceMode(unsigned(mode)) == unsigned(mode <= 3 ? mode : 0), "only source modes affect history");
    }
  }
  check(cs::kCabinMarch == 1u && cs::kCabinMap == 2u && cs::kCabinRt == 3u && cs::kCabinFill == 4u, "debug menu mode values");
  check(cs::CabinPlayerId(1, true, 9u) == 9u, "a redrawn paused cockpit keeps the current player ID");
  check(cs::CabinPlayerId(-1, true, 9u) == 0u, "a photo camera can replace the cockpit rig");
  for (const int rendered_rig : {-1, 0, 1, 2, 3, 6}) {
    for (const bool valid : {false, true}) {
      for (const unsigned id : {0u, 1u, 9u, 23u, 24u}) {
        const unsigned expected = rendered_rig == 1 && valid && id > 0u && id < 24u ? id : 0u;
        check(cs::CabinPlayerId(rendered_rig, valid, id) == expected, "current valid player only in the rendered cockpit view");
      }
    }
  }
  for (unsigned bits = 0; bits < 16u; ++bits) {
    check(cs::CabinMarkDepth(bits, 0u, 9u) == bits, "depth bits stay identical when off");
    check(cs::CabinMarkDepth(bits, 9u, 0u) == bits, "world depth bits stay identical");
    check(cs::CabinMarkDepth(bits, 9u, 9u) == (bits | 8u), "the mark only sets bit 3 for a player pixel");
    check(cs::CabinMarkDepth(bits, 9u, 5u) == (bits & ~8u), "another vehicle always clears its bit 3");
    const bool tagged = (bits & 9u) == 9u;
    check(cs::CabinMarkedDepth(bits) == tagged, "both the car and player bits must be set");
    check(cs::CabinSunMask(4u, bits, 0x15u, 0x5u) == (tagged ? 0x10u : 0x15u), "sun excludes player body and tyres, retains other cars");
    check(cs::CabinSunMask(2u, bits, 0x15u, 0x5u) == 0x15u, "RT mode bit required");
    for (unsigned mode = 0; mode <= 4; ++mode) {
      check(cs::CabinCheckMarch(mode, bits) == (mode == 1 && tagged), "march only checks its own receivers");
      check(cs::CabinSkipMap(mode, bits, false) == (mode == 2 && tagged), "map skips only its own receivers");
      check(!cs::CabinSkipMap(mode, bits, true), "map debug view shows the raw map term");
    }
  }
  for (unsigned id = 0; id < 24u; ++id) {
    check(cs::CabinApplyId(0xabcd0103u | (id << 9)) == id, "no collision with the sample or game's car sun lookup");
  }
  check(cs::CabinFillVisibility(0.0f, 9u, 9u) == 0.75f, "fully shadowed player pixel");
  check(cs::CabinFillVisibility(0.5f, 9u, 9u) == 0.875f, "partially shadowed player pixel");
  check(cs::CabinFillVisibility(1.0f, 9u, 9u) == 1.0f, "fully lit player pixel");
  check(cs::CabinFillVisibility(0.0f, 9u, 5u) == 0.0f, "another vehicle stays shadowed");
  check(cs::CabinFillVisibility(0.0f, 0u, 0u) == 0.0f, "disabled on pixels without a vehicle");

  std::printf("cabin shadow: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
