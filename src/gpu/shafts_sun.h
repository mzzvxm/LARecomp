#pragma once
// ===========================================================================
// MCLA GPU renderer -- where the sun is in the picture, and how strongly it shines (the maths, shafts.h)
// ===========================================================================
// The sun shafts need the sun's place in the picture. The game has two suns:
//
//   the light   VS c16 of the world draws (the direction the sunlight travels): what lights the street. At 17.7 h it
//               is 18.7 degrees above the horizon.
//   the sky's   PS c65 `SunDirection` of the sky shaders (the city's xSkyhat ps_main*, ps_sunonly; the direction
//               TOWARD the sun, in the same world axes the dome's vertices are rotated into): the sun disc the player
//               sees. In the 17.7 h survey (ports/run/w1s-survey-h17_7-both, tour_02_spot0-b.png) the disc is about 5
//               degrees above the view axis while the light's sun came out 21 degrees above it, off the top of the
//               picture: the rays of a blur toward the light's place converged above the picture, not on the disc.
//
// The shafts radiate from the disc, so the place comes from the sky's constant; the light is kept for what the light
// decides (how much sunlight there is: the elevation fade, the colour) and as the cross-check of the sky's constant
// (a constant more than kMaxSkyLightAngle from the light is not the sun in the same axes: it is not used and the log
// says so). A frame without a sky draw falls back to the light, as before.
//
// WHICH DRAWS TELL IT (SKY-ID). The constant is read from the draws of the sky's pixel shaders, found by the renderer's
// shader identity (FNV-1a 64 of the microcode with the vertex fetches normalised: native_gfx/shader_identity.h). The
// first table named the five pixel shaders of xrage_atmoscatt_clouds (the engine's own sky, in the XEX): their identities
// were right (the pack builder's own) but the main view never draws them. The play view's sky is the CITY's, xSkyhat
// (an RPF shader of the city folder): the main view's first colour draw is the dome, VS 7E5802436ED6C520 (xSkyhat
// vs_main, after the dome's stencil mask by vs_stencil 892C3FF3002828BD) with PS D3B9FE4A90C0EF08 (xSkyhat
// ps_main_withstarfield) in the temporal census (ports/run/cen-day: "first draw VS ... PS ...", by the main view), and
// ports/diffs/upscaler-masks-identities.tsv names those identities. All six main-pass pixel shaders of xSkyhat read the
// sun at PS c65 `SunDirection` (the same register in all of them; they differ by the moon and the star field, and
// ps_sunonly draws the disc alone). The shaders that draw into the environment map and the mini sky (ps_dpenvmapsky,
// ps_miniSky, ps_BlurSky) read it too but draw into other targets, so the main-view match never sees them: not listed.
//
// Pure maths, no renderer: shafts_sun_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mcla::gpu::shafts_sun {

struct SkyShader {
  uint64_t identity;
  const char* name;
};

// The pixel shaders whose PS c65 is the sun disc's direction. The city's sky first (the one the census shows in the main
// view), then the engine's own (xrage_atmoscatt_clouds: no census has seen it in the main view; kept in case a scene or
// a game build draws with it, the register is the same).
inline constexpr SkyShader kSkyPixelShaders[] = {
    {0xC7D5174E04D352E4ull, "xSkyhat ps_main"},
    {0x37BB7CE96694769Eull, "xSkyhat ps_main_cheap"},
    {0x45484886A2840899ull, "xSkyhat ps_main_withmoon"},
    {0xD3B9FE4A90C0EF08ull, "xSkyhat ps_main_withstarfield"},
    {0xCE703509D1285287ull, "xSkyhat ps_main_withstarfield_withmoon"},
    {0x347BAEE87E5AE632ull, "xSkyhat ps_sunonly"},
    {0xA6755E24615016F2ull, "xrage_atmoscatt_clouds ps_main"},
    {0xC59AA42D684F1D98ull, "xrage_atmoscatt_clouds ps_main_withmoon"},
    {0x42099300C5CFA72Full, "xrage_atmoscatt_clouds ps_main_withstarfield"},
    {0xA2B20192EA547E84ull, "xrage_atmoscatt_clouds ps_main_withstarfield_withmoon"},
    {0xF1CFDA6F8EC76266ull, "xrage_atmoscatt_clouds ps_sunonly"},
};

// The vertex shaders that draw the sky dome of the main view. A dome draw whose pixel shader is not in the table above
// is logged (a game build or a variant the table does not know) and tells nothing.
inline constexpr SkyShader kSkyVertexShaders[] = {
    {0x7E5802436ED6C520ull, "xSkyhat vs_main"},
    {0xBA77A519EF33AFBDull, "xSkyhat vs_main_fast"},
    {0x8E498B4ADDCDB58Bull, "xrage_atmoscatt_clouds vs_main"},
    {0xF5A4D43E31D42CABull, "xrage_atmoscatt_clouds vs_main_fast"},
};

// The name of a listed shader, null when `identity` is not in `table`.
template <size_t N>
inline const char* SkyShaderName(const SkyShader (&table)[N], uint64_t identity) {
  for (const SkyShader& s : table) {
    if (s.identity == identity) {
      return s.name;
    }
  }
  return nullptr;
}
inline const char* SkyPixelShaderName(uint64_t identity) {
  return SkyShaderName(kSkyPixelShaders, identity);
}
inline const char* SkyVertexShaderName(uint64_t identity) {
  return SkyShaderName(kSkyVertexShaders, identity);
}

// What a main-view colour draw is to the sky constant.
enum class SkyDraw : uint8_t {
  kNone,       // not a sky draw
  kTells,      // a sky pixel shader: its PS c65 is the sun's direction
  kUnknownPs,  // the sky dome's vertex shader with a pixel shader the table does not have: says so, tells nothing
};

inline SkyDraw ClassifySkyDraw(uint64_t vs_identity, uint64_t ps_identity) {
  if (SkyPixelShaderName(ps_identity)) {
    return SkyDraw::kTells;
  }
  return SkyVertexShaderName(vs_identity) ? SkyDraw::kUnknownPs : SkyDraw::kNone;
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegrees = 180.0 / kPi;

// The sun's elevation fades, in degrees: none at and under 0, full from 2.5, falling to none at 45 (the full range
// starts to fall at 27).
constexpr double kElevFadeIn = 2.5;
constexpr double kElevFadeOutStart = 27.0;
constexpr double kElevFadeOutEnd = 45.0;
// The view fade: full while the sun is within this many screen half-widths of the centre (1: the screen's edge) and
// none from kViewFadeEnd, one and a half screens (three half-widths) beyond the edge. The play camera's field of view
// is narrow (25 degrees vertically), so a sun at dusk is often above the picture, and its shafts come in from the top.
constexpr double kViewFadeStart = 1.0;
constexpr double kViewFadeEnd = 4.0;
// A direction is in front of the camera when its forward component is more than this.
constexpr double kMinForward = 0.05;
// The sky's sun constant may differ from the light's: the visible sun sets lower than the light falls (16 degrees at
// 17.7 h). Further apart than this it is taken for a constant in other axes (or not the sun) and is not used.
constexpr double kMaxSkyLightAngle = 50.0;
// The shortest and longest a sky sun constant may be (the shader normalises it, so it need not be a unit vector).
constexpr double kSkyMinLength = 0.25;
constexpr double kSkyMaxLength = 4.0;

inline double Smooth(double edge0, double edge1, double x) {
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

inline double Length(const double v[3]) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// A direction in the world's axes (y up) to its place in the view: x right, y up, z forward, of unit length.
// `rotation` is the camera's view rotation, v_view[j] = sum over i of v_world[i] * rotation[i][j] (the game's row
// vector convention; the previous camera's `view`, or the transpose of the view inverse's rotation); the game's view
// space looks along -z, so z changes sign. False for a vector without a direction.
inline bool WorldToView(const double rotation[3][3], const double world[3], double out[3]) {
  double view[3] = {};
  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i) {
      view[j] += world[i] * rotation[i][j];
    }
  }
  view[2] = -view[2];
  const double length = Length(view);
  if (!(length > 1e-6) || !std::isfinite(length)) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    out[i] = view[i] / length;
  }
  return true;
}

// Where a view-space direction lands in a pinhole picture whose half field of view has tangents tan_x and tan_y.
struct Placement {
  bool in_front = false;        // the direction points forward (a sun behind the camera has no place)
  double ndc[2] = {0.0, 0.0};   // x right, y up, the picture's edges at +-1; 0 when not in front
  double uv[2] = {0.5, 0.5};    // the same as texture coordinates (y down); the centre when not in front
  double away = 0.0;            // the larger of |ndc x| and |ndc y|
};

inline Placement Place(const double to_sun[3], double tan_x, double tan_y) {
  Placement p;
  if (to_sun[2] > kMinForward) {
    p.in_front = true;
    p.ndc[0] = to_sun[0] / (to_sun[2] * tan_x);
    p.ndc[1] = to_sun[1] / (to_sun[2] * tan_y);
    p.away = std::max(std::fabs(p.ndc[0]), std::fabs(p.ndc[1]));
    p.uv[0] = 0.5 + 0.5 * p.ndc[0];
    p.uv[1] = 0.5 - 0.5 * p.ndc[1];
  }
  return p;
}

// 1 while the sun is on the screen, falling to 0 a screen and a half beyond its edge, 0 behind the camera.
inline double ViewFade(const Placement& p) {
  return p.in_front ? 1.0 - Smooth(kViewFadeStart, kViewFadeEnd, p.away) : 0.0;
}

// The elevation of a world direction toward an object (y up), in degrees.
inline double ElevationDegrees(const double toward[3]) {
  const double length = Length(toward);
  if (!(length > 1e-9)) {
    return 0.0;
  }
  return std::asin(std::clamp(toward[1] / length, -1.0, 1.0)) * kDegrees;
}

// The angle between two directions, in degrees; 180 for a vector without a direction.
inline double AngleDegrees(const double a[3], const double b[3]) {
  const double la = Length(a);
  const double lb = Length(b);
  if (!(la > 1e-9) || !(lb > 1e-9)) {
    return 180.0;
  }
  const double dot = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
  return std::acos(std::clamp(dot, -1.0, 1.0)) * kDegrees;
}

// What became of the sky's sun constant of a frame.
enum class SkyState : uint8_t {
  kNotSeen,       // no sky draw told it (or its constant was too old)
  kUnusable,      // not finite, or not a direction (almost no length)
  kFarFromLight,  // further than kMaxSkyLightAngle from the light's sun: not the sun in these axes
  kUsed,          // the sun's place comes from it
};

struct SkyJudgement {
  SkyState state = SkyState::kNotSeen;
  double elevation = 0.0;       // degrees (the sky's sun; valid unless not seen or unusable)
  double light_angle = 0.0;     // degrees between the sky's sun and the light's
};

// `seen`: a sky draw's constant is at hand. `sky` is that constant, `light_toward` the world direction toward the
// light's sun (minus c16).
inline SkyJudgement JudgeSky(bool seen, const double sky[3], const double light_toward[3]) {
  SkyJudgement j;
  if (!seen) {
    return j;
  }
  const double length = Length(sky);
  if (!std::isfinite(length) || !(length > kSkyMinLength) || !(length < kSkyMaxLength)) {
    j.state = SkyState::kUnusable;
    return j;
  }
  j.elevation = ElevationDegrees(sky);
  j.light_angle = AngleDegrees(sky, light_toward);
  j.state = j.light_angle > kMaxSkyLightAngle ? SkyState::kFarFromLight : SkyState::kUsed;
  return j;
}

// How much sunlight the elevation allows. The light's elevation says how much light there is and when the day is too
// high for shafts (the fade out). The fade in is of the lower of the light's and, when the sky's sun is used, the
// sky's: a visible sun at or under the horizon gives none while the light still stands above it.
inline double ElevationFade(double light_elevation, bool sky_used, double sky_elevation) {
  const double low = sky_used ? std::min(light_elevation, sky_elevation) : light_elevation;
  return Smooth(0.0, kElevFadeIn, low) * (1.0 - Smooth(kElevFadeOutStart, kElevFadeOutEnd, light_elevation));
}

}  // namespace mcla::gpu::shafts_sun
