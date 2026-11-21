// MCLA GPU renderer -- the passive LightManager. See light_manager.h; the rules (collecting, the checks) are in light_manager_rules.h and tested without a renderer.

#include "light_manager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "headlight_rig.h"
#include "local_shadow.h"
#include "mc_engine/cinematic/capture_mode.h"
#include "upload.h"
#include "volumetrics_headlights.h"

namespace mcla::gpu {

namespace lm = larecomp::gpu::light_manager;

namespace {

constexpr const char* kPrefix = "[lights]";
// A copy of the lamp list this old is not the game's list any more (the volume's own limit: volumetrics_inputs.cpp).
constexpr std::chrono::milliseconds kMaxListAge{500};
// The upload's alignment (a constant buffer's).
constexpr uint64_t kUploadAlignment = 256;

using Clock = std::chrono::steady_clock;
using Carry = lm::ShadowCarry<larecomp::gpu::LocalShadowFrame>;

// A difference a check found, logged at once (the first few of each kind: SelfCheck decides).
void OnCheckFailed(lm::Check kind, const std::string& what) { REXLOG_WARN("{} self-check, {}: {}", kPrefix, lm::CheckName(kind), what); }

// The replay's atlas as the one light description says it: the sampler is the fixed point / clamp one the rig hands the lit shaders (local_shadow.cpp HandOver).
larecomp::gpu::ShadowAtlasInfo AtlasOf(const LocalShadowAtlas& a) {
  larecomp::gpu::ShadowAtlasInfo info;
  info.valid = a.valid;
  info.slot = a.slot;
  info.sampler = kSamplerPointClamp;
  info.width = a.width;
  info.height = a.height;
  info.tile_small = a.tile;  // CHECK: one tile size today; LU-5's atlas has two classes (small and large) and fills both
  return info;
}

std::array<uint32_t, lm::kRigLights> KeysOf(const HeadlightSet& set) {
  std::array<uint32_t, lm::kRigLights> keys{};
  for (uint32_t i = 0; i < set.count && i < keys.size(); ++i) {
    keys[i] = set.lights[i].car_key;
  }
  return keys;
}

// How long a scope took, added to the frame's cost.
class Cost {
 public:
  explicit Cost(Clock::duration* sum) : sum_(sum), start_(Clock::now()) {}
  ~Cost() { *sum_ += Clock::now() - start_; }
  Cost(const Cost&) = delete;
  Cost& operator=(const Cost&) = delete;

 private:
  Clock::duration* sum_;
  Clock::time_point start_;
};

}  // namespace

LightManager::LightManager() = default;
LightManager::~LightManager() = default;

void LightManager::Initialize(UploadRing& upload, HeadlightRig& rig, LocalShadow& shadow) {
  upload_ = &upload;
  rig_ = &rig;
  shadow_ = &shadow;
  Reset();
}

void LightManager::Shutdown() {
  Reset();
  upload_ = nullptr;
  rig_ = nullptr;
  shadow_ = nullptr;
}

void LightManager::Reset() {
  collector_.Reset();
  carry_.Clear();
  check_ = lm::SelfCheck{};
  check_.SetOnFail(&OnCheckFailed);
  list_.clear();
  list_seq_ = 0;
  active_ = false;
  begun_ = false;
  volume_seen_ = false;
  presents_ = 0;
  frames_total_ = 0;
  grid_lamps_ = grid_headlights_ = 0;
  volume_lamp_count_ = volume_headlight_count_ = 0;
  uploads_ = 0;
  last_upload_offset_ = 0;
  upload_failed_ = 0;
  frame_cpu_ = {};
  window_ = Window{};
  lights_seen_ = false;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void LightManager::BeginFrame(uint64_t frame, uint32_t upload_slot) {
  const int every = larecomp::settings::mcla_gpu_lights_census.Get();
  if (every <= 0 || rig_ == nullptr || shadow_ == nullptr) {
    if (active_) {
      REXLOG_INFO("{} off (mcla_gpu_lights_census = {})", kPrefix, every);
      Reset();
    }
    return;
  }
  if (!active_) {
    Reset();
    active_ = true;
    REXLOG_INFO("{} on (mcla_gpu_lights_census = {}, live): collects the headlight rig's lights, the replay's shadow views and the volume's street lamps into one frame every guest frame, "
                "checks it against the headlight block and the volume's inputs, uploads {} bytes of GpuLights a frame; nothing reads it (the picture is unchanged)",
                kPrefix, every, sizeof(larecomp::gpu::GpuLightBuffer));
  }
  every_ = every;
  upload_slot_ = upload_slot;  // the latest list's: the upload is made at the present, into the slot that is being recorded
  if (begun_) {
    return;  // a second list of the same guest frame: the set is what it was
  }
  Cost cost(&frame_cpu_);
  begun_ = true;
  volume_seen_ = false;
  frame_ = frame;
  const HeadlightSet& set = CurrentHeadlights();
  const double now_s = mc::cinematic::SceneClockNowMs(frame) * 1.0e-3;
  collector_.BeginFrame(uint32_t(frame), now_s, set);
  // The maps the rig holds at this point are those of the last trigger, moved to this set by the car; the replay gave them up at its own BeginFrame if the frame before had no trigger.
  if (!Carry::Published(shadow_->Frame(), AtlasOf(shadow_->AtlasInfo()))) {
    carry_.Clear();
  }
  const std::array<uint32_t, lm::kRigLights> keys = KeysOf(set);
  const larecomp::gpu::LocalShadowFrame moved = carry_.ForSet(
      keys.data(), set.count, [](const larecomp::gpu::LocalShadowFrame& f, const uint32_t* old_keys, uint32_t old_count, const uint32_t* new_keys, uint32_t new_count) {
        return headlight_rules::RemapShadowViews(f, old_keys, old_count, new_keys, new_count);
      });
  collector_.SetShadows(moved.count != 0 ? &moved : nullptr, carry_.has() ? carry_.atlas() : larecomp::gpu::ShadowAtlasInfo{});
  lm::CheckHeadlightBlock(collector_.frame(), collector_.rig(), rig_->Block(), lm::Check::kBlockBefore, &check_);
}

void LightManager::RefreshCarry() {
  // What headlight_rig::SetLocalShadows does at the replay's trigger: a copy of the views with the car key of each light of the set (and nothing when the replay has no map).
  const larecomp::gpu::ShadowAtlasInfo atlas = AtlasOf(shadow_->AtlasInfo());
  const larecomp::gpu::LocalShadowFrame& frame = shadow_->Frame();
  const std::array<uint32_t, lm::kRigLights> keys = KeysOf(CurrentHeadlights());
  carry_.Set(Carry::Published(frame, atlas) ? &frame : nullptr, atlas, keys.data(), CurrentHeadlights().count);
}

void LightManager::OnResolve(bool shadow_recorded) {
  if (!active_ || !begun_) {
    return;
  }
  const larecomp::gpu::ShadowAtlasInfo atlas = AtlasOf(shadow_->AtlasInfo());
  const larecomp::gpu::LocalShadowFrame& frame = shadow_->Frame();
  // A resolve that is not the replay's trigger changes nothing; the trigger hands the rig its maps (a recording, or a change of what is published: a day with no map hands none).
  if (!shadow_recorded && !carry_.Differs(frame, atlas)) {
    return;
  }
  Cost cost(&frame_cpu_);
  RefreshCarry();
  const bool have = Carry::Published(frame, atlas);
  collector_.SetShadows(have ? &frame : nullptr, have ? atlas : larecomp::gpu::ShadowAtlasInfo{});
  lm::CheckHeadlightBlock(collector_.frame(), collector_.rig(), rig_->Block(), lm::Check::kBlockAfter, &check_);
}

void LightManager::NoteVolumeInputs(const volumetrics::VolFrameContext& context, const volumetrics::VolInputs& in, bool lamps_on, bool heads_on, uint32_t first_lamp) {
  if (!active_ || !begun_) {
    return;
  }
  Cost cost(&frame_cpu_);
  volume_seen_ = true;
  const uint32_t first = std::min<uint32_t>(first_lamp, in.lamp_count);
  if (lamps_on) {
    // The game's list as the hook's table has it (the volume's own copy is private to it): a new copy when the hook has published one since ours.
    bool stale = false;
    bool changed = vol_lamps::Global().Latest(&list_, &list_seq_, &stale, kMaxListAge);
    if (stale) {
      changed = changed || !list_.empty();
      list_.clear();
      list_seq_ = 0;
    }
    collector_.SetLamps(list_.data(), uint32_t(list_.size()), changed, in.lamps.data() + first, in.lamp_count - first);
  }
  const LocalShadowAtlas& atlas = shadow_->AtlasInfo();
  grid_lamps_ = context.grid.lamps;
  grid_headlights_ = context.grid.headlights;
  volume_lamp_count_ = in.lamp_count;
  volume_headlight_count_ = in.headlight_count;
  collector_.NoteBudgets(atlas.slot, atlas.width, atlas.height, grid_lamps_, grid_headlights_);
  lm::VolumeCheckSpec spec;
  spec.lamps_on = lamps_on;
  spec.heads_on = heads_on;
  spec.first_lamp = first;
  lm::CheckVolumeInputs(collector_.frame(), collector_, in, spec, [](const volumetrics::VolShadowView& v) { return volumetrics::UsableView(v); }, &check_);
}

void LightManager::OnPresent() {
  if (!active_ || !begun_) {
    return;
  }
  {
    Cost cost(&frame_cpu_);
    lm::CheckFrameInvariants(collector_.frame(), collector_.has_previous() ? &collector_.previous() : nullptr, &check_);
    Upload();
    collector_.EndFrame();
  }
  ++presents_;
  ++frames_total_;
  const lm::Collector::Stats& st = collector_.stats();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(frame_cpu_).count());
  ++window_.frames;
  window_.with_lights += st.headlights + st.lamps != 0 ? 1u : 0u;
  window_.with_volume += volume_seen_ ? 1u : 0u;
  window_.max_headlights = std::max(window_.max_headlights, st.headlights);
  window_.max_lamps = std::max(window_.max_lamps, st.lamps);
  window_.kept += st.slots_kept;
  window_.added += st.slots_added;
  window_.gone += st.slots_gone;
  window_.cpu_ns += ns;
  window_.cpu_max_ns = std::max(window_.cpu_max_ns, ns);
  frame_cpu_ = {};
  begun_ = false;
  if (!lights_seen_ && st.headlights + st.lamps != 0) {
    lights_seen_ = true;
    Census();  // the first frame with a light: the line is there at once, and the window starts again after it
  } else if (every_ > 0 && presents_ % uint64_t(every_) == 0) {
    Census();
  }
}

void LightManager::Upload() {
  if (upload_ == nullptr) {
    return;
  }
  larecomp::gpu::GpuLightBuffer buffer;
  lm::BuildGpuLightBuffer(collector_.frame(), &buffer);
  const UploadSpan span = upload_->Allocate(upload_slot_, sizeof(buffer), kUploadAlignment);
  if (!span || span.cpu == nullptr) {
    ++upload_failed_;
    return;
  }
  std::memcpy(span.cpu, &buffer, sizeof(buffer));
  ++uploads_;
  last_upload_offset_ = span.offset;
}

// ---------------------------------------------------------------------------
// the census
// ---------------------------------------------------------------------------

void LightManager::Census() {
  const larecomp::gpu::LightFrame& f = collector_.frame();
  const lm::Collector::Stats& st = collector_.stats();
  const lm::SelfCheck::Window w = check_.TakeWindow();
  const double frames = window_.frames != 0 ? double(window_.frames) : 1.0;
  std::string checks;
  for (uint32_t i = 0; i < lm::kCheckCount; ++i) {
    if (w.checks[i] != 0) {
      checks += std::format("{}{} {}/{}", checks.empty() ? "" : ", ", lm::CheckName(lm::Check(i)), w.checks[i], w.bad[i]);
    }
  }
  const std::string check_text = w.Checks() == 0 ? std::string("no check made") : std::format("{} checks, {} differed ({}){}", w.Checks(), w.Bad(), checks,
                                                                                            w.first.empty() ? std::string() : "; first: " + w.first);
  const std::string atlas = f.shadow.atlas.valid ? std::format("{}x{} slot {}", f.shadow.atlas.width, f.shadow.atlas.height, f.shadow.atlas.slot) : std::string("none");
  REXLOG_INFO("{} census, frame {} (every {}): {} guest frames since the last line ({} with lights, {} with the volume's fill); now {} headlights ({} the player's) + {} lamps "
              "({} from the game's list of {} records, {} differ from their record, {} without one; the volume holds {} lamps and {} headlights, its budget {} / {}) in {} of {} slots, "
              "generation {}; in the window at most {} headlights and {} lamps, slots a frame {:.1f} kept, {:.2f} added, {:.2f} gone; shadows: {} lights read {} views (atlas {}); "
              "rankings: surface {}, volume {}, shadow {}; GPU buffer {} B uploaded {} times{} (last at +{} B); manager CPU {:.1f} us a frame (most {:.1f}); self-check: {}",
              kPrefix, f.frame, every_, window_.frames, window_.with_lights, window_.with_volume, st.headlights, st.player, st.lamps, st.from_list, st.listed, st.differs,
              st.unlisted, volume_lamp_count_, volume_headlight_count_, grid_lamps_, grid_headlights_, f.count, larecomp::gpu::kMaxLights, f.generation, window_.max_headlights,
              window_.max_lamps, double(window_.kept) / frames, double(window_.added) / frames, double(window_.gone) / frames, st.shadowed, st.views, atlas,
              f.role_count[uint32_t(larecomp::gpu::LightRole::kSurface)], f.role_count[uint32_t(larecomp::gpu::LightRole::kVolume)],
              f.role_count[uint32_t(larecomp::gpu::LightRole::kShadow)], sizeof(larecomp::gpu::GpuLightBuffer), uploads_,
              upload_failed_ != 0 ? std::format(", {} failed", upload_failed_) : std::string(), last_upload_offset_, double(window_.cpu_ns) / frames * 1.0e-3,
              double(window_.cpu_max_ns) * 1.0e-3, check_text);
  window_ = Window{};
}

}  // namespace mcla::gpu
