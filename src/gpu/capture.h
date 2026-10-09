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
// Requests come from F11 (FrameCapture::Request, any thread; the SDK's own F11
// bind calls an IGraphicsSystem that does not exist in this mode, so the
// renderer replaces it as the legacy runtime does) or from a file named
// native_gfx_rdc_trigger in the working directory, polled every
// kTriggerPollPresents presents and consumed by deleting it -- the protocol
// tools/rdc/rdc_launch.py already speaks.
//
// rex::ui::RenderDocAPI::Get() is the SDK's process-wide instance, so the
// capture state is shared with anything else in the process that brackets
// captures. Device and window are passed as null: every device, every window.
// A capture is never nested (IsFrameCapturing is checked first: the legacy
// runtime once stacked 177 starts with no end). The SDK finds RenderDoc with
// LoadLibrary, so a renderdoc.dll on PATH without injection yields an API that
// captures nothing; the end of such a capture is logged as FAILED.
//
// Threading: Request from any thread; everything else on the render thread.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace rex::ui {
class RenderDocAPI;
}

namespace mcla::gpu {

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
  rex::ui::RenderDocAPI* api_ = nullptr;
  std::atomic<bool> requested_{false};
  bool capturing_ = false;
  uint64_t presents_ = 0;
  uint32_t captures_ = 0;
};

}  // namespace mcla::gpu
