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
//   mcla_gpu_headlight_log       debug: a census line every 300 frames
//
// THE FROZEN INTERFACE (HEADLIGHT-SHADOW-REPLAY consumes it): Headlight, HeadlightSet and CurrentHeadlights() below. The set is the frame being
// built: the render thread reads it at any time in the frame and it does not change until the next guest present.
//
// WHERE THE LIGHTS COME FROM. A car's lamps are lens draws: the housing and glass of every lamp, the player's and the racers' xCarLight::PS_Light and the
// traffic's xAmbientCarLight::PSAmbientCarLight (by pixel shader identity; on the retail build CarFrame lists the player's car only, the other cars' draws are
// on the ray tracing branches, so the rig has its own hook). The rig is told of every main-view draw (Scene::NoteAmbientOcclusion, like CarFrame) and for a lens
// draw it takes the lens mesh's model-space box centre (read once from guest memory and kept by the mesh's address; the draw's own bone matrix times gWorld puts
// it in the world) and the car's own frame (gWorld: right, up, back, position; the lens draws of one car share it, which is how they are grouped: no stencil
// ID is needed). At the guest present a car's lenses on its front half (along its forward, minus the third row) are its headlights (headlight_rig_rules.h FrontLamp); the cars
// become tracks that keep a key from frame to frame, at most four are picked by importance (the player's first, then by distance and whether the car is on the
// screen, with a hysteresis so that two equal cars do not trade places), and they fade in and out. The set made at the present of frame N is what frame N + 1
// draws with (one frame of latency: a car's lamps are drawn before the world, but not every world draw is after every lamp), moved one frame ahead by the
// track's own velocity.
//
// THE GAME'S OWN LIGHT POINTS (headlight_points.h, src/mc_engine/hooks/headlight_points.cpp: a midasm hook at 0x8237AFB4, which the night probe proved). The
// game's light submit has every vehicle's light points by kind; the Headlight points have a world position, a racer's and the player's a direction (the car's
// back axis) and an intensity, which is exactly the lights' on state: 2.00 at night, 0.00 by day. The hook (only while the setting is on) writes them to a
// table; the rig matches them to the lens lamps (the nearest within 3 m): the lamp takes the points' place, the car's forward is minus their direction (the
// pose's convention is then only a fallback), and the colour follows the intensity (a car whose points read 0 has no light and no place among the four). A racer's or
// the player's points that no lens lamp is near make a lamp of their own, so the player's lights work even if the lens identity is wrong. A traffic car's points have
// no owner and no direction: they only give the place and the state, and the lens lamp (its pose) gives the forward.
//
// WHEN THEY ARE ON, WITHOUT THEM. The lamps' state is then the sun's: the light's travel direction is in every car draw's vertex constants (c16, as the ambient
// occlusion reads it): at and below 3 degrees of elevation it is night, from 12 degrees up it is day, smooth between (headlight_rig_rules.h NightFactor); a car
// whose points were not seen is lit by that. In the day the hook is called one frame in 30 to see the sun.
//
// THE SHADOW HOOK (src/gpu/local_shadow_types.h): a producer sets the frame's views and the atlas they are in (SetLocalShadows); the variant's shaders
// sample the atlas for the lights that have a valid view. None set: every light is lit and no atlas is read.
//
// Threading: render thread only (Initialize and Shutdown on the lifecycle thread). CurrentHeadlights() and VariantSelected() read what the render thread
// last published.
// ===========================================================================

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "car_dump_rules.h"
#include "car_mesh_read.h"
#include "constants.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "headlight_points.h"
#include "headlight_rig_rules.h"
#include "local_shadow_types.h"
#include "shaders.h"
#include "topology.h"

// ---------------------------------------------------------------------------
// the frozen interface
// ---------------------------------------------------------------------------

namespace mcla::gpu {

// One headlight as the rig hands it over. Positions and directions are world space (the game's: y up). `dir` is a unit vector (the beam's axis, dipped a
// little below the car's forward), `cos_outer` / `cos_inner` the cosines of the cone's outer and inner half angles, `range` the reach in metres, `color` the
// light's colour already times the strength, the fade and the night factor (so a light that fades is a dimmer one), `car_key` a number that stays with a car from
// frame to frame, `is_player` the player's car's.
struct Headlight {
  float pos[3];
  float dir[3];
  float cos_outer;
  float cos_inner;
  float range;
  float color[3];
  uint32_t car_key;
  bool is_player;
};

struct HeadlightSet {
  std::array<Headlight, 4> lights;
  uint32_t count = 0;
};

// The set of the frame being built (empty: no light, and the game's shaders draw).
const HeadlightSet& CurrentHeadlights();

}  // namespace mcla::gpu

// The same names where the shared interface's other half (local_shadow_types.h) lives.
namespace larecomp::gpu {
using mcla::gpu::CurrentHeadlights;
using mcla::gpu::Headlight;
using mcla::gpu::HeadlightSet;
}  // namespace larecomp::gpu

namespace mcla::gpu {

class GuestMemory;

namespace headlight_rig {

// The pixel shaders of the lens draws, by identity on the retail game (the runtime's own: FNV-1a 64 of the microcode).
// CHECK: the traffic lens' identity is from the survey's census (LOCAL-SHADOWS-SURVEY.md section 1.1); the player's is confirmed by perf/out/G8A.txt.
inline constexpr uint64_t kLensPs = 0xCC710F7164C151DEull;         // xCarLight::PS_Light: the player's and the racers' lamps
inline constexpr uint64_t kTrafficLensPs = 0x25E3BB37714FA645ull;  // xAmbientCarLight::PSAmbientCarLight: the traffic's

// The settings' words, as the render thread reads them. `auto` is the class's cell; a word that is no value counts as `auto`.
enum class Mode : uint8_t { kOff, kOn };

// Whether the lit shaders' headlight variant is to be chosen for a draw now: the rig published a light (pipelines.cpp ChoosePackedVariant). Any thread.
bool VariantSelected();

// The shadow producer's side: this frame's views and the atlas they are in (the bindless slots of the texture and of a sampler). `frame` null: none. The
// pointer must stay valid until the next call (the frame's end). Render thread.
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
  void OnMainDraw(const MainDraw& draw);
  void OnPresent();
  // Whether the Scene is to tell the rig of the frame's draws (the setting is on, and it is night or the one frame in 30 that looks at the sun).
  bool wants_draws() const { return enabled_ && observing_; }

  // The block a draw of the variant reads after the shared constants: the frame's lights, the box they reach and the camera of the frame's lens draws (the
  // Scene writes the draw's own camera over it: its pixel constants c12-c15 are the view inverse), and the shadow views of the producer (SetLocalShadows) as of
  // now. Valid until the next call.
  const headlight_rules::HeadlightConstants& Block();
  // A draw of the variant took the block (`alpha`: the alpha test twin), for the census.
  void NoteVariantDraw(bool alpha) {
    ++variant_draws_;
    variant_alpha_draws_ += alpha ? 1u : 0u;
  }
  // A draw that should have taken the variant drew the game's own shader because the variant's pipeline was still being made (Scene::Prepare), for the census.
  void NoteFallbackDraw() { ++fallback_draws_; }

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
  void ReadFrameFacts(const uint8_t* vs_bank, const car_dump::CarMatrices& m);
  bool LensCentre(const MainDraw& draw, const car_dump::CarMatrices& m, double out[3]);
  void Publish(const std::array<headlight_rules::Tracked, headlight_rules::kMaxLights>& tracked, size_t count);
  void RebuildBlock();
  void Log(size_t picked);
  void ClearPublished();

  GuestMemory* memory_ = nullptr;
  bool enabled_ = false;     // the setting resolved to on (read once a frame)
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
  car_mesh::PlayerVehicle player_;
  uint32_t boxes_read_ = 0;

  // Across frames.
  headlight_rules::Tracker tracker_;
  std::unordered_map<uint64_t, LensBox> boxes_;
  car_mesh::Triangles mesh_;
  car_mesh::Scratch scratch_;
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
  uint32_t variant_draws_ = 0, variant_alpha_draws_ = 0, fallback_draws_ = 0;
  uint64_t last_log_ = 0;
  uint64_t night_frames_ = 0, frames_ = 0;
  uint32_t last_cars_ = 0, last_cars_lit_ = 0;
  // The game's own headlight points (headlight_points.h), as the last present saw them.
  std::vector<headlight_points::Point> points_;
  bool points_on_ = false;  // a recent point had light
  uint32_t pts_recent_ = 0, pts_on_ = 0, pts_matched_lamps_ = 0, pts_only_lamps_ = 0, pts_disagree_ = 0;
};

}  // namespace mcla::gpu
