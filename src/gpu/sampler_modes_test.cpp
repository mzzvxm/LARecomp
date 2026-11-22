// Check of the sampler address modes a device can make (sampler_modes.h). A program of its own: no Plume, no renderer; exits 0 when every
// check holds.
//
//   cmake --build <build dir> --target larecomp_sampler_modes_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_sampler_modes_test

#include <cstdio>

#include "sampler_modes.h"

using namespace mcla::gpu::sampler_modes;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what);
  }
}

}  // namespace

int main() {
  constexpr Mode kAll[] = {Mode::kWrap, Mode::kMirror, Mode::kClamp, Mode::kMirrorOnce, Mode::kBorder};

  // A device that can make mirror once keeps every mode as it is.
  for (Mode m : kAll) {
    Check(ForDevice(m, true) == m, "supported: every mode is kept");
  }
  // One that cannot: only mirror once changes, to mirrored wrap; no other mode moves.
  Check(ForDevice(Mode::kMirrorOnce, false) == Mode::kMirror, "unsupported: mirror once becomes mirrored wrap");
  for (Mode m : kAll) {
    if (m != Mode::kMirrorOnce) {
      Check(ForDevice(m, false) == m, "unsupported: the other modes are kept");
    }
  }
  // The answer is always a mode the device has: never mirror once when it cannot, and the rule is idempotent.
  for (Mode m : kAll) {
    Check(ForDevice(ForDevice(m, false), false) == ForDevice(m, false), "idempotent");
    Check(ForDevice(m, false) != Mode::kMirrorOnce, "no mirror once when unsupported");
  }
  // Usable in a constant expression (the table in samplers.cpp is one).
  static_assert(ForDevice(Mode::kMirrorOnce, false) == Mode::kMirror);
  static_assert(ForDevice(Mode::kMirrorOnce, true) == Mode::kMirrorOnce);

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
