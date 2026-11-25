#pragma once
// ===========================================================================
// MCLA GPU renderer -- the shared rear view: THE header the guest's hooks and the host both use (perf/MIRROR-VIEW.md, sections 10.1, 11.2 and 12)
// ===========================================================================
// Only <algorithm>, <atomic>, <cmath> and <cstdint>, so a hook in src/mc_engine may include it (`#include "gpu/mirror_view_link.h"`, as rear_view.cpp includes gpu/mirror_draws.h) without any renderer
// header. Everything that travels between the two halves is here, defined once, in one namespace (mcla::gpu::mirror_view); the hooks' rules (hooks/rear_view_shared.h) and the host's
// (gpu/mirror_view_rules.h) include it and add nothing to the link. ONE SWITCH drives both halves: mcla_gpu_mirror_view (0 = the game's own mirror, nothing is sent, nothing is redirected).
//
// GUEST -> HOST (all on the guest render thread unless said, in command stream order with the draws):
//   1. mcla::gpu::mirror_draws::Publish(rect)        before the phase, every frame a phase is requested, also for a phase only the side mirrors asked for (gpu/mirror_draws.h)
//   2. SetStampWanted(bool)                          true while the HUD mirror is up (the default), false for a phase that only the side mirrors asked for (below)
//   3. mcla::gpu::NoteMirrorPhase(true)              at the phase's begin (Patch_RearViewDiag, the entry of sub_82308178)
//      mcla::gpu::NoteMirrorPhase(false)             at its end (Patch_RearViewPhaseEnd, r30 = 0x1000000)
//      (declared in gpu/gpu.h, defined in gpu/mirror_view.cpp: it sends the marker below only while mcla_gpu_mirror_view is on, and an end only for a begin it sent). On a frame whose phase is held (the
//      every-Nth-frame cadence) call the two back to back, with no draw between, where the phase would have run: the host then stamps the held picture.
//   4. PublishGuest(GuestFacts) once a frame from the frame tick, PublishCamera(CameraFacts) at each camera build (facts for the host: nothing in the host needs them to work yet).
// The markers go through the composite record's bucket, the side channel of the UI messages (mc_engine/display/ultrawide.h: kUiWorldBegin is 0x7F000001; IsUiMessage tests the top byte 0x7F).
// They are NOT UI messages: the renderer tests IsMarker first and hands them to the view, which needs the open command list.
//
// HOST -> GUEST (the render thread, once a frame, from MirrorView::BeginFrame and nowhere else: the one publisher, so a block is never half of two frames):
//   PublishRequest(Request): whether the host redirects the phase at all this frame (`redirect`: without it the guest changes nothing the game would not do alone), whether the car's side mirrors need the
//   view (`side_need`: MV3's mirror_glass::SideNeed(), the glass pair found and wide enough on the picture), the field and the cadence the view wants. WithdrawRequest() (all zero) when the renderer shuts
//   down: a request nobody withdrew would keep the guest's last state. With the switch off BeginFrame publishes redirect = false (mirror_view_rules.h RequestFor); the guest half also reads the switch
//   itself, so the two never disagree for more than a frame.
//
// Threading: each block is ONE relaxed atomic (so a reader never sees half of it); Publish on the writer's thread, Current on the reader's. Nothing here includes the renderer or the guest: the tests of
// the halves (rear_view_shared_test.cpp, mirror_view_rules_test.cpp, mirror_view_join_test.cpp) compile with clang alone.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::mirror_view {

// ---------------------------------------------------------------------------
// the markers
// ---------------------------------------------------------------------------

// The values ultrawide.h uses are 0x7F000001 .. 0x7F00000A; these two follow at 0x10 so a later piece of the HUD cannot meet them.
inline constexpr uint32_t kMarkerBegin = 0x7F000010u;
inline constexpr uint32_t kMarkerEnd = 0x7F000011u;
static_assert((kMarkerBegin & 0xFF000000u) == 0x7F000000u && (kMarkerEnd & 0xFF000000u) == 0x7F000000u,
              "the markers must be messages of the composite record's side channel (ultrawide.h IsUiMessage)");
inline constexpr bool IsMarker(uint32_t bucket) { return bucket == kMarkerBegin || bucket == kMarkerEnd; }

// ---------------------------------------------------------------------------
// guest -> host: the stamp
// ---------------------------------------------------------------------------

// Whether the picture of the phase is stamped into the rectangle: true while the HUD mirror is up (the default: with the host alone the HUD mirror is the only reason for a phase), false for a phase that
// only the side mirrors asked for (the HUD form is hidden, the picture is for the glass). The guest sets it where it decides the phase (TickRearView); one relaxed atomic, like mirror_draws::Publish.
// This is the ONLY channel of the fact (GuestFacts below does not repeat it).
inline std::atomic<bool> g_stamp_wanted{true};
inline void SetStampWanted(bool wanted) { g_stamp_wanted.store(wanted, std::memory_order_relaxed); }
inline bool StampWanted() { return g_stamp_wanted.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// host -> guest
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMaxEvery = 8;  // the cadence's range (the setting rearview_every is 1 to 8 too)

struct Request {
  bool redirect = false;    // the host redirects the mirror's phase into its own view this frame (the switch is on, the shaders made, the images not blocked). FALSE unless the host says so: with it false the
                            // guest runs no phase for the side mirrors (it would draw an unframed mirror into the main picture) and leaves the game's own camera-step clear alone
  bool side_need = false;   // the car's side mirrors need the view this frame (MV3's need rule: the glass pair found, wide enough on the picture, held 20 frames); counts only with `redirect`
  uint32_t every = 1;       // the view is redrawn every Nth frame (the class's cadence); 1 = every frame
  float hfov_deg = 0.0f;    // the field of view the view is drawn with, across and up (0 = none asked for: the guest keeps the game's own)
  float vfov_deg = 0.0f;
  bool has_field() const { return hfov_deg > 0.0f && vfov_deg > 0.0f; }
  bool operator==(const Request&) const = default;
};

// Centidegrees in 16 bits (0 to 655.35 degrees); anything that is not a positive number is 0.
inline uint32_t ToCenti(float deg) {
  if (!(deg > 0.0f) || !std::isfinite(deg)) {
    return 0;
  }
  return uint32_t(std::min(65535.0, std::floor(double(deg) * 100.0 + 0.5)));
}
inline float FromCenti(uint32_t centi) { return float(centi) * 0.01f; }

inline uint64_t Pack(const Request& r) {
  uint64_t v = (r.side_need ? 1u : 0u) | (r.redirect ? 2u : 0u);
  v |= uint64_t(std::clamp<uint32_t>(r.every, 1, 255)) << 8;
  v |= uint64_t(ToCenti(r.hfov_deg)) << 16;
  v |= uint64_t(ToCenti(r.vfov_deg)) << 32;
  return v;
}
inline Request UnpackRequest(uint64_t v) {
  Request r;
  r.side_need = (v & 1u) != 0;
  r.redirect = (v & 2u) != 0;
  r.every = std::clamp<uint32_t>(uint32_t((v >> 8) & 0xFFu), 1, kMaxEvery);
  r.hfov_deg = FromCenti(uint32_t((v >> 16) & 0xFFFFu));
  r.vfov_deg = FromCenti(uint32_t((v >> 32) & 0xFFFFu));
  return r;
}

inline std::atomic<uint64_t> g_request{0};

// The host's render thread, once a frame (MirrorView::BeginFrame).
inline void PublishRequest(const Request& r) { g_request.store(Pack(r), std::memory_order_relaxed); }
// The host shuts down: the guest does what it did before.
inline void WithdrawRequest() { g_request.store(0, std::memory_order_relaxed); }
// The guest.
inline Request CurrentRequest() { return UnpackRequest(g_request.load(std::memory_order_relaxed)); }

// ---------------------------------------------------------------------------
// guest -> host: facts
// ---------------------------------------------------------------------------

struct GuestFacts {
  bool phase_asked = false; // the guest asks for the mirror's phase this frame (the HUD mirror, the side mirrors, or both)
  bool side_only = false;   // ... for the side mirrors only: the HUD form is hidden, no stamp (SetStampWanted(false))
  int32_t rig = -1;         // the game's camera rig (r24 of sub_822C0320, camera_look.cpp): -1 unknown, 1 the cockpit, 0 the bumper (CHECK 12)
  bool operator==(const GuestFacts&) const = default;
};

inline uint64_t Pack(const GuestFacts& g) {
  uint64_t v = (g.phase_asked ? 1u : 0u) | (g.side_only ? 2u : 0u);
  v |= uint64_t(uint32_t(std::clamp<int32_t>(g.rig + 1, 0, 255))) << 8;
  return v;
}
inline GuestFacts UnpackGuest(uint64_t v) {
  GuestFacts g;
  g.phase_asked = (v & 1u) != 0;
  g.side_only = (v & 2u) != 0;
  g.rig = int32_t((v >> 8) & 0xFFu) - 1;
  return g;
}

inline std::atomic<uint64_t> g_guest{0};

// The guest's frame tick (TickRearView), once a frame, in every state (the rig is a fact for the log even when no phase runs).
inline void PublishGuest(const GuestFacts& g) { g_guest.store(Pack(g), std::memory_order_relaxed); }
// The host.
inline GuestFacts CurrentGuest() { return UnpackGuest(g_guest.load(std::memory_order_relaxed)); }

struct CameraFacts {
  float retail_hfov_deg = 0.0f;  // what the game's own camera call asked for, across: the HUD mirror's retail field (2 atan(tan(f1 / 2) * f2), f1 the vertical field, f2 the aspect)
  float used_hfov_deg = 0.0f;    // what the camera was built with (the same as the request's when the guest applied it, else the game's own)
  float used_vfov_deg = 0.0f;
  uint32_t seq = 0;              // counts camera builds, modulo 256: a host that sees it change knows a new camera was built
  bool valid() const { return used_hfov_deg > 0.0f && used_vfov_deg > 0.0f; }
  bool operator==(const CameraFacts&) const = default;
};

inline uint64_t Pack(const CameraFacts& c) {
  uint64_t v = uint64_t(ToCenti(c.retail_hfov_deg));
  v |= uint64_t(ToCenti(c.used_hfov_deg)) << 16;
  v |= uint64_t(ToCenti(c.used_vfov_deg)) << 32;
  v |= uint64_t(c.seq & 0xFFu) << 56;
  return v;
}
inline CameraFacts UnpackCamera(uint64_t v) {
  CameraFacts c;
  c.retail_hfov_deg = FromCenti(uint32_t(v & 0xFFFFu));
  c.used_hfov_deg = FromCenti(uint32_t((v >> 16) & 0xFFFFu));
  c.used_vfov_deg = FromCenti(uint32_t((v >> 32) & 0xFFFFu));
  c.seq = uint32_t((v >> 56) & 0xFFu);
  return c;
}

inline std::atomic<uint64_t> g_camera{0};

// The guest, in the hook at the mirror camera's build (the begin of the colour pass: before the mirror's phase of the same frame).
inline void PublishCamera(const CameraFacts& c) { g_camera.store(Pack(c), std::memory_order_relaxed); }
inline void WithdrawCamera() { g_camera.store(0, std::memory_order_relaxed); }
// The host.
inline CameraFacts CurrentCamera() { return UnpackCamera(g_camera.load(std::memory_order_relaxed)); }

}  // namespace mcla::gpu::mirror_view
