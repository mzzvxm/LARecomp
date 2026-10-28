#pragma once

// Audio output that never fails to open.
//
// The game gives up on its whole audio engine when the host cannot open an
// output device, then dereferences that engine later and crashes. With no
// usable device (no speakers, a broken driver, an RDP session) the SDL system
// is swapped for a null output that plays nothing but consumes frames at the
// hardware's rate, so the guest keeps its timing. --mcla_audio_output=null
// forces it.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

REXCVAR_DEFINE_STRING(mcla_audio_output, "auto", "MCLA/Audio",
                      "auto: the default device, or silence when none opens. null: always silent.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::audio {

// XAudio hands out 256-sample frames at 48 kHz: 5.333 ms each.
inline constexpr std::chrono::nanoseconds kNullFramePeriod{256'000'000'000ll / 48'000};

class NullOutputDriver final : public rex::audio::AudioDriver {
 public:
  NullOutputDriver(rex::memory::Memory* memory, rex::thread::Semaphore* frame_semaphore)
      : AudioDriver(memory), frame_semaphore_(frame_semaphore), clock_([this] { Consume(); }) {}

  ~NullOutputDriver() override {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_one();
    clock_.join();
  }

  void SubmitFrame(uint32_t) override {
    {
      std::lock_guard lock(mutex_);
      ++queued_;
    }
    wake_.notify_one();
  }

 private:
  // Hands each submitted frame back one period after the previous one, the way
  // a device that finished playing it would.
  void Consume() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point due{};
    std::unique_lock lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return stop_ || queued_ > 0; });
      if (stop_) return;
      const auto now = Clock::now();
      // After an idle stretch start a fresh schedule instead of releasing a
      // burst of frames to catch up.
      due = (due + kNullFramePeriod < now) ? now + kNullFramePeriod : due + kNullFramePeriod;
      if (wake_.wait_until(lock, due, [this] { return stop_; })) return;
      --queued_;
      lock.unlock();
      frame_semaphore_->Release(1, nullptr);
      lock.lock();
    }
  }

  rex::thread::Semaphore* frame_semaphore_;
  std::mutex mutex_;
  std::condition_variable wake_;
  uint32_t queued_ = 0;
  bool stop_ = false;
  std::thread clock_;  // last: it reads the members above
};

class OutputWithFallback final : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;

  rex::X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                             rex::audio::AudioDriver** out_driver) override {
    if (REXCVAR_GET(mcla_audio_output) != "null") {
      if (SDLAudioSystem::CreateDriver(index, semaphore, out_driver) == X_STATUS_SUCCESS) {
        return X_STATUS_SUCCESS;
      }
      REXLOG_WARN("[audio] no output device could be opened; continuing without sound");
    }
    *out_driver = new NullOutputDriver(memory_, semaphore);
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override {
    if (auto* null_driver = dynamic_cast<NullOutputDriver*>(driver)) {
      delete null_driver;
    } else {
      SDLAudioSystem::DestroyDriver(driver);
    }
  }
};

inline std::unique_ptr<rex::system::IAudioSystem> CreateAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<OutputWithFallback>(function_dispatcher);
}

}  // namespace mcla::audio
