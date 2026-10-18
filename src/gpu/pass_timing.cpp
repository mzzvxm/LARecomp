// MCLA GPU renderer -- GPU time per pass. See pass_timing.h.

#include "pass_timing.h"

#include <algorithm>
#include <format>

#include <rex/logging.h>

#include "backend.h"
#include "device.h"

namespace mcla::gpu {

namespace {

// Enough for a measured MCLA frame (~150 boundaries) with room; a frame with
// more grows the pools (Grow) up to the ceiling.
constexpr uint32_t kInitialMarks = 256;
constexpr uint32_t kMaxMarks = 8192;
constexpr size_t kReportedLabels = 16;
constexpr const char* kFrameStart = "(frame start)";
constexpr const char* kFrameEnd = "(frame end)";

}  // namespace

PassTiming::~PassTiming() {
  Shutdown();
}

void PassTiming::Initialize(Device& device, bool enabled) {
  if (enabled_ || !enabled) {
    return;
  }
  if (!backend::TimestampsSupported(device.device())) {
    REXLOG_WARN("[gpu] pass timing asked for, but {} cannot write timestamps on its graphics "
                "queue; it stays off",
                device.backend_info());
    return;
  }
  device_ = &device;
  wanted_ = kInitialMarks;
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    pools_[slot] = device.CreateQueryPool(kInitialMarks, "pass timestamps");
    if (!pools_[slot]) {
      REXLOG_ERROR("[gpu] pass timing stays off: its timestamp pools could not be made");
      Shutdown();
      return;
    }
    pending_[slot] = false;
    labels_[slot].clear();
    labels_[slot].reserve(kInitialMarks);
  }
  enabled_ = true;
  REXLOG_INFO("[gpu] pass timing on (mcla_gpu_pass_timing): a timestamp per pass boundary, "
              "{} per frame to start with",
              kInitialMarks);
}

void PassTiming::Shutdown() {
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    pools_[slot].reset();
    pending_[slot] = false;
    labels_[slot].clear();
  }
  enabled_ = false;
  open_ = false;
  device_ = nullptr;
}

uint16_t PassTiming::Intern(std::string_view label) {
  const auto it = ids_.find(std::string(label));
  if (it != ids_.end()) {
    return it->second;
  }
  const auto id = uint16_t(names_.size());
  names_.emplace_back(label);
  label_ns_.push_back(0);
  ids_.emplace(names_.back(), id);
  return id;
}

void PassTiming::Write(plume::RenderCommandList* list, uint16_t label) {
  std::vector<uint16_t>& labels = labels_[slot_];
  // The pool's last query is the frame end's.
  if (labels.size() + 1 >= pools_[slot_]->getCount()) {
    ++lost_marks_;
    wanted_ = std::min(kMaxMarks, std::max(wanted_, uint32_t(pools_[slot_]->getCount()) * 2));
    if (!reported_overflow_) {
      reported_overflow_ = true;
      REXLOG_WARN("[gpu] pass timing: a frame has more than {} pass boundaries; the extra ones "
                  "are counted, not timed, and the pools grow to {} (reported once)",
                  pools_[slot_]->getCount() - 1, wanted_);
    }
    return;
  }
  list->writeTimestamp(pools_[slot_].get(), uint32_t(labels.size()));
  labels.push_back(label);
}

bool PassTiming::Grow(Device& device, uint32_t marks) {
  // The slot's last submission was waited (FrameRing) and harvested: no query
  // of its pool is in flight.
  auto pool = device.CreateQueryPool(marks, "pass timestamps");
  if (!pool) {
    REXLOG_ERROR("[gpu] pass timing: a pool of {} timestamps could not be made; the frames keep "
                 "losing marks",
                 marks);
    wanted_ = pools_[slot_]->getCount();
    return false;
  }
  pools_[slot_] = std::move(pool);
  labels_[slot_].reserve(marks);
  return true;
}

void PassTiming::Harvest(uint32_t slot) {
  if (!pending_[slot]) {
    return;
  }
  pending_[slot] = false;
  const std::vector<uint16_t>& labels = labels_[slot];
  if (labels.size() < 2) {
    return;
  }
  pools_[slot]->queryResults();
  const uint64_t* t = pools_[slot]->getResults();
  for (size_t i = 1; i < labels.size(); ++i) {
    if (t[i] < t[i - 1]) {
      // A counter that ran backwards (a GPU clock reset, a TDR recovery).
      ++discarded_;
      return;
    }
  }
  for (size_t i = 0; i + 1 < labels.size(); ++i) {
    label_ns_[labels[i]] += t[i + 1] - t[i];
  }
  frame_ns_ += t[labels.size() - 1] - t[0];
  ++frames_;
}

void PassTiming::BeginFrame(plume::RenderCommandList* list, uint32_t slot) {
  if (!enabled_ || !list) {
    return;
  }
  Harvest(slot);
  slot_ = slot;
  if (wanted_ > pools_[slot]->getCount() && device_) {
    Grow(*device_, wanted_);
  }
  labels_[slot].clear();
  list->resetQueryPool(pools_[slot].get(), 0, pools_[slot]->getCount());
  open_ = true;
  Write(list, Intern(kFrameStart));
}

void PassTiming::Mark(plume::RenderCommandList* list, std::string_view label) {
  if (!enabled_ || !open_ || !list) {
    return;
  }
  Write(list, Intern(label));
}

void PassTiming::CompleteFrame(plume::RenderCommandList* list, uint32_t slot) {
  if (!enabled_ || !open_ || !list || slot != slot_) {
    return;
  }
  open_ = false;
  std::vector<uint16_t>& labels = labels_[slot];
  const uint32_t count = pools_[slot]->getCount();
  // Write reserves this one.
  list->writeTimestamp(pools_[slot].get(), uint32_t(labels.size()));
  labels.push_back(Intern(kFrameEnd));
  max_marks_ = std::max(max_marks_, uint32_t(labels.size()));
  for (uint32_t i = uint32_t(labels.size()); i < count; ++i) {
    list->writeTimestamp(pools_[slot].get(), i);
  }
}

void PassTiming::OnSubmitted(uint32_t slot) {
  if (enabled_) {
    pending_[slot] = true;
  }
}

std::string PassTiming::TakeSummary() {
  if (!enabled_) {
    return "pass timing off";
  }
  if (frames_ == 0) {
    return "pass timing: no frame measured yet";
  }
  std::vector<uint16_t> order;
  for (uint16_t id = 0; id < label_ns_.size(); ++id) {
    if (label_ns_[id] != 0) {
      order.push_back(id);
    }
  }
  std::sort(order.begin(), order.end(),
            [&](uint16_t a, uint16_t b) { return label_ns_[a] > label_ns_[b]; });
  const double frame_ms = double(frame_ns_) / double(frames_) / 1e6;
  std::string text = std::format("pass timing over {} frames: GPU {:.3f} ms/frame, {} labels, "
                                 "up to {} marks/frame",
                                 frames_, frame_ms, order.size(), max_marks_);
  for (size_t i = 0; i < order.size() && i < kReportedLabels; ++i) {
    const double ms = double(label_ns_[order[i]]) / double(frames_) / 1e6;
    text += std::format(" | {} {:.3f} ms ({:.1f}%)", names_[order[i]], ms,
                        frame_ms > 0 ? 100.0 * ms / frame_ms : 0.0);
  }
  if (order.size() > kReportedLabels) {
    uint64_t rest = 0;
    for (size_t i = kReportedLabels; i < order.size(); ++i) {
      rest += label_ns_[order[i]];
    }
    text += std::format(" | {} more {:.3f} ms", order.size() - kReportedLabels,
                        double(rest) / double(frames_) / 1e6);
    // The resolve memory's labels among them, which compares
    // between runs however small they get.
    std::string memory;
    for (size_t i = kReportedLabels; i < order.size(); ++i) {
      if (names_[order[i]].starts_with("resolve memory")) {
        memory += std::format("{}{} {:.3f} ms", memory.empty() ? "" : ", ", names_[order[i]],
                              double(label_ns_[order[i]]) / double(frames_) / 1e6);
      }
    }
    if (!memory.empty()) {
      text += " (among them " + memory + ")";
    }
  }
  if (lost_marks_) {
    text += std::format(" | {} marks lost to a full pool", lost_marks_);
  }
  if (discarded_) {
    text += std::format(" | {} frames discarded", discarded_);
  }
  std::fill(label_ns_.begin(), label_ns_.end(), 0);
  frame_ns_ = frames_ = discarded_ = lost_marks_ = 0;
  max_marks_ = 0;
  return text;
}

}  // namespace mcla::gpu
