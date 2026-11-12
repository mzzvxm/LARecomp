#pragma once
// ===========================================================================
// MCLA GPU renderer -- the FidelityFX upscaler backend (F4 step 2, F4-DESIGN.md section 7)
// ===========================================================================
// AMD's FidelityFX SDK 2.4 upscaler through its ffx API: FSR 3.1 and, where the runtime's own
// provider list has it and a context really gets it, FSR 4. Compiled only when CMake is given
// LARECOMP_FFX_SDK_DIR (the headers, and the four signed DLLs and their licence copied next to the
// exe); otherwise MakeFfxBackend() returns null and nothing of it exists.
//
// Files. amd_fidelityfx_loader_<dx12|vk>.dll and amd_fidelityfx_upscaler_<dx12|vk>.dll, opened by
// their FULL PATH in the folder of the exe (LoadLibraryExW with the DLL's own folder and the system
// folder as the only places its dependencies are looked for), never by name through the search
// path. The loader is the one the ffx functions come from; the upscaler DLL is opened first, by the
// same rule, so that when the loader asks for it by name Windows hands it the module that is
// already there (CHECK: that this is how the loader finds it; the alternative is the loader's own
// search, which would also look in the exe's folder).
//
// Probe (worker thread): load both, ask the runtime which upscaler versions it offers for this
// device (ffxQueryDescGetVersions), tell the 3.1 provider from the 4.x one by version number, and
// for a 4.x one make a small context to see that its provider really loads (AMD's API falls back to
// 3.1 silently otherwise: ffxQueryGetProviderVersion says which one a context got). What is found
// is Status(): the menu lists only that. FSR 3.1 is made with the runtime's version override, so
// that "FSR 3.1" is never FSR 4 on a card that has it.
//
// Context. Made and destroyed on the worker (0.3 to 0.6 s on the logged runs, hundreds of MB at 4K):
// for one request (the upscaler, the sizes) at a time; a request that differs retires the old one
// behind the frames that may use it. A second context can be prepared beside the current one (Prepare,
// Commit: a live change of mode or upscaler): the old one dispatches until the new one is made, so
// for about half a second the memory of two is held. Flags: HDR input (the scene is linear and scene referred), inverted depth (the
// game stores 1 near, 0 far), automatic exposure, motion vectors at render size with the jitter
// already taken out (no jitter cancellation), no dynamic resolution.
//
// Dispatch (Upscale): the inputs arrive in the states the caller transitioned them to (the
// graphics-and-compute-readable images at the compute stage: D3D12 NON_PIXEL_SHADER_RESOURCE, Vulkan
// SHADER_READ_ONLY_OPTIMAL, declared to FFX as COMPUTE_READ; the output in GENERAL / UNORDERED_ACCESS,
// declared as UNORDERED_ACCESS): FFX takes its resources back to those states.
//
// Threads: see temporal_backend.h.
// ===========================================================================

#include <memory>

#include "temporal_backend.h"

namespace mcla::gpu {

class FfxBackend final : public TemporalBackend {
 public:
  FfxBackend();
  ~FfxBackend() override;

  const char* Name() const override { return "FidelityFX"; }
  void Probe(Device& device, FrameRing& frames) override;
  BackendStatus Status() const override;
  void Request(const BackendRequest& request) override;
  bool Ready(const BackendRequest& request) const override;
  uint32_t JitterPhaseCount() const override;
  void Prepare(const BackendRequest& next) override;
  bool PreparedReady(const BackendRequest& next) const override;
  bool Commit(const BackendRequest& next) override;
  void CancelPrepared() override;
  void Release() override;
  void Fail(const std::string& reason) override;
  bool Upscale(plume::RenderCommandList* list, const TemporalInputs& inputs) override;
  void Shutdown() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mcla::gpu
