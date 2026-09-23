#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — GPU Profiler & Diagnostics
// ===========================================================================
// Provides GPU hardware timestamp queries (D3D12_QUERY_HEAP_TYPE_TIMESTAMP)
// and PIX/RenderDoc event markers on D3D12 graphics command lists.
//
// Uses a 3-slot ring of readback buffers matching kFramesInFlight, allowing
// completed frame timestamps to be read back asynchronously with zero CPU/GPU
// stalls.
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>

#include <rex/cvar.h>
#include <rex/ui/d3d12/d3d12_api.h>

REXCVAR_DECLARE(bool, mcla_native_gfx_gpu_profiler);
REXCVAR_DECLARE(bool, mcla_native_gfx_pix_markers);

namespace mcla::native_gfx {

class D3D12Context;

class GpuProfiler {
 public:
  static constexpr uint32_t kMaxQueries = 64;
  static constexpr uint32_t kRingSize = 3;

  static GpuProfiler& Instance();

  bool Initialize(D3D12Context& context);
  void Shutdown();
  bool IsInitialized() const { return initialized_; }

  void BeginFrame(D3D12Context& context, ID3D12GraphicsCommandList* cl);
  void EndFrame(D3D12Context& context, ID3D12GraphicsCommandList* cl);

  void BeginScope(ID3D12GraphicsCommandList* cl, const char* name);
  void EndScope(ID3D12GraphicsCommandList* cl);
  void SetMarker(ID3D12GraphicsCommandList* cl, const char* name);

  double frame_time_ms() const { return last_frame_time_ms_; }
  double scope_time_ms(std::string_view name) const;
  std::string Summary() const;

 private:
  GpuProfiler() = default;
  ~GpuProfiler();

  GpuProfiler(const GpuProfiler&) = delete;
  GpuProfiler& operator=(const GpuProfiler&) = delete;

  struct ScopeRecord {
    std::string name;
    uint32_t start_query = 0;
    uint32_t end_query = 0;
  };

  struct FrameSlot {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    std::vector<ScopeRecord> scopes;
    uint32_t query_count = 0;
    uint64_t frame_index = 0;
    bool active = false;
  };

  void ReadbackSlot(uint32_t slot);

  bool initialized_ = false;
  uint64_t timestamp_frequency_ = 0;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> query_heap_;

  FrameSlot slots_[kRingSize];
  uint32_t current_slot_ = 0;
  uint32_t next_query_ = 0;
  std::vector<uint32_t> open_scopes_;

  double last_frame_time_ms_ = 0.0;
  std::unordered_map<std::string, double> last_scope_times_ms_;
};

class ScopedGpuEvent {
 public:
  ScopedGpuEvent(ID3D12GraphicsCommandList* cl, const char* name) : cl_(cl) {
    if (cl_) {
      GpuProfiler::Instance().BeginScope(cl_, name);
    }
  }

  ~ScopedGpuEvent() {
    if (cl_) {
      GpuProfiler::Instance().EndScope(cl_);
    }
  }

  ScopedGpuEvent(const ScopedGpuEvent&) = delete;
  ScopedGpuEvent& operator=(const ScopedGpuEvent&) = delete;

 private:
  ID3D12GraphicsCommandList* cl_ = nullptr;
};

}  // namespace mcla::native_gfx
