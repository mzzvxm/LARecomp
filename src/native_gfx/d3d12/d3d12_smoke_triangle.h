#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — D3D12 smoke test (colored triangle)
// ===========================================================================
// Proves the minimal native D3D12 path end to end:
//
//   device (shared with the SDK provider)
//     -> command allocator / command list
//     -> root signature (empty) + PSO (embedded DXIL)
//     -> vertex buffer (upload heap)
//     -> RTV on an intermediate R10G10B10A2 texture
//     -> clear + draw
//     -> CopyResource into the presenter's guest output texture
//     -> Presenter paints it into the real MCLA window
//
// Presentation path: the existing rex::ui::Presenter / RefreshGuestOutput.
// No debug window. The guest output texture has no RTV capability
// (ALLOW_UNORDERED_ACCESS only) and must be returned in
// D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, hence the intermediate
// render target + copy. The refresh callback runs on the provider's main
// direct queue per the Presenter contract; the paint job runs on the same
// queue, so queue ordering makes the copy visible without a CPU wait.
//
// This file is the Phase 2 smoke test only: no game shaders, no shader
// database, no real vertex declarations. It will be superseded by the real
// draw path and can then be deleted.
// ===========================================================================

#include <cstdint>

namespace rex::ui {
class Presenter;
}
namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace mcla::native_gfx {

class D3D12SmokeTriangle {
 public:
  ~D3D12SmokeTriangle();

  // Creates all D3D12 objects. Safe to call once; returns false on failure
  // (the caller falls back to doing nothing — the game keeps rendering
  // through the normal path).
  bool Initialize(const rex::ui::d3d12::D3D12Provider& provider);

  // Renders one triangle frame into the presenter's guest output and hands
  // it to the paint path. Called once per guest frame.
  bool Present(rex::ui::Presenter* presenter);

  // Waits for the GPU to finish and releases everything. Called on shutdown;
  // also invoked by the destructor.
  void Shutdown();

  bool initialized() const { return initialized_; }

 private:
  struct State;
  State* state_ = nullptr;
  bool initialized_ = false;
  uint64_t frame_index_ = 0;
};

}  // namespace mcla::native_gfx
