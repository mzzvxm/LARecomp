#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — application bring-up with no emulated GPU
// ===========================================================================
// The three calls LarecompApp makes to run without rexgpu-xenos in the
// process at all.
//
// What "no emulated GPU" means here, precisely, because the distinction has
// been got wrong before:
//
//   * ReXApp::SetupPresentation loads the GPU plugin only when gpu_plugin is
//     non-empty (src/ui/rex_app.cpp). Leaving it empty means rexgpu-xenos is
//     never loaded into the process -- the command processor, PM4 parser,
//     register file, EDRAM render-target cache, shared memory and shader
//     translator are all in that plugin, and none of them exist.
//   * What IS used is src/ui/d3d12: D3D12Provider, D3D12Presenter, the ImGui
//     immediate drawer. That directory is a swapchain, descriptor pools,
//     upload buffers and a submission tracker -- there is no PM4, Xenos,
//     EDRAM, register file or shader translator anywhere in it. It is the
//     same kind of thing as DXGI, and the emulator merely happened to use it
//     too.
//   * The kernel's Vd* exports already have a designed path for a process
//     with no GPU emulation ("no GPU emulation loaded (gpu_plugin not set);
//     call ignored"), and MCLA's D3D device init survives it: its ring buffer
//     setup only needs MmGetPhysicalAddress, which belongs to the memory
//     subsystem.
//
// What the guest still needs, and what nocp/guest_gpu.cpp supplies, is the
// GPU register window and the vblank -- neither of which is drawing.
// ===========================================================================

#include <memory>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Presenter;
class Window;
namespace d3d12 {
class D3D12Provider;
}  // namespace d3d12
}  // namespace rex::ui

namespace mcla::native_gfx::nocp {

// The master switch, read before the runtime is built. Requires a restart:
// whether the emulated GPU is in the process is decided once, at startup.
bool WantNoCommandProcessor();

// Detached mode's drawer. Creates the D3D12 provider as a side effect, since
// ReXApp asks for the drawer before the app gets a chance to run anything
// else.
std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer();

// Called after ReXApp::SetupPresentation has opened the window and taken its
// detached branch. Creates the presenter over the provider and attaches it to
// the window and the overlays. Returns false only when the process cannot
// present at all.
bool AttachPresentation(rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui_drawer,
                        rex::ui::ImmediateDrawer* immediate_drawer);

// Installs the guest-visible GPU: the register window and the vblank source.
//
// Deliberately NOT part of AttachPresentation. ReXApp::Run calls
// SetupPresentation (line 117) BEFORE ConstructRuntime (line 159), and the
// memory system and kernel state are created inside runtime_->Setup. Mapping
// an MMIO range needs both, so this belongs to OnPostSetup, which runs at the
// end of ConstructRuntime.
bool InstallGuestGpu();

// Records that the guest entered a hooked D3D function, by name. One relaxed
// store; the pointer is a string literal with static lifetime.
//
// This is the only way to tell WHERE a guest that has gone quiet is stuck: the
// fence pair, the write pointer and the kick limit all say what it is NOT
// waiting on, and none of them say what it is. The last function entered, plus
// a total that stops moving, says it directly.
void NoteHook(const char* name);

// The two calls that say whether the guest is completing frames at all: the
// end of a rendered frame and the swap. A last-hook name cannot answer that --
// draws drown it out -- and "is it swapping" decides whether the present path
// is missing or simply never reached.
void NoteFrameEnd();
void NoteSwapCall();

// The provider and presenter this mode owns, for the native renderer to attach
// to. Both are null until AttachPresentation has run.
//
// They are NOT the emulator's. The graphics system is what normally hands
// these out, and it does not exist here -- these were created straight from
// rex::ui::d3d12, which is a swapchain and descriptor plumbing with no PM4,
// EDRAM or register file anywhere in it.
rex::ui::d3d12::D3D12Provider* Provider();
rex::ui::Presenter* PresenterPtr();

// For the periodic report in guest_gpu.cpp.
const char* LastHookName();
uint64_t HookCallCount();
uint64_t FrameEndCount();
uint64_t SwapCount();

}  // namespace mcla::native_gfx::nocp
