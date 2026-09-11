#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — D3D12 context
// ===========================================================================
// Owns the native runtime's per-frame D3D12 machinery on top of the SDK
// provider's shared device and main direct queue:
//
//   - command allocator ring (one per in-flight frame) + command list
//   - fence-based frame synchronization
//   - upload ring buffer for per-frame transient data (constants, small
//     uploads); persistent resources are created by their own subsystems
//   - deferred destruction of resources still referenced by the GPU
//
// The Presenter contract requires all native submissions on the provider's
// main direct queue; that queue is shared with the Xenia command processor
// and the presenter paint path, so queue order is the synchronization
// primitive between rendering, the guest-output copy and the paint.
// ===========================================================================

#include <cstdint>
#include <mutex>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace mcla::native_gfx {

inline constexpr uint32_t kFramesInFlight = 2;

class D3D12Context {
 public:
  ~D3D12Context();

  bool Initialize(const rex::ui::d3d12::D3D12Provider& provider);
  void Shutdown();
  bool initialized() const { return initialized_; }

  ID3D12Device* device() const { return device_; }
  ID3D12CommandQueue* queue() const { return queue_; }

  // Begins a native frame: reclaims the frame slot's allocator (waiting on
  // its fence if the GPU is still using it), resets the command list.
  // Returns the open command list, or nullptr on failure.
  ID3D12GraphicsCommandList* BeginFrame();

  // Closes and submits the command list, signals the frame fence and
  // advances the frame slot. Returns false if Close/Execute failed.
  bool EndFrame();

  // The command list of the frame currently open, so a caller that records
  // across several calls does not have to carry it around. Null when no frame
  // is open.
  ID3D12GraphicsCommandList* CurrentCommandList() const {
    return frame_open_ ? command_list_.Get() : nullptr;
  }

  // Transient upload allocation valid for the current frame only.
  // Returns a CPU pointer and the GPU virtual address; alignment must be a
  // power of two (256 for CBVs). Returns false when the ring is exhausted
  // (caller should treat it as a dropped frame, not a crash).
  struct UploadAlloc {
    void* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    // For CopyBufferRegion sources: the backing upload buffer + offset.
    ID3D12Resource* buffer = nullptr;
    uint64_t offset = 0;
  };
  // `tag` names the caller so an exhaustion report can say WHICH cache filled
  // the ring. The ring is shared by constants, textures, geometry and the
  // capture path, and without this the error only gives a size.
  enum class UploadTag : uint32_t {
    kConstants = 0,
    kTexture = 1,
    kGeometry = 2,
    kCapture = 3,
    kOther = 4,
    kCount = 5,
  };
  bool AllocateUpload(uint64_t size, uint64_t alignment, UploadAlloc& out,
                      UploadTag tag = UploadTag::kOther);
  static void ResetUploadAccounting();

  // Queues a resource for release once the GPU has finished with it. Takes over
  // one reference. The resource is held UNTAGGED until EndFrameReleases() stamps
  // it with the frame's final fence value; only then can ReleaseCompleted free
  // it. This is because a resource retired mid-frame is still referenced by the
  // ~30 later command-list submissions of the SAME frame -- tagging it at retire
  // time (fence_value_+1) freed it while the GPU was still using it (use-after-
  // free: shattered geometry / DEVICE_HUNG).
  void DeferRelease(IUnknown* resource);

  // Stamps every resource retired since the last call with the CURRENT fence
  // value (the frame's final submission), so ReleaseCompleted frees it only
  // after the WHOLE frame that retired it has finished on the GPU. Call once per
  // continuous frame, at the frame boundary AFTER the last submission.
  void EndFrameReleases();

  // Blocks until every submitted frame has completed on the GPU.
  void WaitForIdle();

  // Drains the D3D12 debug layer's message queue into the log. Command list
  // Close() reports a failure with no reason of its own — the actual cause is
  // only in the InfoQueue, which nothing else reads, so it stays silent
  // unless drained explicitly. No-op when the debug layer is off.
  void DrainDebugMessages(const char* context_label);

  // Like DrainDebugMessages but silent when the queue is empty (no "InfoQueue
  // empty" spam), so it can be called every frame to catch the first invalid
  // API call that removes the shared device. Returns true if it logged
  // anything. No-op when the debug layer is off.
  bool DrainDebugMessagesIfAny(const char* context_label);

  // Discards whatever is already queued so a later drain only reports what
  // happened after this point. The device is shared with the Xenia command
  // processor, so without this its messages get attributed to us.
  void ClearDebugMessages();

  // Reports the FIRST resource creation failure with its HRESULT and the
  // device removed reason. Once a device is lost or memory runs out, every
  // later creation fails too, so without capturing the first one the log only
  // shows the cascade and not what started it. E_OUTOFMEMORY (0x8007000E) and
  // DXGI_ERROR_DEVICE_REMOVED (0x887A0005) are the two answers that matter and
  // they demand opposite fixes.
  void ReportCreateFailure(const char* what, long hr);

  // Dumps DRED (Device Removed Extended Data). The provider already arms it
  // with forced auto-breadcrumbs and page-fault reporting, but nothing reads
  // it back, so a GPU hang produced only DXGI_ERROR_DEVICE_HUNG and no
  // indication of WHICH operation hung. The breadcrumb history names the last
  // GPU operations that actually completed per command list, which is the one
  // thing a CPU-side log cannot show: with batching the CPU runs ahead, so the
  // last line logged is not the faulting draw.
  void DumpDeviceRemovedData();

  uint64_t frame_index() const { return frame_index_; }

  // Resources retired but not yet freed. A number that only grows means the
  // fence-gated reclaim is not running (EndFrameReleases never stamping them,
  // or the GPU never reaching the stamped value), which leaks every retired
  // resource for the rest of the session.
  size_t pending_release_count() const { return pending_releases_.size(); }

  // Waits up to `timeout_ms` for the LAST submitted command list to finish on
  // the GPU. Returns true if it completed, false on timeout. Used by the
  // hang-find diagnostic: submitting one draw at a time and waiting with a
  // timeout pinpoints the exact draw whose shader spins forever (a TDR the
  // async breadcrumb can only bracket, not name).
  bool WaitLastSubmitTimeout(uint32_t timeout_ms);

  // Fase-A TDR instrumentation: microseconds this context's BeginFrame spent
  // blocked in the frames-in-flight fence wait (WaitForSingleObject INFINITE)
  // since the last call. That wait time is the guest thread stalling for the GPU
  // to drain the shared queue, so it is a proxy for GPU-busy time per frame:
  // if it approaches the ~2 s TDR budget, the frame is GPU-load-bound. Returns
  // the accumulated microseconds and resets the accumulator.
  double TakeGpuWaitUs();

 private:
  void ReleaseCompleted(uint64_t completed_value);

  double gpu_wait_us_ = 0.0;  // accumulated BeginFrame fence-wait, Fase-A timing
  std::mutex* submit_mutex_ = nullptr;  // shared with the Xenia CP; serializes queue submits

  ID3D12Device* device_ = nullptr;  // owned by the provider
  ID3D12CommandQueue* queue_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[kFramesInFlight];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  uint64_t fence_value_ = 0;
  uint64_t slot_fence_value_[kFramesInFlight] = {};

  // Upload ring: one buffer per frame slot, linear-allocated, reset when the
  // slot's fence completes.
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffers_[kFramesInFlight];
  uint8_t* upload_mapped_[kFramesInFlight] = {};
  uint64_t upload_offset_[kFramesInFlight] = {};
  uint64_t upload_capacity_ = 0;

  struct PendingRelease {
    uint64_t fence_value;
    IUnknown* resource;
  };
  std::vector<PendingRelease> pending_releases_;

  uint64_t frame_index_ = 0;
  bool frame_open_ = false;
  bool initialized_ = false;
  bool device_removed_reported_ = false;
};

}  // namespace mcla::native_gfx
