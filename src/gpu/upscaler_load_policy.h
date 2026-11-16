#pragma once
// ===========================================================================
// MCLA GPU renderer -- when the upscalers' runtime libraries are loaded (pure rules)
// ===========================================================================
// What this decides, and nothing else (no Windows, no renderer, so upscaler_load_policy_test.cpp
// checks it alone):
//
//   FidelityFX (temporal_ffx.cpp). Its loader and its upscaler DLL (amd_fidelityfx_upscaler_<api>.dll,
//   55 MiB on D3D12, 34 MiB on Vulkan) used to be opened by the probe at every start, on every GPU,
//   only to ask which upscaler versions the runtime offers: FSR 3.1 and, where its provider really
//   loads, FSR 4. FSR 4 is an RDNA 4 feature and the one that can only be known by asking the
//   runtime, so the question is still asked at the start on an AMD GPU. Every other GPU (NVIDIA,
//   Intel, anything else) can never be offered FSR 4: FSR 3.1 is listed from the checks that need no
//   runtime (the files are there, the device takes shader model 6.2) and the runtime is opened when
//   an FSR is first asked for, on the worker, with the frames drawn without it until the first
//   context is made (the live switch's own rule). An FSR that is the saved choice at the start
//   still loads at the start: the start decision (the scene's size) needs the runtime's answer.
//
//   DLSS (temporal_dlss.cpp). Nothing of NVIDIA's is loaded by the probe, and on a GPU that is not an
//   NVIDIA one nothing of it ever is. On D3D12 on an RTX card Streamline (sl.interposer.dll,
//   sl.common.dll, sl.dlss.dll and, on an RTX 40 or 50 series card, the frame generation, Reflex and
//   PC latency plugins, then nvngx_dlss.dll through NGX) used to start in the background five
//   seconds after the game first drew, whether DLSS was ever used or not (the warm-up: about ten
//   seconds of one worker thread, so that the first live switch to DLSS takes tens of milliseconds
//   and not ten seconds). The warm-up now waits for a sign that the player may switch: the UPSCALING
//   row was shown (the pause menu's UPSCALING page), or DLSS frame generation is chosen. DLSS chosen
//   itself starts Streamline as before (Request and Prepare). On Vulkan Streamline has to be loaded
//   before the instance and only when DLSS is the saved choice at the start: not lazy, not changed.
//
// The A/B switch: mcla_gpu_upscaler_lazy_load (debug only, launch argument, default on). Off gives
// the old behaviour exactly: FFX loaded by the probe on every GPU, the DLSS warm-up five seconds
// after the first draw.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace mcla::gpu::upscaler_load {

constexpr uint32_t kVendorAmd = 0x1002;
constexpr uint32_t kVendorNvidia = 0x10DE;
constexpr uint32_t kVendorIntel = 0x8086;

// ---- FidelityFX ----------------------------------------------------------------------------------

enum class FfxLoad : uint8_t {
  kAtStart,     // the probe opens the runtime and asks it (the old behaviour)
  kOnFirstUse,  // listed from the runtime-free checks; opened when an FSR is first asked for
};

struct FfxLoadInputs {
  bool lazy_enabled = true;   // mcla_gpu_upscaler_lazy_load
  uint32_t vendor = 0;        // the adapter's PCI vendor id
  bool fsr_saved = false;     // mcla_gpu_upscaler is fsr31 or fsr4 at the start
};

constexpr FfxLoad DecideFfxLoad(const FfxLoadInputs& in) {
  if (!in.lazy_enabled) {
    return FfxLoad::kAtStart;
  }
  if (in.fsr_saved) {
    return FfxLoad::kAtStart;  // the start's scene size needs the runtime's answer
  }
  if (in.vendor == kVendorAmd) {
    return FfxLoad::kAtStart;  // only the runtime can say whether FSR 4 is offered
  }
  return FfxLoad::kOnFirstUse;
}

// The lazily loaded runtime of one backend: nothing loaded yet, being loaded, loaded, or failed.
// Not thread safe: the owner keeps it under its own mutex.
class LazyRuntime {
 public:
  enum class State : uint8_t { kIdle, kLoading, kLoaded, kFailed };

  // Something needs the runtime. True exactly once, for the caller that has to start the load; false
  // while it is loading, after it loaded and after it failed.
  bool Need() {
    if (state_ != State::kIdle) {
      return false;
    }
    state_ = State::kLoading;
    return true;
  }
  // The load ended (the worker's report). Ignored unless it was loading.
  void Ended(bool ok) {
    if (state_ == State::kLoading) {
      state_ = ok ? State::kLoaded : State::kFailed;
    }
  }
  // The runtime was loaded by the start (not on demand): there is nothing to wait for.
  void LoadedAtStart(bool ok) { state_ = ok ? State::kLoaded : State::kFailed; }

  State state() const { return state_; }
  // Whether the menu may list what the runtime serves: until it has failed, it is listed.
  bool Listed() const { return state_ != State::kFailed; }
  bool NeverNeeded() const { return state_ == State::kIdle; }

 private:
  State state_ = State::kIdle;
};

// ---- DLSS ----------------------------------------------------------------------------------------

// Something the player did that makes a live switch to DLSS (or DLSS frame generation) likely. Set from
// any thread, read by the render thread.
class Interest {
 public:
  // True for the first call only (the caller logs it once).
  bool Note() { return !noted_.exchange(true, std::memory_order_relaxed); }
  bool Noted() const { return noted_.load(std::memory_order_relaxed); }
  void ClearForTest() { noted_.store(false, std::memory_order_relaxed); }

 private:
  std::atomic<bool> noted_{false};
};

struct WarmupInputs {
  bool lazy_enabled = true;     // mcla_gpu_upscaler_lazy_load
  bool interest = false;        // an Interest was noted
};

// Whether the DLSS warm-up (Streamline's start and the features, in the background) may begin. Its
// own delay after the first draw (temporal.h kPrewarmDelay) applies on top of this.
constexpr bool WarmupMayStart(const WarmupInputs& in) {
  return !in.lazy_enabled || in.interest;
}

}  // namespace mcla::gpu::upscaler_load
