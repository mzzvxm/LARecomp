#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — application bring-up with no emulated GPU.
// See nocp_app.h for exactly what is and is not in the process in this mode.

#include "nocp_app.h"

#include <atomic>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "guest_gpu.h"

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_nocp, false, "MCLA/NativeGfx",
    "Run with NO emulated GPU in the process. Leaves gpu_plugin empty, so ReXApp never calls "
    "LoadGpuPlugin and rexgpu-xenos is never loaded: no command processor, no PM4 parser, no "
    "register file, no EDRAM render-target cache, no shader translator. The app then supplies "
    "what the guest still needs and what none of that was: the GPU register window, the vblank "
    "source, and a swapchain to present on. "
    "Decided once at startup -- whether the emulator is in the process is not a runtime setting.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_nocp_vsync, false, "MCLA/NativeGfx",
    "Frame pacing for the no-command-processor mode: on = the guest video mode's refresh rate, "
    "off = 1000 Hz, matching what the emulated path does with vsync off. "
    "This exists because the SDK's `vsync` cvar is defined in "
    "src/graphics/command_processor.cpp, so it lives in the GPU plugin and is simply ABSENT once "
    "that plugin is not loaded -- a `vsync` line in larecomp.toml is an unknown key in this mode, "
    "and querying it by name would silently answer false whatever it was set to.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::native_gfx::nocp {

namespace {

// Outlives the presenter it backs, so it is deliberately never freed: the
// window and the overlays hold raw pointers into both for the life of the
// process, and tearing them down in the right order at exit buys nothing on a
// path that ends in _Exit.
std::unique_ptr<rex::ui::d3d12::D3D12Provider> g_provider;
std::unique_ptr<rex::ui::Presenter> g_presenter;

}  // namespace

bool WantNoCommandProcessor() { return REXCVAR_GET(mcla_native_gfx_nocp); }

namespace {
std::atomic<const char*> g_last_hook{nullptr};
std::atomic<uint64_t> g_hook_calls{0};
std::atomic<uint64_t> g_frame_ends{0};
std::atomic<uint64_t> g_swaps{0};
}  // namespace

void NoteHook(const char* name) {
  g_last_hook.store(name, std::memory_order_relaxed);
  g_hook_calls.fetch_add(1, std::memory_order_relaxed);
}

const char* LastHookName() {
  const char* n = g_last_hook.load(std::memory_order_relaxed);
  return n ? n : "(none)";
}

uint64_t HookCallCount() { return g_hook_calls.load(std::memory_order_relaxed); }

rex::ui::d3d12::D3D12Provider* Provider() { return g_provider.get(); }
rex::ui::Presenter* PresenterPtr() { return g_presenter.get(); }

void NoteFrameEnd() { g_frame_ends.fetch_add(1, std::memory_order_relaxed); }
void NoteSwapCall() {
  g_swaps.fetch_add(1, std::memory_order_relaxed);

  // Publish the frame timing the F3 "Debug Frames" overlay reads.
  //
  // rex::perf::SetFrameStats has exactly one caller in the SDK:
  // src/graphics/command_processor.cpp:1404, on the swap packet. That is the
  // emulator, so in this mode nothing publishes and the overlay shows nothing
  // -- the counters left with the command processor. The swap is still the
  // frame boundary though, and this hook is on it, so the numbers can simply
  // come from here instead.
  static uint64_t last_tick = 0;
  const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  if (last_tick && now > last_tick && freq) {
    const uint64_t delta = now - last_tick;
    const int64_t frame_us = int64_t(delta * 1000000ull / freq);
    const int64_t fps = int64_t(freq / delta);
    rex::perf::SetFrameStats(frame_us, fps);
  }
  last_tick = now;
}
uint64_t FrameEndCount() { return g_frame_ends.load(std::memory_order_relaxed); }
uint64_t SwapCount() { return g_swaps.load(std::memory_order_relaxed); }

std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer() {
  if (!g_provider) {
    g_provider = rex::ui::d3d12::D3D12Provider::Create();
    if (!g_provider) {
      REXLOG_ERROR("[nocp] D3D12Provider::Create failed; the app cannot present");
      return nullptr;
    }
    REXLOG_INFO("[nocp] D3D12 provider created without a graphics system");
  }
  return g_provider->CreateImmediateDrawer();
}

bool AttachPresentation(rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui_drawer,
                        rex::ui::ImmediateDrawer* immediate_drawer) {
  if (!g_provider) {
    REXLOG_ERROR("[nocp] no D3D12 provider; OnCreateImmediateDrawer must run first");
    return false;
  }
  g_presenter = g_provider->CreatePresenter();
  if (!g_presenter) {
    REXLOG_ERROR("[nocp] CreatePresenter failed; the app cannot present");
    return false;
  }
  // The detached branch wired the overlays with a null presenter because in
  // that mode the app owns the surface. Now that one exists, hand it to
  // everything that paints.
  if (window) {
    window->SetPresenter(g_presenter.get());
  }
  if (immediate_drawer) {
    immediate_drawer->SetPresenter(g_presenter.get());
  }
  if (imgui_drawer) {
    imgui_drawer->SetPresenterAndImmediateDrawer(g_presenter.get(), immediate_drawer);
  }

  REXLOG_INFO("[nocp] presentation attached with no emulated GPU in the process");
  return true;
}

bool InstallGuestGpu() {
  // Only now, with no graphics system to have claimed it, can the guest's GPU
  // register window be taken over. Install() refuses if something already
  // mapped it, which is the check that this is a whole-path switch.
  if (!Install()) {
    REXLOG_ERROR(
        "[nocp] the guest GPU could not be installed; the game will run without a vblank and "
        "will not get past its first frame pace");
    return false;
  }
  return true;
}

}  // namespace mcla::native_gfx::nocp

#endif  // REXGLUE_HAS_XEO3_TARGET
