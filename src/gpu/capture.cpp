// MCLA GPU renderer -- RenderDoc capture of one frame. See capture.h.
//
// Protocol (trigger file, F11 replacing the SDK bind, no nesting) carried over
// from the legacy runtime: native_gfx.cpp and d3d12/renderdoc_hook.cpp.

#include "capture.h"

#include <cstdio>

#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

namespace mcla::gpu {

void FrameCapture::Initialize() {
  api_ = rex::ui::RenderDocAPI::Get();
  if (api_) {
    REXLOG_INFO("[gpu] RenderDoc is attached: F11 or a '{}' file captures the next frame",
                kTriggerFile);
  }
}

void FrameCapture::Shutdown() {
  if (api_ && capturing_) {
    const bool ok = api_->EndFrameCapture(nullptr, nullptr);
    REXLOG_INFO("[gpu] RenderDoc capture closed at shutdown: {}", ok ? "written" : "FAILED");
  }
  capturing_ = false;
  api_ = nullptr;
}

void FrameCapture::OnPresented() {
  ++presents_;
  // Polled even without RenderDoc so a stale trigger cannot fire much later in
  // a session that does have it.
  const bool file_request =
      presents_ % kTriggerPollPresents == 0 && std::remove(kTriggerFile) == 0;
  const bool key_request = requested_.exchange(false, std::memory_order_acq_rel);

  if (capturing_) {
    capturing_ = false;
    const bool ok = api_->EndFrameCapture(nullptr, nullptr);
    REXLOG_INFO("[gpu] RenderDoc capture {} ended: {}", captures_, ok ? "written" : "FAILED");
  }
  if (!file_request && !key_request) {
    return;
  }
  if (!api_) {
    REXLOG_ERROR("[gpu] RenderDoc capture requested, but the process was not started under "
                 "RenderDoc (tools/rdc/rdc_launch.py)");
    return;
  }
  if (api_->IsFrameCapturing()) {
    REXLOG_WARN("[gpu] RenderDoc is already capturing; not starting another capture");
    return;
  }
  api_->StartFrameCapture(nullptr, nullptr);
  capturing_ = true;
  ++captures_;
  REXLOG_INFO("[gpu] RenderDoc capture {} started at present {} ({})", captures_, presents_,
              key_request ? "F11" : kTriggerFile);
}

}  // namespace mcla::gpu
