// MCLA GPU renderer -- the groundwork of the temporal upscalers. See temporal.h.

#include "temporal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "records.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_temporal, false, "MCLA/GPU",
    "Development switch for the temporal anti-aliasing and upscaling that is being built: moves "
    "the 3D scene by a different sub-pixel amount every frame (the world only: the menus, the HUD "
    "and the post-processing keep their positions) and reads the game's camera. Nothing smooths "
    "the movement out yet, so the scene shimmers a little. Logs '[gpu] temporal:' lines. Read "
    "every frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] temporal:";
constexpr uint32_t kMaxResetLines = 16;
constexpr double kPi = 3.14159265358979323846;

// The anchor's three textures, in fetch slots 0, 1, 2 (census 1.1: it has a fourth, a half-size
// colour texture, which is not checked).
constexpr uint32_t kAnchorFormats[3] = {
    uint32_t(xe::TextureFormat::k_16_16_16_16_EXPAND),  // the scene colour (as float)
    uint32_t(xe::TextureFormat::k_8_8_8_8),             // the stencil view of the depth resolve
    uint32_t(xe::TextureFormat::k_24_8_FLOAT),          // the depth, 1 near
};
constexpr const char* kAnchorFormatNames[3] = {"16_16_16_16_EXPAND", "8_8_8_8", "24_8_FLOAT"};

const char* StatusName(TemporalSystem::AnchorStatus status) {
  using S = TemporalSystem::AnchorStatus;
  switch (status) {
    case S::kNotSeen: return "no anchor draw";
    case S::kAccepted: return "accepted";
    case S::kNoMainView: return "no main view draw before it";
    case S::kView4x: return "the main view is at 4x samples";
    case S::kFetches: return "its fetches are not the scene colour, stencil view and depth";
    case S::kTarget: return "its target is not the half-size float image";
    case S::kFrameSize: return temporal::RejectName(temporal::Reject::kFrameSize);
    case S::kConstants: return temporal::RejectName(temporal::Reject::kConstants);
    case S::kViewProjection: return temporal::RejectName(temporal::Reject::kViewProjection);
    case S::kViewInverse: return temporal::RejectName(temporal::Reject::kViewInverse);
    case S::kLens: return temporal::RejectName(temporal::Reject::kLens);
    case S::kCount: break;
  }
  return "?";
}

TemporalSystem::AnchorStatus StatusOf(temporal::Reject reject) {
  using S = TemporalSystem::AnchorStatus;
  switch (reject) {
    case temporal::Reject::kFrameSize: return S::kFrameSize;
    case temporal::Reject::kConstants: return S::kConstants;
    case temporal::Reject::kViewProjection: return S::kViewProjection;
    case temporal::Reject::kViewInverse: return S::kViewInverse;
    case temporal::Reject::kLens: return S::kLens;
    default: return S::kAccepted;
  }
}

const char* ResetName(uint32_t index) {
  static const char* const kNames[TemporalSystem::kResetReasons] = {
      "no anchor in the frame before", "camera cut", "size changed", "gap over 100 ms",
      "no previous matrix"};
  return index < TemporalSystem::kResetReasons ? kNames[index] : "?";
}

std::string ResetText(uint32_t mask) {
  std::string text;
  for (uint32_t i = 0; i < TemporalSystem::kResetReasons; ++i) {
    if (mask & (1u << i)) {
      text += std::format("{}{}", text.empty() ? "" : ", ", ResetName(i));
    }
  }
  return text.empty() ? std::string("none") : text;
}

uint32_t Sum(const std::array<uint32_t, TemporalSystem::kKinds>& a) {
  return a[0] + a[1] + a[2];
}

}  // namespace

// ---------------------------------------------------------------------------
// the switch and the frame
// ---------------------------------------------------------------------------

void TemporalSystem::SetEnabled(bool enabled) {
  enabled_ = enabled;
  ResetRun();
  window_ = Window{};
  window_start_ = Clock::now();
  shown_mode_ = Mode::kCount;
  pending_mode_ = Mode::kCount;
  pending_frames_ = 0;
  status_logged_.fill(false);
  reset_lines_ = 0;
  frame_number_ = 0;
  if (enabled) {
    REXLOG_WARN("{} ON (mcla_gpu_temporal, development): the 3D scene is moved by a sub-pixel "
                "amount that changes every frame (the world only: the UI keeps still), and the "
                "game's camera is read from its motion blur draw; nothing smooths the movement "
                "out yet. A summary every {:.0f} s",
                kPrefix, kReportSeconds);
  } else {
    REXLOG_INFO("{} off", kPrefix);
  }
}

void TemporalSystem::ResetRun() {
  ran_last_ = false;
  jitter_index_ = 0;
  misses_ = 0;
  disarmed_ = false;
  frame_open_ = false;
  frame_ = FrameState{};
}

void TemporalSystem::BeginFrame(uint32_t slot) {
  const bool want = REXCVAR_GET(mcla_gpu_temporal);
  if (want != enabled_ && !frame_open_) {
    SetEnabled(want);  // only between frames: a frame is jittered whole or not at all
  }
  if (!enabled_ || frame_open_) {
    return;  // off, or a frame reopened after an abandoned list: its state stands
  }
  frame_open_ = true;
  frame_ = FrameState{};
  frame_.frame = frame_number_;
  frame_.slot = slot;
  // Armed only after a frame in which the anchor was accepted: a frame without one never shakes
  // (F4-DESIGN.md 2.5).
  frame_.armed = ran_last_ && !disarmed_;
  if (frame_.armed) {
    frame_.phase_count = kPhasesAtRatio1;  // 8 x ratio^2, ratio 1
    frame_.jitter_index = jitter_index_ % frame_.phase_count;
    // Halton(2,3) from index 1, minus a half: in (-0.5, 0.5) render pixels, +y down.
    frame_.jitter[0] = float(temporal::Halton(frame_.jitter_index + 1, 2) - 0.5);
    frame_.jitter[1] = float(temporal::Halton(frame_.jitter_index + 1, 3) - 0.5);
  }
}

void TemporalSystem::OnComposite() {
  if (enabled_ && frame_open_) {
    frame_.composite_seen = true;
  }
}

// ---------------------------------------------------------------------------
// draws
// ---------------------------------------------------------------------------

void TemporalSystem::OnDraw(const DrawFacts& facts, SharedConstants& shared) {
  if (!frame_open_) {
    return;
  }
  const DrawRecord& r = facts.view->record();
  if (facts.ps && r.ps_identity == temporal::kAnchorPs && r.vs_identity == temporal::kAnchorVs) {
    OnAnchor(facts, shared);
    return;
  }
  const temporal::MainMatch match = temporal::MatchMainView(facts.color0, facts.depth);
  if (match) {
    OnMainView(facts, match, shared);
  }
}

uint32_t TemporalSystem::EvaluateGate(const XenosState& s, const DrawRecord& r,
                                      const ViewportInfo& vp, Scale scale) {
  uint32_t failed = 0;
  if (s.clip.clip_disable != 0) {
    failed |= kGateClipOff;
  }
  if (!(s.vte.vport_x_scale_ena && s.vte.vport_x_offset_ena && s.vte.vport_y_scale_ena &&
        s.vte.vport_y_offset_ena && s.vte.vport_z_scale_ena && s.vte.vport_z_offset_ena)) {
    failed |= kGateNoViewport;
  }
  if (r.primitive == uint32_t(xe::PrimitiveType::kRectangleList)) {
    failed |= kGateRectList;
  }
  // The target is the screen's pitch wide (the match says so); a world draw covers all of it.
  if (vp.viewport.width + 0.5f < float(scale.Edge(temporal::kMainPitch)) ||
      !(vp.viewport.height >= 1.0f)) {
    failed |= kGateNarrow;
  }
  return failed;
}

void TemporalSystem::NoteStencil(const XenosState& s) {
  // The vehicle IDs: a draw that REPLACEs the stencil with a reference of 4k + {0, 1} writes ID k
  // (census 1.1). Either face may do it.
  if (!s.depth_bound || !s.depth_control.stencil_enable) {
    return;
  }
  const xreg::RB_DEPTHCONTROL& dc = s.depth_control;
  if ((s.stencil.stencilwritemask & 0xFFu) != 0 && dc.stencilzpass == xe::StencilOp::kReplace) {
    frame_.stencil_ids |= uint64_t(1) << ((s.stencil.stencilref & 0xFFu) >> 2);
  }
  if (dc.backface_enable && (s.stencil_bf.stencilwritemask & 0xFFu) != 0 &&
      dc.stencilzpass_bf == xe::StencilOp::kReplace) {
    frame_.stencil_ids |= uint64_t(1) << ((s.stencil_bf.stencilref & 0xFFu) >> 2);
  }
}

void TemporalSystem::OnMainView(const DrawFacts& f, const temporal::MainMatch& match,
                                SharedConstants& shared) {
  FrameState& fr = frame_;
  if (match.samples != temporal::kPlaySamples) {
    ++fr.view4x_draws;  // the pause menu, the map: known, never moved
    return;
  }
  if (fr.anchor_seen || fr.composite_seen) {
    // The UI phase draws into a depth key that equals the main view's when the game runs without
    // MSAA; the anchor and the marker are what separate them (census 3.4).
    ++fr.late_draws;
    return;
  }
  const Kind kind = match.color ? (match.depth ? kColorDepth : kColorOnly) : kDepthOnly;
  if (fr.scale.none()) {
    fr.scale = f.scale;
  }
  ++fr.main_draws[kind];
  NoteStencil(*f.state);

  const uint32_t failed = EvaluateGate(*f.state, f.view->record(), *f.viewport, f.scale);
  if (failed != 0) {
    ++fr.gate_rejected;
    for (uint32_t i = 0; i < kGateReasons; ++i) {
      fr.gate_failed[i] += (failed >> i) & 1u;
    }
    return;
  }
  if (!fr.armed) {
    return;
  }
  // Every gated draw of the frame is moved by the same offset, so the depth-only draws, the world
  // and the colour-only draws agree. The viewport is in host pixels; NDC spans 2 of them, +y up
  // (ComputeViewport negated y), so a shift down by jy pixels is -2 jy / height.
  shared.pos_offset[0] += 2.0f * fr.jitter[0] / f.viewport->viewport.width;
  shared.pos_offset[1] -= 2.0f * fr.jitter[1] / f.viewport->viewport.height;
  ++fr.jittered[kind];
}

TemporalSystem::AnchorStatus TemporalSystem::CheckAnchor(const DrawFacts& f, uint32_t* width,
                                                         uint32_t* height,
                                                         std::string* why) const {
  const FrameState& fr = frame_;
  if (fr.view4x_draws != 0) {
    return AnchorStatus::kView4x;
  }
  if (Sum(fr.main_draws) == 0) {
    return AnchorStatus::kNoMainView;
  }
  // Fetches 0, 1, 2: 2D textures of one size, the colour, the stencil view and the depth.
  const uint32_t mask = f.ps->texture_fetch_mask;
  if ((mask & 7u) != 7u) {
    *why = std::format("the pixel shader's fetch mask is {:#x}", mask);
    return AnchorStatus::kFetches;
  }
  uint32_t sizes[3][2] = {};
  for (uint32_t slot = 0; slot < 3; ++slot) {
    uint32_t dwords[6];
    f.view->FetchGroup(slot, dwords);
    xe::xe_gpu_texture_fetch_t t;
    std::memcpy(&t, dwords, sizeof(t));
    sizes[slot][0] = uint32_t(t.size_2d.width) + 1;
    sizes[slot][1] = uint32_t(t.size_2d.height) + 1;
    if (t.type != xe::FetchConstantType::kTexture ||
        uint32_t(t.dimension) != uint32_t(xe::DataDimension::k2DOrStacked) ||
        uint32_t(t.format) != kAnchorFormats[slot]) {
      *why = std::format("fetch {} should be a 2D texture of format {}: it is type {}, "
                         "dimension {}, format {}",
                         slot, kAnchorFormatNames[slot], uint32_t(t.type), uint32_t(t.dimension),
                         uint32_t(t.format));
      return AnchorStatus::kFetches;
    }
  }
  if (sizes[1][0] != sizes[0][0] || sizes[2][0] != sizes[0][0] || sizes[1][1] != sizes[0][1] ||
      sizes[2][1] != sizes[0][1]) {
    *why = std::format("the fetches' sizes differ: {}x{}, {}x{}, {}x{}", sizes[0][0], sizes[0][1],
                       sizes[1][0], sizes[1][1], sizes[2][0], sizes[2][1]);
    return AnchorStatus::kFetches;
  }
  *width = sizes[0][0];
  *height = sizes[0][1];
  // Its target: the half-size float image. The target is allocated a little taller than the
  // picture (640x384 for 640x360, census 1.1), so the viewport, not the allocation, is compared.
  const uint32_t half_w = f.scale.Edge(*width / 2);
  const uint32_t half_h = f.scale.Edge(*height / 2);
  if (!f.color0 ||
      f.color0->format != uint8_t(xe::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) ||
      std::fabs(f.viewport->viewport.width - float(half_w)) >= 0.5f ||
      std::fabs(f.viewport->viewport.height - float(half_h)) >= 0.5f) {
    *why = std::format("its viewport is {:.0f}x{:.0f}, half the frame is {}x{} at scale {}, "
                       "colour format {}",
                       f.viewport->viewport.width, f.viewport->viewport.height, half_w, half_h,
                       f.scale.Text(), f.color0 ? int(f.color0->format) : -1);
    return AnchorStatus::kTarget;
  }
  return AnchorStatus::kAccepted;
}

void TemporalSystem::OnAnchor(const DrawFacts& f, const SharedConstants& shared) {
  FrameState& fr = frame_;
  ++window_.anchors;
  fr.anchor_seen = true;  // the main view's draws are done: no more jitter
  if (fr.anchors++ != 0) {
    ++window_.second_anchors;
    return;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  std::string why;
  AnchorStatus status = CheckAnchor(f, &width, &height, &why);
  temporal::Camera camera;
  if (status == AnchorStatus::kAccepted) {
    const temporal::Reject reject = temporal::ExtractCamera(
        f.banks->ps.data(), width, height, fr.frame, fr.stencil_ids, &camera);
    if (reject != temporal::Reject::kNone) {
      status = StatusOf(reject);
      why = temporal::RejectName(reject);
    }
  }
  fr.anchor_status = status;
  if (status != AnchorStatus::kAccepted) {
    if (status != AnchorStatus::kView4x && !status_logged_[size_t(status)]) {
      status_logged_[size_t(status)] = true;
      REXLOG_INFO("{} the anchor draw of frame {} is not taken: {}{}{} (the first of its kind; "
                  "the summaries count them)",
                  kPrefix, fr.frame, StatusName(status), why.empty() ? "" : ": ", why);
    }
    return;
  }

  // Accepted. What the frame's camera is, and whether its history starts again.
  fr.anchor_ok = true;
  fr.camera = camera;
  fr.guest_size[0] = camera.width;
  fr.guest_size[1] = camera.height;
  fr.render_size[0] = fr.scale.Edge(camera.width);
  fr.render_size[1] = fr.scale.Edge(camera.height);
  for (uint32_t i = 0; i < 3; ++i) {
    f.view->FetchGroup(i, fr.anchor_textures.fetch[i]);
    fr.anchor_textures.table[i] = shared.texture_2d[i];
  }
  fr.anchor_textures.bound = true;

  const Clock::time_point now = Clock::now();
  uint32_t reset = 0;
  temporal::Cut cut;
  if (!ran_last_) {
    reset |= kResetFirst;
  } else {
    const double gap_ms = std::chrono::duration<double, std::milli>(now - last_anchor_time_).count();
    fr.frame_time_ms = float(std::clamp(gap_ms, 1.0, kMaxGapMs));
    if (gap_ms > kMaxGapMs) {
      reset |= kResetTime;
    }
    if (last_camera_.width != camera.width || last_camera_.height != camera.height ||
        last_scale_ != fr.scale) {
      reset |= kResetSize;
    }
    cut = temporal::TestCut(camera, last_camera_);
    if (cut.fires) {
      reset |= kResetCut;
    }
  }
  if (!(camera.previous_valid & 1u)) {
    reset |= kResetNoPrevious;
  }
  last_anchor_time_ = now;
  fr.reset = reset;

  if (reset != 0 && reset_lines_ < kMaxResetLines) {
    ++reset_lines_;
    REXLOG_INFO("{} history starts again in frame {}: {}{} (camera at {:.1f}, {:.1f}, {:.1f}, field "
                "of view {:.2f} deg){}",
                kPrefix, fr.frame, ResetText(reset),
                (reset & kResetCut)
                    ? std::format(" (moved {:.1f}, turn cosine {:.4f}, field of view changed "
                                  "{:.1f}%)",
                                  cut.distance, cut.cosine, 100.0 * cut.fov_change)
                    : std::string(),
                camera.position[0], camera.position[1], camera.position[2],
                camera.fov_y * 180.0 / kPi,
                reset_lines_ == kMaxResetLines ? " (the last of these lines; the summaries count)"
                                               : "");
  }

  // c69 against the view-projection this renderer saw a frame ago: is it the previous frame's, and
  // in which pixel layout (the census could not tell: the camera never moved).
  if (ran_last_ && reset == 0) {
    const temporal::History h = temporal::CompareToHistory(camera, last_camera_);
    ++window_.history_checks;
    window_.history_flip += h.flip < temporal::kHistoryTolerance;
    window_.history_noflip += h.noflip < temporal::kHistoryTolerance;
  }
}

// ---------------------------------------------------------------------------
// the end of the frame, the state machine and the log
// ---------------------------------------------------------------------------

TemporalSystem::Mode TemporalSystem::ModeOf(const FrameState& fr) {
  if (Sum(fr.main_draws) == 0 && fr.view4x_draws == 0) {
    return Mode::kNoScene;
  }
  if (fr.anchor_ok) {
    return Mode::kRunning;
  }
  if (fr.view4x_draws != 0) {
    return Mode::kView4x;
  }
  return Mode::kNoAnchor;
}

void TemporalSystem::EndFrame() {
  if (!enabled_) {
    return;
  }
  if (!frame_open_) {
    // A present with no scene record before it (a loading screen, a movie): no 3D scene.
    frame_ = FrameState{};
    frame_.frame = frame_number_;
  }
  const FrameState& fr = frame_;
  const bool ran = fr.anchor_ok;

  // The safety net: a frame that was jittered, had the main view and no accepted anchor.
  if (fr.armed && Sum(fr.jittered) != 0) {
    if (ran) {
      misses_ = 0;
    } else {
      ++window_.misses;
      if (++misses_ >= kDisarmAfterMisses && !disarmed_) {
        disarmed_ = true;
        REXLOG_WARN("{} {} frames in a row were jittered and had no usable anchor ({}): the "
                    "jitter is off until mcla_gpu_temporal is turned off and on",
                    kPrefix, misses_, StatusName(fr.anchor_status));
      }
    }
  }

  // The run for the next frame.
  ran_last_ = ran;
  if (ran) {
    last_camera_ = fr.camera;
    last_scale_ = fr.scale;
    ++jitter_index_;  // advances in the frames the pass ran in only
  }

  // The window.
  Window& w = window_;
  ++w.frames;
  w.armed += fr.armed;
  const Mode mode = ModeOf(fr);
  ++w.modes[size_t(mode)];
  ++w.anchor_status[size_t(fr.anchor_status)];
  for (uint32_t i = 0; i < kKinds; ++i) {
    w.main_draws[i] += fr.main_draws[i];
    w.jittered[i] += fr.jittered[i];
  }
  for (uint32_t i = 0; i < kGateReasons; ++i) {
    w.gate_failed[i] += fr.gate_failed[i];
  }
  w.gate_rejected += fr.gate_rejected;
  w.view4x_draws += fr.view4x_draws;
  w.late_draws += fr.late_draws;
  if (ran) {
    if (fr.reset != 0) {
      ++w.resets;
      for (uint32_t i = 0; i < kResetReasons; ++i) {
        w.reset_reasons[i] += (fr.reset >> i) & 1u;
      }
    }
    w.vehicle_matrices += uint64_t(std::popcount(fr.camera.previous_valid));
    w.pixel_to_world_agrees += fr.camera.pixel_to_world_agrees;
    w.have_camera = true;
    w.camera = fr.camera;
    w.render_size[0] = fr.render_size[0];
    w.render_size[1] = fr.render_size[1];
  }

  NoteMode(mode, fr);
  ++frame_number_;
  frame_open_ = false;
  if (std::chrono::duration<double>(Clock::now() - window_start_).count() >= kReportSeconds) {
    Report("interval");
  }
}

void TemporalSystem::NoteMode(Mode mode, const FrameState& fr) {
  if (shown_mode_ != Mode::kCount && mode == shown_mode_) {
    pending_mode_ = mode;
    pending_frames_ = 0;
    return;
  }
  if (shown_mode_ != Mode::kCount) {
    // A change counts once it has lasted a few frames (a pause menu opens through single frames).
    pending_frames_ = mode == pending_mode_ ? pending_frames_ + 1 : 1;
    pending_mode_ = mode;
    if (pending_frames_ < kModeDebounce) {
      return;
    }
  }
  shown_mode_ = mode;
  pending_mode_ = mode;
  pending_frames_ = 0;
  switch (mode) {
    case Mode::kNoScene:
      REXLOG_INFO("{} frame {}: no 3D scene (a menu, loading, a movie): nothing is moved, no camera",
                  kPrefix, fr.frame);
      break;
    case Mode::kView4x:
      REXLOG_INFO("{} frame {}: the 3D scene is at 4x samples (the pause menu, the map): nothing is "
                  "moved, no camera", kPrefix, fr.frame);
      break;
    case Mode::kNoAnchor:
      REXLOG_INFO("{} frame {}: the 3D scene is there but its anchor draw is not taken ({}): the "
                  "frames are not temporal",
                  kPrefix, fr.frame, StatusName(fr.anchor_status));
      break;
    case Mode::kRunning:
      REXLOG_INFO("{} frame {}: running: the 3D scene at 2x samples, the anchor taken (camera at "
                  "{:.1f}, {:.1f}, {:.1f}, field of view {:.2f} deg, near {:.2f}, far {:.0f}, "
                  "frame {}x{}, render {}x{}), jitter {}",
                  kPrefix, fr.frame, fr.camera.position[0], fr.camera.position[1],
                  fr.camera.position[2], fr.camera.fov_y * 180.0 / kPi,
                  fr.camera.near_plane, fr.camera.far_plane, fr.guest_size[0], fr.guest_size[1],
                  fr.render_size[0], fr.render_size[1],
                  fr.armed ? std::format("{:+.3f}, {:+.3f} px (phase {} of {})", fr.jitter[0],
                                         fr.jitter[1], fr.jitter_index, fr.phase_count)
                           : std::string("starts with the next frame"));
      break;
    case Mode::kCount:
      break;
  }
}

void TemporalSystem::Report(const char* why) {
  const Window& w = window_;
  const double seconds = std::chrono::duration<double>(Clock::now() - window_start_).count();
  const uint64_t running = w.modes[size_t(Mode::kRunning)];
  const uint64_t scene = running + w.modes[size_t(Mode::kNoAnchor)];
  const double per_scene = scene != 0 ? 1.0 / double(scene) : 0.0;

  std::string refused;
  for (size_t i = size_t(AnchorStatus::kNoMainView); i < size_t(AnchorStatus::kCount); ++i) {
    if (w.anchor_status[i] != 0 && i != size_t(AnchorStatus::kView4x)) {
      refused += std::format("{}{} x{}", refused.empty() ? "" : "; ", StatusName(AnchorStatus(i)),
                             w.anchor_status[i]);
    }
  }
  std::string resets;
  for (uint32_t i = 0; i < kResetReasons; ++i) {
    if (w.reset_reasons[i] != 0) {
      resets += std::format("{}{} {}", resets.empty() ? "" : ", ", ResetName(i), w.reset_reasons[i]);
    }
  }
  std::string camera = "none";
  if (w.have_camera) {
    camera = std::format("({:.1f}, {:.1f}, {:.1f}), field of view {:.2f} deg, near {:.2f}, far "
                         "{:.0f}, frame {}x{}, render {}x{}",
                         w.camera.position[0], w.camera.position[1], w.camera.position[2],
                         w.camera.fov_y * 180.0 / kPi, w.camera.near_plane, w.camera.far_plane,
                         w.camera.width, w.camera.height, w.render_size[0], w.render_size[1]);
  }
  REXLOG_INFO("{} ({}) {} frames in {:.1f} s: 3D scene {} (anchor taken {}, at 4x samples {}), no "
              "scene {}, jitter armed {} | refused anchors: {} | history restarts: {} ({}) | camera "
              "{}",
              kPrefix, why, w.frames, seconds, scene, running, w.modes[size_t(Mode::kView4x)],
              w.modes[size_t(Mode::kNoScene)], w.armed, refused.empty() ? "none" : refused,
              w.resets, resets.empty() ? "none" : resets, camera);

  std::string gate;
  static const char* const kGateNames[kGateReasons] = {"clipping off", "no viewport transform",
                                                       "rectangle list", "narrow viewport"};
  for (uint32_t i = 0; i < kGateReasons; ++i) {
    if (w.gate_failed[i] != 0) {
      gate += std::format("{}{} {:.2f}", gate.empty() ? "" : ", ", kGateNames[i],
                          double(w.gate_failed[i]) * per_scene);
    }
  }
  const uint64_t drawn = w.main_draws[kColorDepth] + w.main_draws[kColorOnly] + w.main_draws[kDepthOnly];
  const uint64_t moved = w.jittered[kColorDepth] + w.jittered[kColorOnly] + w.jittered[kDepthOnly];
  REXLOG_INFO("{} ({}) per 3D scene frame: main view draws {:.1f} (colour+depth {:.1f}, colour only "
              "{:.1f}, depth only {:.1f}), moved {:.1f} (colour+depth {:.1f}, colour only {:.1f}, "
              "depth only {:.1f}), kept still {:.2f} ({}), after the anchor {:.2f}, second anchors "
              "{} | c69 is the previous view-projection times the pixel matrix in {} of {} checks "
              "(with y down: {}) | usable previous matrices {:.2f} a frame | c64-c67 agree with "
              "the inverse view-projection in {} of {}",
              kPrefix, why, double(drawn) * per_scene, double(w.main_draws[kColorDepth]) * per_scene,
              double(w.main_draws[kColorOnly]) * per_scene,
              double(w.main_draws[kDepthOnly]) * per_scene, double(moved) * per_scene,
              double(w.jittered[kColorDepth]) * per_scene,
              double(w.jittered[kColorOnly]) * per_scene,
              double(w.jittered[kDepthOnly]) * per_scene, double(w.gate_rejected) * per_scene,
              gate.empty() ? "none" : gate, double(w.late_draws) * per_scene, w.second_anchors,
              w.history_flip, w.history_checks, w.history_noflip,
              running != 0 ? double(w.vehicle_matrices) / double(running) : 0.0,
              w.pixel_to_world_agrees, running);
  window_ = Window{};
  window_start_ = Clock::now();
}

void TemporalSystem::Finish() {
  if (enabled_ && window_.frames != 0) {
    Report("final");
  }
}

}  // namespace mcla::gpu
