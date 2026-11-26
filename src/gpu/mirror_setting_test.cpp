#include "mirror_setting.h"

#include <cstdio>
#include <initializer_list>
#include <string_view>

namespace ms = mcla::gpu::mirror_setting;

int main() {
  int failures = 0;
  int checks = 0;
  const auto check = [&](bool condition, const char* message) {
    ++checks;
    if (!condition) {
      ++failures;
      std::printf("FAIL: %s\n", message);
    }
  };
  for (std::string_view legacy : {"auto", "high", "ultra"}) {
    check(ms::CanonicalWord(legacy) == "on", "legacy raster word migrates to On");
    check(ms::Resolve(legacy, 0, false, false) == ms::Result{1, true, true}, "legacy raster word draws the shared view");
  }
  check(ms::CanonicalWord("off") == "off", "explicit Off survives");
  check(ms::Resolve("off", 0, false, false) == ms::Result{}, "Off does not run the side glass");
  check(ms::Resolve("on", 0, false, false) == ms::Result{1, true, true}, "On runs the fixed view and side glass");
  check(ms::Resolve("off", 3, true, false) == ms::Result{3, true, false}, "debug size and finder can be selected independently");
  check(ms::Resolve("on", 4, false, false) == ms::Result{4, true, true}, "forced view does not disable the readers");
  check(ms::Resolve("off", 7, false, false) == ms::Result{}, "out-of-range debug size does not force a view");
  check(ms::LegacyRtWord("auto") == "on" && ms::LegacyRtWord("on") == "on", "old RT enabled words use the shared view");
  check(ms::LegacyRtWord("off") == "off", "old RT Off is preserved when it is the only row");
  check(!ms::LegacyRtWord("unknown"), "unrecognized RT words do not replace the default");
  std::printf("%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
