#pragma once
// ===========================================================================
// MCLA GPU renderer -- RenderDoc capture of one frame
// ===========================================================================
// RenderDoc's own capture key slices at the swap chain's Present, which is not
// where a guest frame begins or ends once frames are replayed on the render
// thread. The in-application API brackets work directly: a capture spans from
// the end of Present N to the end of Present N+1 on the render thread, so from
// M4 on it holds one whole guest frame and nothing else.
//
// Requests come from F11 (FrameCapture::Request, any thread; ui.bind.gpu_renderdoc_capture) or from a file named
// native_gfx_rdc_trigger in the working directory, polled every
// kTriggerPollPresents presents and consumed by deleting it -- the protocol
// tools/rdc/rdc_launch.py already speaks.
//
// The in-application API is found here, not through the SDK: since 0.11 its RenderDocAPI is private to the
// Xenia GPU module (src/gpu/xenos/host). The module RenderDoc injected is looked up, never loaded: a
// renderdoc.dll LoadLibrary'd by us would give an API that captures nothing. Device and window are passed as
// null: every device, every window. A capture is never nested (IsFrameCapturing is checked first: the legacy
// runtime once stacked 177 starts with no end).
//
// Threading: Request from any thread; everything else on the render thread.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace mcla::gpu {

// RenderDoc's in-application API, version 1.0.0 (capture.cpp declares the part this file calls).
struct RenderDocApi100;

class FrameCapture {
 public:
  static constexpr uint32_t kTriggerPollPresents = 60;
  static constexpr const char* kTriggerFile = "native_gfx_rdc_trigger";

  void Initialize();
  // Ends a capture still open.
  void Shutdown();

  void Request() { requested_.store(true, std::memory_order_release); }

  // Render thread, right after a frame was presented.
  void OnPresented();

 private:
  const RenderDocApi100* api_ = nullptr;
  std::atomic<bool> requested_{false};
  bool capturing_ = false;
  uint64_t presents_ = 0;
  uint32_t captures_ = 0;
};

}  // namespace mcla::gpu
