#pragma once
// ===========================================================================
// MCLA GPU renderer -- the response to the video memory budget
// ===========================================================================
// The caches keep what they made for 600 frames (kEvictFrames) and the EDRAM
// targets for good. On a card with room that is free; on a card without it the
// OS pages the process's allocations to system memory over PCIe, which shows
// as frame times in the hundreds of milliseconds (the fork, which had this for
// its emulated renderer: a pretended 3.2 GB card went from 12 FPS to 171-196,
// peak 4.0 -> 2.7 GB at 2x).
//
// Every kCheckFrames frames, at the start of a frame, the process's use of the
// adapter's local video memory is read against the OS budget for it
// (Device::QueryVideoMemory: DXGI QueryVideoMemoryInfo on D3D12;
// VK_EXT_memory_budget on Vulkan, when the driver answers it). Past
// kHighPercent of the budget -- or when a texture or buffer creation failed
// for want of room (Device::TakeFailedBytes), which is the only sign a driver
// that reports no budget gives -- unused resources are released, the longest
// idle first, until about kTargetPercent of the budget is in use. In three
// passes: what has been idle for kIdleBands[0] frames, then [1], then
// kIdleBands[2] -- so lifetimes shorten as the shortage lasts, and nothing
// that was used in the last kIdleBands[2] frames (two seconds at 60 FPS) is
// ever touched: that is the working set, and releasing it would only make it
// again. The resources released are the ones a cache can make again from what
// it still has:
//   - inactive resolve products (a reservoir waiting for their key to be
//     resolved again; the active ones are what the guest last rendered there),
//   - EDRAM targets nothing drew into, cleared or resolved from,
//   - textures decoded from the resolve memory,
//   - textures decoded from guest memory (and unheld mod replacements).
// Each cache's own release keeps what the rest of the renderer holds a pointer
// to or a bindless slot of valid until the GPU is done with it (FrameRing
// serials, the TextureCache's epoch).
//
// One kind of resource has no lifetime of its own: an EDRAM target stays for
// good. Some screens draw the world at 4x MSAA (the fork's census ties it to the
// pause menu and the GPS map; the logs show three targets made once, at the
// start of a session), which is 72 MiB at 1x, 287 MiB at 2x and 646 MiB at 3x
// that nothing uses outside them, and that the shortage response above would
// release only once video memory ran short. So a target that nothing used for
// about a minute (kLazyIdleSeconds, and at least kLazyMinIdleFrames frames) is
// released with no shortage at all; the next use makes it again, cleared, as
// after a release under shortage (one creation, a few milliseconds, when the
// screen opens). The products, textures and decoded textures have their own
// idle limits (kEvictFrames, 600 frames).
//
// This costs nothing while the budget is not near: a check is one DXGI call
// every half second. Where the OS budget is the size of system memory (an
// integrated GPU: 15 GB on a 485 MiB one) the response does not trigger. A
// failed creation (Device::kBackoffFrames) triggers it on any adapter.
//
// What the response cannot release. Frame generation (frame_generation.h) holds images and a
// FidelityFX context that the caches cannot make again from what they keep, and the OS counts them
// in the use it reports, so they weigh on every check above. They are not trimmed. Instead what asks
// for them first asks HasRoomFor: when they would take the use past kHighPercent, they are not made,
// so that taking them never makes the response release the caches' resources (the textures, the
// products, the EDRAM targets) to fit them. AddHolder lists such memory, so that the lines that say
// why memory is short and what was released say how much of it the response had no hand in.
//
// Threading: render thread only.
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "backend.h"
#include "texture_bytes.h"

namespace mcla::gpu {

class Device;
class EdramTargets;
class ResolveCache;
class ResolveMemory;
class TextureCache;

class VideoMemoryBudget {
 public:
  // Frames between two looks at the budget.
  static constexpr uint64_t kCheckFrames = 30;
  // Releasing starts at this share of the budget in use, and aims at the lower one.
  static constexpr uint32_t kHighPercent = 90;
  static constexpr uint32_t kTargetPercent = 80;
  // Idle frames, in the order the passes take them. The last is the shortest a
  // resource is ever released after: twice the TextureCache's refresh of its
  // memoized bindings (kRefreshFrames), which is how current their last use is.
  static constexpr uint64_t kIdleBands[3] = {300, 180, 120};
  // On top of a failed request, released so the retry has room to land.
  static constexpr uint64_t kFailureMargin = 64ull << 20;
  // Most resources one cache releases in one pass of one check, so a release is
  // not a hitch (the driver frees each one on this thread).
  static constexpr uint32_t kMaxItems = 64;
  // Frames between two log lines about what was released.
  static constexpr uint64_t kLogFrames = 600;
  // An EDRAM target that nothing used for kLazyIdleSeconds (measured in frames from
  // the frame rate of the last checks, never fewer than kLazyMinIdleFrames) is released
  // even with no shortage, looked for every kLazyEveryChecks checks.
  static constexpr double kLazyIdleSeconds = 60.0;
  static constexpr uint64_t kLazyMinIdleFrames = 3600;
  static constexpr uint32_t kLazyEveryChecks = 10;

  // Whether `extra` bytes of new video memory keep the use under the line the response starts at
  // (kHighPercent of the budget, the use counted as the OS reports it now). True when the adapter
  // reports no budget: there is nothing to compare with, and a creation that fails is that adapter's
  // only sign. Any thread.
  static bool HasRoomFor(const backend::VideoMemory& memory, uint64_t extra) {
    return memory.budget == 0 || (memory.used + extra) * 100 < memory.budget * kHighPercent;
  }

  void Initialize(Device& device, TextureCache& textures, ResolveCache& resolve,
                  ResolveMemory& resolve_memory, EdramTargets& edram);
  void Shutdown();

  // Lists memory the response cannot release (see above): `bytes` is asked for the lines it writes
  // and is called on the render thread. The name is plain text for the log ("frame generation").
  void AddHolder(const char* name, std::function<uint64_t()> bytes);

  // At the start of a frame, before any record of it (Renderer::OpenFrameList),
  // after the caches' own BeginFrame.
  void BeginFrame();

 private:
  // What the adapter says is in use and the budget for it, now. The one
  // place the budget is read.
  backend::VideoMemory Sample();
  // Releases what has been idle, longest first, until `need` bytes are.
  // Returns the bytes released.
  uint64_t Trim(uint64_t need);
  // One line about what was released since the last, and the use at the check.
  void LogReleased(const backend::VideoMemory& memory);
  // With no shortage: releases the EDRAM targets that nothing used for about a
  // minute (see the top of this file). Every kLazyEveryChecks-th check.
  void ReleaseIdleTargets();
  // The frame rate between this check and the last, smoothed.
  void NoteFrameRate(uint64_t frame);
  // " | held here, not released by this: frame generation 1180 MiB", or nothing while the holders
  // hold nothing.
  std::string HoldersText() const;

  struct Window {
    TrimResult products;
    TrimResult targets;
    TrimResult decoded;
    TrimResult textures;
    uint64_t checks = 0;
    uint64_t short_checks = 0;
    uint64_t failures = 0;  // checks that a failed creation triggered
    uint64_t skipped = 0;   // creations the device refused (Device::kBackoffFrames)
    bool any() const {
      return products.items != 0 || targets.items != 0 || decoded.items != 0 ||
             textures.items != 0;
    }
  };

  struct Holder {
    const char* name = "";
    std::function<uint64_t()> bytes;
  };
  std::vector<Holder> holders_;
  Device* device_ = nullptr;
  TextureCache* textures_ = nullptr;
  ResolveCache* resolve_ = nullptr;
  ResolveMemory* resolve_memory_ = nullptr;
  EdramTargets* edram_ = nullptr;
  uint64_t next_check_ = 0;
  uint64_t last_log_ = 0;
  uint64_t last_onset_ = 0;  // the frame a shortage was last announced in; 0: never
  bool short_ = false;
  Window window_;
  // The frame rate of the last checks, from the clock, for the idle limit of the
  // targets released without a shortage.
  std::chrono::steady_clock::time_point last_check_time_{};
  uint64_t last_check_frame_ = 0;
  double frames_per_second_ = 0.0;  // 0: not measured yet
  uint32_t lazy_checks_ = 0;
};

}  // namespace mcla::gpu
