#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — D3D12 Enhanced Barriers & Resource Transitions
// ===========================================================================
// Unifies resource state transitions across the native runtime. When the
// underlying GPU and OS support Enhanced Barriers (ID3D12GraphicsCommandList7
// via Agility SDK / Windows 11 SDK) and mcla_native_gfx_enhanced_barriers is
// enabled, transitions are issued via cl7->Barrier(...) with fine-grained
// synchronization scopes (D3D12_BARRIER_SYNC) and access flags (D3D12_BARRIER_ACCESS),
// eliminating coarse-grained pipeline flushes.
//
// Automatically falls back to legacy cl->ResourceBarrier(...) when running
// on older runtimes or when disabled via CVar.
// ===========================================================================

#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/d3d12/d3d12_api.h>

REXCVAR_DECLARE(bool, mcla_native_gfx_enhanced_barriers);

namespace mcla::native_gfx {

class BarrierBatch {
 public:
  BarrierBatch() = default;
  ~BarrierBatch() = default;

  void AddTexture(ID3D12Resource* resource,
                  D3D12_RESOURCE_STATES state_before,
                  D3D12_RESOURCE_STATES state_after,
                  UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

  void AddBuffer(ID3D12Resource* resource,
                 D3D12_RESOURCE_STATES state_before,
                 D3D12_RESOURCE_STATES state_after);

  void AddGlobal(D3D12_RESOURCE_STATES state_before,
                 D3D12_RESOURCE_STATES state_after);

  void AddUav(ID3D12Resource* resource = nullptr);

  bool empty() const {
    return textures_.empty() && buffers_.empty() && globals_.empty() && uavs_.empty();
  }
  size_t size() const {
    return textures_.size() + buffers_.size() + globals_.size() + uavs_.size();
  }

  void Clear() {
    textures_.clear();
    buffers_.clear();
    globals_.clear();
    uavs_.clear();
  }

  // Flushes all queued transitions onto the command list.
  void Flush(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList7* cl7 = nullptr);

  // Immediate helper functions for one-off transitions:
  static void Transition(ID3D12GraphicsCommandList* cl,
                         ID3D12Resource* resource,
                         D3D12_RESOURCE_STATES state_before,
                         D3D12_RESOURCE_STATES state_after,
                         UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         ID3D12GraphicsCommandList7* cl7 = nullptr);

  static void TransitionBuffer(ID3D12GraphicsCommandList* cl,
                               ID3D12Resource* resource,
                               D3D12_RESOURCE_STATES state_before,
                               D3D12_RESOURCE_STATES state_after,
                               ID3D12GraphicsCommandList7* cl7 = nullptr);

  static void UavBarrier(ID3D12GraphicsCommandList* cl,
                         ID3D12Resource* resource = nullptr,
                         ID3D12GraphicsCommandList7* cl7 = nullptr);

 private:
  struct TextureTransition {
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES state_before;
    D3D12_RESOURCE_STATES state_after;
    UINT subresource;
  };

  struct BufferTransition {
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES state_before;
    D3D12_RESOURCE_STATES state_after;
  };

  struct GlobalTransition {
    D3D12_RESOURCE_STATES state_before;
    D3D12_RESOURCE_STATES state_after;
  };

  std::vector<TextureTransition> textures_;
  std::vector<BufferTransition> buffers_;
  std::vector<GlobalTransition> globals_;
  std::vector<ID3D12Resource*> uavs_;
};

}  // namespace mcla::native_gfx
