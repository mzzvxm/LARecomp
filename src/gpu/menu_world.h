#pragma once
// ===========================================================================
// MCLA GPU renderer -- the menu worlds: which effects run in which 3D view (the rules)
// ===========================================================================
// HOW THE GAME DRAWS ITS 3D VIEWS (read from the code and from the run logs, 2026-11-13):
//
//   game view     the city in play and in a race: the main view's colour k_2_10_10_10_FLOAT@1024 and depth
//                 kD24FS8@0 at pitch 1280 with 2 guest samples (temporal_identities.h kPlaySamples).
//   pause menu    the pause menu, the race pause, the replay confirmation, the photo mode and the first 18
//                 frames after the start press: the same two keys at 4 guest samples (kPauseSamples), the
//                 city drawn again every frame (about 3,000 draws), the camera the game's pause camera (25 to
//                 54 degrees, near 0.2); the UI in a 4x k_8_8_8_8@1024 target.
//   garage        the same 4x keys and the same UI target (a run's composite line says "colour
//                 k_8_8_8_8@1024 pitch 1280 4x" for every garage frame, and its history counters count the
//                 garage frames as "pause/map" frames: perf run cf-garage): an interior of its own, lit by
//                 the TODGarage set (the lighting manager's garage byte, mcLightingManager+14546, which
//                 hooks/time_of_day.cpp reads and hands here), the camera orbiting a parked car; the
//                 sub-menus' own screens draw a few draws, no world.
//   map           the GPS map and the arcade map: the same 4x keys again, the camera 8,000 to 9,000 units
//                 above the city looking straight down (field of view 25 degrees, near plane 7,200 to
//                 8,000, far 10,000 to 11,000).
//
// All three menu worlds are therefore ONE guest identity, "the pause view" of the host (edram.cpp
// NoteMainViews, temporal.cpp View::kPause), and everything the player set reaches them the same way: the
// host gives the three 4x targets the player's MSAA level as host samples (PAUSE-VIEW-DESIGN.md step 1;
// the log says "the pause view ... is drawn with N samples on the host"), the scene's render scale and the
// temporal upscaler (EdramTargets::IsSceneKey, temporal.cpp), and the HDR layer of the pause UI target
// (frame_layers.cpp). The world behind the menu is LIVE, not frozen: the game draws it again every frame
// with the settings in force (an effect turned on in the pause menu shows a few frames later).
//
// WHAT THE EFFECTS DO IN EACH (this file's rules):
//   pause menu, garage   everything the game view has: the occlusion, the contact shadows, the bounce light
//                        and, where the sky is drawn, the sun shafts. The garage's light is the garage's own
//                        sun (a lamp-like light), the gates the effects already have (its height, the
//                        wetness) decide what the contact shadows do there; no sky is drawn in the garage, so
//                        the shafts record nothing there. The car self-reflections (car_selfrefl.h) run in both:
//                        the car in the garage is the one the effect is for, the history is the previous frame's
//                        picture under the camera that orbits it, and the car is taken as still.
//   map                  the occlusion and the contact shadows stay as before (they run, the map's own
//                        numbers make them sub-pixel: the occlusion's radius is 3 m and the contact ray 0.6 m
//                        of a city seen from 9 km), and the bounce light and the shafts are NOT run: their
//                        reach (a few metres of the surface, a ray toward a sun the map does not show) is
//                        below a pixel from that height and costs several passes. Nor the car self-reflections:
//                        a car is a few pixels there, and the history pass is a full-screen pass.
// The side views (the sun's cascades, the environment map, the road reflection) are identities of their
// own that the game draws in every one of these worlds, so their resolution settings apply there as they
// do in play; the shadow filter changes the lit shaders and so applies wherever they draw.
//
// Pure rules, no renderer: menu_world_test.cpp builds on its own.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "temporal_camera.h"

namespace mcla::gpu::menu_world {

enum class World : uint8_t {
  kNone,    // no 3D view in the frame (or a frame of both kinds)
  kPlay,    // the game view
  kPause,   // the pause menu, the race pause, the replay, the photo mode
  kGarage,  // the garage
  kMap,     // the GPS map, the arcade map
};

constexpr const char* Name(World world) {
  switch (world) {
    case World::kPlay: return "game view";
    case World::kPause: return "pause menu world";
    case World::kGarage: return "garage";
    case World::kMap: return "map";
    case World::kNone: break;
  }
  return "no 3D view";
}

// The numbers of a camera the classification reads (the anchor's, temporal::Camera).
struct Lens {
  bool valid = false;
  double near_plane = 0.0;  // view-space distance at stored depth 1
  double forward_y = 0.0;   // the world's y (up) of the direction the camera looks along
};

// The lens of a camera the anchor gave (null: none known yet). forward[1] is the world's y: the camera at 9,000
// units looks along (0, -1, 0) in the map.
inline Lens LensOf(const temporal::Camera* camera) {
  Lens lens;
  if (camera) {
    lens.valid = true;
    lens.near_plane = camera->near_plane;
    lens.forward_y = camera->forward[1];
  }
  return lens;
}

// The map's camera: far above the city and looking straight down (near 7,200 to 8,000, forward y -1.000 in
// every map frame of the recorded runs); the pause camera's near plane is 0.2 to 5, and no other camera of the
// 4x view is within a thousand units of the map's.
inline constexpr double kMapNear = 1000.0;
inline constexpr double kMapDown = -0.9;
inline bool TopDown(const Lens& lens) {
  return lens.valid && lens.near_plane > kMapNear && lens.forward_y < kMapDown;
}

// The world of a frame. `four_x`: the main view's draws were the 4x kind (the pause view's samples);
// `lens`: the camera of that view; `garage_lighting`: the game's garage byte. A camera that is not known
// (the first frame of a spell) is not the map: the pause menu, until it is known.
inline World Classify(bool four_x, const Lens& lens, bool garage_lighting) {
  if (!four_x) {
    return World::kPlay;
  }
  if (TopDown(lens)) {
    return World::kMap;
  }
  return garage_lighting ? World::kGarage : World::kPause;
}

// Which of the effects run in a world.
struct Rules {
  bool occlusion = false;
  bool contact = false;
  bool bounce = false;
  bool shafts = false;
  bool car_refl = false;  // the car self-reflections' history pass and the paint's ray (car_selfrefl.h)
};

constexpr Rules RulesFor(World world) {
  switch (world) {
    case World::kPlay:
    case World::kPause:
    case World::kGarage:
      return {true, true, true, true, true};
    case World::kMap:
      return {true, true, false, false, false};
    case World::kNone: break;
  }
  return {};
}

// The sun shafts in a world: the game view as it always was (the light's place stands in for a sky that
// no draw told), the others only where a sky draw of the frame told where the sun is (the garage is an
// interior: none is drawn, and a shaft needs a sky to start from).
constexpr bool ShaftsRun(World world, bool sky_drawn) {
  return RulesFor(world).shafts && (world == World::kPlay || sky_drawn);
}

// A 4x view's lens must have been steady for the effects that lean on the previous frame's camera (the
// lens is the previous frame's: a wrong lens is worse than none for a frame): the change of the lens
// between the last two frames is known and within the temporal system's cut rule (temporal_camera.h
// kCutFov, 5%). The pause camera's swing when the menu opens and the map's zoom fail it.
inline constexpr double kLensSteadyStep = temporal::kCutFov;
inline bool LensSteady(bool step_valid, double step) {
  return step_valid && step <= kLensSteadyStep;
}

// The first point of a 4x frame (the game's mid-scene depth copy into the occlusion's trigger page): the
// game resolves the picture in three tiles in an order nobody has looked at, so any rectangle inside the
// picture will do (the effects cover the whole picture whichever tile triggers), and the destination must be
// the whole picture (the 1280x720 within the 1280x736 target), not a tile's own texture. The same rule as
// ao.cpp Judge, for the passes that follow its trigger. `height_alignment`: EdramTargets::kHeightAlignment.
inline bool PauseFirstPointGeometry(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t picture_width,
                                    uint32_t picture_height, uint32_t source_width,
                                    uint32_t source_height, uint32_t height_alignment) {
  if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 || uint32_t(x1) > picture_width ||
      uint32_t(y1) > picture_height) {
    return false;
  }
  return picture_width == source_width && picture_height + height_alignment > source_height;
}

// The garage byte of the game's lighting manager, handed to the renderer by the time-of-day hook (the game's
// thread) and read by the render thread. A hint for the name and the sky rule: nothing else depends on it.
inline std::atomic<bool> g_garage_lighting{false};
inline void SetGarageLighting(bool on) { g_garage_lighting.store(on, std::memory_order_relaxed); }
inline bool GarageLighting() { return g_garage_lighting.load(std::memory_order_relaxed); }

// One effect as the log line tells it: its name and its level in force ("off" when it is off).
struct EffectLevel {
  const char* name;
  std::string level;
};

// "ambient occlusion high, contact shadows high, bounce light low, sun shafts default" for the effects that are
// on in `world`; those on in the settings that the world leaves out come after a semicolon; nothing on says so.
// The order of `effects` is the order of the text; the names are the five of RulesFor (occlusion, contact,
// bounce, shafts, car self-reflections) in that order.
inline std::string EffectsText(World world, std::initializer_list<EffectLevel> effects) {
  const Rules rules = RulesFor(world);
  const bool allowed[5] = {rules.occlusion, rules.contact, rules.bounce, rules.shafts, rules.car_refl};
  std::string running;
  std::string left_out;
  size_t index = 0;
  for (const EffectLevel& effect : effects) {
    const bool on = !effect.level.empty() && effect.level != "off";
    if (on) {
      const bool left = index < 5 && !allowed[index];
      std::string& into = left ? left_out : running;
      if (!into.empty()) {
        into += ", ";
      }
      into += effect.name;
      into += ' ';
      into += effect.level;
      // The shafts need a sky in the picture outside the game view.
      if (index == 3 && world != World::kPlay && !left) {
        into += " (where the sky is drawn)";
      }
    }
    ++index;
  }
  std::string text = running.empty() ? "no effect is on" : running;
  if (!left_out.empty()) {
    text += "; left out here: " + left_out;
  }
  return text;
}

// One line per change of world, not one per frame: a world is announced after it has held for
// kStableFrames frames (the first frames of a pause spell have no effects yet: no camera, then a lens that
// is not known to be steady), and not again until another world has held as long. Frames with no 3D view
// say nothing and break nothing.
class Announcer {
 public:
  static constexpr uint32_t kStableFrames = 4;

  // Once a frame, with the frame's world. True when this frame is the one to announce `world` for.
  bool Observe(World world) {
    if (world == World::kNone) {
      return false;
    }
    if (world != candidate_) {
      candidate_ = world;
      held_ = 1;
    } else if (held_ < kStableFrames) {
      ++held_;
    }
    if (held_ >= kStableFrames && candidate_ != announced_) {
      announced_ = candidate_;
      ++announcements_;
      return true;
    }
    return false;
  }
  World announced() const { return announced_; }
  uint32_t announcements() const { return announcements_; }

 private:
  World candidate_ = World::kNone;
  World announced_ = World::kNone;
  uint32_t held_ = 0;
  uint32_t announcements_ = 0;
};

}  // namespace mcla::gpu::menu_world
