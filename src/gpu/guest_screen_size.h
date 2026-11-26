#pragma once

#include <cstdint>

#include <rex/kernel/memory_view.h>

namespace mcla::gpu {

// grcDevice's width and height, set at start. The scene's tiling buffers and
// the post-processing FrameBufferSize use these dimensions.
inline constexpr uint32_t kGuestScreenWidthAddress = 0x827D4274;
inline constexpr uint32_t kGuestScreenHeightAddress = 0x827D4278;

inline bool ReadGuestScreenSize(rex::kernel::GuestAddressSpace* memory,
                                uint32_t& width, uint32_t& height) {
  if (!memory) {
    return false;
  }
  // The runtime is published before the title image is mapped. Both words fit
  // on one page. Check commitment as well as protection because a reserved
  // page can already carry read permission in the guest page table.
  static_assert(kGuestScreenWidthAddress / 0x1000 ==
                (kGuestScreenHeightAddress + sizeof(uint32_t) - 1) / 0x1000);
  auto* heap = memory->LookupHeap(kGuestScreenWidthAddress);
  rex::kernel::HeapAllocation page{};
  if (!heap || !heap->QueryRegionInfo(kGuestScreenWidthAddress, &page) ||
      !(page.state & rex::kernel::kMemoryAllocationCommit) ||
      !(page.protect & rex::kernel::kMemoryProtectRead)) {
    return false;
  }
  const rex::kernel::MemoryView view(*memory);
  width = view.Load<uint32_t>(kGuestScreenWidthAddress);
  height = view.Load<uint32_t>(kGuestScreenHeightAddress);
  return true;
}

}  // namespace mcla::gpu
