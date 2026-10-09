// MCLA GPU renderer -- transient upload memory. See upload.h.
//
// Design followed: reeot gpu/frame_ring.cpp UploadAllocate (BSD-3-Clause) --
// per-slot linear upload memory reset when the slot's fence is waited. The
// chunk list, the oversized path and the idle pruning are this renderer's.

#include "upload.h"

#include <format>
#include <string>

#include <rex/logging.h>

#include "device.h"

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
  device_ = nullptr;
}

bool UploadRing::CreateChunk(uint32_t slot, uint64_t size, Chunk& out) {
  const plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(
      size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
                plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::DEVICE_ADDRESSABLE);
  const std::string name = std::format("upload slot {} chunk {}", slot, chunks_created_);
  out.buffer = device_->CreateBuffer(desc, name.c_str());
  if (!out.buffer) {
    return false;
  }
  // Written by the CPU only: an empty read range says so to D3D12.
  const plume::RenderRange no_read(0, 0);
  out.cpu = static_cast<uint8_t*>(out.buffer->map(0, &no_read));
  if (!out.cpu) {
    REXLOG_ERROR("[gpu] could not map upload chunk '{}' ({} bytes)", name, size);
    out.buffer.reset();
    return false;
  }
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
    REXLOG_ERROR("[gpu] upload ring: invalid request of {} bytes aligned to {}", size, alignment);
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
    return {c.buffer.get(), 0, c.cpu, size};
  }

  while (s.current < s.chunks.size()) {
    Chunk& c = s.chunks[s.current];
    const uint64_t offset = AlignUp(c.used, alignment);
    if (offset + size <= c.size) {
      c.used = offset + size;
      return {c.buffer.get(), offset, c.cpu + offset, size};
    }
    ++s.current;
  }

  Chunk chunk;
  if (!CreateChunk(slot, kChunkSize, chunk)) {
    return {};
  }
  chunk.used = size;
  s.chunks.push_back(std::move(chunk));
  s.current = s.chunks.size() - 1;
  Chunk& c = s.chunks.back();
  return {c.buffer.get(), 0, c.cpu, size};
}

void UploadRing::ResetSlot(uint32_t slot) {
  if (!device_ || slot >= kFramesInFlight) {
    return;
  }
  Slot& s = slots_[slot];
  for (Chunk& c : s.oversized) {
    DestroyChunk(c);
  }
  s.oversized.clear();
  for (Chunk& c : s.chunks) {
    c.idle_resets = c.used ? 0 : c.idle_resets + 1;
    c.used = 0;
  }
  // Only from the back, so the chunks a frame fills first stay put.
  while (s.chunks.size() > 1 && s.chunks.back().idle_resets >= kIdleResetsBeforePrune) {
    DestroyChunk(s.chunks.back());
    s.chunks.pop_back();
  }
  s.current = 0;
}

size_t UploadRing::chunk_count() const {
  size_t n = 0;
  for (const Slot& s : slots_) {
    n += s.chunks.size() + s.oversized.size();
  }
  return n;
}

}  // namespace mcla::gpu
