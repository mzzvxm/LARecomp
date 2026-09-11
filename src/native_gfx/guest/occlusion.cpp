#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest occlusion queries. See occlusion.h.

#include "occlusion.h"

#include <atomic>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(
    mcla_native_gfx_disable_occlusion, true, "MCLA/NativeGfx",
    "Select the guest's no-occlusion path (byte_8288D5D1 = 0) while the native "
    "backend is active. "
    "MCLA gates a vehicle's body on an occlusion query: sub_82323808 keeps one "
    "slot per object, answers 'occluded' whenever a slot exists, and refreshes "
    "the very timestamp the reaper ages slots by -- so a slot survives only "
    "until the query completes and releases it. This backend has no command "
    "processor and cannot run the query, so the slot is refreshed forever and "
    "the body is never submitted again. Measured: the body block in "
    "sub_8235D500 is reached exactly once and never after, while the wheels, "
    "discs and exhaust keep drawing because their submit paths consult no "
    "query, and traffic shows only its contact shadow for the same reason. "
    "The game itself supports running with occlusion off (sub_82323808 returns "
    "'draw' immediately when this byte is zero), which is where a backend "
    "without query support belongs. Cost: geometry that is genuinely hidden is "
    "still drawn -- GPU time, never a missing object. "
    "Turn off only to reproduce the vanishing body, or once real queries exist.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::native_gfx {
namespace {

// The enable byte, and the only function that sets it (sub_82322C78 writes 1).
// Held rather than patched at the setter so a save/load or a mode change that
// re-enables occlusion cannot silently bring the bug back.
constexpr uint32_t kOcclusionEnabledByte = 0x8288D5D1u;

std::atomic<uint64_t> g_held{0};

}  // namespace

void DisableGuestOcclusionQueries(uint8_t* base) {
  if (!base || !REXCVAR_GET(mcla_native_gfx_disable_occlusion)) {
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
