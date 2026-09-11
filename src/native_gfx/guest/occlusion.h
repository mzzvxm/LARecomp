#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest occlusion queries.
//
// MCLA hides a vehicle's body behind an occlusion query. sub_8235D500 asks
// sub_82323808 whether the body is visible and skips the whole body block when
// the answer is "no":
//
//   8235d754  bl sub_82323808
//   8235d760  beq cr6, loc_8235D898   ; 0 -> the body is not submitted at all
//
// The manager keeps one slot per object. With no slot it issues a query and
// answers "draw"; with a slot it answers "occluded" and refreshes the slot's
// timestamp. A reaper (sub_82323A30) frees slots older than four frames, so on
// hardware an object is skipped for a few frames and then re-tested.
//
// The refresh is the trap: the found-slot branch writes the very field the
// reaper compares against, so a slot that is hit every frame never ages out.
// It only ages out because the query eventually completes and the slot is
// released. This backend has no command processor and cannot run the query, so
// the slot is refreshed forever and the body is never drawn again -- measured
// as the body block being reached exactly once and never after.
//
// The game supports running without occlusion at all: sub_82323808's first line
// is `if (!byte_8288D5D1 || a4) return 1;`. That is the configuration a backend
// without query support belongs in, so the runtime selects it rather than
// leaving the guest to cache "occluded" forever. The cost is drawing geometry
// that is genuinely hidden -- GPU work, never a missing object.
//
// The real implementation (D3D12 query heaps resolved into the guest's own
// buffer at dword_8288D5D4, so the slots complete normally) is future work; it
// needs that buffer's layout decoded first.
#pragma once

#include <cstdint>

namespace mcla::native_gfx {

// Selects the guest's no-occlusion path for this frame. Cheap: one byte compare
// and, at most, one byte store. Call once per frame boundary.
void DisableGuestOcclusionQueries(uint8_t* base);

// "held=N" — how many frames the runtime has had to put the byte back, which is
// non-zero because the guest turns occlusion on again from sub_82322C78.
const char* OcclusionSummary();

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
