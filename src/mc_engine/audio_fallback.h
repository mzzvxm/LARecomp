#pragma once

// Audio backend selection with a silent fallback.
//
// The game aborts its audio engine init if the host cannot create an audio
// driver, and later dereferences the engine it never created. So a driver must
// always exist. SilentAudioDriver accepts frames, discards them and paces the
// guest at real-time rate. It is used when the host has no usable output
// device, or on request with --mcla_audio=none.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/thread.h>

REXCVAR_DEFINE_STRING(mcla_audio, "sdl", "MCLA",
                      "Audio output: sdl (falls back to silent if no device), or none");

namespace mcla {
namespace detail {

using rex::X_STATUS;

// One XAudio frame is 256 samples per channel at 48 kHz.
constexpr auto kFramePeriod = std::chrono::nanoseconds(256ull * 1'000'000'000ull / 48000ull);

class SilentAudioDriver : public rex::audio::AudioDriver {
 public:
  SilentAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), worker_([this] { Run(); }) {}

  ~SilentAudioDriver() override {
    running_ = false;
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void SubmitFrame(uint32_t /*samples_ptr*/) override { ++pending_; }

 private:
  void Run() {
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      next += kFramePeriod;
      std::this_thread::sleep_until(next);
      // Each consumed frame frees one slot for the guest, as a real device would.
      if (pending_.load(std::memory_order_relaxed) > 0) {
        --pending_;
        semaphore_->Release(1, nullptr);
      }
    }
  }

  rex::thread::Semaphore* semaphore_;
  std::atomic<int> pending_{0};
  std::atomic<bool> running_{true};
  std::thread worker_;
};

class FallbackAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher) {
    return std::make_unique<FallbackAudioSystem>(function_dispatcher);
  }

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    if (REXCVAR_GET(mcla_audio) != "none") {
      if (SDLAudioSystem::CreateDriver(index, semaphore, out_driver) == X_STATUS_SUCCESS) {
        return X_STATUS_SUCCESS;
      }
      REXLOG_WARN("audio: no usable output device, running silent");
    }
    *out_driver = new SilentAudioDriver(memory_, semaphore);
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override {
    if (auto* silent = dynamic_cast<SilentAudioDriver*>(driver)) {
      delete silent;
      return;
    }
    SDLAudioSystem::DestroyDriver(driver);
  }
};

}  // namespace detail

inline std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher) {
  return detail::FallbackAudioSystem::Create(function_dispatcher);
}

}  // namespace mcla
