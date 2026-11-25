#pragma once
// ===========================================================================
// The rear view mirror's guest half of the SHARED VIEW: the rules (perf/MIRROR-VIEW.md, package MV2)
// ===========================================================================
// rear_view.cpp holds the hooks; what each one decides is here, as pure functions and small state machines on plain numbers, so a test needs no game (rear_view_shared_test.cpp).
// The host half (the view, the stamp, the glass overlay) talks to this one through gpu/mirror_view_link.h, the one link header; every rule here returns what the game did before when the switch
// (mcla_gpu_mirror_view, the same one that drives the host) is off.
//
//   1. THREE STATES of the mirror's phase (MIRROR-VIEW.md 2.4): the HUD mirror shown (the game's way), the phase run for the side mirrors with the HUD form hidden, and off.
//   2. THE MARKERS that bracket the game's mirror phase for the host: one BEGIN and one END per phase, never nested, never an END without a BEGIN, a lost END closed at the next frame.
//   3. THE CAMERA: the mirror camera's field of view and aspect from the host's request (sub_8217D460's f1 is the VERTICAL field in DEGREES, f2 the aspect w/h).
//   4. THE CADENCE: the view's every-Nth-frame rate is the larger of the debug setting's and the host's.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gpu/mirror_view_link.h"
#include "rear_view_cheap.h"

namespace larecomp::rearview_shared {

namespace vlink = mcla::gpu::mirror_view;  // the link header's namespace; not `link`: unistd.h declares a function of that name

// ---------------------------------------------------------------------------
// 1. the three states
// ---------------------------------------------------------------------------

// What decides the state, read when a hook runs: the guest half's switch (mcla_gpu_mirror_view != 0 and the Plume renderer: the host's own switch), the player's HUD mirror (rearview) and the
// host's request that the side mirrors need the view.
struct Mode {
  bool shared = false;     // the guest half is on
  bool hud = false;        // the HUD mirror is switched on (rearview)
  bool side_need = false;  // the host's request: the side mirrors need the view (only counts while `shared`)
};

// The side mirrors' need as the guest acts on it: the guest half is on, the host says it redirects the phase (a phase run for the side mirrors with nothing to catch its draws would put an
// unframed mirror picture into the main target), and the host's request says the side mirrors need the view. The host builds that bit from MV3's flag (mirror_glass::SideNeed(), in
// MirrorView::BeginFrame): the request is the one source, the guest does not read MV3's flag itself.
inline bool EffectiveSideNeed(bool shared, const vlink::Request& req) { return shared && req.redirect && req.side_need; }

// Is the camera step's own clear skipped (the first sub_82178C40 of sub_82307F28, 0x82307F8C: colour 0xFFFF0000, depth 0.0, stencil, over the rectangle of the MAIN target at the colour
// pass's begin)? When the host redirects the phase the rectangle is not the phase's any more: left alone it stays red with depth 0 under the HUD stamp (or, for the side mirrors' phase,
// bare on the screen with nothing to cover it), and the host effects would see depth 0 there. The phase's own begin clear (sub_82308178) is the one the host redirects.
inline bool SkipCameraClear(bool shared, const vlink::Request& req) { return shared && req.redirect; }

// Does the guest ask for the mirror's phase (and keep its rectangle, its racers and its camera)? The HUD mirror, or the side mirrors while the guest half is on.
inline bool PhaseWanted(const Mode& m) { return m.hud || (m.shared && m.side_need); }

// The phase is asked for with the HUD form hidden: the side mirrors only. No frame, no stamp.
inline bool SideOnly(const Mode& m) { return !m.hud && m.shared && m.side_need; }

// The answer to mcRearView::update's two visibility questions (0x82233C58: the alpha ramp and the phase request; 0x82233CE8: the consumer's active byte). `game_visible` is what the game's
// own widget said. Off, a hidden HUD mirror answers "hidden" (the game never asks for the phase: the cost of a switched-off mirror is nothing); the side mirrors' need makes it "visible"
// whatever the HUD form says, so a hidden HUD does not take the side mirrors' view away.
inline bool PhaseVisible(const Mode& m, bool game_visible) {
  if (m.shared && m.side_need) {
    return true;
  }
  return m.hud ? game_visible : false;
}

// Is the form's frame (the white rectangle around the stamp) read as hidden at 0x82234148 (its alpha reads 0)? Off: when the HUD mirror is off, as before. With the guest half on also
// while the HUD form is hidden (the phase may be running for the side mirrors, whose alpha the game then ramps up: nothing of the form may show).
inline bool FrameHidden(const Mode& m, bool form_visible) {
  if (!m.hud) {
    return true;
  }
  return m.shared && !form_visible;
}

// Does the host draw the HUD stamp into the rectangle? The HUD mirror on and its form visible.
inline bool HudStamp(const Mode& m, bool form_visible) { return m.hud && form_visible; }

// Do the racers' cars join the phase (CARS IN REAR VIEW)? The player's setting, or the side mirrors' need: a side mirror that never shows the car alongside is not a mirror. One view
// serves both readers, so with the HUD mirror also on its stamp then holds the racers too (MIRROR-VIEW.md section 8 notes it; a decision, one line here).
inline bool CarsInView(const Mode& m, bool cars_setting) { return cars_setting || (m.shared && m.side_need); }

// ---------------------------------------------------------------------------
// 2. the markers
// ---------------------------------------------------------------------------

// The phase loop (sub_822E6408) runs the begin step of EVERY phase every frame and its end step too; the mirror's begin step (sub_82308178) is reached whenever the renderer's phase mask has
// the mirror's bit, the HUD mirror up or not, and does nothing unless the pass's active byte is set. The tracker sends a BEGIN for a tracked, requested phase with the consumer active.
// An END goes out for every BEGIN, once; a BEGIN that finds one open is ignored (counted); the frame's start closes one
// whose end never came. A HELD frame (the cadence took the phase out) sends the two back to back, an empty phase at the place the phase would have ended: the host then draws its stamp from
// the picture it kept (MV1's rule: no end marker, no stamp).
class MarkerTracker {
 public:
  bool open() const { return open_; }

  // Returns the marker to send (vlink::kMarkerBegin) or 0.
  uint32_t OnBegin(bool shared, bool phase_requested, bool consumer_active) {
    if (!shared || !phase_requested || !consumer_active) {
      ++not_run_;
      return 0;
    }
    if (open_) {
      ++nested_;
      return 0;
    }
    open_ = true;
    ++begun_;
    return vlink::kMarkerBegin;
  }

  // After the mirror's end callback (0x822E6908 with r30 = the mirror's phase). Returns vlink::kMarkerEnd when a bracket is open.
  uint32_t OnEnd() {
    if (!open_) {
      return 0;
    }
    open_ = false;
    ++ended_;
    return vlink::kMarkerEnd;
  }

  // The mirror's phase end step on a frame whose phase was held (the cadence took the mirror's bit out) with the guest half on, and nothing open: true = send BEGIN and END back to back.
  bool OnHeldFrame(bool shared, bool held_this_frame) {
    if (!shared || !held_this_frame || open_) {
      return false;
    }
    ++held_pairs_;
    return true;
  }

  // The start of a frame (the phase loop's first look): a bracket still open lost its end.
  uint32_t OnFrameStart() {
    if (!open_) {
      return 0;
    }
    open_ = false;
    ++closed_late_;
    return vlink::kMarkerEnd;
  }

  uint64_t begun() const { return begun_; }
  uint64_t ended() const { return ended_; }
  uint64_t closed_late() const { return closed_late_; }
  uint64_t nested() const { return nested_; }
  uint64_t not_run() const { return not_run_; }
  uint64_t held_pairs() const { return held_pairs_; }

 private:
  bool open_ = false;
  uint64_t begun_ = 0;
  uint64_t ended_ = 0;
  uint64_t closed_late_ = 0;
  uint64_t nested_ = 0;
  uint64_t not_run_ = 0;
  uint64_t held_pairs_ = 0;
};

// ---------------------------------------------------------------------------
// 3. the camera
// ---------------------------------------------------------------------------

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kMinFieldDeg = 5.0f;    // a requested field outside these is not believed: the game's own stays
inline constexpr float kMaxFieldDeg = 170.0f;
inline constexpr double kMinAspect = 0.2;
inline constexpr double kMaxAspect = 20.0;

// The horizontal field (degrees) of a camera with the vertical field `vfov_deg` and aspect w/h (the game's projection: the half width's tangent is the half height's times the aspect,
// sub_8217CB68: [cam+716] = [cam+712] x aspect). 0 for input that is not a field.
inline float HorizontalFovDeg(float vfov_deg, float aspect) {
  if (!(vfov_deg > 0.0f) || !(aspect > 0.0f) || !std::isfinite(vfov_deg) || !std::isfinite(aspect) || vfov_deg >= 180.0f) {
    return 0.0f;
  }
  const double half_v = double(vfov_deg) * 0.5 * kPi / 180.0;
  return float(2.0 * std::atan(std::tan(half_v) * double(aspect)) * 180.0 / kPi);
}

// The field (degrees across) the HUD mirror shows WITHOUT any request from the host: the game's own camera call, narrowed by the debug option rearview_fov_scale (the cheap mirror's B) as the build
// below does when the host asks for nothing. The host's HUD stamp cuts exactly this field out of a wider view (CameraFacts::retail_hfov_deg, mirror_view::HudCrop), so the HUD mirror looks as it did.
inline float HudFieldDeg(float game_fov_deg, float game_aspect, double fov_scale) {
  return HorizontalFovDeg(larecomp::rearview_cheap::FieldOfView(game_fov_deg, fov_scale), game_aspect);
}

struct Camera {
  float fov_deg;     // sub_8217D460's f1: the vertical field in degrees
  float aspect;      // its f2: width over height
  bool applied;      // the host's request was used
};

// The camera the guest builds: the host's field (hfov across, vfov up) when it asked for one and it is a believable field, else the game's own, unchanged. The aspect is the one that gives
// the requested field across for the requested field up.
inline Camera SharedCamera(float game_fov_deg, float game_aspect, const vlink::Request& req) {
  const Camera game{game_fov_deg, game_aspect, false};
  if (!req.has_field()) {
    return game;
  }
  if (!(req.hfov_deg >= kMinFieldDeg && req.hfov_deg <= kMaxFieldDeg) || !(req.vfov_deg >= kMinFieldDeg && req.vfov_deg <= kMaxFieldDeg)) {
    return game;
  }
  const double tan_h = std::tan(double(req.hfov_deg) * 0.5 * kPi / 180.0);
  const double tan_v = std::tan(double(req.vfov_deg) * 0.5 * kPi / 180.0);
  const double aspect = tan_h / tan_v;
  if (!std::isfinite(aspect) || aspect < kMinAspect || aspect > kMaxAspect) {
    return game;
  }
  return Camera{req.vfov_deg, float(aspect), true};
}

// What the hook at the mirror camera's build writes back (sub_8217D460's f1, f2, f4): the shared view's field when the guest half is on and the host asked for one (the debug field scale
// is NOT applied to it: it would break the host's lookup and crop math), else the game's own field narrowed by the cheap option's scale as before; the far plane pulled in by the cheap
// option either way (it changes what is drawn, not the picture's geometry). With the guest half off and both cheap options off it is the game's own, bit for bit.
struct CameraBuild {
  float fov_deg;
  float aspect;
  float far_m;
  bool applied;  // the host's request was used
};
inline CameraBuild BuildCamera(float game_fov_deg, float game_aspect, float game_far_m, bool shared_on, const vlink::Request& req, double fov_scale, double far_wanted_m) {
  Camera c{game_fov_deg, game_aspect, false};
  if (shared_on) {
    c = SharedCamera(game_fov_deg, game_aspect, req);
  }
  if (!c.applied) {
    c.fov_deg = larecomp::rearview_cheap::FieldOfView(c.fov_deg, fov_scale);
  }
  return CameraBuild{c.fov_deg, c.aspect, larecomp::rearview_cheap::FarPlane(game_far_m, far_wanted_m), c.applied};
}

// ---------------------------------------------------------------------------
// 4. the cadence
// ---------------------------------------------------------------------------

// The view is redrawn every Nth frame: the larger of the debug setting (rearview_every, 1 to 8) and, with the guest half on, the host's (the class's cadence). Never under 1, never over 8.
inline int EffectiveEvery(int setting_every, const vlink::Request& req, bool shared) {
  int every = std::clamp(setting_every, 1, int(vlink::kMaxEvery));
  if (shared) {
    every = std::max(every, int(std::clamp<uint32_t>(req.every, 1, vlink::kMaxEvery)));
  }
  return every;
}

}  // namespace larecomp::rearview_shared
