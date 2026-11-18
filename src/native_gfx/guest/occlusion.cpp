#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest occlusion queries. See occlusion.h.

#include "occlusion.h"

#include <atomic>
#include <cstdio>

#include "settings/larecomp_settings.h"
#include <rex/memory/mapped_ptr.h>

namespace mcla::native_gfx {
namespace {

// The enable byte, and the only function that sets it (sub_82322C78 writes 1).
// Held rather than patched at the setter so a save/load or a mode change that
// re-enables occlusion cannot silently bring the bug back.
constexpr uint32_t kOcclusionEnabledByte = 0x8288D5D1u;

std::atomic<uint64_t> g_held{0};

}  // namespace

void DisableGuestOcclusionQueries(uint8_t* base) {
  if (!base || !larecomp::settings::mcla_native_gfx_disable_occlusion.Get()) {
    return;
  }
  uint8_t* p = static_cast<uint8_t*>(rex::memory::GuestPtr(base, kOcclusionEnabledByte));
  if (!p || *p == 0) {
    return;
  }
  *p = 0;
  g_held.fetch_add(1, std::memory_order_relaxed);
}

const char* OcclusionSummary() {
  static char buf[64];
  std::snprintf(buf, sizeof(buf), "occlusion desligada, reposta %llu vezes",
                (unsigned long long)g_held.load(std::memory_order_relaxed));
  return buf;
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
