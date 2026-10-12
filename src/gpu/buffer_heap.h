#pragma once
// ===========================================================================
// MCLA GPU renderer -- device-local buffer memory for the guest's geometry
// ===========================================================================
// Ranges of DEFAULT-heap buffers the
// geometry cache copies vertex and index data into (from the upload ring, at
// the end of a frame). One heap per use, because a D3D12 buffer has one read
// state at a time and Plume derives it from the buffer's flags: a VERTEX
// buffer reads as VERTEX_AND_CONSTANT_BUFFER, an INDEX buffer as INDEX_BUFFER
// (plume_d3d12.cpp toBufferState) -- one buffer holding both could not be in
// the right state for both.
//
// Pages of kPageSize, first fit over a free list per page with neighbours
// merged on release, kAlignment-aligned ranges; a request larger than a page
// gets a dedicated buffer. Free() is deferred by the FrameRing's serial rule:
// the range becomes reusable only once every use recorded so far has completed
// on the GPU (frame.h). Pages are kept for the life of the renderer (M4); a
// compaction or trim policy is a measurement for M6.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

class Device;
class FrameRing;

class BufferHeap {
 public:
  static constexpr uint64_t kPageSize = 32ull << 20;
  static constexpr uint64_t kAlignment = 256;

  struct Allocation {
    plume::RenderBuffer* buffer = nullptr;
    uint64_t offset = 0;
    uint64_t size = 0;      // as requested
    uint32_t page = UINT32_MAX;
    explicit operator bool() const { return buffer != nullptr; }
    plume::RenderBufferReference reference() const { return {buffer, offset}; }
  };

  struct Stats {
    uint32_t pages = 0;
    uint32_t dedicated = 0;
    uint64_t reserved_bytes = 0;  // all pages
    uint64_t used_bytes = 0;      // allocated, including ranges waiting for their release
  };

  BufferHeap() = default;
  ~BufferHeap();
  BufferHeap(const BufferHeap&) = delete;
  BufferHeap& operator=(const BufferHeap&) = delete;

  // `flags`: plume::RenderBufferFlag::VERTEX or INDEX. `name` names the pages.
  void Initialize(Device& device, FrameRing& frames, plume::RenderBufferFlags flags,
                  const char* name);
  // The GPU must be idle and the FrameRing drained (pending releases name
  // this heap).
  void Shutdown();

  // Null (logged) when the memory cannot be had.
  Allocation Allocate(uint64_t size);
  // Deferred until the GPU is done with every use recorded so far.
  void Free(const Allocation& allocation);

  // The buffers of the pages, for barriers over everything written this frame.
  plume::RenderBuffer* page_buffer(uint32_t page) const { return pages_[page].buffer.get(); }
  const Stats& stats() const { return stats_; }

 private:
  struct Page {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint64_t size = 0;
    bool dedicated = false;
    std::map<uint64_t, uint64_t> free;  // offset -> length, disjoint, merged
  };

  bool AddPage(uint64_t size, bool dedicated, uint32_t* page_index);
  void Release(uint32_t page, uint64_t offset, uint64_t length);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  plume::RenderBufferFlags flags_ = 0;
  const char* name_ = "";
  std::vector<Page> pages_;
  std::vector<uint32_t> empty_slots_;  // released dedicated pages, reusable indices
  Stats stats_;
  bool reported_failure_ = false;
};

}  // namespace mcla::gpu
