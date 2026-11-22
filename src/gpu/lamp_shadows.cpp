// MCLA GPU renderer -- the street lamps' shadows: the provider of the shadow atlas. See lamp_shadows.h and lamp_shadows_rules.h.

#include "lamp_shadows.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "mc_engine/cinematic/capture_mode.h"
#include "shadow_atlas.h"
#include "volumetrics_lamps.h"

namespace mcla::gpu {

namespace {

namespace la = lamp_shadows;
namespace lb = light_budget;
namespace ls = local_shadow;

constexpr const char* kPrefix = "[lamp-shadows]";
// A copy of the game's lamp list this old is not the list any more (the hook is not running, a menu, a day with the hook idle): the volumetric light's number.
constexpr std::chrono::milliseconds kListMaxAge{500};
constexpr double kGapSeconds = 0.5;          // a gap between two picks this long starts the picks again at full strength (a menu, a cut scene)
constexpr uint64_t kHookWarnFrames = 600;    // frames of night without one copy of the list before the log says the hook is not in the build
constexpr uint64_t kLogFrames = 1800;

}  // namespace

LampShadows::LampShadows() = default;
LampShadows::~LampShadows() = default;

void LampShadows::Reset() {
  Leave();
  vol_lamps::SetWantedBy(vol_lamps::kWantedByLampShadows, false);
  picker_.Reset();
  pick_ = la::PickResult{};
  early_.clear();
  pending_.clear();
  published_.clear();
  ClearFrame();
  for (la::StaticLayer& layer : layers_) {
    layer.Invalidate();
  }
  list_.clear();
  list_seq_ = 0;
  lamps_.clear();
  have_last_ = false;
  active_ = false;
  layout_ = shadow_atlas::Layout{};
  live_pool_ = static_pool_ = -1;
  seen_generation_ = 0;
}

void LampShadows::Join() {
  if (atlas_ != nullptr && !registered_) {
    atlas_->AddProvider(this);
    registered_ = true;
  }
}

void LampShadows::Leave() {
  if (atlas_ != nullptr && registered_) {
    atlas_->RemoveProvider(this);
  }
  registered_ = false;
}

uint32_t LampShadows::AtlasGeneration() const { return atlas_ != nullptr ? atlas_->generation() : 0u; }

// ---------------------------------------------------------------------------
// the frame's start
// ---------------------------------------------------------------------------

bool LampShadows::Enabled() const {
  return la::LampsEnabled(int(larecomp::settings::mcla_gpu_lamp_shadows.Get()), pass_runs_);
}

bool LampShadows::Poll(bool wanted, uint64_t frame) {
  namespace st = larecomp::settings;
  serial_ = frame;
  on_ = Enabled();
  if (on_ != enabled_logged_) {
    enabled_logged_ = on_;
    const int setting = int(st::mcla_gpu_lamp_shadows.Get());
    REXLOG_INFO("{} {} (mcla_gpu_lamp_shadows {}{}; the deferred light pass runs without the lamps: {}; live)", kPrefix, on_ ? "the street lamps are shadowed" : "the street lamps are not shadowed", setting,
                setting < 0 ? ", auto: follows LIGHT SHADOWS while the light pass runs" : (setting == 0 ? ", off" : ", on"), pass_runs_ ? "yes" : "no");
  }
  mode_ = la::ModeOf(int(st::mcla_gpu_lamp_shadows_mode.Get()));
  debug_view_ = uint32_t(std::clamp<int>(int(st::mcla_gpu_lamp_shadows_view.Get()), 0, 3));  // 0..3 (local_lights_rules.h kDebug*): 3 is the units check "remove"
  lamps_override_ = int(st::mcla_gpu_lamp_shadows_lamps.Get());
  cache_ = st::mcla_gpu_lamp_shadows_cache.Get() != 0;
  const bool want = on_ && wanted && atlas_ != nullptr;
  vol_lamps::SetWantedBy(vol_lamps::kWantedByLampShadows, want);
  active_ = false;
  if (!want) {
    if (registered_ || have_last_ || !lamps_.empty() || frame_.count != 0) {
      picker_.Reset();
      have_last_ = false;
      lamps_.clear();
      list_.clear();
      list_seq_ = 0;
      pick_ = la::PickResult{};
      early_.clear();
      pending_.clear();
      Leave();
      ClearFrame();
    }
    return false;
  }
  bool stale = false;
  if (vol_lamps::Global().Latest(&list_, &list_seq_, &stale, kListMaxAge)) {
    lamps_.clear();
    lamps_.reserve(list_.size());
    for (const vol_lamps::GameLamp& g : list_) {
      lamps_.push_back(la::FromGameLamp(g));
    }
  }
  if (stale) {
    list_.clear();
    list_seq_ = 0;
    lamps_.clear();
  }
  if (vol_lamps::Global().published() == 0) {
    if (!table_lamps_.empty()) {
      lamps_ = table_lamps_;  // no list ever came: the static table's selection of the frame (a toolkit scene, a build without the hook)
    } else if (!hook_warned_ && ++hook_silent_frames_ >= kHookWarnFrames) {
      hook_warned_ = true;
      REXLOG_WARN("{} the game's lamp list has not come once in {} frames: the guest hook MCLAVolLampList (0x82302814, larecomp_config.toml) is not in this build (a codegen run is needed): no street lamp "
                  "has a shadow (reported once)",
                  kPrefix, hook_silent_frames_);
    }
  }
  if (lamps_.empty()) {
    picker_.Reset();  // a day, a menu: the lamps are off
    have_last_ = false;
    pick_ = la::PickResult{};
    early_.clear();
    pending_.clear();
    Leave();
    ClearFrame();
    return false;
  }
  active_ = true;
  Join();
  return true;
}

void LampShadows::Pick(lb::Level level, const ls::Camera& last_camera, bool camera_valid, uint64_t frame) {
  level_ = level;
  budget_ = la::BudgetFor(level, lamps_override_, cache_);
  early_.clear();
  pick_ = la::PickResult{};
  pending_.clear();
  if (!active_ || !budget_.on()) {
    picker_.Reset();
    have_last_ = false;
    return;
  }
  if (!camera_valid || !last_camera.valid) {
    return;  // the first frame of a night: the draws are collected to find a camera, the pick follows
  }
  la::View view;
  view.have_eye = true;
  std::memcpy(view.eye, last_camera.eye, sizeof(view.eye));
  view.have_vp = true;
  view.view_projection = last_camera.view_projection;
  const double now_s = mc::cinematic::SceneClockNowMs(frame) * 1.0e-3;
  float dt = 0.0f;
  bool reset = !have_last_;
  if (have_last_) {
    dt = float(now_s - last_time_s_);
    double step2 = 0.0;
    for (int k = 0; k < 3; ++k) {
      const double d = view.eye[k] - last_eye_[k];
      step2 += d * d;
    }
    // A gap or a jump of the camera (a cut, a teleport, a menu): the lamps start again at full strength, with no fades from places that are not near.
    reset = dt > float(kGapSeconds) || dt < 0.0f || step2 > double(volumetrics::kCutMetres) * double(volumetrics::kCutMetres);
    dt = std::max(dt, 0.0f);
  }
  last_time_s_ = now_s;
  std::memcpy(last_eye_, view.eye, sizeof(last_eye_));
  have_last_ = true;
  la::PickParams params;
  params.budget = budget_;
  params.dt = dt;
  params.reset = reset;
  picker_.Update(lamps_, view, params, &pick_);
  for (uint32_t e = 0; e < pick_.count && early_.size() < la::kMaxSlots; ++e) {
    ls::Light light;
    ls::ViewSpec spec;
    if (la::MakeReplayLight(pick_.entries[e].lamp, e, &light) && la::FitLampView(light, &spec)) {
      early_.push_back(spec);
    }
  }
  if (pick_.count != 0) {
    ++frames_with_lamps_;
  }
  const uint32_t state = (uint32_t(level) * 16u + budget_.wanted) * 4u + (budget_.cache ? 2u : 0u) + (mode_ == la::LampMode::kOwn ? 1u : 0u);
  if (!first_logged_ && pick_.count != 0) {
    first_logged_ = true;
    REXLOG_INFO("{} the first street lamp pick: the game's list has {} lamps, {} are spots that light the view, {} picked (+{} fading) of {} (best score {:.3f}); {}; mode {} ({}); a lamp's view is a spot "
                "frustum of about {:.0f} degrees from the lamp down along its axis, its map reaches sqrt(8 I) = 14 to 30 m",
                kPrefix, pick_.listed, pick_.usable, pick_.wanted, pick_.fading, budget_.wanted, pick_.best_score, la::Describe(budget_), int(mode_),
                mode_ == la::LampMode::kOwn ? "own: the pass adds the lamp, the grid has it off" : "subtract: the pass takes the shadowed share of the grid's light away",
                2.0 * (la::kConeCapDegrees + ls::kFovMarginDegrees));
  }
  if (state != last_budget_logged_) {
    last_budget_logged_ = state;
    REXLOG_INFO("{} {} (LIGHT SHADOWS level {}, live)", kPrefix, la::Describe(budget_), int(level));
  }
}

// ---------------------------------------------------------------------------
// the provider
// ---------------------------------------------------------------------------

void LampShadows::RequestPools(shadow_atlas::AtlasSpec* spec) const {
  if (!active_) {
    return;
  }
  la::RequestPools(budget_, spec);
}

void LampShadows::OnLayout(const shadow_atlas::Layout& layout) {
  layout_ = layout;
  live_pool_ = layout.FindPool(shadow_atlas::ProviderId::kLamps, shadow_atlas::PoolRole::kLive);
  static_pool_ = layout.FindPool(shadow_atlas::ProviderId::kLamps, shadow_atlas::PoolRole::kStatic);
}

// ---------------------------------------------------------------------------
// the trigger
// ---------------------------------------------------------------------------

void LampShadows::ClearFrame() {
  frame_ = larecomp::gpu::LampShadowFrame{};
  refs_.clear();
  lights_.clear();
}

void LampShadows::Commit(const Plan& plan, bool recorded, bool partial, double sx, double sy, float bias_scale, uint32_t sampler) {
  ClearFrame();
  if (!recorded || atlas_ == nullptr) {
    // The passes were not recorded: whatever the layers were planned to hold is not in the atlas, and the lit draws see no lamp shadow.
    for (const Pending& p : pending_) {
      if (p.valid) {
        layers_[pick_.entries[p.entry].tile].Invalidate();
      }
    }
    pending_.clear();
    return;
  }
  published_.clear();
  published_.reserve(pending_.size());
  for (const Pending& p : pending_) {
    const la::Entry& entry = pick_.entries[p.entry];
    if (p.valid) {
      la::CommitStatic(&layers_[entry.tile], p.pass, entry.id, serial_, world_epoch_, partial);
      passes_static_ += p.pass.static_draws.empty() ? 0u : 1u;
      passes_live_ += p.pass.live_draws.empty() ? 0u : 1u;
      (p.pass.rebuild() ? rebuilds_ : adds_) += p.pass.cache ? 1u : 0u;
      static_draws_ += p.pass.static_draws.size();
      live_draws_ += p.pass.live_draws.size();
      known_draws_ += p.pass.known;
      over_budget_ += p.pass.over_budget;
    }
    la::Published pub;
    pub.id = entry.id;
    pub.tile = entry.tile;
    pub.weight = entry.weight;
    pub.valid = p.valid;
    pub.has_live = p.has_live;
    pub.reach = p.reach;
    pub.clip = p.clip;
    published_.push_back(pub);
  }
  seen_generation_ = AtlasGeneration();
  la::PublishInput in;
  in.layout = &layout_;
  in.live_pool = live_pool_;
  in.static_pool = static_pool_;
  in.cache = budget_.cache && static_pool_ >= 0;
  in.sx = sx;
  in.sy = sy;
  in.depth_bias = ls::kDepthBias * bias_scale;
  in.atlas_slot = atlas_->slot();
  in.sampler = sampler;
  in.atlas_valid = atlas_->has_texture();
  la::BuildFrame(published_, in, &frame_, &refs_);
  for (size_t i = 0; i < refs_.size() && i < pending_.size(); ++i) {
    if (refs_[i].view_count != 0) {
      lights_.push_back(la::MakePassLight(pick_.entries[pending_[i].entry].lamp, refs_[i], mode_));
    }
  }
  (void)plan;
  Log();
  pending_.clear();
}

void LampShadows::AppendPassLights(std::vector<larecomp::gpu::Light>* out) const {
  if (frame_.empty()) {
    return;
  }
  out->insert(out->end(), lights_.begin(), lights_.end());
}

void LampShadows::Log() {
  if (serial_ < next_log_) {
    return;
  }
  next_log_ = serial_ + kLogFrames;
  const uint64_t passes = rebuilds_ + adds_;
  REXLOG_INFO("{} frame {}: {} lamps with views ({} entries), {} frames with lamps so far; static layers: {} rebuilt, {} added to ({} static draws drawn, {} already held, {} left out for the draw cap), "
              "live passes {} ({} live draws); {}",
              kPrefix, serial_, lights_.size(), frame_.count, frames_with_lamps_, rebuilds_, adds_, static_draws_, known_draws_, over_budget_, passes_live_, live_draws_,
              passes == 0 ? std::string("the cache is off or nothing was drawn yet") : std::format("{:.0f} per cent of the lamp passes only added", 100.0 * double(adds_) / double(passes)));
  rebuilds_ = adds_ = passes_static_ = passes_live_ = 0;
  static_draws_ = live_draws_ = known_draws_ = over_budget_ = 0;
}

}  // namespace mcla::gpu
