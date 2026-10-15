// MCLA GPU renderer -- frame trace. See frame_trace.h.

#include "frame_trace.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "backend.h"
#include "draw_decode.h"

REXCVAR_DEFINE_UINT64(mcla_gpu_trace_frame, 0, "MCLA/GPU",
                      "Write a raw trace of the guest frame that begins after this many guest "
                      "frames (0 = off) to mcla_gpu_trace_<frame>.txt next to the executable. "
                      "Decoded by tools/gpu/trace_analyze.py. A 6000-draw frame is tens of MB.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(mcla_gpu_trace_count, 1, "MCLA/GPU",
                      "How many consecutive frames mcla_gpu_trace_frame writes, one file each.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

// The renderer's trace, for the console command, which only raises an atomic
// flag on it.
std::atomic<FrameTrace*> g_trace_for_command{nullptr};

}  // namespace

}  // namespace mcla::gpu

REXCVAR_DEFINE_COMMAND(
    gpu_trace_next,
    [] {
      if (mcla::gpu::FrameTrace* t =
              mcla::gpu::g_trace_for_command.load(std::memory_order_acquire)) {
        t->RequestNext();
        REXLOG_INFO("[gpu] trace: the frame after the next present will be written");
      }
    },
    "MCLA/GPU", "Write a raw trace of the next guest frame (see mcla_gpu_trace_frame).");

namespace mcla::gpu {

FrameTrace::~FrameTrace() {
  Close();
  FrameTrace* self = this;
  g_trace_for_command.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

bool FrameTrace::ShouldTrace(uint64_t frames) {
  if (g_trace_for_command.load(std::memory_order_relaxed) != this) {
    g_trace_for_command.store(this, std::memory_order_release);
  }
  if (file_) {
    return true;
  }
  if (frames_left_ > 0 && frames == frame_ + 1) {
    --frames_left_;
    return Open(frames);
  }
  if (request_next_.exchange(false, std::memory_order_acq_rel)) {
    armed_next_ = true;
    requested_at_ = frames;
  }
  if (armed_next_ && frames > requested_at_) {
    armed_next_ = false;
    frames_left_ = 0;
    return Open(frames);
  }
  const uint64_t target = REXCVAR_GET(mcla_gpu_trace_frame);
  if (target != 0 && frames == target && !(traced_any_ && frame_ == target)) {
    const uint32_t count = REXCVAR_GET(mcla_gpu_trace_count);
    frames_left_ = count > 1 ? count - 1 : 0;
    return Open(frames);
  }
  return false;
}

bool FrameTrace::Open(uint64_t frames) {
  const std::filesystem::path path = rex::filesystem::GetExecutableFolder() /
                                     ("mcla_gpu_trace_" + std::to_string(frames) + ".txt");
  frame_ = frames;
  traced_any_ = true;
  file_ = std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc);
  if (!file_->is_open()) {
    REXLOG_ERROR("[gpu] trace: could not open {}", path.string());
    file_.reset();
    frames_left_ = 0;
    return false;
  }
  draws_ = 0;
  records_ = 0;
  have_windows_ = false;
  Printf("# mcla gpu frame trace v1 frame=%" PRIu64
         " backend=%s window_r=%u+%u window_f=%u+%u window_p=%u+%u\n",
         frames, BackendName(), guest::kRegisterWindowBegin, guest::kRegisterWindowBytes,
         guest::kDevFetchShadow, guest::kFetchShadowBytes, guest::kEventWindowBegin,
         guest::kEventWindowBytes);
  REXLOG_INFO("[gpu] trace: writing frame {} to {}", frames, path.string());
  return true;
}

void FrameTrace::Close() {
  if (!file_) {
    return;
  }
  Printf("# end records=%" PRIu64 " draws=%" PRIu64 "\n", records_, draws_);
  file_->close();
  const bool ok = !file_->fail();
  file_.reset();
  if (ok) {
    REXLOG_INFO("[gpu] trace: frame {} written ({} records, {} draws)", frame_, records_, draws_);
  } else {
    REXLOG_ERROR("[gpu] trace: writing frame {} failed", frame_);
  }
}

void FrameTrace::Printf(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (n > 0) {
    file_->write(text, std::min<std::streamsize>(n, sizeof(text) - 1));
  }
}

void FrameTrace::WriteHex(const char* key, const uint8_t* bytes, size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  line_.clear();
  line_.reserve(size * 2 + 16);
  line_ += "  ";
  line_ += key;
  line_ += ' ';
  for (size_t i = 0; i < size; ++i) {
    line_ += kDigits[bytes[i] >> 4];
    line_ += kDigits[bytes[i] & 15];
  }
  line_ += '\n';
  file_->write(line_.data(), std::streamsize(line_.size()));
}

void FrameTrace::WriteDwordDelta(const char* key, uint32_t base, const uint8_t* before,
                                 const uint8_t* now, size_t size) {
  line_.clear();
  line_ += "  ";
  line_ += key;
  char text[32];
  for (size_t at = 0; at + 4 <= size; at += 4) {
    if (before && std::memcmp(before + at, now + at, 4) == 0) {
      continue;
    }
    std::snprintf(text, sizeof(text), " %u=%08x", unsigned(base + at), LoadBe32(now + at));
    line_ += text;
  }
  line_ += '\n';
  file_->write(line_.data(), std::streamsize(line_.size()));
}

void FrameTrace::OnRecord(uint64_t frames, const RecordHeader& header, const uint8_t* payload) {
  if (!ShouldTrace(frames) || !file_) {
    return;
  }
  ++records_;
  switch (header.type) {
    case RecordType::kDraw:
      WriteDraw(header, payload);
      break;
    case RecordType::kClear:
      WriteClear(header, *reinterpret_cast<const ClearRecord*>(payload));
      break;
    case RecordType::kResolve:
      WriteResolve(header, *reinterpret_cast<const ResolveRecord*>(payload));
      break;
    case RecordType::kTilingBegin:
      WriteTilingBegin(header, *reinterpret_cast<const TilingBeginRecord*>(payload));
      break;
    case RecordType::kTilingEnd:
      WriteTilingEnd(header, *reinterpret_cast<const TilingEndRecord*>(payload));
      break;
    case RecordType::kPresent:
      WritePresent(header, *reinterpret_cast<const PresentRecord*>(payload));
      Close();
      break;
    case RecordType::kFence: {
      const auto& r = *reinterpret_cast<const FenceRecord*>(payload);
      Printf("FN seq=%" PRIu64 " dev=%08x value=%u\n", header.sequence, r.device, r.value);
      break;
    }
    case RecordType::kLock: {
      const auto& r = *reinterpret_cast<const LockRecord*>(payload);
      Printf("LK seq=%" PRIu64 " res=%08x level=%u addr=%08x size=%u\n", header.sequence,
             r.resource, r.level, r.address, r.size);
      break;
    }
    default:
      Printf("? type=%u seq=%" PRIu64 " size=%u\n", unsigned(header.type), header.sequence,
             header.size);
      break;
  }
}

void FrameTrace::WriteDraw(const RecordHeader& header, const uint8_t* payload) {
  const DrawView view(payload);
  const DrawRecord& r = view.record();
  Printf("D %" PRIu64 " seq=%" PRIu64
         " dev=%08x kind=%u prim=%u count=%u start=%u bv=%d istride=%u ibytes=%u flags=%08x "
         "vs=%016" PRIx64 " ps=%016" PRIx64 " vsv=%u vso=%08x pso=%08x vsu=%08x:%u psu=%08x:%u "
         "decl=%08x:%u ib=%08x:%08x:%08x dflags=%08x viz=%u:%u vsd=%u psd=%u\n",
         draws_, header.sequence, r.device, unsigned(r.kind), r.primitive, r.count, r.start,
         r.base_vertex, r.inline_stride, r.inline_bytes, r.flags, r.vs_identity, r.ps_identity,
         unsigned(r.vs_variant), r.vs_object, r.ps_object, r.vs_ucode_address, r.vs_ucode_size,
         r.ps_ucode_address, r.ps_ucode_size, r.decl_object, unsigned(r.decl_count),
         r.index_object, r.index_dword0, r.index_address, r.device_flags,
         unsigned(r.viz_query_index), unsigned(r.viz_query_id), unsigned(r.vs_delta_count),
         unsigned(r.ps_delta_count));
  Printf("  surf %08x %08x %08x %08x %08x\n", r.surfaces[0], r.surfaces[1], r.surfaces[2],
         r.surfaces[3], r.surfaces[4]);
  line_.clear();
  line_ += "  tex";
  char text[32];
  for (uint32_t s = 0; s < guest::kSamplerCount; ++s) {
    if (r.sampler_textures[s] != 0) {
      std::snprintf(text, sizeof(text), " %u=%08x", s, r.sampler_textures[s]);
      line_ += text;
    }
  }
  line_ += '\n';
  file_->write(line_.data(), std::streamsize(line_.size()));
  WriteHex("strides", r.stream_strides, sizeof(r.stream_strides));
  if (r.decl_count != 0) {
    WriteHex("decl", view.decl_records(), size_t(r.decl_count) * guest::kDeclElementBytes);
  }
  // Windows R and F: whole at the frame's first draw, then the dwords that
  // changed since the previous draw.
  WriteDwordDelta("R", guest::kRegisterWindowBegin, have_windows_ ? registers_.data() : nullptr,
                  r.registers, sizeof(r.registers));
  WriteDwordDelta("F", guest::kDevFetchShadow, have_windows_ ? fetch_.data() : nullptr, r.fetch,
                  sizeof(r.fetch));
  std::memcpy(registers_.data(), r.registers, sizeof(r.registers));
  std::memcpy(fetch_.data(), r.fetch, sizeof(r.fetch));
  have_windows_ = true;
  ++draws_;
}

void FrameTrace::WriteEventState(const EventState& state) {
  WriteHex("P", state.pre, sizeof(state.pre));
  WriteHex("S", &state.surfaces[0][0], sizeof(state.surfaces));
  // Window R after the call, against the same bytes of P before it.
  WriteDwordDelta("RP", guest::kRegisterWindowBegin, state.pre, state.post, sizeof(state.post));
}

void FrameTrace::WriteClear(const RecordHeader& header, const ClearRecord& r) {
  Printf("C seq=%" PRIu64
         " dev=%08x caller=%08x flags=%x z=%.9g stencil=%u rect=%08x color=%08x eflags=%x\n",
         header.sequence, r.device, r.caller, r.clear_flags, double(r.z), r.stencil, r.rect_ptr,
         r.color_ptr, r.flags);
  WriteHex("rect", r.rect, sizeof(r.rect));
  WriteHex("color", r.color, sizeof(r.color));
  WriteEventState(r.state);
}

void FrameTrace::WriteResolve(const RecordHeader& header, const ResolveRecord& r) {
  Printf("X seq=%" PRIu64
         " dev=%08x caller=%08x flags=%08x rect=%08x dest=%08x point=%08x level=%u slice=%u "
         "ccolor=%08x z=%.9g stencil=%u params=%08x eflags=%x\n",
         header.sequence, r.device, r.caller, r.resolve_flags, r.src_rect_ptr, r.dest_ptr,
         r.dest_point_ptr, r.dest_level, r.dest_slice, r.clear_color_ptr, double(r.clear_z),
         r.clear_stencil, r.params_ptr, r.flags);
  WriteHex("rect", r.src_rect, sizeof(r.src_rect));
  WriteHex("point", r.dest_point, sizeof(r.dest_point));
  WriteHex("color", r.clear_color, sizeof(r.clear_color));
  WriteHex("dest", r.dest_header, sizeof(r.dest_header));
  WriteEventState(r.state);
}

void FrameTrace::WriteTilingBegin(const RecordHeader& header, const TilingBeginRecord& r) {
  Printf("TB seq=%" PRIu64
         " dev=%08x caller=%08x flags=%x tiles=%u rects=%08x ccolor=%08x z=%.9g stencil=%u "
         "eflags=%x\n",
         header.sequence, r.device, r.caller, r.tiling_flags, r.tile_count, r.rects_ptr,
         r.clear_color_ptr, double(r.clear_z), r.clear_stencil, r.flags);
  WriteHex("rects", &r.rects[0][0], sizeof(r.rects));
  WriteHex("color", r.clear_color, sizeof(r.clear_color));
  WriteEventState(r.state);
}

void FrameTrace::WriteTilingEnd(const RecordHeader& header, const TilingEndRecord& r) {
  Printf("TE seq=%" PRIu64
         " dev=%08x caller=%08x flags=%x rects=%08x dest=%08x ccolor=%08x z=%.9g stencil=%u "
         "params=%08x nested=%u eflags=%x\n",
         header.sequence, r.device, r.caller, r.resolve_flags, r.rects_ptr, r.dest_ptr,
         r.clear_color_ptr, double(r.clear_z), r.clear_stencil, r.params_ptr, r.nested_resolves,
         r.flags);
  WriteHex("rects", &r.rects[0][0], sizeof(r.rects));
  WriteHex("color", r.clear_color, sizeof(r.clear_color));
  WriteHex("dest", r.dest_header, sizeof(r.dest_header));
  WriteEventState(r.state);
}

void FrameTrace::WritePresent(const RecordHeader& header, const PresentRecord& r) {
  Printf("PR seq=%" PRIu64 " dev=%08x front=%08x flags=%x draws=%u records=%u bytes=%" PRIu64
         " capture_ns=%" PRIu64 "\n",
         header.sequence, r.device, r.front_buffer, r.flags, r.stats.draws, r.stats.records,
         r.stats.bytes, r.stats.capture_ns);
  WriteHex("front", r.front_buffer_header, sizeof(r.front_buffer_header));
}

}  // namespace mcla::gpu
