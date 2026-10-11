#pragma once
// ===========================================================================
// MCLA GPU renderer -- frame trace (diagnostic)
// ===========================================================================
// Writes every record of chosen guest frames to a text file, raw: the device
// windows the capture copied (register window R, fetch window F, event window
// P, surface headers), the call arguments, the object headers. Nothing is
// interpreted here; tools/gpu/trace_analyze.py decodes it with the Xenos
// register and fetch-constant layouts (rex/graphics/xenos.h) and answers the
// questions the renderer is designed from (which EDRAM ranges a frame draws to and aliases, what every resolve
// copies where, which later fetch reads a resolve destination and how).
//
// Size: R and F are written whole at a frame's first draw and as changed
// dwords afterwards, so a 6000-draw frame stays in the tens of megabytes.
//
// Control: mcla_gpu_trace_frame = the guest frame (kPresent records executed)
// at which a dump starts (0 = off), mcla_gpu_trace_count = how many frames;
// the console command gpu_trace_next dumps the frame after the next present.
// Files go next to the executable as mcla_gpu_trace_<frame>.txt.
//
// Threading: OnRecord and Finish on the render thread only
// (Renderer::Execute); RequestNext from any thread.
// ===========================================================================

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "records.h"

namespace mcla::gpu {

class FrameTrace {
 public:
  FrameTrace() = default;
  ~FrameTrace();
  FrameTrace(const FrameTrace&) = delete;
  FrameTrace& operator=(const FrameTrace&) = delete;

  // Before a record is executed. `frames` is the number of guest frames
  // (kPresent records) executed so far: the frame the record belongs to.
  void OnRecord(uint64_t frames, const RecordHeader& header, const uint8_t* payload);

  // Any thread: dump the frame that starts after the next guest present.
  void RequestNext() { request_next_.store(true, std::memory_order_release); }

  // Closes a dump in progress (shutdown).
  void Finish() { Close(); }

 private:
  bool ShouldTrace(uint64_t frames);
  bool Open(uint64_t frames);
  void Close();
  void Printf(const char* format, ...);
  void WriteDraw(const RecordHeader& header, const uint8_t* payload);
  void WriteClear(const RecordHeader& header, const ClearRecord& r);
  void WriteResolve(const RecordHeader& header, const ResolveRecord& r);
  void WriteTilingBegin(const RecordHeader& header, const TilingBeginRecord& r);
  void WriteTilingEnd(const RecordHeader& header, const TilingEndRecord& r);
  void WritePresent(const RecordHeader& header, const PresentRecord& r);
  void WriteEventState(const EventState& state);
  void WriteHex(const char* key, const uint8_t* bytes, size_t size);
  // " <offset>=<dword>" for every dword of `now` that differs from `before`
  // (or every dword when `before` is null), offsets from `base`.
  void WriteDwordDelta(const char* key, uint32_t base, const uint8_t* before, const uint8_t* now,
                       size_t size);

  std::unique_ptr<std::ofstream> file_;
  uint64_t frame_ = 0;        // the frame the open file traces
  bool traced_any_ = false;
  uint64_t frames_left_ = 0;  // frames still to write after the open one
  uint64_t draws_ = 0;
  uint64_t records_ = 0;
  std::atomic<bool> request_next_{false};
  uint64_t requested_at_ = 0;  // frame count when gpu_trace_next was seen
  bool armed_next_ = false;
  bool have_windows_ = false;
  std::array<uint8_t, guest::kRegisterWindowBytes> registers_{};
  std::array<uint8_t, guest::kFetchShadowBytes> fetch_{};
  std::string line_;
};

}  // namespace mcla::gpu
