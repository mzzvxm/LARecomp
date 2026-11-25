#pragma once

// The one place the mixed output of the game passes through LARecomp's code on rexglue 0.11 (the take's sound, trailer toolkit
// M2c: mc_engine/cinematic/capture_audio.cpp).
//
// The SDK's audio system mixes the game's streams itself and hands the device a pull function (rex::apu::AudioFill) when it
// opens it: the device's thread calls it for every buffer, and each 256-sample period it mixes takes one frame off every
// stream's queue, folds and gains it, and gives the stream back a credit. audio_output.h's driver sits between the two: it gets
// the SDK's fill in OpenDevice and gives the real device a fill of its own, made here (Attach). Outside a take that fill is the
// SDK's, called as it is (under one mutex, which nothing else holds for more than a pull).
//
// A take Holds the tap. From then on the device's fill writes silence and does not call the SDK's mix, so nothing consumes the
// stream's queue and the game's mixer stops at its credit limit; the take then pulls the SDK's mix itself, one period at a time
// (Pull: 256 samples of 2 or 6 channels, the SDK's own fold and gain), exactly as many as the video frames so far need. Release
// gives the device its mix back.
//
// No SDK or game header in here (a std::function stands for the SDK's AudioFill), so audio_tap_test.cpp builds it on its own.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>

namespace mcla::audio {

class MixTap {
 public:
  // What the SDK's AudioFill is: fills `out` (interleaved f32, `channels` of 2 or 6) in full.
  using Fill = std::function<void(std::span<float> out, uint32_t channels)>;

  // The SDK's fill, as OpenDevice got it. Returns the fill to give the real device. Any thread.
  Fill Attach(Fill sdk_fill) {
    auto sdk = std::make_shared<Fill>(std::move(sdk_fill));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      fill_ = sdk;
    }
    return [this, sdk](std::span<float> out, uint32_t channels) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (held_) {
        std::fill(out.begin(), out.end(), 0.0f);
        ++silent_calls_;
        return;
      }
      (*sdk)(out, channels);
    };
  }

  // The device was closed (or never opened): no fill any more. A Hold stays, so a device that comes back during a take still gets
  // silence; Pull says no until it has.
  void Detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    fill_.reset();
  }

  bool HasDevice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fill_ != nullptr;
  }

  // The take takes the mix over: false when there is no open device (the SDK then mixes on its own timer and cannot be held back).
  // Returns once no call of the device's fill is running.
  bool Hold() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fill_) return false;
    held_ = true;
    return true;
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    held_ = false;
  }

  bool Held() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return held_;
  }

  // One pull of the SDK's mix for the take: false when the tap is not held or the device is gone (nothing is written).
  bool Pull(std::span<float> out, uint32_t channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!held_ || !fill_) return false;
    (*fill_)(out, channels);
    return true;
  }

  // Device calls that got silence because a take held the tap (for the log).
  uint64_t SilentCalls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return silent_calls_;
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<Fill> fill_;
  bool held_ = false;
  uint64_t silent_calls_ = 0;
};

}  // namespace mcla::audio
