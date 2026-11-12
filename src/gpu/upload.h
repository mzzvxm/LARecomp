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
// Making a chunk costs about 4 ms of the thread that does it (the driver
// commits and maps 16 MiB), so a frame that needs one more than its slot has
// must not be the one to make it: a world entry that first uses ~1000 textures
// needs 3 chunks in one frame (~28 MiB of texture staging next to the ~8 MiB of
// constants every frame writes) and paid ~8 ms of its 44. Chunks the slots do
// not hold wait in a shared list of spares (kSpareChunks, made at start-up):
// a slot whose chunks are full takes one at no cost, and a slot's extra chunk
// that no frame touched for kIdleResetsBeforeSpare resets goes back to the
// list instead of being destroyed. Spares are never pruned. Only if the list
// has none left is a chunk made inside the frame, and then the list is made to
// hold one more (up to kMaxSpareChunks) so the next such frame finds it. Low
// on spares, one is made at a quiet frame, a long while after the last take
// (kRefillQuietResets), so that the slots' idle chunks come back first.
//
// Memory: the spares are kSpareChunks x 16 MiB over the one chunk per slot
// (32 MiB more than the ring held at rest before). When the adapter's use of
// local video memory passes kHighPercent of its budget the spares are
// destroyed, and none is made until the use is under kTargetPercent again,
// with room for it (vram_budget.h's thresholds; the budget does not know the
// ring). Upload memory is system memory on D3D12, but with a resizable BAR it
// can be video memory on Vulkan, which is what the check is for.
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

#include "backend.h"
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
  // The GPU address of the span's first byte: its chunk's, read once when
  // the chunk was made, plus the offset (reblue's gpuBase).
  uint64_t device_address = 0;

  explicit operator bool() const { return buffer != nullptr; }
  plume::RenderBufferReference reference() const { return {buffer, offset}; }
};

class UploadRing {
 public:
  static constexpr uint64_t kChunkSize = 16ull << 20;
  static constexpr uint32_t kIdleResetsBeforePrune = 240;
  // Spare chunks, outside the slots: the world entry needs 3 chunks in one slot, which has 1 of its
  // own. The list grows to what a frame that found it empty needed, to kMaxSpareChunks at most.
  // CHECK: 3 is read from the R1 runs (stut2r-samp-pool frame 7349: dev_buffer 2 x 16 MiB made in the
  // frame, 27 MiB of texture staging); read the frame's dev_buffer_n (0 expected) after this.
  static constexpr uint32_t kSpareChunks = 2;
  static constexpr uint32_t kMaxSpareChunks = 4;
  // A slot's extra chunk idle this long (resets of that slot: 30 = 60 frames) is kept as a spare.
  static constexpr uint32_t kIdleResetsBeforeSpare = 30;
  // Resets (of any slot: one per frame) since a spare was last taken before one is made again at a
  // quiet frame: longer than kIdleResetsBeforeSpare takes in frames, so the chunks the frame that
  // took them gave up come back first and nothing is made for a spike that has just passed.
  static constexpr uint64_t kRefillQuietResets = 240;
  // Resets between two attempts to make a spare (a failed one waits kRefillFailedResets).
  static constexpr uint64_t kRefillGapResets = 30;
  static constexpr uint64_t kRefillFailedResets = 600;
  // Resets between two looks at the video memory budget, made while spares are held.
  static constexpr uint64_t kPressureCheckResets = 60;

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

  // Every chunk the ring holds: the slots', the one-frame oversized ones and the spares.
  size_t chunk_count() const;
  size_t spare_count() const { return spares_.size(); }

 private:
  struct Chunk {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint8_t* cpu = nullptr;
    uint64_t device_address = 0;
    uint64_t size = 0;
    uint64_t used = 0;
    uint32_t idle_resets = 0;
  };
  struct Slot {
    std::vector<Chunk> chunks;
    size_t current = 0;
    std::vector<Chunk> oversized;  // one-frame chunks for requests > kChunkSize
  };

  // `slot` only names the buffer; kSpareSlot for one made for the spare list.
  static constexpr uint32_t kSpareSlot = ~0u;
  bool CreateChunk(uint32_t slot, uint64_t size, Chunk& out);
  static void DestroyChunk(Chunk& chunk);

  // Whether `extra` more chunks keep the adapter's use under kTargetPercent of the budget (true
  // when it reports none). Makes new spares only; see the top of the file.
  static bool HasRoomForChunks(const backend::VideoMemory& memory, uint64_t extra);
  // Makes the spare list kSpareChunks long where there is room; at start-up.
  void FillSpares();
  // At a quiet frame: makes one spare when the list is below its target.
  void RefillSpare();
  // Destroys the spares when the adapter's video memory use is past kHighPercent of its budget.
  void ReleaseSparesUnderPressure();
  void DestroySpares();

  Device* device_ = nullptr;
  Slot slots_[kFramesInFlight];
  std::vector<Chunk> spares_;
  uint32_t spare_target_ = kSpareChunks;
  uint64_t resets_ = 0;           // ResetSlot calls, all slots
  // resets_ when a slot last took a spare, or made a chunk for want of one.
  uint64_t last_spare_take_ = 0;
  uint64_t refill_not_before_ = 0;
  uint32_t pressure_releases_ = 0;
  uint64_t chunks_created_ = 0;
};

}  // namespace mcla::gpu
