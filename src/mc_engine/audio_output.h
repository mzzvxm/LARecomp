#pragma once

// Audio output that never fails to open, on rexglue 0.11.
//
// The game gives up on its whole audio engine when the host cannot open an output device, then dereferences that engine later and
// crashes. With no usable device (no speakers, a broken driver, an RDP session) the game must still get its frames consumed at the
// hardware's rate, so it keeps its timing; --mcla_audio_output=null forces it.
//
// 0.10: a subclass of the SDK's SDLAudioSystem swapped in a NullOutputDriver (an AudioDriver with its own clock thread that gave each
// submitted frame back one period later) when the SDL driver failed to open, and carried the trailer toolkit's capture driver.
// 0.11 splits the job, and most of it is the SDK's now (read from src/apu/audio_system.cpp, AudioSystem::Start):
//   * the audio system mixes; a driver is only IAudioDriver::OpenDevice(channels, AudioFill) / CloseDevice / IsDeviceLost. When
//     OpenDevice fails, or there is no driver, the mix runs on a timer thread, one 256-sample frame (5.333 ms at 48 kHz) per period, so
//     every stream still gets its "frame played" callbacks at the hardware's rate: that IS the null output. It then retries OpenDevice
//     about once a second (a device plugged in later is picked up), and a device that is lost is closed and reopened the same way.
//   * what it does NOT do: survive a driver that fails to INITIALIZE. AudioModule::Init returns "no audio driver is available" when
//     the selected drivers could not start and the app requires audio (TitleApp does), and apu.driver = none is only for an app that
//     does not (the batch actions of LarecompApp). The SDL3 driver's Initialize fails when SDL_InitSubSystem(SDL_INIT_AUDIO) fails (a
//     Linux machine with no sound server, for one).
// So this file is a code module serving ONE audio driver, "larecomp" (apu.driver = larecomp, set by LarecompApp), that wraps the SDK's
// "sdl3" driver and turns its failures into the SDK's own null path:
//   * Initialize never fails: if the SDL3 driver cannot start (or is not there) the wrapper has no inner driver and says so once;
//   * OpenDevice returns the inner driver's error, or an error when there is no inner driver or mcla_audio_output = "null": the SDK
//     then mixes on its timer. IsDeviceLost / CloseDevice pass through.
// The inner driver comes from the module that serves "sdl3" (rexapu-sdl3, which LarecompApp::OnModules loads: nothing else asks for
// it while apu.driver is "larecomp"): its DriverRegistration::create gives a fresh instance that this wrapper owns, initializes with
// the context it was given and shuts down itself.
//
// The trailer toolkit's capture driver (the take's sound, mc_engine/cinematic/capture_audio.cpp) is not carried over: see there.

#include <array>
#include <memory>
#include <span>
#include <string_view>

#include <rex/apu/audio_driver.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_module.h>
#include <rex/driver_registration.h>
#include <rex/logging/log_category.h>
#include <rex/module.h>

#include "larecomp_log.h"
#include "settings/larecomp_settings.h"

namespace mcla::audio {

// The driver's name in apu.driver, and the SDK driver it wraps.
inline constexpr std::string_view kDriverName = "larecomp";
inline constexpr std::string_view kInnerDriverName = "sdl3";
// The module that serves the inner driver; LarecompApp::OnModules adds it to the modules rex::Init loads.
inline constexpr const char* kInnerModuleName = "rexapu-sdl3";

namespace detail {

// The factory of the running app, kept by Module::Connect: the wrapper looks the inner driver up at Initialize, when every module has connected.
inline rex::IInterfaceFactory*& Factory() {
  static rex::IInterfaceFactory* factory = nullptr;
  return factory;
}

inline std::unique_ptr<rex::IDriver> CreateInnerDriver() {
  rex::IInterfaceFactory* factory = Factory();
  if (!factory) return nullptr;
  for (rex::IDriverModule* module : factory->FindAll<rex::IDriverModule>()) {
    for (const rex::DriverRegistration& registration : module->GetDrivers()) {
      if (registration.type == rex::DriverType::kAudio && registration.name == kInnerDriverName) {
        return registration.create();
      }
    }
  }
  return nullptr;
}

class OutputDriver final : public rex::apu::IAudioDriver {
 public:
  std::string_view GetName() const override { return kDriverName; }

  rex::Result<void> Initialize(rex::IDriverContext& context) override {
    if (larecomp::settings::mcla_audio_output.Get() == "null") {
      LARECOMP_APP_INFO("[audio] mcla_audio_output = null: the output is silent (the mix runs on its own clock)");
      return rex::Ok();
    }
    inner_ = CreateInnerDriver();
    if (!inner_) {
      LARECOMP_APP_WARN("[audio] the SDL3 audio driver is not available; continuing without sound");
      return rex::Ok();
    }
    if (auto started = inner_->Initialize(context); !started) {
      LARECOMP_APP_WARN("[audio] the SDL3 audio driver could not start ({}); continuing without sound", started.error().message);
      inner_.reset();
    }
    return rex::Ok();
  }

  void Shutdown() override {
    if (inner_) {
      inner_->Shutdown();
      inner_.reset();
    }
  }

  // The SDK's audio system calls this again about once a second while it has no device, so a "no output device" line is logged once.
  rex::Result<rex::u32> OpenDevice(rex::u32 channels, rex::apu::AudioFill fill) override {
    if (!inner_) {
      return rex::Err<rex::u32>(rex::ErrorCategory::kPlatform, "silent output (no audio driver, or mcla_audio_output = null)");
    }
    auto opened = Inner().OpenDevice(channels, std::move(fill));
    if (!opened && !warned_) {
      warned_ = true;
      LARECOMP_APP_WARN("[audio] no output device could be opened ({}); continuing without sound, and trying again about once a second",
                        opened.error().message);
    } else if (opened) {
      if (warned_) LARECOMP_APP_INFO("[audio] an output device opened");
      warned_ = false;
    }
    return opened;
  }

  void CloseDevice() override {
    if (inner_) Inner().CloseDevice();
  }

  bool IsDeviceLost() const override { return inner_ && static_cast<rex::apu::IAudioDriver*>(inner_.get())->IsDeviceLost(); }

 private:
  rex::apu::IAudioDriver& Inner() { return *static_cast<rex::apu::IAudioDriver*>(inner_.get()); }

  std::unique_ptr<rex::IDriver> inner_;
  bool warned_ = false;
};

inline std::unique_ptr<rex::IDriver> CreateDriver() { return std::make_unique<OutputDriver>(); }

inline constexpr std::array<rex::DriverRegistration, 1> kDrivers = {
    rex::DriverRegistration{rex::DriverType::kAudio, kDriverName, rex::GraphicsFamily::kNone, &CreateDriver}};

class Module final : public rex::IModule, public rex::IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override { return "larecomp-audio"; }
  rex::Result<void> Connect(rex::IInterfaceFactory& factory) override {
    Factory() = &factory;
    return rex::Ok();
  }
  void Disconnect() override { Factory() = nullptr; }
  rex::Result<void> Init() override { return rex::Ok(); }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }
};

}  // namespace detail

// The module to hand to App::OnRegisterModules. Lives for the whole process.
inline rex::IModule& Module() {
  static detail::Module module;
  return module;
}

}  // namespace mcla::audio
