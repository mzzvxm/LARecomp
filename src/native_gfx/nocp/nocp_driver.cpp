#ifndef REXGLUE_HAS_XEO3_TARGET
// The no-command-processor renderer as an in-process Xenos driver. See nocp_driver.h for the design.

#include "nocp_driver.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_module.h>
#include <rex/driver_registration.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include <rex/logging/log_category.h>

#include "guest_gpu.h"
#include "gpu/gpu.h"

namespace mcla::native_gfx::nocp {

namespace {

std::atomic<bool> g_selected{false};
std::mutex g_frame_mutex;
std::condition_variable g_frame_signal;
uint64_t g_swap_count = 0;
uint64_t g_swap_seen = 0;

class NoCPGraphicsDriver final : public rex::gpu::IXenosGraphicsDriver {
 public:
  std::string_view GetName() const override { return kDriverName; }

  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_selected.store(true, std::memory_order_release);
    return rex::Ok();
  }

  void Shutdown() override {
    DetachTitle();
    g_selected.store(false, std::memory_order_release);
  }

  // The window the SDK opened. The Plume renderer starts from LarecompApp::OnInit (it needs the UI system for
  // DrawOverlays, which does not exist yet here), so this only logs what the window is.
  // CHECK: whether Plume should start here instead (earlier, in GraphicsModule::Init) with the NativeWindow.
  rex::Result<void> SetSurface(const rex::NativeWindow& window) override {
    REXLOG_INFO("[nocp] window type {} (the Plume renderer takes it from LarecompApp::OnInit)",
                static_cast<int>(window.type));
    return rex::Ok();
  }

  // The title is attached: the register window is routed to ReadRegister/WriteRegister by the SDK from here to
  // DetachTitle, and the vblank thread may call into the title.
  rex::Result<void> AttachTitle(const rex::TitleInfo& info, rex::IPhysicalMemory&, rex::IPhysicalWriteWatch&,
                                rex::IGuestInterrupts& interrupts) override {
    if (!Install(interrupts, info.video_mode)) {
      return rex::Err(rex::ErrorCategory::kRuntime, "the no-command-processor guest GPU could not be installed");
    }
    return rex::Ok();
  }

  void DetachTitle() override { nocp::Shutdown(); }

  rex::gpu::FrameStatus WaitForFrame(rex::u32 timeout_ms) override {
    std::unique_lock lock(g_frame_mutex);
    g_frame_signal.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] { return g_swap_count != g_swap_seen; });
    rex::gpu::FrameStatus status;
    status.new_frame = g_swap_count != g_swap_seen;
    g_swap_seen = g_swap_count;
    return status;
  }

  void ResizeSurface(rex::u32, rex::u32) override {
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::RequestResize();
#endif
  }

  // The Plume renderer presents on its own thread: no image for the SDK's present thread, which then only sleeps.
  bool BeginPresent() override { return false; }
  void EndPresent() override {}
  void RenderOverlays(ImDrawData&) override {}  // never reached: BeginPresent says no. Plume draws them itself.

  // The ring goes nowhere: the vblank thread retires fences and ring consumption (guest_gpu.cpp), as in 0.10.
  // CHECK: these two hand over exactly the pointers guest_gpu.cpp reads out of the D3DDevice struct
  // (block = dev+10896, RPtr writeback); a later cleanup can take them from here instead.
  void InitializeRingBuffer(rex::u32, rex::u32) override {}
  void EnableReadPointerWriteBack(rex::u32, rex::u32) override {}

  rex::u32 ReadRegister(rex::u32 index) override { return ReadGuestRegister(index); }
  void WriteRegister(rex::u32 index, rex::u32 value) override { WriteGuestRegister(index, value); }
};

std::unique_ptr<rex::IDriver> CreateDriver() { return std::make_unique<NoCPGraphicsDriver>(); }

constexpr std::array<rex::DriverRegistration, 1> kDrivers = {rex::DriverRegistration{
    rex::DriverType::kGraphics, kDriverName, rex::GraphicsFamily::kXenos, &CreateDriver}};

class NoCPModule final : public rex::IModule, public rex::IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override { return "larecomp-nocp"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return rex::Ok(); }
  void Disconnect() override {}
  rex::Result<void> Init() override { return rex::Ok(); }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }
};

}  // namespace

rex::IModule& Module() {
  static NoCPModule module;
  return module;
}

bool DriverSelected() { return g_selected.load(std::memory_order_acquire); }

void NotifyGuestSwap() {
  {
    std::lock_guard lock(g_frame_mutex);
    ++g_swap_count;
  }
  g_frame_signal.notify_all();
}

}  // namespace mcla::native_gfx::nocp

#endif  // REXGLUE_HAS_XEO3_TARGET
