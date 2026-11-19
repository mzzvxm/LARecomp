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

// ---------------------------------------------------------------------------
// The way back to the XENIA renderer (the hidden fallback)
// ---------------------------------------------------------------------------
// The XENIA renderer stays in the program as a fallback for a PC where the native renderer will not run (owner, 2026-11-19). It is
// not offered anywhere a player looks: the debug menu's RENDERER row (RENDERING page) and, when the native renderer cannot start, the
// box that says so (renderer_fallback.h) call it "the compatibility renderer". Both only write gpu.driver into larecomp.toml, which
// applies at the next start (gpu.driver is startup-only). What the row lists, in this order:
enum class RendererPick : int {
  kNative = 0,       // gpu.driver = larecomp
  kXeniaD3d12 = 1,   // gpu.driver = d3d12
  kXeniaVulkan = 2,  // gpu.driver = vulkan
};
inline constexpr int kRendererPickCount = 3;

// The value gpu.driver gets for a pick (what the row writes).
constexpr std::string_view DriverOfPick(RendererPick pick) {
  switch (pick) {
    case RendererPick::kXeniaD3d12: return "d3d12";
    case RendererPick::kXeniaVulkan: return "vulkan";
    case RendererPick::kNative: break;
  }
  return kLarecompDriverName;
}

// The pick for a stored gpu.driver, what the row shows. "any" is the SDK's d3d12 on Windows and vulkan elsewhere; "null" (the SDK's
// driver that draws nothing) and a word nothing knows count as the Xenia renderer of this platform, the closest row there is; an
// empty value (no setting yet) and "none" (the batch actions) are the native renderer, as RendererChoiceOf says.
constexpr RendererPick PickOfDriver(std::string_view gpu_driver, bool windows) {
  if (RendererChoiceOf(gpu_driver) != RendererChoice::kXenia) {
    return RendererPick::kNative;
  }
  if (gpu_driver == "d3d12") {
    return RendererPick::kXeniaD3d12;
  }
  if (gpu_driver == "vulkan") {
    return RendererPick::kXeniaVulkan;
  }
  return windows ? RendererPick::kXeniaD3d12 : RendererPick::kXeniaVulkan;
}

// The text of a pick on the row. Only A-Z, 0-9 and spaces: the row's value text is also the name of a Flash element.
constexpr std::string_view PickLabel(RendererPick pick) {
  switch (pick) {
    case RendererPick::kXeniaD3d12: return "XENIA D3D12";
    case RendererPick::kXeniaVulkan: return "XENIA VULKAN";
    case RendererPick::kNative: break;
  }
  return "NATIVE";
}

// The pick the row steps to from index i, clamped to the list.
constexpr RendererPick PickAt(int i) {
  return i <= 0 ? RendererPick::kNative : (i == 1 ? RendererPick::kXeniaD3d12 : RendererPick::kXeniaVulkan);
}

// The driver the box's compatibility renderer button writes: the API this platform has Xenia code for first (Direct3D 12 on
// Windows, which also runs on a card that only has shader model 5.1 and binding tier 1; Vulkan elsewhere).
constexpr std::string_view FallbackDriver(bool windows) {
  return DriverOfPick(windows ? RendererPick::kXeniaD3d12 : RendererPick::kXeniaVulkan);
}

}  // namespace mcla::gpu
