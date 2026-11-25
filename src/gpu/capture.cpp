// MCLA GPU renderer -- RenderDoc capture of one frame. See capture.h.
//
// Protocol (trigger file, F11, no nesting) carried over from the legacy runtime:
// native_gfx.cpp and d3d12/renderdoc_hook.cpp. The API is declared here against RenderDoc's public,
// version-stable 1.0.0 layout (the same prefix renderdoc_hook.cpp uses) because the SDK's RenderDocAPI
// is private to its Xenia GPU module since 0.11.

#include "capture.h"

#include <cstdint>
#include <cstdio>

#include <rex/logging/log_category.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace mcla::gpu {

// The order of these members is part of RenderDoc's public contract and never changes for a given version,
// which is what makes declaring the prefix here safe: unused slots stay void* so no signature can be got
// subtly wrong.
struct RenderDocApi100 {
  void* GetAPIVersion;
  void* SetCaptureOptionU32;
  void* SetCaptureOptionF32;
  void* GetCaptureOptionU32;
  void* GetCaptureOptionF32;
  void* SetFocusToggleKeys;
  void* SetCaptureKeys;
  void* GetOverlayBits;
  void* MaskOverlayBits;
  void* Shutdown;
  void* UnloadCrashHandler;
  void* SetLogFilePathTemplate;
  void* GetLogFilePathTemplate;
  void* GetNumCaptures;
  void* GetCapture;
  void* TriggerCapture;
  void* IsRemoteAccessConnected;
  void* LaunchReplayUI;
  void* SetActiveWindow;
  void (*StartFrameCapture)(void* device, void* window);
  uint32_t (*IsFrameCapturing)();
  uint32_t (*EndFrameCapture)(void* device, void* window);
};

namespace {

using PfnGetApi = int (*)(uint32_t version, void** out_api);
constexpr uint32_t kApiVersion_1_0_0 = 10000;

// The in-application API when RenderDoc injected itself into this process, else null. GetModuleHandle /
// RTLD_NOLOAD, never a load: a library we loaded would give a live API that captures nothing.
const RenderDocApi100* FindRenderDocApi() {
  void* get_api_symbol = nullptr;
#if defined(_WIN32)
  if (HMODULE module = GetModuleHandleW(L"renderdoc.dll")) {
    get_api_symbol = reinterpret_cast<void*>(GetProcAddress(module, "RENDERDOC_GetAPI"));
  }
#elif !defined(__APPLE__)
  if (void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD)) {
    get_api_symbol = dlsym(module, "RENDERDOC_GetAPI");
  }
#endif
  if (!get_api_symbol) {
    return nullptr;
  }
  void* api = nullptr;
  if (reinterpret_cast<PfnGetApi>(get_api_symbol)(kApiVersion_1_0_0, &api) != 1 || !api) {
    REXLOG_WARN("[gpu] RENDERDOC_GetAPI(1.0.0) failed");
    return nullptr;
  }
  return static_cast<const RenderDocApi100*>(api);
}

}  // namespace

void FrameCapture::Initialize() {
  api_ = FindRenderDocApi();
  if (api_) {
    REXLOG_INFO("[gpu] RenderDoc is attached: F11 or a '{}' file captures the next frame",
                kTriggerFile);
  }
}

void FrameCapture::Shutdown() {
  if (api_ && capturing_) {
    const bool ok = api_->EndFrameCapture(nullptr, nullptr) != 0;
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
    const bool ok = api_->EndFrameCapture(nullptr, nullptr) != 0;
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
  if ((api_->IsFrameCapturing() != 0)) {
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
