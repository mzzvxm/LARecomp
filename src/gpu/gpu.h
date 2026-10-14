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
// Milestone M3 (this code): on top of M2's presentation (bindless heap, upload
// memory, host shaders, SDK overlays, idle presents, GPU timestamps, RenderDoc)
// -- every guest event that produces GPU work is captured with the device
// state it consumed and carried to the render thread in a byte stream (draws,
// clears, resolves, tiled rendering, the present), shader identities are exact,
// constants travel as versioned 64-byte blocks, and the render thread decodes
// and validates all of it against the shader pack (v4 since M4.1), the guest shader layout
// and the format tables..
//
// Milestone M4: the render thread draws it -- geometry
// (M4.4), the EDRAM targets with clears and tiling (M4.5), textures from guest
// memory, the resolve bridge and the guest's front buffer through the display
// gamma table (M4.6). Resolves written back into guest memory for the CPU, and
// memory read in another shape than it was resolved in, are M5.
//
// Call order (all on the UI thread except where noted):
//   CreateOverlayDrawer   from LarecompApp::OnCreateImmediateDrawer
//   Startup               from LarecompApp::SetupPresentation, after the base
//   BeforeGuest*/AfterGuest*, OnGuestFrameEnd, OnGuestSwap
//                         guest threads, from src/native_gfx/hooks.cpp
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

// --- guest capture (hooks.cpp) ------------------------------------------------
// Each hooked D3D call is bracketed: Before* runs before the original (it reads
// what the original consumes and clears -- the constant dirty masks, the state a
// clear or resolve reads), After* after it (the state the original set up). The
// token carries what Before* saw; an inactive token makes After* a no-op, so a
// call bracketed across startup or shutdown is simply not captured.

struct GuestCallToken {
  uint64_t vs_dirty = 0;  // D3DDevice +0 before the call
  uint64_t ps_dirty = 0;  // +8
  uint32_t snapshot = 0;  // event pre-state slot (1-based), 0 = none
  bool active = false;
};

enum class GuestEvent : uint8_t { kClear, kResolve, kTilingBegin, kTilingEnd };

// D3DDevice_DrawIndexedVertices / DrawVertices / BeginVertices.
GuestCallToken BeforeGuestDraw(const uint8_t* base, uint32_t device);
void AfterGuestDrawIndexed(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t primitive, int32_t base_vertex, uint32_t start_index,
                           uint32_t count);
void AfterGuestDrawVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                            uint32_t primitive, uint32_t start_vertex, uint32_t count);
// `buffer` is BeginVertices' return value: where the caller writes the vertices.
void AfterGuestBeginVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                             uint32_t primitive, uint32_t count, uint32_t stride,
                             uint32_t buffer);
void AfterGuestEndVertices(const uint8_t* base, uint32_t device);

// ClearF, Resolve, BeginTiling, EndTiling. `stack_pointer` is the caller's r1
// (Resolve reads ClearStencil and pParameters from the stack); 0 elsewhere.
GuestCallToken BeforeGuestEvent(const uint8_t* base, uint32_t device, GuestEvent event,
                                uint32_t stack_pointer);
// `caller` is the return address (LR) the hook was entered with.
void AfterGuestClear(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                     uint32_t caller, uint32_t flags, uint32_t rect, uint32_t color, float z,
                     uint32_t stencil);
void AfterGuestResolve(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                       uint32_t caller, uint32_t flags, uint32_t source_rect,
                       uint32_t dest_texture, uint32_t dest_point, uint32_t dest_level,
                       uint32_t dest_slice, uint32_t clear_color, float clear_z);
void AfterGuestBeginTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t caller, uint32_t flags, uint32_t tile_count, uint32_t rects,
                           uint32_t clear_color, float clear_z, uint32_t clear_stencil);
void AfterGuestEndTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                         uint32_t caller, uint32_t flags, uint32_t rects, uint32_t dest_texture,
                         uint32_t clear_color, float clear_z, uint32_t clear_stencil,
                         uint32_t params);

// rage::grcDevice::EndFrame, after the original. Keeps the game off its
// command-buffer cache: without a replay of our own, every draw has
// to reach the hooks. Applies whenever the mode is enabled.
void OnGuestFrameEnd(uint8_t* base);

// Guest render thread, D3DDevice_Swap, after the original (`device` and
// `front_buffer` are its r3/r4, read before it). Queues the present behind the
// frame's records and waits until the render thread ran it. No-op unless
// Startup succeeded and Shutdown has not begun.
void OnGuestSwap(const uint8_t* base, uint32_t device, uint32_t front_buffer);

// --- guest GPU fences ------------------------------------------
// The render thread plays the guest's GPU: it holds the write of the guest's
// retired fence from Startup to Shutdown and publishes each fence when it has
// executed every record captured before it.

// sub_82411CD0, AFTER the original. `value` is the device's issued counter
// read before it -- what the inserted EVENT_WRITE carries. Becomes a fence
// record when the capture is on.
void OnGuestFenceInserted(uint32_t device, uint32_t value);
// True while the render thread writes the retired fence and the capture is on.
// D3DDevice_BlockUntilIdle then waits on a real fence.
bool RetiresGuestFences();
// sub_82425C18, the guest's "GPU is hung" detector, in this mode: logs and
// returns; the hook makes the guest keep waiting. Any guest thread.
void OnGuestGpuHangCheck(const uint8_t* base, uint32_t device);
// A D3DDevice_BlockOnFence call that found its fence pending, and how long it
// waited. Any guest thread.
void NoteGuestFenceWait(uint64_t wait_ns);

// UI thread, on a window pixel-size change. The rebuild happens on the render
// thread at the next present (an idle one if the guest is not swapping).
void RequestResize();

// UI thread, at app shutdown: stops the capture, disconnects the overlay passes,
// stops the render thread (with a deadline), drains the GPU and releases every
// GPU object, device last, then hands the retired fence back to the vblank
// thread. Safe to call more than once.
void Shutdown();

}  // namespace mcla::gpu
