#pragma once
// ===========================================================================
// MCLA GPU renderer (Plume) -- what the app and the guest hooks call
// ===========================================================================
// src/gpu/ is the renderer the project is moving to: Plume over D3D12 today,
// Vulkan from the same code, one backend per build. Design, references and
// milestones:.
//
// The mode is mcla_gpu. It owns the window's presentation, so it excludes the
// SDK's D3D12Presenter and the legacy D3D12 runtime (mcla_native_gfx), and it
// needs the no-command-processor mode (mcla_native_gfx_nocp) for a process
// with no emulated GPU in it. Enabled() checks all of that once and says what
// is missing.
//
// Milestone M2 (this code): on top of M1's device, swap chain, frames in
// flight, retirement and render thread -- the bindless heap, transient upload
// memory, host shaders, the SDK overlays (ImGui dialogs, F3, toasts) drawn on
// the render thread from frames the UI thread records, presents while the
// guest does not swap, GPU timestamps, RenderDoc capture, and a deadline on
// stopping the render thread. The guest's own image is not rendered yet:
// each frame is cleared to black under the overlays until M4.
//
// Call order (all on the UI thread except where noted):
//   CreateOverlayDrawer   from LarecompApp::OnCreateImmediateDrawer
//   Startup               from LarecompApp::SetupPresentation, after the base
//   OnGuestSwap           guest render thread, D3DDevice_Swap
//   RequestResize         window pixel-size changes
//   Shutdown              OnWindowCloseRequested and OnShutdown
// ===========================================================================

#include <cstdint>
#include <memory>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace mcla::gpu {

// mcla_gpu is set and every prerequisite holds. Decided on the first call and
// fixed for the process (the cvars it reads all require a restart).
bool Enabled();

// The ImmediateDrawer the SDK draws its overlays through in this mode, or null
// when the mode is off. Needs no device: it records CPU-side frames for the
// renderer, which may not exist yet when the SDK asks for it.
std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer();

// Creates the device, the swap chain on `window`, the renderer's components
// and the render thread, and connects the SDK's ImGui drawer (which must be
// using the drawer from CreateOverlayDrawer). False when any of it fails; the
// reason is logged and the app should not start.
bool Startup(rex::ui::Window* window, rex::ui::WindowedAppContext& app_context,
             rex::ui::ImGuiDrawer* imgui_drawer);

// Guest render thread, from D3DDevice_Swap. Queues one present and waits for
// it, so the guest never runs more than the frames in flight ahead of the GPU.
// No-op unless Startup succeeded and Shutdown has not begun.
void OnGuestSwap();

// UI thread, on a window pixel-size change. The rebuild happens on the render
// thread at the next present (an idle one if the guest is not swapping).
void RequestResize();

// UI thread, at app shutdown: disconnects the overlay passes, stops the render
// thread (with a deadline), drains the GPU and releases every GPU object,
// device last. Safe to call more than once.
void Shutdown();

}  // namespace mcla::gpu
