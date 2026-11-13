#pragma once
// ===========================================================================
// MCLA GPU renderer -- the device
// ===========================================================================
// Owns the Plume interface, the device and the one DIRECT queue every frame is
// submitted on. Nothing here is shared with the ReXGlue SDK: in mcla_gpu mode
// the SDK has no graphics system (gpu_plugin empty) and no D3D12Provider is
// created, so this is the only device in the process. Same ownership as
// reblue (gpu/device.cpp CreateHostDevice) and reeot (gpu/device.cpp).
//
// Creation goes through the Create* members below, never straight through
// device()->create*: Plume returns a live wrapper around a null native object
// when the backend call fails, and binding one faults later somewhere else.
// Each failure is logged with what was being created and checked against
// device loss, which is latched -- once lost, every later creation and every
// fence wait would fail or hang, so callers stop at IsLost().
//
// Threading: Initialize/Shutdown on the thread that owns the renderer's
// lifecycle; queue() submission only from the render thread. Create* is called
// from the render thread and the lifecycle thread. CreateGraphicsPipeline is
// also called from the guest pipelines' precache workers (pipelines.h) and the
// host builder thread (host_precache.h). For that call, in the pinned Plume:
// the D3D12 graphics pipeline's constructor takes no lock and keeps no shared
// state (ID3D12Device::CreateGraphicsPipelineState is free-threaded), and the
// Vulkan one keeps its scratch vectors thread_local and passes no pipeline
// cache to vkCreateGraphicsPipelines. The other Create* are not checked for a
// second thread: do not call them from one.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <plume_render_interface.h>

#include "backend.h"
#include "texture_bytes.h"

namespace mcla::gpu {

class Device {
 public:
  struct Options {
    // Passed to RenderInterface::createDevice; empty picks Plume's default.
    std::string preferred_adapter;
    bool debug_layer = false;
    // The backend asked for (graphics_backend); must be compiled.
    Backend backend = kDefaultBackend;
  };

  Device() = default;
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // Selects options.backend and opens it. If its interface, device, required
  // capabilities or queue cannot be had, the other backend the build has is
  // tried -- logged as an error, and fell_back() says so to the caller, who
  // tells the user. False when no backend opens.
  bool Initialize(const Options& options);
  // Releases the queue, the device and the interface, in that order. Everything
  // created from this device must already be gone.
  void Shutdown();

  plume::RenderDevice* device() const { return device_.get(); }
  plume::RenderCommandQueue* queue() const { return queue_.get(); }
  const plume::RenderDeviceCapabilities& capabilities() const { return device_->getCapabilities(); }
  const plume::RenderDeviceDescription& description() const { return device_->getDescription(); }
  const std::string& backend_info() const { return backend_info_; }
  // The backend Initialize was asked for, and whether another one runs instead.
  Backend requested_backend() const { return requested_backend_; }
  bool fell_back() const { return device_ && ActiveBackend() != requested_backend_; }
  // Why each backend that was tried could not be started, one line each in plain words ("Direct3D
  // 12 on <graphics card>: it offers shader model 5.1 and the shaders need 6.0"), for the message
  // the player is shown when none starts. Empty when nothing was refused. Kept after Shutdown.
  const std::string& refusals() const { return refusals_; }

  // Validated creation. Null on failure, never a null-backed wrapper. `name`
  // becomes the debug name (RenderDoc, PIX, validation messages) where Plume
  // has one for the object and object_names() is on, and names the object in
  // the failure log when the caller passed one (null is logged as '?').
  std::unique_ptr<plume::RenderTexture> CreateTexture(const plume::RenderTextureDesc& desc,
                                                      const char* name);
  std::unique_ptr<plume::RenderTextureView> CreateTextureView(
      const plume::RenderTexture& texture, const plume::RenderTextureViewDesc& desc,
      const char* name);
  std::unique_ptr<plume::RenderBuffer> CreateBuffer(const plume::RenderBufferDesc& desc,
                                                    const char* name);
  std::unique_ptr<plume::RenderSampler> CreateSampler(const plume::RenderSamplerDesc& desc,
                                                      const char* name);
  // `view_count`/`sampler_count`: descriptors the builder describes, checked
  // against what the backend allocated (backend::DescriptorSetAllocated).
  std::unique_ptr<plume::RenderDescriptorSet> CreateDescriptorSet(
      const plume::RenderDescriptorSetBuilder& builder, uint32_t view_count,
      uint32_t sampler_count, const char* name);
  std::unique_ptr<plume::RenderPipelineLayout> CreatePipelineLayout(
      const plume::RenderPipelineLayoutBuilder& builder, const char* name);
  // A shader blob in HostShaderFormat() (the active backend's): the host
  // shaders built with the renderer, and the guest shaders of the pack
  // (shaders.h).
  std::unique_ptr<plume::RenderShader> CreateShader(const void* data, uint64_t size,
                                                    const char* entry_point, const char* name);
  std::unique_ptr<plume::RenderPipeline> CreateGraphicsPipeline(
      const plume::RenderGraphicsPipelineDesc& desc, const char* name);
  std::unique_ptr<plume::RenderPipeline> CreateComputePipeline(
      const plume::RenderComputePipelineDesc& desc, const char* name);
  std::unique_ptr<plume::RenderQueryPool> CreateQueryPool(
      uint32_t query_count, const char* name,
      plume::RenderQueryType type = plume::RenderQueryType::TIMESTAMP);
  std::unique_ptr<plume::RenderFramebuffer> CreateFramebuffer(
      const plume::RenderFramebufferDesc& desc, const char* name);

  // Whether the objects made here get their debug name (a D3D12 SetName is a
  // few microseconds with the UTF-16 conversion, and the textures of a world
  // are made in the hundreds in one frame; the name's own formatting costs as
  // much again). Fixed when the device opens: on when something can read the
  // names, which is a capture tool loaded in the process before the device
  // (RenderDoc, PIX, Nsight Graphics), the D3D12 debug layer, a build without
  // NDEBUG, mcla_gpu_perf_markers, or mcla_gpu_object_names (for the tools that
  // do not load a DLL, and for one that attaches later: the names must be
  // there from the start, a capture cannot ask for them afterwards). Off, a
  // caller that formats a name for each creation passes null instead.
  bool object_names() const { return names_; }

  // A texture or buffer whose creation failed with the device alive is not
  // asked for again -- the same description, from anywhere -- for kBackoffFrames
  // frames (kUploadBackoffFrames for the upload and readback heaps, whose
  // draws are dropped meanwhile), nor before about as many bytes as it needed
  // have been given back (NoteReleased) and the GPU has let go of them. A full
  // video memory asks for the same resolve product or texture on every use,
  // every frame, and each ask is a driver call that fails, and may page other
  // allocations out first. The frame is counted by BeginFrame.
  static constexpr uint64_t kBackoffFrames = 30;
  static constexpr uint64_t kUploadBackoffFrames = 10;
  // Render thread, once per frame (Renderer::OpenFrameList).
  void BeginFrame() { frame_.fetch_add(1, std::memory_order_relaxed); }
  uint64_t frame() const { return frame_.load(std::memory_order_relaxed); }
  // A cache retired `bytes` of texture or buffer memory. The GPU lets go of it
  // within kFramesInFlight frames.
  void NoteReleased(uint64_t bytes);
  uint64_t released_bytes() const { return released_bytes_.load(std::memory_order_relaxed); }
  // Whether CreateTexture would refuse `desc` now, for a creation that failed
  // lately (callers that build more than the texture check this first).
  bool TextureBlocked(const plume::RenderTextureDesc& desc);
  // The largest video memory request (a texture, or a buffer in the default
  // heap) that failed since the last call; 0 when none. Taken by the
  // budget (vram_budget.h), which answers it by releasing what is unused.
  uint64_t TakeFailedBytes() { return failed_bytes_.exchange(0, std::memory_order_relaxed); }
  // Creations refused by the back-off since the last call.
  uint64_t TakeSkipped() { return skipped_.exchange(0, std::memory_order_relaxed); }

  // True once device loss has been seen. Latched.
  bool IsLost() const { return lost_.load(std::memory_order_acquire); }
  // Asks the backend whether the device is gone; logs and latches the first
  // time it is. `context` names the operation that noticed. Returns IsLost().
  bool CheckLost(const char* context);

  backend::VideoMemory QueryVideoMemory() const;

 private:
  // One attempt on the active backend; leaves nothing behind on failure.
  bool Open(const Options& options);
  // Adds a line to refusals(): the active backend's plain name, then `what`.
  void Refuse(const std::string& what);
  // Names `object` unless names are off or the caller gave none.
  template <class Object>
  void Name(Object& object, const char* name) const {
    if (names_ && name) {
      object.setName(name);
    }
  }
  void ReportCreationFailure(const char* what, const char* name);
  // Whether this creation failure is logged in full (counts every failure).
  bool LogThisFailure();
  std::atomic<uint64_t> creation_failures_{0};

  // The back-off (above): what was asked for, by description.
  static uint64_t BackoffKey(const plume::RenderTextureDesc& desc);
  static uint64_t BackoffKey(const plume::RenderBufferDesc& desc);
  // True while a creation of `key` failed lately and nothing was freed since.
  bool Blocked(uint64_t key);
  // A creation of `bytes` failed with the device alive; `video_memory`: the
  // request was for video memory (not an upload or readback heap).
  void NoteFailed(uint64_t key, uint64_t bytes, bool video_memory);
  static constexpr size_t kBackoffEntries = 256;
  // Frames after a release before the GPU is taken to be done with what it freed.
  static constexpr uint64_t kReleaseSettleFrames = 3;
  struct Backoff {
    uint64_t retry_frame = 0;   // tried again from this frame at the latest
    uint64_t released_at = 0;   // released_bytes() when it failed
    uint64_t bytes = 0;         // what the request needed
  };
  std::atomic<uint64_t> frame_{0};
  std::atomic<uint64_t> released_bytes_{0};
  std::atomic<uint64_t> release_frame_{0};  // the frame of the last NoteReleased
  std::atomic<uint64_t> failed_bytes_{0};
  std::atomic<uint64_t> skipped_{0};
  std::atomic<uint32_t> backoff_count_{0};  // backoff_.size(), read without the lock
  std::mutex backoff_mutex_;
  std::unordered_map<uint64_t, Backoff> backoff_;

  // Declaration order is destruction order in reverse: queue, device, interface.
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  std::string backend_info_;
  std::string refusals_;
  Backend requested_backend_ = kDefaultBackend;
  bool debug_layer_ = false;
  bool names_ = false;  // object_names(); set by Open, before any creation or second thread
  std::atomic<bool> lost_{false};
};

// The options the renderer's Startup opens the device with: mcla_gpu_adapter, else the GPU "auto"
// chose Vulkan for, the debug layer cvar, and the backend graphics_backend asks for (or `forced`,
// when not null: the adapter "auto" chose then applies only if it is for that backend). Defined in
// renderer.cpp, which keeps what Enabled() decided; call it only after Enabled() returned true.
// The pipeline prebuild (pipeline_prebuild.h) opens its device the same way.
Device::Options PlannedDeviceOptions(const Backend* forced);

}  // namespace mcla::gpu
