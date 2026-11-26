#pragma once
// ===========================================================================
// MCLA GPU renderer -- the ray tracing WORLD FRAME: THE PUBLIC HEADER (CITY-RT package FRAME; for R4 the SSR fallback, R7 emissive and lamps, G2 the probe update)
// ===========================================================================
// Every ray traced pass of the world needs, in EVERY frame that has a top level, the same numbers: the world's light (sun, ambient, sky, the game's sun cascades, the environment map), the
// camera, the exposure, the hour, the weather, the hit shading's calibration and the lamps near the camera. Until now only the car scene made them, from the car's own draws, so a top level
// with no car (a Director camera, a hidden car, the city alone in the predicted camera) had none and the city hit library answered "no data". RtWorldFrame makes them once a frame, for the top
// level the frame has, car or no car, into TWO buffers the ray tracing descriptor set carries:
//
//   t10 gRtWorld   StructuredBuffer<uint4>   48 rows: rows 0..31 are the HIT FRAME (rt_hit_frame.h) of the WORLD, rows 32..47 the world rows (hour, schedule, units, wetness, weather, camera,
//   t11 gRtLamps   StructuredBuffer<uint4>   the sun's direction in the world, the lamp grid's window, the calibration's cadence, the class gains)
//                                            + the lamps of the frame in view space in 64 m cells
// The layout, the numbers and the rules are rt_world_frame_rules.h (read its first page: it is the specification); the shader side is hlsl/rt_world_frame.hlsli (RtWorldLoad, RtWorldHitFrame,
// RtWorldClassGain, the lamp cursor), and hlsl/rt_world_probe_cs.hlsl is a reference consumer that compiles for DXIL and SPIR-V.
//
// WHAT A CONSUMER DOES (the whole API; nothing else is needed and nothing else is promised)
//   1. In its shader: `#define RT_WORLD_SPACE <the RT set's register space>` and include hlsl/rt_world_frame.hlsli. t10 and t11 are in the SAME descriptor set as the top level (t0) and the
//      hit data (t6..t9): a pass that binds the car scene's set (CarRtView::set, as the city shadow and the hit shading's check do) has them, bound by the scene (below), no more to write.
//      `RT_HIT_FRAME_ROW(i)` defaults to gRtWorld[i], so RtHitLoadFrame() and every function of city_hit_shade.hlsli read the world's light; a pass with its own frame rows (the car passes':
//      gCarRtMaterials[i], the car's light) defines RT_HIT_FRAME_ROW first and calls RtWorldHitFrame() where it wants the world's light for a city hit.
//        const RtWorld w = RtWorldLoad();   if ((w.flags & RT_WORLD_VALID) == 0u) { ...no data: keep what you had... }
//        p.units = w.units;  p.wet = w.wet;  p.lampFactor = w.lampFactor;  (CITY_HIT_P_CALIBRATE when w.statsFrame != 0u)  #define CITY_HIT_GAIN(cls) RtWorldClassGain(cls)
//      The lamp term and the glow (R7, done): `#define CITY_HIT_LAMP_GRID` before including city_hit_shade.hlsli makes CityHitLamps take the cursor's lamps around the hit (the world rows' lookup radius,
//      at most kCityHitLampMax = 96 of them that reach the hit, the walk bounded at kCityHitLampWalkMax = 288 lamps, their light's sum capped at kCityHitLampSumCap = 6 in luminance; the params' lampFirst / lampCount are not used, p.lampFactor = w.lampFactor);
//      `#define CITY_HIT_GLOW_FACTOR(kind) RtWorldGlowFactor(kind)` makes a lens glow by the street lamps' schedule, neon, signs and lit windows by the fill lights', a traffic signal always (row 47);
//      `#define CITY_HIT_CENSUS_ADD(word, value) <your counter buffer>.InterlockedAdd((word) * 4u, (value))` counts, in a stats frame, the lamp term and the glowing hits (eight words: hand them
//      to AddLampCensus). hlsl/rt_world_lamp_light_cs.hlsl is the reference consumer: it compiles beside city_hit_shade.hlsli for DXIL and SPIR-V and validates with spirv-val
//      (tools/check_rt_hit_identity.py runs it). The first consumer that shades with it is the car reflections' (REFL-LAMPS, hlsl/city_refl.hlsli: the paint, the other cars' paint and the glass in one shader
//      file, the chrome through the paint's image; the car scene's set they bind has t10 and t11 already, no binding added; mcla_gpu_city_refl_lamps is its switch). The loop the library does, for a
//      consumer that wants its own:
//        RtLampCursor c = RtLampBegin(w, hitViewPosition, 64.0);
//        while (c.done == 0u) { const CityHitLamp lamp = CityHitLampFromRows(RtLampRow(w, c.index, 0u), RtLampRow(w, c.index, 1u), RtLampRow(w, c.index, 2u));
//                               sum += CityHitLampAt(p, lamp, position, normal).radiance;   c = RtLampStep(w, c); }
//   2. On the host, in a pass that makes its own descriptor set with these bindings (the shared RT set already has them; a pass of its own set does it so): AddBindings(builder) before
//      the builder's end(), the set created with `kBindingCount` more views (D3D12 sizes a set by the count, not by the ranges: the same two mistakes were found on 2026-11-21 in the city
//      shadow's and the probe field's sets), and Bind(set, bindings, frame slot) each frame before the pass.
//   3. Tell the scene service it wants the scene (rt_scene_service.h Want): the world frame is made in every frame that has a top level, so a want for the city is a want for this.
//   4. The calibration: a pass that counts the library's samples (CITY_HIT_STAT_ADD, rt_hit_calibration.h) hands its counters to AddCalibration(words) once its readback is back; the host
//      closes a window every kWindowFrames frames and the class gains of the rows follow (clamped 0.5 to 3 against the model, the glowing class' 1/4 to 64). One window for every consumer.
//
// WHEN THE ROWS ARE MADE (the renderer's wiring; scene.cpp and car_rt_scene.cpp each have one or two calls, marked CITY-RT FRAME):
//   BeginFrame(frame)       the scene's frame start: the frame's inputs are forgotten
//   SetSunCascades          the scene's trigger block, from the volumetric light's collector (the same object it hands the car scene)
//   NoteView(view, flip_z)  the top level's builder (CarRtScene::OnResolve, both paths: with the car's camera, or the predicted one for the city alone) says which view space the top level is in:
//                           every row that holds a view space vector (the sun, the world's axes, the lamps) is in THAT one
//   Bind(set, b, slot)      the builder writes the slot's two buffers into the RT set (and clears them when they were not filled this frame)
//   OnTrigger(slot)         the scene's trigger block, right after the car scene's OnResolve: gathers the light (the ambient occlusion's draw hook: the commonest light of the main view's
//                           draws, with the road's wetness), the clock, the exposure, the live lamp list, makes the rows and the lamp window and writes them. Host visible memory: no copy pass,
//                           nothing for the GPU to wait for. Costs about 50 microseconds at night (a few hundred lamps), nothing by day.
//   A frame in which no top level was built is not filled at all (nothing can trace in it); `Valid(slot)` says whether the slot's rows are this frame's.
//
// WHERE EVERYTHING COMES FROM, and what is not known (CHECK = nothing here ran against the game)
//   light         ao.h FrameLight: PS c26 (ambient) and c39 (sun colour) and VS c16 (the sun's direction) of the commonest main view draw before the trigger. The draw hooks run only while an effect
//                 of the AO family or a screen product consumer is on, so this class asks for them (AmbientOcclusion::SetWorldLightWanted) from the frame after the first top level on, and
//                 asks no more 600 frames after the last. The first frame of a want has no light (kFrameShade clear: the library answers "no data"). Without the AO's light the car frame's, when
//                 it has one (the car's key light, not the sun: flagged by kWorldLight clear).
//   sky           the car frame's c57 when a car frame had one, else the ambient. CHECK: nothing in the city hit library reads the sky row yet.
//   environment   the 512x256 dual paraboloid's slots as a car's paint, chrome or glass draw bound them (CarFrame): the last pair seen, kept 120 frames (kEnvStaleFrames) then dropped (the
//                 slot is the texture cache's, valid for the frame it was bound in). CHECK: a car-less session has no source until a world draw that binds the map is found
//                 (the building windows' shader reads a reflection sampler: it may be the same map); the library then takes no sky for glass, metal and water.
//   cascades      the volumetric light's collector (the game's sun shadow map); none where the volume is off or the frame has none (night, rain): the sun ray against the top level alone.
//   exposure      ScreenShared::ExposureFactor (2^exp_bias of the main colour target, tracked while a screen product consumer is on), else the game's 2^4 = 16.
//   hour, weather world_clock.h: published by the time of day hook once a frame (a pinned hour reaches the renderer a frame late). Unknown (-1): the lamp schedule is the world sun's night factor.
//   wetness       ao.h FrameWetness (VS c43.x, 0.095 dry to 1.17 in rain) mapped to 0..1; 0 where no draw counted it.
//   lamps         the pack's lights.xlm table (read once from the open pack: CityRtScene::pack_if_open, PackReader::ReadLamps) and the game's live per-frame list (vol_lamp_list.h, the hook that
//                 copies it runs while we say so: kWantedByWorldFrame), merged by rt_world_frame_rules.h; SetStaticLamps hands in a table that already has the street lamps merged (lamp_merge.h
//                 MergeLive) for the far ones. CHECK: where the street lamps beyond the game's 300 m list come from (the offline source of the 6,298 was not found: lamp_table.h); until a merged table
//                 is given, lamps beyond 300 m are the file's fill lights only. CHECK: the live lamp hours (18 and 6) against the lighting manager's floats, the fill lights' night curve against the
//                 game's ShaderGlobals.w.
//   budget        rt_world::BudgetFor by the scene service's merged city level (High 512 lamps in 320 m, Ultra 1024 in 448 m; no city level: 256 in 192 m).
//   the check     a debug pass can read both buffers back through the real set with hlsl/rt_world_probe_cs.hlsl and compare with Current() (the same rows and words): not wired.
//
// THREADING: render thread, except Initialize and Shutdown (the lifecycle thread). Shutdown with the GPU idle and the FrameRing drained.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "citypack/lamp_table.h"
#include "frame.h"
#include "rt_hit_calibration.h"
#include "rt_hit_frame.h"
#include "rt_world_frame_rules.h"
#include "temporal_math.h"

namespace mcla::gpu {

class AmbientOcclusion;
class CarFrame;
class CityRtScene;
class Device;
class LightManager;
class RtSceneService;
class ScreenShared;

class RtWorldFrame {
 public:
  // The registers in the ray tracing descriptor set (hlsl/rt_world_frame.hlsli), after the car scene's t0..t5 and the hit data's t6..t9.
  static constexpr uint32_t kWorldRegister = rt_world::kWorldRegister;
  static constexpr uint32_t kLampsRegister = rt_world::kLampsRegister;
  static constexpr uint32_t kBindingCount = rt_world::kBindingCount;  // views the two bindings add to a set
  // The environment map's slots are kept this many frames after a car frame last gave them.
  static constexpr uint64_t kEnvStaleFrames = 120;
  // Frames without a top level after which the draw hooks and the lamp list are not asked for any more (10 s at 60).
  static constexpr uint64_t kIdleReleaseFrames = 600;

  struct Bindings {
    uint32_t world = 0;
    uint32_t lamps = 0;
  };
  // Adds the two bindings to a set's builder (before its end()); the registers default to the RT set's.
  static Bindings AddBindings(plume::RenderDescriptorSetBuilder& builder, uint32_t world_register = kWorldRegister, uint32_t lamps_register = kLampsRegister);

  RtWorldFrame();   // (declared here, made in the .cpp where the emitters' state is complete: a unique_ptr to it is a member)
  ~RtWorldFrame();
  RtWorldFrame(const RtWorldFrame&) = delete;
  RtWorldFrame& operator=(const RtWorldFrame&) = delete;

  // Not owned; any may be null (the rows then lack what it gave: the census says which).
  struct Sources {
    AmbientOcclusion* ao = nullptr;       // the light, the road's wetness, the kind of world
    const CarFrame* cars = nullptr;       // the car's light, the sky and the environment map's slots when a car frame has them
    const ScreenShared* screen = nullptr; // the exposure
    const CityRtScene* city = nullptr;    // the open pack (the lamp table)
    const RtSceneService* service = nullptr;  // the merged city level (the lamp budget), whether anything wants the scene
    LightManager* lights = nullptr;       // GI-BOUNCE-RT: the headlights of the frame and the cars' lamps (the emitters); the manager is asked to run while the emitters are wanted
  };
  bool Initialize(Device& device, FrameRing& frames, const Sources& sources);
  // GPU idle and the FrameRing drained.
  void Shutdown();
  bool initialized() const { return device_ != nullptr; }

  // ---- the frame ----
  void BeginFrame(uint64_t frame);
  void SetSunCascades(const rt_hit::SunCascades& cascades) { cascades_ = cascades; }
  // The top level's view space: `view` the world to view matrix of the frame's camera (the car frame's, or the predicted one), `flip_z` as the car scene's instances are placed with it.
  // `car` the player's car is in this top level.
  void NoteView(const temporal::Mat4& view, bool flip_z, bool car);
  // Writes t10 and t11 of a frame slot's set (the slot's buffers are made on the first call). Not filled yet this frame: the buffers are cleared (the rows read as invalid) until OnTrigger.
  void Bind(plume::RenderDescriptorSet* set, const Bindings& bindings, uint32_t slot);
  // Makes the frame's rows and lamp window and writes them into the slot's buffers. False: nothing was filled (no top level this frame, or no buffers).
  bool OnTrigger(uint32_t slot);

  // ---- what the frame holds, for the host ----
  // This frame's rows and lamp words as written (valid after OnTrigger until the next BeginFrame); null before.
  struct Current {
    const uint32_t* rows = nullptr;        // rt_world::kTotalRows x 4 words
    const uint32_t* lamp_words = nullptr;  // rt_world::LampFrame::words
    uint32_t lamp_rows = 0;
    rt_world::WorldDecoded decoded;
    const rt_world::LampFrame* lamps = nullptr;
  };
  Current current() const;
  bool Valid() const { return filled_frame_ == frame_ && filled_frame_ != ~uint64_t(0); }
  // Replaces the static lamp table (a table with the street lamps merged in: lamp_merge.h MergeLive); null: the pack's own again.
  void SetStaticLamps(std::shared_ptr<const larecomp::citypack::LampTable> table);

  // ---- the emitters (GI-BOUNCE-RT, rt_emitters_rules.h) ----
  // The ray traced bounce light asks for the cars' lamps (neon, headlights, tail lamps, police bars, the mods' outside lamps) every frame it runs (GiUpdate::OnWant, before the frame's trigger); `ultra` and `weak`
  // choose the budget table's tier (rt_emitters::TierFor). They are made at OnTrigger, as GpuLight rows in the lamp buffer after the lamps (row 44 of the world rows says where), for every consumer of the
  // world frame: the probe field reads the static ones, the per-pixel pass and the reflections all of them. Not wanted: no list, no rows, and the light manager is not asked to run for it.
  void WantEmitters(bool wanted, bool ultra, bool weak);
  void WantBounceEmitters(bool wanted);
  void UpdateEmitterWant();
  // GI-BOUNCE-JOIN (gi_bounce_carrier.h): what THIS frame's list says about a light of the deferred light pass, by the light's id: not in the list (not wanted, too far, over the budget, dark, a hole, a frame the
  // world frame was not filled in), a STATIC emitter (the probe field carries its bounce) or a MOVING one (the per-pixel pass does, at ULTRA). The light pass asks it for every light it draws, after OnTrigger.
  enum class EmitterMotion : uint8_t { kNotListed = 0, kStatic = 1, kMoving = 2 };
  EmitterMotion EmitterMotionOf(uint64_t light_id) const;

  // ---- the calibration ----
  // True in a stats frame (rt_world::kStatsPeriod): a pass sets CITY_HIT_P_CALIBRATE (the rows' flag says the same).
  bool StatsFrame() const { return rt_world::StatsFrameDue(frame_); }
  // A stats frame's counters (rt_hit_calibration.h kStatsWords words) of any consumer, added into the open window.
  void AddCalibration(const uint32_t words[rt_hit_cal::kStatsWords]) { gains_.Add(words); }
  // A stats frame's lamp term counters (hlsl CITY_HIT_CENSUS_*: eight words) of a consumer that counts them, summed for the census line (R7).
  void AddLampCensus(const uint32_t words[city_hit_shade::kCensusWords]) { lamp_census_.Add(words); }
  const rt_hit_cal::ClassGains& gains() const { return gains_; }

  // ---- the census ----
  std::string CensusText() const;  // "; ray tracing world frame: ..." for the renderer's census line, empty when nothing was ever filled
  uint64_t HeldBytes() const;

 private:
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> world;
    std::unique_ptr<plume::RenderBuffer> lamps;
    uint8_t* world_cpu = nullptr;
    uint8_t* lamps_cpu = nullptr;
    uint64_t filled_frame = ~uint64_t(0);
  };
  bool EnsureSlot(Slot& slot, uint32_t index);
  std::unique_ptr<plume::RenderBuffer> MakeUpload(uint64_t bytes, uint8_t** cpu, const char* name);
  void LoadStaticLamps();
  void RefreshLiveList();
  void BuildEmitters(rt_world::WorldInput* in, const double camera[3]);
  void Wanted(bool on);
  void Log(const rt_world::WorldInput& in, const rt_world::LampFrame& lamps, float radius_m);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  Sources src_;
  Slot slots_[kFramesInFlight];

  uint64_t frame_ = 0;
  uint64_t filled_frame_ = ~uint64_t(0);
  uint64_t view_frame_ = ~uint64_t(0);       // the frame NoteView was called in
  uint64_t last_view_frame_ = ~uint64_t(0);  // the last frame any top level had
  temporal::Mat4 view_;
  bool flip_z_ = true;
  bool car_ = false;
  bool hooks_wanted_ = false;
  rt_hit::SunCascades cascades_;

  // sticky inputs
  uint32_t env_sharp_ = 0, env_blur_ = 0;
  uint64_t env_frame_ = 0;
  float sky_[3] = {};
  bool sky_known_ = false;

  // lamps
  std::shared_ptr<const larecomp::citypack::LampTable> table_;
  std::shared_ptr<const larecomp::citypack::LampTable> table_given_;
  bool table_tried_ = false;
  bool pack_was_open_ = false;
  std::vector<rt_world::LiveLamp> live_;
  uint64_t live_seq_ = 0;
  bool live_fresh_ = false;
  rt_world::LampFrame lamp_frame_;
  std::array<uint32_t, rt_world::kTotalRows * 4> rows_{};
  rt_world::WorldDecoded decoded_;
  rt_world::Schedule schedule_;

  // the emitters (GI-BOUNCE-RT): the state lives in the .cpp (rt_emitters_rules.h is not light)
  struct EmitterState;
  std::unique_ptr<EmitterState> emitters_;

  // calibration
  rt_hit_cal::ClassGains gains_;
  rt_world::LampCensus lamp_census_;

  // census
  rt_world::Census census_;
  uint64_t last_log_frame_ = 0;
  uint64_t window_start_ = 0;
  bool logged_first_ = false;
};

}  // namespace mcla::gpu
