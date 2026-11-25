// MCLA GPU renderer -- the side mirrors' glass found without ray tracing. See mirror_glass.h.

#include "mirror_glass.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

namespace mcla::gpu {

namespace mg = mirror_glass;

namespace {

constexpr const char* kPrefix = "[mirror glass]";
// The window's line: every 1800 frames (half a minute at 60 FPS) while the finder is on.
constexpr uint64_t kLogEveryFrames = 1800;

double Length3(const float v[3]) { return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]); }

}  // namespace

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void MirrorGlass::Initialize(CarFrame& car_frame) {
  car_frame_ = &car_frame;
  car_frame.AddListener(this);
  finder_ = mg::Finder{};
  hold_.Reset();
  result_ = mg::Result{};
  evaluated_ = false;
  active_ = false;
  published_ = false;
  frame_ = 0;
  window_ = Window{};
  pair_logged_ = false;
  first_need_logged_ = false;
  ready_ = true;
}

void MirrorGlass::Shutdown() {
  if (published_) {
    mg::WithdrawNeed();
  }
  published_ = false;
  active_ = false;
  ready_ = false;
  car_frame_ = nullptr;
  finder_ = mg::Finder{};
  hold_.Reset();
  result_ = mg::Result{};
  evaluated_ = false;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void MirrorGlass::BeginFrame() {
  if (!ready_ || !car_frame_) {
    return;
  }
  const bool wanted = larecomp::settings::mcla_gpu_mirror_glass.Get();
  if (wanted != active_) {
    REXLOG_INFO("{} the glass finder is {} (mcla_gpu_mirror_glass)", kPrefix, wanted ? "on" : "off");
    if (!wanted && published_) {
      mg::WithdrawNeed();  // the guest reads "no need" from the next frame on
      published_ = false;
      hold_.Reset();
    }
    window_ = Window{};
    window_.first_frame = frame_;
  }
  active_ = wanted;
  evaluated_ = false;
  if (!active_) {
    result_ = mg::Result{};  // Current() of a finder that is off is the empty answer
    return;
  }
  // CarFrame collects the next frame's draws when asked in this one (a request is for the frame after, as every user's).
  car_frame_->Request();
  finder_.BeginFrame(frame_);
}

void MirrorGlass::OnPlayerDraw(const car_frame::Draw& draw, const car_frame::MeshReader& reader) {
  if (!ready_ || !active_) {
    return;  // another effect asked CarFrame for the draws: nothing to read for this one
  }
  ++window_.draws;
  // CHECK: the cost of this call for ~210 draws a frame on the render thread (a probe each once the meshes are known) and of the first frames' reads (at most kReadsPerFrame, each inside the draw's call with the guest held back).
  const mg::Finder::Reply reply = finder_.OnDraw(draw.part == car_frame::Part::kBody, draw.mesh_key, draw.geometry.count, draw.ps,
                                                 [&](std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals) {
                                                   return reader.ReadTriangles(positions, indices, normals);
                                                 });
  if (reply.skip != mg::Skip::kNone) {
    ++window_.skipped[size_t(reply.skip)];
    return;
  }
  ++window_.looked_at;
  switch (reply.outcome) {
    case mg::Finder::Outcome::kMeasured: {
      ++window_.measured;
      const mg::GlassCache::Entry* e = finder_.cache().Find(draw.mesh_key);
      if (e && e->stats.valid) {
        ++window_.patches_seen;
      }
      break;
    }
    case mg::Finder::Outcome::kUnreadable: ++window_.unreadable; break;
    case mg::Finder::Outcome::kDeferred: ++window_.deferred; break;
    case mg::Finder::Outcome::kWaiting: ++window_.waiting; break;
    default: break;
  }
}

// The frame's complete draw list in, the pair out. Called when the draws are in (the scene's last colour step, the present): see Current().
void MirrorGlass::Evaluate(const car_frame::Frame& frame) {
  evaluated_ = true;
  evaluated_frame_ = frame_;
  evaluated_draws_ = frame.draws.size();
  evaluated_valid_ = frame.valid;
  result_ = mg::Result{};
  result_.guest_frame = frame_;
  if (!active_ || !frame.valid || frame.draws.empty()) {
    result_.why = mg::Why::kNoCar;
    return;
  }
  finder_.BeginEvaluation();
  for (uint32_t i = 0; i < frame.draws.size(); ++i) {
    const car_frame::Draw& d = frame.draws[i];
    finder_.Consider(i, d.part == car_frame::Part::kBody, d.mesh_key, d.geometry.count, d.ps, d.model_to_world);
  }
  mg::Camera camera;
  camera.view = frame.view;
  camera.view_projection = frame.view_projection;
  camera.flip_z = false;  // Frame::view is the occlusion's view space (z forward) by construction (mirror_glass_rules.h Camera)
  result_ = finder_.Choose(frame_, camera);
}

const mg::Result& MirrorGlass::Current() {
  if (!ready_ || !car_frame_ || !active_) {
    return result_;  // the empty answer (BeginFrame, Initialize and Shutdown leave it so while the finder is off)
  }
  const car_frame::Frame& frame = car_frame_->Current();
  if (!evaluated_ || evaluated_frame_ != frame_ || evaluated_draws_ != frame.draws.size() || evaluated_valid_ != frame.valid) {
    Evaluate(frame);
  }
  return result_;
}

const car_frame::Draw* MirrorGlass::DrawOf(uint32_t k) {
  const mg::Result& r = Current();
  if (!r.found || k > 1 || !car_frame_) {
    return nullptr;
  }
  const car_frame::Frame& frame = car_frame_->Current();
  const uint32_t at = r.glass[k].draw;
  return at < frame.draws.size() ? &frame.draws[at] : nullptr;
}

void MirrorGlass::OnPresent() {
  if (!ready_) {
    return;
  }
  if (!active_) {
    ++frame_;  // the finder's clock runs on while it is off: a re-enabled finder's cache ages by real frames
    return;
  }
  const mg::Result& r = Current();
  ++window_.frames;
  ++window_.why[size_t(r.why)];
  window_.found += r.found ? 1u : 0u;
  window_.needed += r.need.need() ? 1u : 0u;
  if (r.found) {
    LogPair(r);
  }
  // The guest's flag: this frame's need, held for a few frames so a missing draw or a camera cut does not toggle the phase.
  const mg::NeedState published = hold_.Update(r.need);
  mg::PublishNeed(published);
  published_ = true;
  if (published.need() && !first_need_logged_) {
    first_need_logged_ = true;
    REXLOG_INFO("{} frame {}: the side mirrors need the shared rear view (mask {}, widest {} px at 1080 lines)", kPrefix, frame_, unsigned(published.mask), unsigned(published.widest));
  }
  window_.swept += finder_.SweepIfDue(frame_);
  ++frame_;
  if (frame_ >= window_.first_frame + kLogEveryFrames) {
    LogWindow();
  }
}

// ---------------------------------------------------------------------------
// the log
// ---------------------------------------------------------------------------

// The pair, when it is new or has changed: what MIRROR-RT's CHECK asks for (area, normals, separation, height) and the widths on the picture.
void MirrorGlass::LogPair(const mg::Result& r) {
  if (pair_logged_ && pair_keys_[0] == r.glass[0].mesh_key && pair_keys_[1] == r.glass[1].mesh_key) {
    return;
  }
  pair_logged_ = true;
  pair_keys_[0] = r.glass[0].mesh_key;
  pair_keys_[1] = r.glass[1].mesh_key;
  const mg::Glass& a = r.glass[0];
  const mg::Glass& b = r.glass[1];
  float d[3];
  for (int c = 0; c < 3; ++c) {
    d[c] = b.centre_view[c] - a.centre_view[c];
  }
  const double separation = Length3(d);
  const double height = std::fabs(double(d[0]) * r.up_view[0] + double(d[1]) * r.up_view[1] + double(d[2]) * r.up_view[2]);
  REXLOG_INFO("{} frame {}: a pair of mirror glasses: draws {} and {} (keys {:016x} and {:016x}), areas {:.4f} and {:.4f} m2, normals ({:.2f}, {:.2f}, {:.2f}) and ({:.2f}, {:.2f}, {:.2f}) in the world, "
              "separation {:.2f} m, height difference {:.3f} m, score {:.2f}; on the picture (1080 lines): left {} px, right {} px: {}",
              kPrefix, frame_, a.draw, b.draw, a.mesh_key, b.mesh_key, double(a.area), double(b.area), double(a.normal_world[0]), double(a.normal_world[1]), double(a.normal_world[2]),
              double(b.normal_world[0]), double(b.normal_world[1]), double(b.normal_world[2]), separation, height, double(r.score), a.px_wide, b.px_wide, mg::WhyName(r.why));
}

void MirrorGlass::LogWindow() {
  const Window& w = window_;
  std::string why;
  for (size_t i = 0; i < size_t(mg::Why::kCount); ++i) {
    if (w.why[i] != 0) {
      why += std::format("{}{} {}", why.empty() ? "" : ", ", mg::WhyName(mg::Why(i)), w.why[i]);
    }
  }
  std::string skipped;
  for (size_t i = size_t(mg::Skip::kNotBody); i < size_t(mg::Skip::kCount); ++i) {
    if (w.skipped[i] != 0) {
      skipped += std::format("{}{} {}", skipped.empty() ? "" : ", ", mg::SkipName(mg::Skip(i)), w.skipped[i]);
    }
  }
  REXLOG_INFO("{} frames {}..{}: {} evaluated, {} with a pair, {} needing a view; frames: {}; {} draws of the car, {} looked at ({} meshes measured, {} of them patches, {} unreadable, "
              "{} left for later frames, {} waiting to retry), left out: {}; {} meshes cached ({} swept)",
              kPrefix, w.first_frame, frame_, w.frames, w.found, w.needed, why.empty() ? "none" : why, w.draws, w.looked_at, w.measured, w.patches_seen, w.unreadable, w.deferred,
              w.waiting, skipped.empty() ? "none" : skipped, finder_.cache().size(), w.swept);
  window_ = Window{};
  window_.first_frame = frame_;
}

}  // namespace mcla::gpu
