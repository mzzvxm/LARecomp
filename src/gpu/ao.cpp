// MCLA GPU renderer -- screen-space ambient occlusion. See ao.h.

#include "ao.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(
    mcla_gpu_ao_census, 0, "MCLA/GPU",
    "Diagnostic for the ambient occlusion design (feature-parity/AO-DESIGN.md step 0): every Nth "
    "guest frame (and the first three frames with the main view), one block of log lines with the "
    "main view's draws around the game's mid-scene depth resolve, that resolve (where it is, how "
    "big, inside EndTiling or not), the light constants the occlusion would use (PS c26 ambient, PS "
    "c39 sun colour, VS c16 sun direction), the depth clear value and the lens the game's motion "
    "blur draw gives. Frames where the main view does not have exactly one qualifying resolve are "
    "logged as they come (the first 40). 0: off. Live.")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[ao census]";

// The registers the design reads (AO-DESIGN.md 3.2): PS c26 gLightAmbient, PS c39 gLightColor, VS
// c16 gLightPosDir.
constexpr uint32_t kPsAmbient = 26;
constexpr uint32_t kPsSunColor = 39;
constexpr uint32_t kVsSunDirection = 16;

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

double Length3(const float v[3]) {
  return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
}

std::string Vec3(const float v[3]) {
  return std::format("({:.4g}, {:.4g}, {:.4g})", v[0], v[1], v[2]);
}

std::string RefusalNames(uint32_t refused) {
  static const char* const kNames[] = {
      "not a depth copy",        "not the main view's depth",
      "inside EndTiling",        "not the first qualifying one of the frame",
      "after the anchor or the composite marker", "not into 078B0000",
      "the same call clears the depth", "not the whole picture from the origin"};
  std::string out;
  for (uint32_t i = 0; i < 8; ++i) {
    if (refused & (1u << i)) {
      out += std::format("{}{}", out.empty() ? "" : ", ", kNames[i]);
    }
  }
  return out;
}

}  // namespace

bool AmbientOcclusion::LightRaw::operator==(const LightRaw& o) const {
  return std::memcmp(dwords, o.dwords, sizeof(dwords)) == 0;
}

void AmbientOcclusion::DecodeLight(const LightRaw& raw, float ambient[3], float sun[3],
                                   float dir[3]) {
  for (int i = 0; i < 3; ++i) {
    ambient[i] = std::bit_cast<float>(raw.dwords[i]);
    sun[i] = std::bit_cast<float>(raw.dwords[3 + i]);
    dir[i] = std::bit_cast<float>(raw.dwords[6 + i]);
  }
}

void AmbientOcclusion::BeginFrame() {
  census_every_ = uint32_t(std::max(0, REXCVAR_GET(mcla_gpu_ao_census)));
}

// ---------------------------------------------------------------------------
// what the draws tell
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnMainDraw(const MainDraw& draw, const ConstantBanks::Banks& banks) {
  frame_had_main_view_ = true;
  if (draw.match.samples == temporal::kPauseSamples) {
    ++view4x_draws_;  // the pause menu's and the map's world: no occlusion there
    return;
  }
  play_view_ = true;
  if (anchor_seen_ || composite_seen_) {
    ++draws_at_anchor_;
    return;
  }
  if (qualifying_resolves_ != 0) {
    ++draws_after_;
    if (draw.match.depth && draw.writes_depth) {
      ++depth_writes_after_;
    }
    return;
  }
  ++draws_before_;
  if (!draw.match.color) {
    return;
  }
  ++color_draws_before_;
  // The registers the banks of this draw hold (a draw whose shaders do not use them leaves what an
  // earlier one set: the value most colour draws carry is the frame's).
  LightRaw light;
  const uint8_t* ps = banks.ps.data();
  const uint8_t* vs = banks.vs.data();
  for (uint32_t i = 0; i < 3; ++i) {
    light.dwords[i] = LoadBe32(ps + kPsAmbient * 16 + i * 4);
    light.dwords[3 + i] = LoadBe32(ps + kPsSunColor * 16 + i * 4);
    light.dwords[6 + i] = LoadBe32(vs + kVsSunDirection * 16 + i * 4);
  }
  if (light_draws_ == 0) {
    first_light_ = light;
  }
  last_light_ = light;
  ++light_draws_;
  for (uint32_t i = 0; i < light_count_; ++i) {
    if (lights_[i].raw == light) {
      ++lights_[i].draws;
      return;
    }
  }
  if (light_count_ < kLightSlots) {
    lights_[light_count_].raw = light;
    lights_[light_count_].draws = 1;
    ++light_count_;
  } else {
    ++light_overflow_;
  }
}

bool AmbientOcclusion::PickLight(LightRaw* out, uint32_t* draws_with_it) const {
  if (light_draws_ == 0) {
    return false;
  }
  if (light_count_ == 0) {
    *out = last_light_;
    *draws_with_it = 0;
    return true;
  }
  uint32_t best = 0;
  for (uint32_t i = 1; i < light_count_; ++i) {
    if (lights_[i].draws >= lights_[best].draws) {
      best = i;
    }
  }
  *out = lights_[best].raw;
  *draws_with_it = lights_[best].draws;
  return true;
}

void AmbientOcclusion::OnDepthClear(const EdramKey& key, float z) {
  const uint32_t samples = temporal::MainDepthSamples(key);
  if (samples == 0 || samples == temporal::kPauseSamples) {
    return;
  }
  ++depth_clears_;
  depth_clear_z_ = z;
}

void AmbientOcclusion::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  const uint8_t* ps = banks.ps.data();
  // The fetch size the camera test compares c62 against is c62 itself here: only the temporal
  // system knows the anchor's fetches, and a frame size that is not the main view's is caught by
  // the rectangle of the trigger (ao.cpp Judge).
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  have_camera_ = false;
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    reject_ = temporal::Reject::kFrameSize;
    have_previous_ = false;
    return;
  }
  temporal::Camera camera;
  reject_ = temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_,
                                    &camera);
  if (reject_ != temporal::Reject::kNone) {
    have_previous_ = false;
    if (rejects_logged_ < 8) {
      ++rejects_logged_;
      REXLOG_INFO("[ao] the anchor's constants gave no camera: {} (frame {}, logged {} of 8)",
                  temporal::RejectName(reject_), frame_, rejects_logged_);
    }
    return;
  }
  // The lens the previous frame's camera gave the trigger, against this frame's own (CHECK 7: the
  // previous frame's lens is this frame's outside cuts).
  lens_delta_valid_ = have_previous_ && previous_frame_ + 1 == frame_;
  if (lens_delta_valid_) {
    const auto rel = [](double a, double b) {
      return std::fabs(a - b) / std::max(1e-30, std::fabs(b));
    };
    lens_delta_[0] = rel(camera.fov_y, previous_.fov_y);
    lens_delta_[1] = rel(camera.aspect, previous_.aspect);
    lens_delta_[2] = rel(camera.near_plane, previous_.near_plane);
    lens_delta_[3] = rel(camera.far_plane, previous_.far_plane);
    double dx = 0.0;
    double dot = 0.0;
    for (int i = 0; i < 3; ++i) {
      const double d = camera.position[i] - previous_.position[i];
      dx += d * d;
      dot += camera.forward[i] * previous_.forward[i];
    }
    lens_delta_[4] = std::sqrt(dx);
    lens_delta_[5] = dot;
  }
  camera_ = camera;
  have_camera_ = true;
  previous_ = camera;
  have_previous_ = true;
  previous_frame_ = frame_;
}

// ---------------------------------------------------------------------------
// the trigger
// ---------------------------------------------------------------------------

uint32_t AmbientOcclusion::Judge(const ResolveRecord& record,
                                 const ResolveCache::Summary& summary) const {
  if (!summary.depth_copy) {
    return kRefusedNotDepthCopy;
  }
  uint32_t refused = 0;
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  if (samples == 0 || samples == temporal::kPauseSamples) {
    refused |= kRefusedNotMainDepth;
  }
  if (record.flags & kEventInsideEndTiling) {
    refused |= kRefusedEndTiling;
  }
  if (qualifying_resolves_ != 0) {
    refused |= kRefusedNotFirst;
  }
  if (anchor_seen_ || composite_seen_) {
    refused |= kRefusedAfterAnchor;
  }
  if (summary.dest_address != kTriggerDestination) {
    refused |= kRefusedDestination;
  }
  if (summary.clears_depth) {
    refused |= kRefusedClearsDepth;
  }
  if (summary.x0 != 0 || summary.y0 != 0 || summary.x1 <= 0 || summary.y1 <= 0) {
    refused |= kRefusedRectangle;
  }
  return refused;
}

void AmbientOcclusion::OnResolve(const ResolveRecord& record,
                                 const ResolveCache::Summary& summary) {
  if (!summary.depth_copy) {
    ++other_resolves_;
    return;
  }
  const uint32_t refused = Judge(record, summary);
  const bool qualifies = (refused & ~uint32_t(kRefusedNotFirst)) == 0;
  if (census_every_ != 0 && depth_resolves_.size() < 16) {
    DepthResolveNote note;
    note.caller = record.caller;
    note.flags = record.flags;
    note.source = summary.source_key;
    note.dest_address = summary.dest_address;
    note.dest_format = summary.dest_format;
    note.dest_width = summary.dest_width;
    note.dest_height = summary.dest_height;
    note.rect[0] = summary.x0;
    note.rect[1] = summary.y0;
    note.rect[2] = summary.x1;
    note.rect[3] = summary.y1;
    note.host_sample = summary.host_sample;
    note.host_samples = summary.source ? summary.source->host_samples : 0;
    note.inside_end_tiling = (record.flags & kEventInsideEndTiling) != 0;
    note.clears_depth = summary.clears_depth;
    note.refused = refused;
    note.draws_before = draws_before_ + draws_after_;
    note.anchor_before = anchor_seen_;
    depth_resolves_.push_back(note);
  }
  if (qualifies) {
    ++qualifying_resolves_;
  }
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void AmbientOcclusion::OnPresent() {
  if (census_every_ != 0 && frame_had_main_view_) {
    Census();
  }
  anchor_seen_ = false;
  composite_seen_ = false;
  play_view_ = false;
  frame_had_main_view_ = false;
  qualifying_resolves_ = 0;
  draws_before_ = 0;
  color_draws_before_ = 0;
  draws_after_ = 0;
  depth_writes_after_ = 0;
  draws_at_anchor_ = 0;
  view4x_draws_ = 0;
  light_count_ = 0;
  light_overflow_ = 0;
  light_draws_ = 0;
  depth_clears_ = 0;
  depth_resolves_.clear();
  other_resolves_ = 0;
  lens_delta_valid_ = false;
  have_camera_ = false;
  ++frame_;
}

void AmbientOcclusion::Census() {
  const bool anomaly = play_view_ && qualifying_resolves_ != 1 && census_anomalies_logged_ < 40;
  const bool periodic = census_frames_logged_ < 3 || frame_ % census_every_ == 0;
  if (!anomaly && !periodic) {
    return;
  }
  if (anomaly) {
    ++census_anomalies_logged_;
  }
  ++census_frames_logged_;
  REXLOG_INFO("{} frame {}{}: {} view, main view draws {} before the first qualifying depth "
              "resolve ({} with colour), {} after it before the anchor ({} of them write the main "
              "depth), {} after the anchor, 4x (pause/map) draws {}; qualifying resolves {} "
              "(expect exactly 1); anchor draw {}; clears of the main depth {} (last z {}); "
              "resolves of other kinds {}",
              kPrefix, frame_, anomaly ? " (UNEXPECTED trigger count)" : "",
              play_view_ ? "play" : "pause/map", draws_before_, color_draws_before_, draws_after_,
              depth_writes_after_, draws_at_anchor_, view4x_draws_, qualifying_resolves_,
              anchor_seen_ ? "seen" : "NOT seen", depth_clears_,
              depth_clears_ ? std::format("{}", depth_clear_z_) : std::string("none"),
              other_resolves_);
  uint32_t index = 0;
  for (const DepthResolveNote& n : depth_resolves_) {
    REXLOG_INFO("{}   depth resolve {}: caller {:#010x}, source {}, host sample {} of {}, "
                "destination {:08x} format {} {}x{}, rect ({}, {})-({}, {}), {}, {}, after {} main "
                "view draws, anchor {}: {}",
                kPrefix, index++, n.caller, DescribeEdramKey(n.source), n.host_sample,
                n.host_samples, n.dest_address, n.dest_format, n.dest_width, n.dest_height,
                n.rect[0], n.rect[1], n.rect[2], n.rect[3],
                n.inside_end_tiling ? "inside EndTiling" : "plain resolve",
                n.clears_depth ? "clears the depth after" : "no clear", n.draws_before,
                n.anchor_before ? "already drawn" : "not yet",
                n.refused == 0 ? "THE TRIGGER"
                               : std::format("not the trigger ({})", RefusalNames(n.refused)));
  }
  LightRaw picked;
  uint32_t with_it = 0;
  if (PickLight(&picked, &with_it)) {
    float ambient[3], sun[3], dir[3];
    DecodeLight(picked, ambient, sun, dir);
    float first_a[3], first_s[3], first_d[3], last_a[3], last_s[3], last_d[3];
    DecodeLight(first_light_, first_a, first_s, first_d);
    DecodeLight(last_light_, last_a, last_s, last_d);
    REXLOG_INFO("{}   light constants: {} distinct values over {} colour draws before the trigger "
                "({} draws past the table); the commonest ({} draws): PS c26 ambient {}, PS c39 sun "
                "colour {}, VS c16 sun direction {} (length {:.4f}); first draw: {} {} {}; last "
                "draw: {} {} {}",
                kPrefix, light_count_ + (light_overflow_ ? 1 : 0), light_draws_, light_overflow_,
                with_it, Vec3(ambient), Vec3(sun), Vec3(dir), Length3(dir), Vec3(first_a),
                Vec3(first_s), Vec3(first_d), Vec3(last_a), Vec3(last_s), Vec3(last_d));
    if (have_camera_) {
      // The sun direction through this frame's view (row vectors, v' = v M, the game's view space
      // looks along -z): the up-facing ground at noon wants L.y > 0 if c16 points at the sun.
      double view[3] = {};
      for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
          view[j] += double(dir[i]) * camera_.view.m[i][j];
        }
      }
      const double ambient_luma =
          0.2126 * ambient[0] + 0.7152 * ambient[1] + 0.0722 * ambient[2];
      const double sun_luma = 0.2126 * sun[0] + 0.7152 * sun[1] + 0.0722 * sun[2];
      REXLOG_INFO("{}   sun: world direction y {:+.3f} (a sun overhead at noon with c16 pointing at "
                  "it gives +), in the game's view space ({:+.3f}, {:+.3f}, {:+.3f}); luma "
                  "ambient {:.4g}, sun {:.4g}: ambient share for a surface facing the sun "
                  "{:.3f}, a surface in shade 1.000",
                  kPrefix, double(dir[1]), view[0], view[1], view[2], ambient_luma, sun_luma,
                  ambient_luma + sun_luma > 1e-9 ? ambient_luma / (ambient_luma + sun_luma) : 1.0);
    }
  } else {
    REXLOG_INFO("{}   light constants: no colour draw of the main view before the trigger", kPrefix);
  }
  if (have_camera_) {
    const temporal::Camera& c = camera_;
    REXLOG_INFO("{}   lens (the game's motion blur draw, this frame): vertical field of view {:.3f} "
                "deg, aspect {:.4f}, near {:.4f}, far {:.1f}, c62 {}x{}, camera at ({:.1f}, {:.1f}, "
                "{:.1f}) looking ({:+.3f}, {:+.3f}, {:+.3f})",
                kPrefix, c.fov_y * 180.0 / 3.14159265358979323846, c.aspect, c.near_plane,
                c.far_plane, c.width, c.height, c.position[0], c.position[1], c.position[2],
                c.forward[0], c.forward[1], c.forward[2]);
    if (lens_delta_valid_) {
      REXLOG_INFO("{}   the previous frame's lens (what the trigger would use) against this one's: "
                  "relative change fov {:.2e}, aspect {:.2e}, near {:.2e}, far {:.2e}; camera moved "
                  "{:.3f}, cosine of the turn {:.6f}",
                  kPrefix, lens_delta_[0], lens_delta_[1], lens_delta_[2], lens_delta_[3],
                  lens_delta_[4], lens_delta_[5]);
    } else {
      REXLOG_INFO("{}   no previous frame's camera: the trigger would give no occlusion this frame",
                  kPrefix);
    }
  } else {
    REXLOG_INFO("{}   no camera this frame ({})", kPrefix,
                anchor_seen_ ? temporal::RejectName(reject_) : "no anchor draw");
  }
}

}  // namespace mcla::gpu
