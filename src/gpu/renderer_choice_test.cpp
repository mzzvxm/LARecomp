// A check that needs no game: which renderer each value of the SDK's gpu.driver means (gpu/renderer_choice.h). Build:
// cmake --build <build dir> --target larecomp_renderer_choice_test, then run it; exit 0 = passed.
//
// The case of 2026-11-18: gpu.driver = d3d12 / vulkan ran the boot build of the shader packs, because the test of the day (the
// old mcla_gpu setting, true by default) said "Plume".

#include <cstdio>

#include "renderer_choice.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

}  // namespace

int main() {
  using mcla::gpu::RendererChoice;
  using mcla::gpu::RendererChoiceOf;

  // LARecomp's own renderer: the default, and the batch actions' "none".
  CHECK(RendererChoiceOf("larecomp") == RendererChoice::kLarecomp);
  CHECK(RendererChoiceOf("none") == RendererChoice::kLarecomp);

  // The SDK's Xenia GPU: every driver the SDK ships, and "any" (d3d12 on Windows, vulkan elsewhere).
  CHECK(RendererChoiceOf("any") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("d3d12") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("vulkan") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("null") == RendererChoice::kXenia);

  // No answer before the setting exists; a word nothing knows is not LARecomp's renderer (the SDK refuses to start with it).
  CHECK(RendererChoiceOf("") == RendererChoice::kNotSet);
  CHECK(RendererChoiceOf("Larecomp") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("larecomp ") == RendererChoice::kXenia);

  static_assert(RendererChoiceOf("larecomp") == RendererChoice::kLarecomp);
  static_assert(RendererChoiceOf("vulkan") == RendererChoice::kXenia);

  std::printf("renderer choice: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
