#pragma once
// ===========================================================================
// MCLA GPU renderer -- transient upload memory
// ===========================================================================
// CPU-written memory the GPU reads during one frame: overlay vertices and
// indices, staging for texture uploads, and (M3) the guest constant buffers
// reached as root CBVs on D3D12 and buffer device addresses on Vulkan.
//
// Each frame slot owns a list of 16 MiB UPLOAD chunks, mapped once when they
// are created and unmapped before they are destroyed (VMA treats destroying a
// mapped allocation as a leak, vk_mem_alloc.h VMA_ASSERT_LEAK). Allocation is
// linear with the alignment the caller asks for; a request larger than a chunk
// gets a chunk of its own that lives for the frame. When the FrameRing comes
// back to a slot its fence has been waited, so ResetSlot rewinds the slot's
// chunks. A chunk no frame of the slot has touched for kIdleResetsBeforePrune
// resets is destroyed (a slot always keeps one), so a spike does not pin its
// memory forever and usage that alternates between N and N+1 chunks does not
// create and destroy one every frame.
//
// Every chunk is VERTEX | INDEX | CONSTANT | DEVICE_ADDRESSABLE:
// getDeviceAddress() is the GPU virtual address on D3D12 too, so M3 binds
// constants from here on both backends without a backend branch.
//
// Texture copies from a span follow the D3D12 placement rules on both
// backends: offset aligned to kTextureCopyOffsetAlignment, rows to
// kTextureCopyPitchAlignment (plume_d3d12.cpp toD3D12 asserts both).
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "frame.h"

namespace mcla::gpu {

class Device;

inline constexpr uint64_t kTextureCopyOffsetAlignment = 512;
inline constexpr uint64_t kTextureCopyPitchAlignment = 256;

struct UploadSpan {
  plume::RenderBuffer* buffer = nullptr;
  uint64_t offset = 0;
  uint8_t* cpu = nullptr;
  uint64_t size = 0;

  explicit operator bool() const { return buffer != nullptr; }
  plume::RenderBufferReference reference() const { return {buffer, offset}; }
};

class UploadRing {
 public:
  static constexpr uint64_t kChunkSize = 16ull << 20;
  static constexpr uint32_t kIdleResetsBeforePrune = 240;

  UploadRing() = default;
  ~UploadRing();
  UploadRing(const UploadRing&) = delete;
  UploadRing& operator=(const UploadRing&) = delete;

  bool Initialize(Device& device);
  // The GPU must be idle.
  void Shutdown();

  // `slot` is the frame slot being recorded; `alignment` a power of two. An
  // empty span when the memory cannot be had (logged).
  UploadSpan Allocate(uint32_t slot, uint64_t size, uint64_t alignment);

  // After the FrameRing waited `slot`'s fence: everything allocated from it is
  // free again.
  void ResetSlot(uint32_t slot);

  size_t chunk_count() const;

 private:
  struct Chunk {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint8_t* cpu = nullptr;
    uint64_t size = 0;
    uint64_t used = 0;
    uint32_t idle_resets = 0;
  };
  struct Slot {
    std::vector<Chunk> chunks;
    size_t current = 0;
    std::vector<Chunk> oversized;  // one-frame chunks for requests > kChunkSize
  };

  bool CreateChunk(uint32_t slot, uint64_t size, Chunk& out);
  static void DestroyChunk(Chunk& chunk);

  Device* device_ = nullptr;
  Slot slots_[kFramesInFlight];
  uint64_t chunks_created_ = 0;
};

}  // namespace mcla::gpu
