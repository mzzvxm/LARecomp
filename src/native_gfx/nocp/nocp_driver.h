#pragma once
// ===========================================================================
// The no-command-processor renderer on rexglue 0.11: an in-process Xenos driver
// ===========================================================================
// What 0.10 did: LarecompApp left `gpu_plugin` empty, so rexgpu-xenos was never loaded, and nocp/guest_gpu.cpp mapped the
// GPU register window by hand (AddVirtualMappedRange), started its own XHostThread for the vblank and called the guest's
// interrupt callback through the function dispatcher. The Plume renderer owned the window's swap chain.
//
// What 0.11 offers for the same job, and what this file does with it (the spike, UPSTREAM-0.11-PLAN.md D3):
//
//   * TitleApp requires the Xenos graphics family. gpu.driver names the driver within the family; DriverModules
//     compiled into the program are found by name (rex::GetDriverModules skips the DLL of a name a code module
//     registers). So LARecomp registers ONE driver module, "larecomp", and the renderer choice is the SDK's own
//     setting: gpu.driver = larecomp (the default, Plume, no emulated GPU in the process) or any / d3d12 / vulkan (the
//     SDK's Xenia GPU, the old XENIA renderer). One TitleApp, no GetRequirements override, no gpu_plugin hack.
//   * The SDK attaches the GPU register window (0x7FC80000) to the driver in TitleState::AttachSystems, at the
//     right moment (after the memory exists, before the main thread starts) and detaches it again at unload: the driver
//     implements ReadRegister/WriteRegister(index). A graphics_family = kNone TitleApp cannot do this: the null
//     graphics system attaches its OWN register recipient in Launch (title_state.cpp:295), after OnTitleLaunch, and
//     an app has no IGuestInterrupts outside a driver's AttachTitle. (kNone works only with an SDK hook: see the report.)
//   * IGuestInterrupts (StartHostThread + RaiseGraphicsInterrupt) and the title's video mode arrive in AttachTitle.
//     The vblank thread, the fence and ring retirement and the flip capture stay in guest_gpu.cpp, which this driver
//     feeds.
//   * Presentation: the SDK runs a present thread when a driver exists (WaitForFrame / BeginPresent / overlays /
//     EndPresent). The Plume renderer has a render thread of its own and presents itself, so BeginPresent reports
//     "no image" (the present is skipped) and the renderer draws the overlays itself with IUISystem::DrawOverlays.
//     WaitForFrame is still woken by each guest swap, so IGraphicsSystem::GetFrameStats counts guest frames for free.
//
// CHECK items are marked in nocp_driver.cpp; none of this has been compiled.
// ===========================================================================

#include <string_view>

#include <rex/module.h>

namespace mcla::native_gfx::nocp {

// The driver's name in gpu.driver.
inline constexpr std::string_view kDriverName = "larecomp";

// The module to hand to App::OnRegisterModules. Lives for the whole process.
rex::IModule& Module();

// True once the SDK has selected this driver (gpu.driver = larecomp): the replacement of the old
// mcla_native_gfx_nocp switch. False before Init and when the XENIA renderer (a d3d12/vulkan driver) runs.
bool DriverSelected();

// Called from the guest's swap hook (nocp_app.cpp NoteSwapCall): wakes WaitForFrame.
void NotifyGuestSwap();

}  // namespace mcla::native_gfx::nocp
