// MCLA GPU renderer -- device-local buffer memory. See buffer_heap.h.

#include "buffer_heap.h"

#include <rex/logging.h>

#include "device.h"
#include "frame.h"

namespace mcla::gpu {

namespace {

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

BufferHeap::~BufferHeap() {
  Shutdown();
}

void BufferHeap::Initialize(Device& device, FrameRing& frames, plume::RenderBufferFlags flags,
                            const char* name) {
  device_ = &device;
  frames_ = &frames;
  flags_ = flags;
  name_ = name;
}

void BufferHeap::Shutdown() {
  pages_.clear();
  empty_slots_.clear();
  stats_ = Stats{};
  device_ = nullptr;
  frames_ = nullptr;
}

bool BufferHeap::AddPage(uint64_t size, bool dedicated, uint32_t* page_index) {
  std::unique_ptr<plume::RenderBuffer> buffer =
      device_->CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(size, flags_), name_);
  if (!buffer) {
    if (!reported_failure_) {
      reported_failure_ = true;
      REXLOG_ERROR("[gpu] {}: could not create a {} MiB buffer (reported once)", name_,
                   size >> 20);
    }
    return false;
  }
  Page page;
  page.buffer = std::move(buffer);
  page.size = size;
  page.dedicated = dedicated;
  if (!dedicated) {
    page.free.emplace(0, size);
  }
  uint32_t index;
  if (!empty_slots_.empty()) {
    index = empty_slots_.back();
    empty_slots_.pop_back();
    pages_[index] = std::move(page);
  } else {
    index = uint32_t(pages_.size());
    pages_.push_back(std::move(page));
  }
  ++(dedicated ? stats_.dedicated : stats_.pages);
  stats_.reserved_bytes += size;
  *page_index = index;
  return true;
}

BufferHeap::Allocation BufferHeap::Allocate(uint64_t size) {
  Allocation a;
  if (size == 0 || !device_) {
    return a;
  }
  const uint64_t length = AlignUp(size, kAlignment);
  if (length > kPageSize) {
    uint32_t index;
    if (!AddPage(length, /*dedicated=*/true, &index)) {
      return a;
    }
    a.buffer = pages_[index].buffer.get();
    a.offset = 0;
    a.size = size;
    a.page = index;
    stats_.used_bytes += length;
    return a;
  }
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t index = 0; index < pages_.size(); ++index) {
      Page& page = pages_[index];
      if (!page.buffer || page.dedicated) {
        continue;
      }
      for (auto it = page.free.begin(); it != page.free.end(); ++it) {
        if (it->second < length) {
          continue;
        }
        const uint64_t offset = it->first;
        const uint64_t remaining = it->second - length;
        page.free.erase(it);
        if (remaining) {
          page.free.emplace(offset + length, remaining);
        }
        a.buffer = page.buffer.get();
        a.offset = offset;
        a.size = size;
        a.page = index;
        stats_.used_bytes += length;
        return a;
      }
    }
    uint32_t added;
    if (pass == 0 && !AddPage(kPageSize, /*dedicated=*/false, &added)) {
      return a;
    }
  }
  return a;
}

void BufferHeap::Release(uint32_t page_index, uint64_t offset, uint64_t length) {
  if (page_index >= pages_.size() || !pages_[page_index].buffer) {
    return;
  }
  Page& page = pages_[page_index];
  stats_.used_bytes -= length;
  if (page.dedicated) {
    // The GPU is done with it (this runs from a retirement): destroy directly.
    stats_.reserved_bytes -= page.size;
    --stats_.dedicated;
    page = Page{};
    empty_slots_.push_back(page_index);
    return;
  }
  uint64_t start = offset;
  uint64_t end = offset + length;
  auto next = page.free.lower_bound(start);
  if (next != page.free.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == start) {
      start = prev->first;
      page.free.erase(prev);
    }
  }
  if (next != page.free.end() && next->first == end) {
    end = next->first + next->second;
    page.free.erase(next);
  }
  page.free.emplace(start, end - start);
}

void BufferHeap::Free(const Allocation& allocation) {
  if (!allocation || !frames_) {
    return;
  }
  const uint32_t page = allocation.page;
  const uint64_t offset = allocation.offset;
  const uint64_t length = AlignUp(allocation.size, kAlignment);
  frames_->RetireAction([this, page, offset, length] { Release(page, offset, length); });
}

}  // namespace mcla::gpu
