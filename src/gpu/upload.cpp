// MCLA GPU renderer -- transient upload memory. See upload.h.
//
// Design followed: reeot gpu/frame_ring.cpp UploadAllocate (BSD-3-Clause) --
// per-slot linear upload memory reset when the slot's fence is waited. The
// chunk list, the oversized path and the idle pruning are this renderer's.

#include <algorithm>
#include <atomic>
#include "upload.h"

#include <format>
#include <string>

#include <rex/logging.h>

#include "device.h"
#include "vram_budget.h"

namespace mcla::gpu {

namespace {

constexpr bool IsPowerOfTwo(uint64_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

constexpr uint64_t AlignUp(uint64_t v, uint64_t alignment) {
  return (v + alignment - 1) & ~(alignment - 1);
}

}  // namespace

UploadRing::~UploadRing() {
  Shutdown();
}

bool UploadRing::Initialize(Device& device) {
  if (device_) {
    return true;
  }
  device_ = &device;
  // One chunk per slot up front: the first frame should not pay for creating
  // them, and a device that cannot give 2 x 16 MiB of upload memory is better
  // refused here than found out mid-frame.
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    Chunk chunk;
    if (!CreateChunk(i, kChunkSize, chunk)) {
      Shutdown();
      return false;
    }
    slots_[i].chunks.push_back(std::move(chunk));
  }
  // The spares are not needed to start: a device that cannot give them now is asked again at a
  // quiet frame (RefillSpare).
  FillSpares();
  return true;
}

void UploadRing::Shutdown() {
  for (Slot& s : slots_) {
    for (Chunk& c : s.chunks) {
      DestroyChunk(c);
    }
    for (Chunk& c : s.oversized) {
      DestroyChunk(c);
    }
    s = Slot{};
  }
  for (Chunk& c : spares_) {
    DestroyChunk(c);
  }
  spares_.clear();
  spare_target_ = kSpareChunks;
  resets_ = 0;
  last_spare_take_ = 0;
  refill_not_before_ = 0;
  pressure_releases_ = 0;
  device_ = nullptr;
}

bool UploadRing::HasRoomForChunks(const backend::VideoMemory& memory, uint64_t extra) {
  // Under kTargetPercent, not kHighPercent: a spare made right at the line the ring gives them
  // back at would be made and destroyed again.
  return memory.budget == 0 || (memory.used + extra * kChunkSize) * 100 <
                                   memory.budget * VideoMemoryBudget::kTargetPercent;
}

void UploadRing::FillSpares() {
  const backend::VideoMemory memory = device_->QueryVideoMemory();
  while (spares_.size() < spare_target_ && HasRoomForChunks(memory, spares_.size() + 1)) {
    Chunk chunk;
    if (!CreateChunk(kSpareSlot, kChunkSize, chunk)) {
      break;
    }
    spares_.push_back(std::move(chunk));
  }
  REXLOG_INFO("[gpu] upload ring: {} slots with a {} MiB chunk each, {} of {} spare chunks made "
              "ahead",
              kFramesInFlight, kChunkSize >> 20, spares_.size(), spare_target_);
}

void UploadRing::RefillSpare() {
  if (spares_.size() >= spare_target_ || resets_ < refill_not_before_ ||
      resets_ - last_spare_take_ < kRefillQuietResets) {
    return;
  }
  refill_not_before_ = resets_ + kRefillGapResets;
  if (!HasRoomForChunks(device_->QueryVideoMemory(), 1)) {
    return;
  }
  Chunk chunk;
  if (!CreateChunk(kSpareSlot, kChunkSize, chunk)) {
    // The device refused (the same refusal would meet a frame that needed one): not again soon.
    refill_not_before_ = resets_ + kRefillFailedResets;
    return;
  }
  spares_.push_back(std::move(chunk));
}

void UploadRing::DestroySpares() {
  for (Chunk& c : spares_) {
    device_->NoteReleased(c.size);
    DestroyChunk(c);
  }
  spares_.clear();
}

void UploadRing::ReleaseSparesUnderPressure() {
  const backend::VideoMemory memory = device_->QueryVideoMemory();
  if (VideoMemoryBudget::HasRoomFor(memory, 0)) {
    return;
  }
  // CHECK: nothing here has run under a real shortage (a pretended small budget).
  if (pressure_releases_++ < 8) {
    REXLOG_INFO("[gpu] upload ring: video memory {} of {} MiB in use, {} spare chunks of {} MiB "
                "released",
                memory.used >> 20, memory.budget >> 20, spares_.size(), kChunkSize >> 20);
  }
  DestroySpares();
}

bool UploadRing::CreateChunk(uint32_t slot, uint64_t size, Chunk& out) {
  const plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(
      size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
                plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::DEVICE_ADDRESSABLE);
  const std::string name = slot == kSpareSlot
                               ? std::format("upload spare chunk {}", chunks_created_)
                               : std::format("upload slot {} chunk {}", slot, chunks_created_);
  out.buffer = device_->CreateBuffer(desc, name.c_str());
  if (!out.buffer) {
    return false;
  }
  // Written by the CPU only: an empty read range says so to D3D12.
  const plume::RenderRange no_read(0, 0);
  out.cpu = static_cast<uint8_t*>(out.buffer->map(0, &no_read));
  if (!out.cpu) {
    // Can repeat every frame while the cause lasts: the first 8 are logged.
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_ERROR("[gpu] could not map upload chunk '{}' ({} bytes)", name, size);
    }
    out.buffer.reset();
    return false;
  }
  // Once per chunk: on Vulkan every query is a vkGetBufferDeviceAddress call.
  out.device_address = out.buffer->getDeviceAddress();
  out.size = size;
  out.used = 0;
  out.idle_resets = 0;
  ++chunks_created_;
  return true;
}

void UploadRing::DestroyChunk(Chunk& chunk) {
  if (chunk.buffer && chunk.cpu) {
    chunk.buffer->unmap(0, nullptr);
  }
  chunk.cpu = nullptr;
  chunk.buffer.reset();
}

UploadSpan UploadRing::Allocate(uint32_t slot, uint64_t size, uint64_t alignment) {
  if (!device_ || slot >= kFramesInFlight) {
    return {};
  }
  if (size == 0 || !IsPowerOfTwo(alignment) || alignment > kChunkSize) {
    // Can repeat every frame while the cause lasts: the first 8 are logged.
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_ERROR("[gpu] upload ring: invalid request of {} bytes aligned to {}", size, alignment);
    }
    return {};
  }
  Slot& s = slots_[slot];

  if (size > kChunkSize) {
    Chunk chunk;
    if (!CreateChunk(slot, size, chunk)) {
      return {};
    }
    chunk.used = size;
    s.oversized.push_back(std::move(chunk));
    Chunk& c = s.oversized.back();
    return {c.buffer.get(), 0, c.cpu, size, c.device_address};
  }

  while (s.current < s.chunks.size()) {
    Chunk& c = s.chunks[s.current];
    const uint64_t offset = AlignUp(c.used, alignment);
    if (offset + size <= c.size) {
      c.used = offset + size;
      return {c.buffer.get(), offset, c.cpu + offset, size, c.device_address + offset};
    }
    ++s.current;
  }

  Chunk chunk;
  if (!spares_.empty()) {
    // The chunks were made ahead (see the top of upload.h): taking one is a move.
    chunk = std::move(spares_.back());
    spares_.pop_back();
    chunk.idle_resets = 0;
  } else if (!CreateChunk(slot, kChunkSize, chunk)) {
    return {};
  }
  last_spare_take_ = resets_;
  chunk.used = size;
  s.chunks.push_back(std::move(chunk));
  s.current = s.chunks.size() - 1;
  // A frame that took or made its slot's n-th chunk will want n - 1 spares again.
  spare_target_ = std::max<uint32_t>(
      spare_target_, std::min<uint32_t>(kMaxSpareChunks, uint32_t(s.chunks.size() - 1)));
  Chunk& c = s.chunks.back();
  return {c.buffer.get(), 0, c.cpu, size, c.device_address};
}

void UploadRing::ResetSlot(uint32_t slot) {
  if (!device_ || slot >= kFramesInFlight) {
    return;
  }
  ++resets_;
  Slot& s = slots_[slot];
  // The round that is over fitted in the slot's first chunk: it took, made and needed no more.
  const bool quiet = s.current == 0 && s.oversized.empty();
  for (Chunk& c : s.oversized) {
    device_->NoteReleased(c.size);
    DestroyChunk(c);
  }
  s.oversized.clear();
  for (Chunk& c : s.chunks) {
    c.idle_resets = c.used ? 0 : c.idle_resets + 1;
    c.used = 0;
  }
  // Only from the back, so the chunks a frame fills first stay put. An idle chunk goes back to the
  // spares while they are short of their target (free: nothing is made or destroyed), else it is
  // destroyed once it has been idle long enough.
  while (s.chunks.size() > 1) {
    Chunk& back = s.chunks.back();
    if (back.idle_resets >= kIdleResetsBeforeSpare && spares_.size() < spare_target_) {
      back.idle_resets = 0;
      spares_.push_back(std::move(back));
    } else if (back.idle_resets >= kIdleResetsBeforePrune) {
      device_->NoteReleased(back.size);
      DestroyChunk(back);
    } else {
      break;
    }
    s.chunks.pop_back();
  }
  s.current = 0;

  if (!spares_.empty() && resets_ % kPressureCheckResets == 0) {
    ReleaseSparesUnderPressure();
  }
  if (quiet) {
    RefillSpare();
  }
}

size_t UploadRing::chunk_count() const {
  size_t n = spares_.size();
  for (const Slot& s : slots_) {
    n += s.chunks.size() + s.oversized.size();
  }
  return n;
}

}  // namespace mcla::gpu
