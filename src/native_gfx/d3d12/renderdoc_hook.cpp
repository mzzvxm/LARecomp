#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — programmatic RenderDoc capture.
// See renderdoc_hook.h for why the API is declared here rather than included.

#include "renderdoc_hook.h"

#include <windows.h>

#include <rex/logging.h>

namespace mcla::native_gfx {

namespace {

// RenderDoc's in-application API, version 1.0.0. The order of these members is
// part of RenderDoc's public contract and never changes for a given version,
// which is what makes declaring the prefix here safe: only the members up to
// the ones actually called have to be right, and unused slots are kept as
// void* so no signature can be got subtly wrong.
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
  void(__cdecl* StartFrameCapture)(void* device, void* window);
  uint32_t(__cdecl* IsFrameCapturing)();
  uint32_t(__cdecl* EndFrameCapture)(void* device, void* window);
};

using PFN_GetAPI = int(__cdecl*)(uint32_t version, void** out_api);
constexpr uint32_t kApiVersion_1_0_0 = 10000;

RenderDocApi100* g_api = nullptr;
bool g_probed = false;

RenderDocApi100* Api() {
  if (g_probed) {
    return g_api;
  }
  g_probed = true;
  // GetModuleHandle, never LoadLibrary: the API is only meaningful when
  // RenderDoc injected itself into this process. Loading the DLL ourselves
  // would produce a live API that captures nothing.
  HMODULE module = GetModuleHandleW(L"renderdoc.dll");
  if (!module) {
    return nullptr;
  }
  auto get_api = reinterpret_cast<PFN_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
  if (!get_api) {
    REXLOG_WARN("[native_gfx] renderdoc.dll is loaded but RENDERDOC_GetAPI is missing");
    return nullptr;
  }
  void* api = nullptr;
  if (get_api(kApiVersion_1_0_0, &api) != 1 || !api) {
    REXLOG_WARN("[native_gfx] RENDERDOC_GetAPI(1.0.0) failed");
    return nullptr;
  }
  g_api = static_cast<RenderDocApi100*>(api);
  REXLOG_INFO("[native_gfx] RenderDoc in-application API acquired");
  return g_api;
}

}  // namespace

bool RenderDocAvailable() { return Api() != nullptr; }

bool RenderDocBeginCapture(void* device) {
  RenderDocApi100* api = Api();
  if (!api || !api->StartFrameCapture) {
    return false;
  }
  // Refuse to nest. RenderDoc holds exactly one capture at a time, and a second
  // StartFrameCapture while one is open does not start a new capture -- it
  // extends the open one and leaves RenderDoc believing it is still recording.
  // That state also kills RenderDoc's own capture key, since it will not begin a
  // capture while one is active, which is what "the hotkey does nothing" looks
  // like from outside. Measured: 177 starts and 0 ends in one session.
  if (api->IsFrameCapturing && api->IsFrameCapturing() != 0) {
    REXLOG_WARN("[native_gfx] RenderDoc capture already active; not starting another");
    return false;
  }
  api->StartFrameCapture(device, nullptr);
  REXLOG_INFO("[native_gfx] RenderDoc capture started");
  return true;
}

bool RenderDocEndCapture(void* device) {
  RenderDocApi100* api = Api();
  if (!api || !api->EndFrameCapture) {
    return false;
  }
  if (api->IsFrameCapturing && api->IsFrameCapturing() == 0) {
    // Nothing open: EndFrameCapture on an idle RenderDoc discards rather than
    // writes, so saying so beats reporting a capture that never existed.
    REXLOG_WARN("[native_gfx] RenderDoc end requested with no capture active");
    return false;
  }
  const bool ok = api->EndFrameCapture(device, nullptr) != 0;
  REXLOG_INFO("[native_gfx] RenderDoc capture ended: {}", ok ? "written" : "FAILED");
  return ok;
}

bool RenderDocIsCapturing() {
  RenderDocApi100* api = Api();
  return api && api->IsFrameCapturing && api->IsFrameCapturing() != 0;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
