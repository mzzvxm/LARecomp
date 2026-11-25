#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city in the player's side mirrors: the rules that need no renderer (MIRROR-CITY)
// ===========================================================================
// perf/MIRROR-CITY.md has the design and the cost. Until now the ray traced side mirrors (car_mirror_rt.h) traced the player's own car and the cars beside it and showed the game's environment map where
// the mirror ray found nothing (hlsl/car_mirror_rt_cs.hlsl MirrorWorld: "the hook for the city"). The city line (city_rt_scene.h, city_hit_scene.h, hlsl/city_hit_shade.hlsli) already puts the city's
// triangles in the same top level and shades a hit on them from their material; this is the mirror's side of it:
//
//   THE RAY        the mirror's view is the camera reflected in the glass: a ray from every glass pixel leaves along reflect(view, normal), its start lifted off the glass (car_mirror_rt_rules.h Reflect,
//                  kLift); the ray from the VIRTUAL CAMERA (MirrorMatrix) through the glass point is the same line (the test holds the two together). The mirror flips handedness (determinant -1).
//   THE TRACE      one closest-hit query in the one top level (CityHitTrace: the cars' parts and the city's triangles, the cut-outs' exact alpha in its candidate loop) over the longer of the two reaches
//                  (the cars' and the city cell's: city_refl_rules.h CastLength), going on through the player's own see-through panes. A hit on the city is shaded with the city hit library (ResolveHit
//                  without the picture: the mirror's point is behind the camera, the picture does not show it): the world frame's light and lamps, the sun's ray, the class's gain. A hit on a car is shaded
//                  as before (CarHitShade).
//   THE RULES      (1) a hit with no data in the pool, or past the city's reach, is a MISS: the environment map stays (CITY-RT-REVIEW M5), never a black hit; (2) a car's part that weighs nothing (past
//                  its reach) must not hide the city behind it: the ray goes on for the city alone; (3) a car in its fade band shows the city (or the environment) through it, not black; (4) with neither
//                  the environment map nor a shaded city hit the game's glass stays (car_mirror_rt_rules.h WorldKnown); (5) nothing is counted before the glass is known (no pixel off the glass casts a ray);
//                  (6) there is no history and no noise: the sun's ray is sharp and the lamp sum has no ray, so nothing can be stale after a camera cut and no pattern can slide.
//   THE VISIBILITY a mirror that is only a few pixels (a chase camera: the glass is 10 x 14 pixels at 1080p) shows nothing of the street worth the city's structures, hit data and textures: the city is
//                  traced for a pass whose larger mirror covers at least kCityMinPixels, and the city's wants go out (and are kept) only while such a pass ran in the last kCityHoldFrames frames (the scene
//                  service's own idle release): a budget is not spent before a mirror that can show it exists (CITY-RT-REVIEW S4 / M9 class). The first frames of a camera switch to the cockpit show the environment
//                  until the sectors behind the car are built (CHECK).
//   THE SETTING    mcla_gpu_car_mirror_rt_world (debug, live): auto (the default), env (the environment map: the mirror as before this package), city. `auto` FOLLOWS THE PLAYER'S RT CITY REFLECTIONS
//                  ROW (mcla_gpu_rt_reflections, the row before RT SIDE MIRRORS on GRAPHICS > REFLECTIONS): the city where that row comes to on (its AUTO is a strong ray tracing card; HIGH or ULTRA by hand
//                  on a weak one runs the Mid class's cell), the environment where it comes to off (OFF, or AUTO on a weak card): one control for "the ray traced city in the reflections", the
//                  mirror an ordinary part of it, and no city kept alive for the mirror alone by a player who turned the city's reflections off. The mirror's cell is always HIGH's (150 m on a strong card, 60 m
//                  on the Mid class), never ULTRA's 1,000 m. `city` is the city wherever the card can trace, whatever that row says; `env` never. The data gate is the reflections'
//                  (city_refl_setting.h: the pack and the hit data stand), the wants go out meanwhile (the scene service, rt_scene_rules.h Consumer::kCityMirror) and the mirror shows the environment until then.
//
// This header includes no renderer: car_mirror_city_rules_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "car_mirror_rt_rules.h"
#include "city_refl_rules.h"
#include "city_refl_setting.h"
#include "gpu_class.h"
#include "rt_scene_rules.h"
#include "rt_tier.h"
#include "temporal_math.h"

namespace mcla::gpu::car_mirror_city {

namespace cm = car_mirror;
namespace cr = city_refl;
namespace crs = city_refl_setting;
namespace gc = gpu_class;
using Level = cr::Level;

// ---------------------------------------------------------------------------
// the numbers shared with hlsl/car_mirror_rt_cs.hlsl (the test checks that the shader holds them)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kCityMask = cr::kPartCity;  // 0x80: the city's instance bit, added to the mirror ray's parts while the city runs
static_assert(kCityMask == 0x80 && (kCityMask & cm::kReflectionMask) == 0, "the city's bit is the one the cars' parts leave free");
// The push block's flags word (city.x): the city is traced this frame; the lamp term (the reflections' own switch mcla_gpu_city_refl_lamps); the world frame's light (mcla_gpu_city_refl_world_light).
inline constexpr uint32_t kFlagOn = 1;
inline constexpr uint32_t kFlagLamps = 2;
inline constexpr uint32_t kFlagWorldLight = 4;
inline constexpr float kMinFade = 1.0f / 512.0f;       // a city hit that weighs less is a miss (city_refl.hlsli kCityMinFade)
inline constexpr float kSunTan = 0.0f;                 // the hit's sun ray is sharp: the mirror image has no history to average a jitter, and a jitter would be grain
inline constexpr float kAoMix = cr::kAoMix;            // the share of the baked vertex occlusion in a hit's albedo
inline constexpr float kSunReach = cr::kSunReach;      // 0: the library's 2,000 m
inline constexpr float kLodBias = cr::kLodBias;
inline constexpr uint32_t kPushBytes = 80;
inline constexpr uint64_t kCityMinPixels = 2048;   // the larger mirror's rectangle must cover this many pixels for the city to be traced (a 45 x 45 pixel mirror); CHECK: first guess
inline constexpr uint64_t kCityHoldFrames = 600;   // the city's wants are kept this many frames after the last pass that was big enough (10 s at 60 FPS: the scene service's idle release)             // the trace's push block: 64 bytes of the side mirrors' and the city's 16

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

enum class World : uint8_t { kAuto, kEnv, kCity };

inline World WorldOf(std::string_view text) {
  if (text == "env") {
    return World::kEnv;
  }
  if (text == "city") {
    return World::kCity;
  }
  return World::kAuto;  // `auto`, and any word that is no value
}

inline const char* WorldName(World w) {
  switch (w) {
    case World::kEnv: return "env";
    case World::kCity: return "city";
    case World::kAuto: break;
  }
  return "auto";
}

// What the word is to the city reflections' level rule (city_refl_rules.h LevelFor): city is HIGH (never ultra: a mirror is small), env is off, and auto is what the player's RT CITY REFLECTIONS word
// (`paint_word`: mcla_gpu_rt_reflections, auto / off / high / ultra) comes to, capped at HIGH: off is off, high and ultra are high, anything else (auto, a word that is none) is the reflections' own Auto.
inline const char* LevelWord(World w, std::string_view paint_word) {
  switch (w) {
    case World::kEnv: return "off";
    case World::kCity: return "high";
    case World::kAuto: break;
  }
  if (paint_word == "off") {
    return "off";
  }
  if (paint_word == "high" || paint_word == "ultra") {
    return "high";
  }
  return "auto";
}

// Why a mirror shows the environment map and not the city.
enum class Why : uint8_t {
  kCity,        // it does show the city
  kSetting,     // mcla_gpu_car_mirror_rt_world = env
  kPaintOff,    // auto, and the player's RT CITY REFLECTIONS row is OFF: the mirror follows it
  kCard,        // the card does not trace rays, or the class is Low: no city for the mirror either
  kAutoOff,     // auto comes to off on this card (a weak ray tracing card: the city is the strong card's look)
  kData,        // the city's data is being built or cannot be had: the environment until it stands
  kCount
};

inline const char* WhyName(Why w) {
  switch (w) {
    case Why::kCity: return "the city";
    case Why::kSetting: return "the environment map (the setting says env)";
    case Why::kPaintOff: return "the environment map (RT CITY REFLECTIONS is off)";
    case Why::kCard: return "the environment map (the graphics card does not trace the city)";
    case Why::kAutoOff: return "the environment map (auto is off on this card: a weak ray tracing card)";
    case Why::kData: return "the environment map until the city's data stands";
    case Why::kCount: break;
  }
  return "?";
}

struct Decision {
  Level wanted = Level::kOff;  // what the word comes to on this card (the scene service is asked for it whether or not the data stands: that is what makes the app build the data)
  Level level = Level::kOff;   // what the pass traces now: `wanted` once the data stands and the gate is open
  cr::Cell cell;               // the cell of the wanted level on the ray tracing budget class (a weak card by hand: the Mid class's 60 m); off while nothing is wanted
  crs::Data data = crs::Data::kUnknown;
  Why why = Why::kSetting;
  bool operator==(const Decision&) const = default;
  bool city() const { return level != Level::kOff; }
};

// `paint_word` is the player's RT CITY REFLECTIONS word (mcla_gpu_rt_reflections), `cls` and `tier` the card's (gpu_class::Current(), CurrentRtTier()), `rt_class` the budget class every ray traced effect reads
// (CurrentRtClass()), `data` the city's files' state (city_refl_setting::DataNow()). The same call the player's paint makes (city_refl.cpp BeginFrame: crs::Decide with its word, CellFor of the class), with
// the mirror's word and HIGH's cell.
inline Decision Decide(World world, std::string_view paint_word, gc::GpuClass cls, gc::RtTier tier, gc::GpuClass rt_class, crs::Data data) {
  Decision d;
  d.data = data;
  const crs::Decision base = crs::Decide(cls, tier, LevelWord(world, paint_word), true, data);
  d.wanted = base.wanted;
  if (d.wanted == Level::kOff) {
    // off for a reason: the setting, the player's row, a card that cannot (rung none: no ray tracing units, or the Low class), or auto's own cell
    if (world == World::kEnv) {
      d.why = Why::kSetting;
    } else if (gc::RtRung(cls, tier) == gc::RtTier::kNone) {
      d.why = Why::kCard;
    } else {
      d.why = (world == World::kAuto && paint_word == "off") ? Why::kPaintOff : Why::kAutoOff;
    }
    return d;
  }
  d.cell = cr::CellFor(d.wanted, rt_class);
  if (d.cell.level == Level::kOff) {
    // the budget class is Low (a card that does not trace, forced by hand): nothing to ask for
    d.wanted = Level::kOff;
    d.why = Why::kCard;
    return d;
  }
  if (base.level == Level::kOff) {
    d.why = Why::kData;  // wanted, the structures and the hit data are asked for, the environment shows until they stand
    return d;
  }
  d.level = d.wanted;
  d.why = Why::kCity;
  return d;
}

// What the pass asks the scene service for (rt_scene_rules.h Needs): the cars, the city's structures and hit data at the level, every texture (a hit is shaded from its diffuse). The same wants the
// reflections make at the same level, so the service's merge (one structure, one pool) holds both.
inline rt_scene::Needs NeedsFor(const Decision& d) {
  rt_scene::Needs n;
  if (d.wanted != Level::kOff) {
    const cr::Wants w = cr::WantsFor(d.cell);
    n.cars = w.cars;
    n.city = w.city;
    n.hit = w.hit;
    n.textures = w.textures;
  }
  return n;
}

// The mirror ray's parts: the cars' (kReflectionMask) and, where the city runs, the city's bit. The ray's length: the longer of the cars' reach and the city's (city_refl_rules.h CastLength).
inline uint32_t Parts(const Decision& d) { return cr::ReflectionParts(cm::kReflectionMask, d.level); }
inline float CastLength(const Decision& d, float cars_reach) { return cr::CastLength(Parts(d), cars_reach, d.city() ? d.cell.reach : 0.0f); }

// Whether a pass's mirrors are big enough to show the city: the larger rectangle's area in pixels (car_mirror_rt_rules.h RectFor, the rectangles the pass uses).
inline bool BigEnough(uint64_t largest_rect_pixels) { return largest_rect_pixels >= kCityMinPixels; }
// Whether the city's wants are out in `frame`: a pass that was big enough ran at `seen_frame` and not more than kCityHoldFrames ago.
inline bool CityHeld(uint64_t frame, bool ever_seen, uint64_t seen_frame) { return ever_seen && frame >= seen_frame && frame - seen_frame <= kCityHoldFrames; }

// The push block's flags word. `big_enough` is BigEnough of this pass; `lamps` and `world_light` are the reflections' own two debug switches (the same city hit, lit the same way); both are on by default.
inline uint32_t FlagsFor(const Decision& d, bool big_enough, bool lamps, bool world_light) {
  if (!d.city() || !big_enough) {
    return 0;
  }
  return kFlagOn | (lamps ? kFlagLamps : 0u) | (world_light ? kFlagWorldLight : 0u);
}

// The weight of a city hit at `distance` along the mirror ray: 1 up to the cell's fade start, a smoothstep to 0 at its reach (city_refl_rules.h CityFade); a hit that weighs less than kMinFade is none.
inline float CityWeight(const Decision& d, float distance) {
  const float w = cr::CityFade(distance, d.cell.reach, d.cell.fade_from);
  return w < kMinFade ? 0.0f : w;
}

// ---------------------------------------------------------------------------
// the mirror's view: the camera reflected in the glass
// ---------------------------------------------------------------------------

// The direction of the primary ray through pixel (px, py) of a render_w x render_h picture plus a tap's offset (in pixels), in the view space of the top level (x right, y up, z forward; hlsl/car_mirror_rt_cs.hlsl
// MirrorPixel): a ray from the origin with this direction hits at RayT = the view z.
inline void PrimaryDirection(double px, double py, double tap_x, double tap_y, double render_w, double render_h, double tan_x, double tan_y, double out[3]) {
  const double u = (px + 0.5 + tap_x) / render_w;
  const double v = (py + 0.5 + tap_y) / render_h;
  out[0] = (u * 2.0 - 1.0) * tan_x;
  out[1] = (1.0 - v * 2.0) * tan_y;  // the picture's y goes down, the view's up
  out[2] = 1.0;
}

// The mirror ray of a glass point: the point is the primary direction times the view z `t`, the ray starts off the glass along its normal (kLift and kLiftPerMetre a metre of the view distance) and goes
// along reflect(view, normal). `view` and `normal` are unit; the normal faces the viewer (dot(view, normal) < 0).
struct MirrorRay {
  double glass[3] = {};
  double origin[3] = {};
  double direction[3] = {};
};
inline MirrorRay MirrorRayOf(const double primary[3], double t, const double normal[3]) {
  MirrorRay r;
  const double len = std::sqrt(primary[0] * primary[0] + primary[1] * primary[1] + primary[2] * primary[2]);
  double view[3];
  for (int c = 0; c < 3; ++c) {
    view[c] = primary[c] / len;
    r.glass[c] = primary[c] * t;  // the primary direction has z = 1: its point at RayT t is direction * t
  }
  const double lift = double(cm::kLift) + double(cm::kLiftPerMetre) * t;
  const double k = 2.0 * (view[0] * normal[0] + view[1] * normal[1] + view[2] * normal[2]);
  for (int c = 0; c < 3; ++c) {
    r.origin[c] = r.glass[c] + normal[c] * lift;
    r.direction[c] = view[c] - k * normal[c];
  }
  return r;
}

// The matrix (row vectors, p' = p M) that reflects a point in the plane through `point` with the unit `normal`: the mirror's own view of the world is the camera put through it. Its 3x3 has the
// determinant -1: a mirror reverses handedness (a picture made through it is the world's, left for right).
inline temporal::Mat4 MirrorMatrix(const double point[3], const double normal[3]) {
  temporal::Mat4 m = temporal::Identity();
  const double d = point[0] * normal[0] + point[1] * normal[1] + point[2] * normal[2];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      m.m[i][j] = (i == j ? 1.0 : 0.0) - 2.0 * normal[i] * normal[j];
    }
    m.m[3][i] = 2.0 * d * normal[i];
  }
  return m;
}
inline void Reflected(const temporal::Mat4& m, const double p[3], double out[3]) {
  for (int j = 0; j < 3; ++j) {
    out[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + m.m[3][j];
  }
}
inline double Determinant3(const temporal::Mat4& m) {
  return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) + m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
}

// ---------------------------------------------------------------------------
// the pixel: what the mirror shows
// ---------------------------------------------------------------------------

// What the mirror ray found, as two layers. The CITY layer: a city hit that shows (its data is in the pool and it is inside the city's reach: MirrorCityHit returned true), `weight` the weight of its distance
// (CityWeight), `rgb` its colour; or, behind a car that fades or has nothing to show, the city beyond it. The CAR layer: a car's part that has a normal and a material (MirrorCarHit returned true), `other`
// another car's (the player's own parts weigh 1), `fade` the fade of its distance over the cars' reach (car_mirror::CarFade).
struct CityLayer {
  bool valid = false;
  float weight = 0.0f;
  float rgb[3] = {};
};
struct CarLayer {
  bool valid = false;
  bool other = false;
  float fade = 1.0f;
  float rgb[3] = {};
};
struct Pixel {
  bool shown = false;  // false: the game's glass stays (alpha 0)
  float rgb[3] = {};
};

// A car's weight over what is behind it: a part of the player's own car weighs 1 wherever it is; another car's weighs its fade over the cars' reach, but only where there is a background to fade into (the
// environment map, or a city hit of full weight); with none it weighs 1 (it is the only thing there is to show, the rule of old: `(other && worldKnown)`).
inline float CarWeight(bool other, bool background_shown, float fade) { return (other && background_shown) ? std::clamp(fade, 0.0f, 1.0f) : 1.0f; }

// The pixel from the environment map (`env_known`: the frame holds the map or the shading rows: car_mirror_rt_rules.h WorldKnown), the city layer and the car layer of the mirror ray: the twin of the
// shader's MirrorCityRay tail. The background is the city over the environment by the city's weight (a city hit with no data, or past the reach, is not a layer: the environment stays, never black); the
// car's part is over that by CarWeight. The pixel is shown where something real is: the environment, a city hit of full weight, a car of full weight. A NaN or an infinity is not a colour (the game's glass stays).
inline Pixel Compose(bool env_known, const float env[3], const CityLayer& city, const CarLayer& car) {
  Pixel p;
  float background[3];
  const float wc = city.valid ? std::clamp(city.weight, 0.0f, 1.0f) : 0.0f;
  for (int c = 0; c < 3; ++c) {
    background[c] = city.valid ? env[c] + (city.rgb[c] - env[c]) * wc : env[c];
  }
  const bool background_shown = env_known || (city.valid && wc >= 1.0f);
  const float wk = car.valid ? CarWeight(car.other, background_shown, car.fade) : 0.0f;
  for (int c = 0; c < 3; ++c) {
    p.rgb[c] = car.valid ? background[c] + (car.rgb[c] - background[c]) * wk : background[c];
  }
  p.shown = background_shown || (car.valid && wk >= 1.0f);
  for (int c = 0; c < 3; ++c) {
    if (!std::isfinite(p.rgb[c])) {
      p.shown = false;
    }
  }
  return p;
}

// Whether the frame's world rows were filled and the wet road may be read from them (rt_world_frame_rules.h kWorldValid): the hit's wetness is the world's then, else 0 (as the reflections').
inline float WetFor(uint32_t world_flags, float wet) { return (world_flags & cr::kWorldValidFlag) != 0 && std::isfinite(wet) ? std::clamp(wet, 0.0f, 1.0f) : 0.0f; }

}  // namespace mcla::gpu::car_mirror_city
