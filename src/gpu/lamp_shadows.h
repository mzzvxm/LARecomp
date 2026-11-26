#pragma once
// ===========================================================================
// MCLA GPU renderer -- the street lamps' shadows, second try: the PROVIDER of the shadow atlas that the deferred light pass reads (perf/LAMP-SHADOWS-2.md, package LAMP-SHADOWS-4)
// ===========================================================================
// WHAT IT IS. A ShadowProvider (shadow_provider.h) of the one shadow atlas (shadow_atlas.h): it asks for two pools of tiles (a lamp's LIVE tile and its STATIC twin, sticky: a tile stays with a lamp),
// picks the lamps that get them (lamp_shadows_rules.h Picker: the nearest N by what a lamp adds to the view, N = the LIGHT SHADOWS level's row), plans what each lamp's passes draw from the casters the
// headlight replay collected (LocalShadow: the same main-view draws, replayed from the lamp), publishes the views in a table of its own (lamp_shadow_frame.h) and hands the light pass the lamps'
// lights (AppendPassLights) with their ShadowRefs. The pass (local_lights.h) reads the table through the one lookup and, for a lamp the game lights, takes the shadowed share away.
//
// THE CALLS (all from LocalShadow, one frame):
//   Poll       BeginFrame, before the collecting decision: reads the settings and the lamp list, registers with or leaves the atlas; true when lamps are to be shadowed (the setting is on, LIGHT SHADOWS
//              is not off and the list holds lamps: by night).
//   Pick       BeginFrame, after the level is fitted to the memory: the budget, the pick (with the last frame's camera), the frusta the cone cull works from (EarlySpecs).
//   PlanPasses the trigger, after the atlas is made: from the collected casters, the passes of each picked lamp (a static pass, a live pass) and the static tiles to clear.
//   Commit     the trigger, after the passes were recorded: what the static layers hold now, the published frame (Frame()), the lights (AppendPassLights).
//
// WHERE THE LAMPS COME FROM. The game's per-frame list (vol_lamp_list.h; the guest hook MCLAVolLampList runs while this or the volume wants it: SetWantedBy). By day the list is empty and the
// provider costs the settings' reads and one lock. With no list at all (the hook is not in this build, a toolkit scene) the lamps may be handed over by SetTableLamps (the static lamp table's
// selection of the frame, CITY-RT G2b: lamp_shadows_rules.h FromTableLamp). LocalShadow does not call it yet (CHECK: the main session wires it when the city pack's lamp table is on main).
//
// THE SETTINGS (settings_gpu.h, debug only; the row comes after the owner has seen the pictures): mcla_gpu_lamp_shadows (0 off, the default; 1 on: the level of LIGHT SHADOWS decides how many lamps, a
// level of off means none), mcla_gpu_lamp_shadows_mode (0 subtract: the game's grid lights the lamps and the pass takes the shadowed share away, the default; 1 own: the lamps are the pass's, the grid
// has them off: the lamp A/B's mod lamps), mcla_gpu_lamp_shadows_view (0 off, 1 the shadow as a picture, 2 the lamps' rows), mcla_gpu_lamp_shadows_lamps (debug: 0 the level's, n lamps, -1 none),
// mcla_gpu_lamp_shadows_cache (1: the static layers). While it is on the replay records into the shared atlas (it forces what mcla_gpu_light_frame bit 8 asks for).
//
// THREADING: render thread only. The hook's table is read under its own lock. Logs: "[lamp-shadows]" (the first pick, a summary every 1800 frames, changes of budget).
// ===========================================================================

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "lamp_shadow_frame.h"
#include "lamp_shadows_rules.h"
#include "light_budget.h"
#include "light_frame.h"
#include "local_shadow_rules.h"
#include "shadow_atlas_rules.h"
#include "shadow_provider.h"
#include "temporal_math.h"
#include "vol_lamp_list.h"

namespace mcla::gpu {

class ShadowAtlas;

class LampShadows final : public ShadowProvider {
 public:
  // One pass of the replay for a lamp: the frustum, the matrices (the replay draws every kept caster through `replay`), the tile it draws into and the casters it keeps (the replay's indices).
  struct ReplayView {
    local_shadow::ViewSpec spec;
    temporal::Mat4 clip;
    temporal::Mat4 replay;
    shadow_atlas::Rect tile;
    float uv[4] = {};
    std::vector<uint32_t> keep;
    bool is_static = false;  // into the lamp's static tile (else its live one)
  };
  struct Plan {
    std::vector<ReplayView> views;                // the static pass (when there is anything to add or the layer is built again), then the live pass, lamp after lamp
    std::vector<shadow_atlas::Rect> clears;       // static tiles built again: cleared after the atlas's own clear, before the passes
    uint32_t static_draws = 0, live_draws = 0, rebuilds = 0;
    bool empty() const { return views.empty() && clears.empty(); }
  };

  LampShadows();
  ~LampShadows() override;
  LampShadows(const LampShadows&) = delete;
  LampShadows& operator=(const LampShadows&) = delete;

  // The shared atlas (not owned; it must outlive this object). The provider joins it only while lamps are wanted.
  void SetShadowAtlas(ShadowAtlas* atlas) { atlas_ = atlas; }
  // Leaves the atlas and forgets everything (Shutdown, a release of the atlas's images).
  void Reset();

  // 1. Reads the settings and the lamp list. `wanted` = LIGHT SHADOWS is not off (the replay is on). True when lamps are to be shadowed this frame.
  bool Poll(bool wanted, uint64_t frame, bool garage);
  bool active() const { return active_; }
  // Whether the lamps are to be shadowed at all: mcla_gpu_lamp_shadows (-1 follows LIGHT SHADOWS while the deferred light pass runs for anything else, 0 off, 1 on: lamp_shadows_rules.h LampsEnabled)
  // and SetPassRuns. Read from the settings each time (live).
  bool Enabled() const;
  // The light pass tells this provider whether it runs for any other reason (mcla_gpu_local_lights, the headlight route): the "auto" setting follows it. Render thread, once a frame.
  void SetPassRuns(bool runs) { pass_runs_ = runs; }
  bool setting_on() const { return on_; }
  // 2. The pick for this frame, from the last frame's camera (valid: it was derived from the banks), at the level the atlas could fit.
  void Pick(light_budget::Level level, const local_shadow::Camera& last_camera, bool camera_valid, uint64_t frame);
  bool HasViews() const { return active_ && budget_.on() && pick_.count != 0; }
  // The picked lamps' frusta (the cone cull of the collection drops a draw no view can see).
  const std::vector<local_shadow::ViewSpec>& EarlySpecs() const { return early_; }
  // 3. The passes of the trigger. `facts` are the collected casters that can be drawn (a usable place, a made pipeline), `id_of(i)` the static identity of facts[i] (made lazily). False: no lamp has a
  // pass (no layout, no valid view): nothing is recorded for the lamps.
  template <class IdOf>
  bool PlanPasses(const local_shadow::Camera& camera, double sx, double sy, bool cone_cull, const lamp_shadows::CasterFact* facts, uint32_t count, IdOf&& id_of, Plan* out);
  // 4. After the passes were recorded (`recorded`: the pass ran; `partial`: it ran out of upload room), the static layers and the published frame. `bias_scale` = mcla_gpu_headlight_shadow_bias.
  void Commit(const Plan& plan, bool recorded, bool partial, double sx, double sy, float bias_scale, uint32_t sampler);
  // The trigger found no map (a menu, no casters): the lit draws see no lamp shadow until one is recorded.
  void ClearFrame();

  // What the light pass reads.
  const larecomp::gpu::LampShadowFrame& Frame() const { return frame_; }
  // The lights of the picked lamps with views, for the pass (as the handed over lights): none while there is no frame.
  void AppendPassLights(std::vector<larecomp::gpu::Light>* out) const;
  uint32_t debug_view() const { return debug_view_; }
  lamp_shadows::LampMode mode() const { return mode_; }
  // The world around the lamps changed (a city sector streamed): the static layers are built again (CHECK: nothing calls it yet; lamp_shadows_rules.h DecideStatic).
  void BumpWorldEpoch() { ++world_epoch_; }
  // The static lamp table's selection of this frame, used while the game's list never came (the hook is not in the build).
  void SetTableLamps(std::vector<lamp_shadows::Lamp> lamps) { table_lamps_ = std::move(lamps); }

  // ShadowProvider.
  shadow_atlas::ProviderId provider_id() const override { return shadow_atlas::ProviderId::kLamps; }
  void RequestPools(shadow_atlas::AtlasSpec* spec) const override;
  void OnLayout(const shadow_atlas::Layout& layout) override;
  void CollectViews(shadow_atlas::ShadowFrameBuilder* builder) const override { (void)builder; }  // the lamps' views are in their own table (lamp_shadow_frame.h), not in the headlights'
  bool wants_static_pool() const override { return budget_.cache; }

  uint64_t frames_with_lamps() const { return frames_with_lamps_; }

 private:
  // What a lamp's plan leaves for Commit: its entry of the pick, the planner's answer (the static layer's rules), the lamp's clip matrix and whether a view was made.
  struct Pending {
    uint32_t entry = 0;             // index into pick_.entries
    lamp_shadows::PassPlan pass;
    bool valid = false;
    bool has_live = false;
    float reach = 0.0f;
    temporal::Mat4 clip;
  };
  void Join();
  void Leave();
  void Log();
  uint32_t AtlasGeneration() const;

  ShadowAtlas* atlas_ = nullptr;
  bool registered_ = false;
  bool on_ = false;
  bool pass_runs_ = false;  // the deferred light pass runs without the lamps (SetPassRuns)
  bool enabled_logged_ = false;  // the state the log last said (Poll logs a change once)
  bool active_ = false;
  bool garage_ = false;
  lamp_shadows::LampMode mode_ = lamp_shadows::LampMode::kSubtract;
  uint32_t debug_view_ = 0;
  int lamps_override_ = 0;
  bool cache_ = true;
  lamp_shadows::Budget budget_;
  light_budget::Level level_ = light_budget::Level::kOff;

  // the lamps
  std::vector<vol_lamps::GameLamp> list_;
  uint64_t list_seq_ = 0;
  std::vector<lamp_shadows::Lamp> lamps_;
  std::vector<lamp_shadows::Lamp> table_lamps_;
  bool hook_warned_ = false;
  uint64_t hook_silent_frames_ = 0;
  lamp_shadows::Picker picker_;
  lamp_shadows::PickResult pick_;
  std::vector<local_shadow::ViewSpec> early_;
  bool have_last_ = false;
  double last_time_s_ = 0.0;
  double last_eye_[3] = {};

  // the layout the atlas planned and what the static layers hold
  shadow_atlas::Layout layout_;
  int live_pool_ = -1;
  int static_pool_ = -1;
  std::array<lamp_shadows::StaticLayer, lamp_shadows::kMaxSlots> layers_;
  uint32_t seen_generation_ = 0;
  uint64_t world_epoch_ = 0;
  uint64_t serial_ = 0;  // the replay's frame counter (LocalShadow::frame_)

  // the trigger
  std::vector<Pending> pending_;
  std::vector<lamp_shadows::Published> published_;
  std::vector<lamp_shadows::LampRef> refs_;
  larecomp::gpu::LampShadowFrame frame_;
  std::vector<larecomp::gpu::Light> lights_;

  // the log
  uint64_t frames_with_lamps_ = 0;
  uint64_t passes_static_ = 0, passes_live_ = 0, rebuilds_ = 0, adds_ = 0;
  uint64_t static_draws_ = 0, live_draws_ = 0, known_draws_ = 0, over_budget_ = 0;
  bool first_logged_ = false;
  uint32_t last_budget_logged_ = ~0u;
  uint64_t next_log_ = 0;
};

// ---------------------------------------------------------------------------
// the planner (a template over the draw identity: the caller makes it lazily, only for the world draws a static layer is asked about)
// ---------------------------------------------------------------------------

template <class IdOf>
bool LampShadows::PlanPasses(const local_shadow::Camera& camera, double sx, double sy, bool cone_cull, const lamp_shadows::CasterFact* facts, uint32_t count, IdOf&& id_of, Plan* out) {
  namespace la = lamp_shadows;
  namespace ls = local_shadow;
  (void)sx;
  (void)sy;
  *out = Plan{};
  pending_.clear();
  if (!HasViews() || !layout_.valid() || live_pool_ < 0 || !camera.valid) {
    return false;
  }
  const bool cache = budget_.cache && static_pool_ >= 0;
  // A texture made again (or given back, or its pools moved) holds none of what the static layers believe they hold.
  const bool atlas_fresh = AtlasGeneration() != seen_generation_;
  if (atlas_fresh) {
    for (la::StaticLayer& layer : layers_) {
      layer.Invalidate();
    }
  }
  pending_.reserve(pick_.count);
  bool any = false;
  for (uint32_t e = 0; e < pick_.count; ++e) {
    const la::Entry& entry = pick_.entries[e];
    Pending p;
    p.entry = e;
    ls::Light light;
    ls::ViewSpec spec;
    temporal::Mat4 replay;
    if (entry.tile >= budget_.tiles() || !la::MakeReplayLight(entry.lamp, e, &light) || !la::FitLampView(light, &spec) || !ls::LightClip(spec, camera.right_handed, &p.clip) ||
        !ls::ReplayMatrix(camera, p.clip, &replay)) {
      pending_.push_back(std::move(p));  // not valid: no view, not published
      continue;
    }
    la::PassInput in;
    in.spec = spec;
    in.cone_cull = cone_cull;
    in.cache = cache;
    in.tile = entry.tile;
    in.id = entry.id;
    in.frame = serial_;
    in.world_epoch = world_epoch_;
    in.atlas_fresh = atlas_fresh;
    la::PlanLampPass(layers_[entry.tile], in, facts, count, id_of, &p.pass);
    p.valid = true;
    p.reach = float(la::ShadowReach(entry.lamp.intensity));
    p.has_live = !p.pass.live_draws.empty();
    const auto add_view = [&](const shadow_atlas::Rect& tile, const std::vector<uint32_t>& keep, bool is_static) {
      ReplayView v;
      v.spec = spec;
      v.clip = p.clip;
      v.replay = replay;
      v.tile = tile;
      shadow_atlas::UvOf(layout_, tile, v.uv);
      v.keep = keep;
      v.is_static = is_static;
      out->views.push_back(std::move(v));
    };
    const shadow_atlas::Rect live_tile = layout_.SlotRect(uint32_t(live_pool_), entry.tile);
    if (cache) {
      const shadow_atlas::Rect static_tile = layout_.SlotRect(uint32_t(static_pool_), entry.tile);
      if (p.pass.rebuild()) {
        out->clears.push_back(static_tile);
        ++out->rebuilds;
      }
      if (!p.pass.static_draws.empty()) {
        add_view(static_tile, p.pass.static_draws, true);
        out->static_draws += uint32_t(p.pass.static_draws.size());
      }
    }
    if (!p.pass.live_draws.empty()) {
      add_view(live_tile, p.pass.live_draws, false);
      out->live_draws += uint32_t(p.pass.live_draws.size());
    }
    any = true;
    pending_.push_back(std::move(p));
  }
  return any;
}

}  // namespace mcla::gpu
