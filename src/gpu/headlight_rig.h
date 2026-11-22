#pragma once
// ===========================================================================
// MCLA GPU renderer -- car headlights as lights: the rig that finds them, picks four and hands them to the lit shaders
// ===========================================================================
// perf/LOCAL-SHADOWS-SURVEY.md finding 1 and section 5.1 (option P0). In the original game a car's headlights light nothing: a glow, a corona and an
// emissive lens. This adds them as spot lights to the city's lit pixel shaders (the PSMultiLight family and the traffic cars' body: the shader variant
// shaderpack/variants/headlight.h), for the player's car, the nearest opponents and the nearest traffic cars, at night.
//
//   mcla_gpu_headlights          auto | off | on, a graphics setting (the pause menu's HEADLIGHTS row on LIGHTING AND SHADOWS, live, saved): `auto` is the
//                                class's cell (gpu_class.h): on for Mid and High, off for Low (the integrated GPU). Off: the game's own shaders draw, bit for
//                                bit (the variant is not chosen), and nothing here costs anything but a flag read.
//   mcla_gpu_headlight_strength  debug: the light's strength, a number the colours are multiplied by (1: the rules' look)
//   mcla_gpu_headlight_range     debug: the reach in metres (rules: kRange)
//   mcla_gpu_headlight_cone      debug: the outer half angle in degrees (rules: kOuterDegrees; the inner is the same share of it)
//   mcla_gpu_headlight_night     debug: -1 follow the game's own lights, else the sun (the default), 0 never, 1 always (a day test; the game's own light points are not
//                                read then)
//   mcla_gpu_headlight_edge      debug: on (the default) a car is ranked by how much of its BEAM is in the view and keeps its light after the game stops drawing it (it left the
//                                frame) while the beam is still in the view; off is the first version (a light dropped within 8 frames, a car whose lamp is off the screen at 0.35)
//   mcla_gpu_headlight_log       debug: a census line every 300 frames
//
// THE FROZEN INTERFACE (HEADLIGHT-SHADOW-REPLAY consumes it): Headlight, HeadlightSet and CurrentHeadlights() below. The set is the frame being
// built: the render thread reads it at any time in the frame and it does not change until the next guest present.
//
// WHERE THE LIGHTS COME FROM. A car's lamps are lens draws: the housing and glass of every lamp, the player's and the racers' xCarLight::PS_Light and the
// traffic's xAmbientCarLight::PSAmbientCarLight (by pixel shader identity; on the retail build CarFrame lists the player's car only, the other cars' draws are
// on the ray tracing branches, so the rig has its own hook). The rig is told of every main-view draw (Scene::NoteAmbientOcclusion, like CarFrame) and for a lens
// draw it takes the lens mesh's model-space box centre (read once from guest memory and kept by the mesh's address; the draw's own bone matrix times gWorld puts
// it in the world) and the car's own frame: gWorld is the car's root translation ONLY (the lens draws of one car share it, which is how they are grouped: no stencil
// ID is needed), the car's turn is in the draw's bone (PoseFromBone: the rows right, up, back of the bone's rotation). A lens draw whose vertex shader reads no bone
// palette (a glow or a sprite with a world matrix of its own) or whose centre is at the car's root is no lamp and is left out. At the guest present a car's lenses on
// its front half (along its forward, minus the third row) are its headlights (headlight_rig_rules.h FrontLamp); the cars
// become tracks that keep a key from frame to frame, at most four are picked by importance (the player's first, then by distance and whether the car is on the
// screen, with a hysteresis so that two equal cars do not trade places), and they fade in and out. The set made at the present of frame N is what frame N + 1
// draws with (one frame of latency: a car's lamps are drawn before the world, but not every world draw is after every lamp), moved one frame ahead by the
// track's own velocity.
//
// THE FRAME'S EDGE (HL-EDGE-FADE, owner 2026-11-20: "as traffic cars get near the edge of the frame, their headlights seem to turn off, or at least get much weaker"). The game draws a car
// only while its bounds are in the view, so the rig knows a car only then, and the first version dropped a car's light within 8 frames of the game's last draw of it, and ranked a car whose
// LAMP was off the screen at 0.35: a car at the edge of the frame lit the road in the picture and was treated as if it lit nothing. Now (light_influence.h; headlight_rig_rules.h Tracker,
// given the frame's frustum) a car's importance is how much of its BEAM is in the view (the beam's cross sections along its axis, weighted by the light's strength there), a car's light fades
// with its distance from the camera (in full to 90 m, gone at 140 m) and, when a car that was picked left through the picture's edge and the game stopped drawing it, its light goes on, the
// car moved on by its own velocity (slowing a little a frame), for up to 90 frames while its beam is in the view (the light follows the beam out of the picture and tapers off over the last
// 36 frames; a car that vanished in the middle of the picture fades as before). The same view ranks the cars for the shadow replay (local_shadow_rules.h ViewPicker). mcla_gpu_headlight_edge
// off gives the first version back. The game's own glows (the halo of a lamp, glow_soft.h) had a twin of the same fault at the screen's edge: a glow whose centre was off the screen was tested
// against the pixel at the edge in its row.
//
// THE GAME'S OWN LIGHT POINTS (headlight_points.h, src/mc_engine/hooks/headlight_points.cpp: a midasm hook at 0x8237AFB8 (one instruction after the night probe's own at 0x8237AFB4, which proved the place: two hooks at one address are one), which the night probe proved). The
// game's light submit has every vehicle's light points by kind; the Headlight points have a world position, a racer's and the player's a direction (the car's
// back axis) and an intensity, which is exactly the lights' on state: 2.00 at night, 0.00 by day. The hook (only while the setting is on) writes them to a
// table; the rig matches them to the lens lamps (the nearest within 3 m): the lamp takes the points' place, the car's forward is minus their direction (the
// pose's convention is then only a fallback), and the colour follows the intensity (a car whose points read 0 has no light and no place among the four). A racer's or
// the player's points that no lens lamp is near make a lamp of their own, so the player's lights work even if the lens identity is wrong. A traffic car's points have
// no owner and no direction: they only give the place and the state, and the lens lamp (its pose) gives the forward.
//
// WHEN THEY ARE ON, WITHOUT THEM. The lamps' state is then the sun's: the world's light direction, as the ambient occlusion picks it from the frame's draws (the
// Scene hands it over at the present, SetWorldSun): at and below 3 degrees of elevation it is night, from 12 degrees up it is day, smooth between
// (headlight_rig_rules.h NightFactor); a car whose points were not seen is lit by that. A car's OWN draws' c16 is not the world's sun (the headlights run of
// 2026-11-20 read -5 degrees at noon from the lens draws), so it is only the fallback while the occlusion gives none (then the hook is called one frame in 30).
//
// THE SHADOW HOOK (src/gpu/local_shadow_types.h): a producer sets the frame's views and the atlas they are in (SetLocalShadows); the variant's shaders
// sample the atlas for the lights that have a valid view. None set: every light is lit and no atlas is read.
//
// THE LIGHT IS A CAR'S AND ITS LAMPS ARE TWO (HL-PER-LAMP, 2026-11-20). The light is one term for the pair of headlamps (the middle of them), and each Headlight says where
// its two lamps are (`lamp_offset`: the vector from the light to the car's right lamp) for the shadow maps, which are made from each lamp: half the distance between the
// lamps is read from the game's two Headlight points (their distance), else from the front lenses across the car, else it is headlight_rules::kDefaultHalfSpan (0.55 m).
// A light has up to two shadow views (HeadlightConstants::shadow_of and shadow_of_b), and the shader's share of the light is the mean of the two.
//
// Threading: render thread only (Initialize and Shutdown on the lifecycle thread). CurrentHeadlights() and VariantSelected() read what the render thread
// last published.
// ===========================================================================

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "car_dump_rules.h"
#include "car_mesh_read.h"
#include "constants.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "headlight_pass_rules.h"
#include "headlight_points.h"
#include "headlight_rig_rules.h"
#include "headlight_types.h"
#include "light_pick.h"
#include "light_pick_record.h"
#include "local_shadow_types.h"
#include "shaders.h"
#include "topology.h"

namespace mcla::gpu {

class GuestMemory;

namespace headlight_rig {

// The pixel shaders of the lens draws, by identity on the retail game (the runtime's own: FNV-1a 64 of the microcode).
// CHECK: the traffic lens' identity is from the survey's census (LOCAL-SHADOWS-SURVEY.md section 1.1); the player's is confirmed by perf/out/G8A.txt.
inline constexpr uint64_t kLensPs = 0xCC710F7164C151DEull;         // xCarLight::PS_Light: the player's and the racers' lamps
inline constexpr uint64_t kTrafficLensPs = 0x25E3BB37714FA645ull;  // xAmbientCarLight::PSAmbientCarLight: the traffic's

// The settings' words, as the render thread reads them. `auto` is the class's cell; a word that is no value counts as `auto`.
enum class Mode : uint8_t { kOff, kOn };

// Whether the lit shaders' headlight variant is to be asked for by a draw now: the rig published a light AND the headlight route keeps the forward stage (GB-H2: mcla_gpu_headlight_pass is
// not `pass`, or it is and mcla_gpu_headlight_pass_keep_forward is on: the Scene then drops the stage again for the draws the deferred pass lights, headlight_pass_rules.h; the route is read once a
// frame, at the Scene's frame start: SampleRoute, and the switch in the rig's BeginFrame) (pipelines.cpp WantedFeatures). Any thread.
bool VariantSelected();

// Whether HEADLIGHTS (mcla_gpu_headlights, `auto` resolved for this graphics card) is on: the master of both the forward stage and the deferred pass's headlights. Read from the live setting; the
// deferred pass (local_lights.cpp) asks it for its wants of the G-buffer. Render thread.
bool SettingOn();

// The route this frame was started with (what VariantSelected decides by): SampleRoute's last value. Any thread.
larecomp::gpu::headlight_pass::Route CurrentRoute();

// Reads the route's setting (mcla_gpu_headlight_pass, through LiveText: the debug menu's thread writes it) ONCE for the frame and publishes it to CurrentRoute(). Scene::BeginFrame calls it before
// the light pass's and the rig's BeginFrame, which both take the route from CurrentRoute(). Render thread.
larecomp::gpu::headlight_pass::Route SampleRoute();

// The shadow producer's side: this frame's views and the atlas they are in (the bindless slots of the texture and of a sampler). `frame` null, or no
// view, or no atlas slot: none (every light is lit and no atlas is read). The views are copied, and moved to the lights of the set that reads them by
// the car (the set of the next frame can be in another order: RemapShadowViews). Call it at the producer's trigger, in the frame whose lights the
// views were made for (the set does not change until the next present), and again when the views are given up. Render thread.
void SetLocalShadows(const larecomp::gpu::LocalShadowFrame* frame, uint32_t atlas_texture_slot, uint32_t atlas_sampler_slot);

}  // namespace headlight_rig

class HeadlightRig {
 public:
  // One prepared main-view draw (Scene::NoteAmbientOcclusion): everything it is read from stays valid for the call.
  struct MainDraw {
    const DrawView* view = nullptr;
    const ConstantBanks::Banks* banks = nullptr;
    const XenosState* state = nullptr;
    const ShaderStore::Entry* vs = nullptr;
    HostPrimitive primitive;
    uint32_t stencil_ids = 0;  // vehicle_motion.h StencilIdsWritten
  };

  HeadlightRig() = default;
  HeadlightRig(const HeadlightRig&) = delete;
  HeadlightRig& operator=(const HeadlightRig&) = delete;

  void Initialize(GuestMemory& memory);
  void Shutdown();

  // The Scene's hooks. BeginFrame: reads the settings. OnMainDraw: a lens draw is noted, every other draw costs two compares. OnPresent: the frame is over,
  // the lights for the next one are made and published.
  void BeginFrame();
  // The world's sun as the frame's draws carried it (AmbientOcclusion::SunDirection: the unit direction TO the sun), or null when there is none: called by the Scene
  // before the present, so that the night factor of this frame's lights is the world's, not that of a lens draw's constants.
  void SetWorldSun(const double* to_sun);
  void OnMainDraw(const MainDraw& draw);
  // `scene_frame`: the Scene's frame serial, which the scene clock's virtual time (a trailer take) goes by; the wall clock otherwise. The lights' fades, lingers and coasts are in seconds of it.
  void OnPresent(uint64_t scene_frame = ~0ull);
  // The LightManager's picker (light_pick.h LightPicker): the tracker picks with its `headlights` and the rig notes the HEADLIGHTS setting into its budget each frame. Null: the tracker's own.
  void SetLightPicker(light_pick::LightPicker* picker) {
    light_picker_ = picker;
    tracker_.SetPicker(picker ? &picker->headlights : nullptr);
  }
  // Whether the Scene is to tell the rig of the frame's draws (the setting is on, and it is night or the one frame in 30 that looks at the sun).
  bool wants_draws() const { return enabled_ && observing_; }

  // The block a draw of the variant reads after the shared constants: the frame's lights, the box they reach and the camera of the frame's lens draws (the
  // Scene writes the draw's own camera over it: its pixel constants c12-c15 are the view inverse), and the shadow views of the producer (SetLocalShadows) as of
  // now. Valid until the next call.
  const headlight_rules::HeadlightConstants& Block();
  // The world position the block carries as its fallback camera (the camera of the frame's lens draws, in float): what Block().camera holds, without making the block. The light manager's
  // projection of the headlight block (light_project.h, LU-4a) takes it; every draw whose pixel constants hold the camera overwrites it in the block anyway.
  void FrameCamera(float out[3]) const {
    for (int c = 0; c < 3; ++c) {
      out[c] = float(camera_[c]);
    }
  }
  // A draw of the variant took the block (`alpha`: the alpha test twin), for the census.
  void NoteVariantDraw(bool alpha) {
    ++variant_draws_;
    variant_alpha_draws_ += alpha ? 1u : 0u;
  }
  // A draw that should have taken the variant drew the game's own shader because the variant's pipeline was still being made (Scene::Prepare), for the census.
  void NoteFallbackDraw() { ++fallback_draws_; }
  // A draw that asked for the variant dropped it because the deferred pass lights its pixels (the route `pass`, Scene::GBufferKey, headlight_pass_rules.h), for the census.
  void NoteDroppedDraw() { ++dropped_draws_; }

 private:
  // What one car's lens draws showed in this frame: the car is told by its own frame (gWorld: the lens draws of a car share it), not by a stencil ID,
  // which the traffic's lens draws may not carry and which a menu's view of the car does not.
  struct CarObservation {
    headlight_rules::Pose pose;
    std::vector<std::array<double, 3>> lenses;
  };
  // The model-space box of a lens mesh, by the mesh's address (kept while it is seen).
  struct LensBox {
    float lo[3] = {};
    float hi[3] = {};
    bool valid = false;
    uint64_t seen = 0;
  };

  void ReadSettings();
  void ApplyNightOverride();
  void ReadFrameFacts(const uint8_t* vs_bank, const car_dump::CarMatrices& m);
  bool LensCentre(const MainDraw& draw, const car_dump::CarMatrices& m, double out[3]);
  void Publish(const std::array<headlight_rules::Tracked, headlight_rules::kMaxLights>& tracked, size_t count);
  void RebuildBlock();
  void Log(size_t picked);
  uint32_t BoxesUnreadable() const;
  std::string UnreadableReasons() const;
  void ClearPublished();

  GuestMemory* memory_ = nullptr;
  bool enabled_ = false;     // the setting resolved to on (read once a frame)
  bool edge_ = true;         // mcla_gpu_headlight_edge: the tracker is given the frame's view (the beams count, a car that left the frame keeps its light while its beam is in the view)
  bool last_view_ = false;   // the last present had a view (the census)
  bool observing_ = false;   // the hook is wanted in this frame
  uint64_t frame_ = 0;       // guest presents seen
  uint64_t last_night_ = 0;  // the last frame that saw night

  // The frame being observed.
  std::vector<CarObservation> cars_;
  bool facts_read_ = false;  // the camera, the sun and the player's vehicle were read in this frame
  double camera_[3] = {};
  bool camera_ok_ = false;
  temporal::Mat4 view_projection_;
  bool projection_ok_ = false;
  double night_ = 0.0;
  double sun_degrees_ = 90.0;
  bool sun_seen_ = false;
  bool world_sun_ = false;         // the Scene gave the world's sun lately: night_ is made from it, the lens draws' c16 is not read
  uint64_t world_sun_frame_ = 0;   // the frame it was last given in
  car_mesh::PlayerVehicle player_;
  uint32_t boxes_read_ = 0;

  // Across frames.
  headlight_rules::Tracker tracker_;
  light_pick::LightPicker* light_picker_ = nullptr;  // the manager's (SetLightPicker)
  // mcla_gpu_light_pick_record: the tracker's inputs of every guest frame, written for light_pick_test to replay (light_pick_record.h).
  std::string record_path_;
  std::ofstream record_file_;
  light_pick::record::RigFrame record_;
  std::unordered_map<uint64_t, LensBox> boxes_;
  car_mesh::Triangles mesh_;
  car_mesh::Scratch scratch_;
  car_mesh::ShaderReadsCache reads_;  // what a lens draw's vertex shader reads: a bone palette (a mesh on the car) or not (a sprite)
  headlight_rules::HeadlightConstants block_{};
  std::array<headlight_rules::Lamp, headlight_rules::kMaxLights> lamps_{};  // the published lights as the shader reads them
  size_t lamp_count_ = 0;
  uint64_t shadow_serial_ = 0;  // SetLocalShadows' serial the block was made with
  float strength_ = 1.0f;
  float range_ = headlight_rules::kRange;
  float outer_degrees_ = headlight_rules::kOuterDegrees;
  int night_override_ = -1;
  bool log_ = false;
  bool state_logged_ = false;

  // The census.
  uint32_t lens_draws_ = 0, lens_player_ps_ = 0, lens_traffic_ps_ = 0, lens_no_pose_ = 0, lens_no_id_ = 0;
  uint32_t lens_no_bone_ = 0, lens_at_root_ = 0;  // left out: no bone palette in the draw (sprites), a centre at the car's root
  // Why a lens's centre falls to the car's root (HL-EDGE-FADE census): meshes whose box could not be had by the reason (car_mesh::Problem; 0 = read but empty), and draws with no mesh to read.
  uint32_t boxes_readable_ = 0;
  std::array<uint32_t, 9> boxes_unreadable_{};
  uint32_t lens_inline_ = 0, lens_no_element_ = 0, lens_no_stream_ = 0;
  uint32_t variant_draws_ = 0, variant_alpha_draws_ = 0, fallback_draws_ = 0, dropped_draws_ = 0;
  uint64_t last_log_ = 0;
  uint64_t night_frames_ = 0, frames_ = 0;
  uint32_t last_cars_ = 0, last_cars_lit_ = 0;
  // The game's own headlight points (headlight_points.h), as the last present saw them.
  std::vector<headlight_points::Point> points_;
  bool points_on_ = false;  // a recent point had light
  uint32_t pts_recent_ = 0, pts_on_ = 0, pts_matched_lamps_ = 0, pts_only_lamps_ = 0, pts_disagree_ = 0;
};

}  // namespace mcla::gpu
