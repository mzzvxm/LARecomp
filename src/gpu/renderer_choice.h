#pragma once
// ===========================================================================
// Which renderer runs, from the SDK's gpu.driver (rexglue 0.11)
// ===========================================================================
// The one rule behind mcla::gpu::Enabled() (gpu.h). Pure: gpu/renderer_choice_test.cpp compiles this header alone.
//
// gpu.driver is a startup-only SDK setting. "larecomp" is LARecomp's own renderer (Plume, the in-process no-command-processor
// driver of native_gfx/nocp/nocp_driver.h). "any", "d3d12", "vulkan" and "null" are drivers of the SDK's own Xenia GPU (the
// XENIA renderer), and then nothing of src/gpu runs: no shader pack, no pipeline cache, no Plume device. "none" is what the batch
// actions (--precache_pipelines) leave, because they load no driver at all: the device they build on is LARecomp's own.
//
// Before 0.11 the choice was the pair mcla_gpu / mcla_native_gfx_nocp. Both are gone as settings (their default, true, was also
// what a XENIA run had, so reading them could not tell the renderers apart): a line naming one in larecomp.toml, on the command line
// or in the environment is ignored with a note (kRemovedNames in settings/larecomp_settings.cpp), and ApplyPlayerConfig reads the
// two keys from the raw file only to set gpu.driver for an old file.
#include <string_view>

namespace mcla::gpu {

// The name of LARecomp's own driver in gpu.driver. renderer.cpp checks it against nocp::kDriverName.
inline constexpr std::string_view kLarecompDriverName = "larecomp";

enum class RendererChoice {
  kNotSet,    // the setting does not exist yet (asked before the settings are up): no answer
  kLarecomp,  // LARecomp's own renderer
  kXenia,     // the SDK's Xenia GPU
};

constexpr RendererChoice RendererChoiceOf(std::string_view gpu_driver) {
  if (gpu_driver.empty()) {
    return RendererChoice::kNotSet;
  }
  if (gpu_driver == kLarecompDriverName || gpu_driver == "none") {
    return RendererChoice::kLarecomp;
  }
  return RendererChoice::kXenia;
}

}  // namespace mcla::gpu
