// MCLA GPU renderer -- census of the guest memory the draws read. See
// memory_census.h.

#include "memory_census.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "draw_decode.h"
#include "guest_device.h"
#include "native_gfx/vertex_declaration.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_memory_census, false, "MCLA/GPU",
    "Watch every vertex and index range the executed draws read, the way the buffer cache will, "
    "and log every 600 frames what that cache would upload ("
    "24.21). Diagnostic: it write-protects guest pages.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

void MemoryCensus::OnDraw(const uint8_t* payload) {
  if (!enabled_) {
    return;
  }
  const DrawView view(payload);
  const DrawRecord& r = view.record();
  if (r.kind == DrawKind::kInline) {
    return;
  }
  // The streams the declaration names, each once.
  uint32_t streams = 0;
  if (!(r.flags & (kDrawDeclUnreadable | kDrawDeclTooLong))) {
    for (uint32_t d = 0; d < r.decl_count; ++d) {
      const nfx::DeclarationElement e = nfx::ReadDeclarationElement(view.decl_records(), d);
      if (e.stream == 0xFF) {
        break;  // D3DDECL_END
      }
      if (e.stream < 32) {
        streams |= 1u << e.stream;
      }
    }
  }
  for (uint32_t stream = 0; stream < 32; ++stream) {
    if (!(streams & (1u << stream))) {
      continue;
    }
    const uint32_t slot = nfx::FetchSlotForStream(stream);
    if (slot >= guest::kFetchGroupCount * 3) {
      continue;
    }
    uint32_t d0 = 0, d1 = 0;
    view.VertexFetch(slot, &d0, &d1);
    if ((d0 & 3u) != 3u) {
      continue;  // not a vertex fetch constant
    }
    const uint32_t size = d1 & 0x03FFFFFCu;
    if (size != 0) {
      ++vertex_uses_;
      Touch(d0 & 0xFFFFFFFCu, size, /*index=*/false);
    }
  }
  if (r.kind == DrawKind::kIndexed && !(r.flags & kDrawIndexUnreadable) && r.count != 0) {
    const uint32_t index_bytes = (r.index_dword0 & 0x80000000u) ? 4u : 2u;
    const uint32_t address = GuestMemory::ToPhysical(r.index_address) + r.start * index_bytes;
    ++index_uses_;
    Touch(address, r.count * index_bytes, /*index=*/true);
  }
}

void MemoryCensus::ApplyInvalidations() {
  if (!memory_.HasInvalidations()) {
    return;
  }
  drained_.clear();
  memory_.Drain(drained_);
  for (const GuestMemory::Range& range : drained_) {
    regions_.ForEachOverlap(range.start, range.length, [&](RegionIndex::Id id) {
      RegionState& state = state_[id];
      if (!state.written) {
        state.written = true;
        ++written_;
        written_bytes_ += regions_.region(id).length;
      }
    });
  }
}

void MemoryCensus::Touch(uint32_t start, uint32_t length, bool index) {
  // Invalidations first, as the cache will before it resolves a record.
  ApplyInvalidations();
  RegionIndex::Id id = regions_.Find(start, length);
  if (id == RegionIndex::kNone) {
    if (!memory_.Read(start, length)) {
      ++unreadable_;
      return;
    }
    id = regions_.Insert(start, length);
    if (state_.size() < regions_.id_bound()) {
      state_.resize(regions_.id_bound());
    }
    state_[id] = RegionState{};
    state_[id].index = index;
    memory_.Arm(start, length);
    ++new_regions_;
    new_bytes_ += length;
  } else if (state_[id].written) {
    // Arm before the copy the cache would make now.
    memory_.Arm(start, length);
    ++reuploads_;
    reupload_bytes_ += length;
  }
  RegionState& state = state_[id];
  if (state.last_frame != frame_) {
    state.last_frame = frame_;
    ++state.frames_used;
    if (state.written) {
      ++state.frames_written;
    }
  }
  state.written = false;
}

void MemoryCensus::OnPresent() {
  if (!enabled_) {
    return;
  }
  ++frame_;
  ++frames_;
  if (frames_ >= kWindowFrames) {
    Report("periodic");
  }
}

void MemoryCensus::Finish() {
  if (enabled_ && frames_ != 0) {
    Report("shutdown");
  }
}

void MemoryCensus::Report(const char* when) {
  uint64_t vertex_regions = 0, index_regions = 0, watched_bytes = 0;
  uint64_t always_written = 0, always_written_bytes = 0;
  for (size_t id = 0; id < regions_.id_bound(); ++id) {
    const RegionIndex::Region& region = regions_.region(RegionIndex::Id(id));
    if (regions_.Find(region.start, region.length) != RegionIndex::Id(id)) {
      continue;  // a freed id
    }
    const RegionState& state = state_[id];
    (state.index ? index_regions : vertex_regions) += 1;
    if (!state.written) {
      watched_bytes += region.length;
    }
    if (state.frames_used >= kAlwaysWrittenMinFrames &&
        state.frames_written * 100 >= state.frames_used * kAlwaysWrittenPercent) {
      ++always_written;
      always_written_bytes += region.length;
    }
  }
  const GuestMemory::Stats stats = memory_.TakeStats();
  const double f = double(frames_ ? frames_ : 1);
  constexpr double kMb = 1.0 / (1024.0 * 1024.0);
  REXLOG_INFO(
      "[gpu] memory census ({}, {} frames): {} regions ({} vertex, {} index), {:.1f} MiB watched | "
      "per frame: {:.0f} vertex + {:.0f} index uses, {:.2f} new ({:.3f} MB), {:.1f} re-uploads "
      "({:.3f} MB), {:.1f} regions written ({:.3f} MB), {:.1f} notifications ({:.3f} MB), {:.1f} "
      "arms ({:.3f} MB) | written in >= {}% of the frames they are used in: {} regions ({:.2f} "
      "MB) | unreadable {}",
      when, frames_, vertex_regions + index_regions, vertex_regions, index_regions,
      double(watched_bytes) * kMb, double(vertex_uses_) / f, double(index_uses_) / f,
      double(new_regions_) / f, double(new_bytes_) * kMb / f, double(reuploads_) / f,
      double(reupload_bytes_) * kMb / f, double(written_) / f, double(written_bytes_) * kMb / f,
      double(stats.notifications) / f, double(stats.notified_bytes) * kMb / f,
      double(stats.arms) / f, double(stats.armed_bytes) * kMb / f, kAlwaysWrittenPercent,
      always_written, double(always_written_bytes) * kMb, unreadable_);
  frames_ = 0;
  vertex_uses_ = index_uses_ = 0;
  new_regions_ = new_bytes_ = 0;
  reuploads_ = reupload_bytes_ = 0;
  written_ = written_bytes_ = 0;
  unreadable_ = 0;
}

}  // namespace mcla::gpu
