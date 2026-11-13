// Check of where the sun shafts put the sun in the picture (shafts_sun.h): the world-to-view mapping, the pinhole
// placement, the view and elevation fades, and the choice between the sky's sun constant and the light. A program of
// its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_shafts_sun_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_shafts_sun_test

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>

#include "shafts_sun.h"

using namespace mcla::gpu::shafts_sun;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double tolerance) {
  return std::fabs(a - b) <= tolerance;
}

void Cross(const double a[3], const double b[3], double out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

// A camera looking along `forward` with `up` roughly up: the rows of its view inverse's rotation are right, up and
// back (the game's view space looks along -z), and `rotation` is that matrix's transpose, as shafts.cpp builds it.
struct Camera {
  double rotation[3][3];
  double right[3], up[3], forward[3];
};

Camera MakeCamera(double yaw_degrees, double pitch_degrees) {
  const double yaw = yaw_degrees / kDegrees;
  const double pitch = pitch_degrees / kDegrees;
  Camera c;
  // x east, y up, z south; yaw 0 looks north (-z), positive yaw turns toward east
  c.forward[0] = std::sin(yaw) * std::cos(pitch);
  c.forward[1] = std::sin(pitch);
  c.forward[2] = -std::cos(yaw) * std::cos(pitch);
  const double world_up[3] = {0.0, 1.0, 0.0};
  Cross(c.forward, world_up, c.right);
  const double l = Length(c.right);
  for (double& x : c.right) x /= l;
  Cross(c.right, c.forward, c.up);
  const double back[3] = {-c.forward[0], -c.forward[1], -c.forward[2]};
  const double* rows[3] = {c.right, c.up, back};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      c.rotation[i][j] = rows[j][i];  // rotation = the transpose of the view inverse's rows
    }
  }
  return c;
}

// A world direction at `azimuth` degrees from north (positive toward east) and `elevation` degrees above the horizon.
void Toward(double azimuth_degrees, double elevation_degrees, double out[3]) {
  const double az = azimuth_degrees / kDegrees;
  const double el = elevation_degrees / kDegrees;
  out[0] = std::sin(az) * std::cos(el);
  out[1] = std::sin(el);
  out[2] = -std::cos(az) * std::cos(el);
}

void WorldToViewAxes() {
  const Camera cam = MakeCamera(37.0, -12.0);
  double v[3];
  Check(WorldToView(cam.rotation, cam.forward, v) && Near(v[0], 0, 1e-12) && Near(v[1], 0, 1e-12) && Near(v[2], 1, 1e-12),
        "the camera's forward is view +z");
  Check(WorldToView(cam.rotation, cam.right, v) && Near(v[0], 1, 1e-12) && Near(v[1], 0, 1e-12) && Near(v[2], 0, 1e-12),
        "the camera's right is view +x");
  Check(WorldToView(cam.rotation, cam.up, v) && Near(v[0], 0, 1e-12) && Near(v[1], 1, 1e-12) && Near(v[2], 0, 1e-12),
        "the camera's up is view +y");
  const double back[3] = {-cam.forward[0], -cam.forward[1], -cam.forward[2]};
  Check(WorldToView(cam.rotation, back, v) && Near(v[2], -1, 1e-12), "behind the camera is view -z");
  const double scaled[3] = {5 * cam.forward[0], 5 * cam.forward[1], 5 * cam.forward[2]};
  Check(WorldToView(cam.rotation, scaled, v) && Near(Length(v), 1.0, 1e-12) && Near(v[2], 1, 1e-12),
        "the result is a unit direction whatever the length put in");
  const double zero[3] = {0, 0, 0};
  Check(!WorldToView(cam.rotation, zero, v), "no direction, no view vector");
  const double nan[3] = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
  Check(!WorldToView(cam.rotation, nan, v), "a NaN direction gives nothing");
}

void PlacementBasics() {
  const double tan_y = 0.222;
  const double tan_x = tan_y * 16.0 / 9.0;
  const double centre[3] = {0, 0, 1};
  Placement p = Place(centre, tan_x, tan_y);
  Check(p.in_front && Near(p.ndc[0], 0, 1e-12) && Near(p.ndc[1], 0, 1e-12) && Near(p.uv[0], 0.5, 1e-12) && Near(p.uv[1], 0.5, 1e-12),
        "straight ahead is the centre");
  // the top edge: tan of the half field of view
  const double l = std::sqrt(1 + tan_y * tan_y);
  const double top[3] = {0, tan_y / l, 1 / l};
  p = Place(top, tan_x, tan_y);
  Check(Near(p.ndc[1], 1.0, 1e-9) && Near(p.uv[1], 0.0, 1e-9) && Near(p.away, 1.0, 1e-9), "the half field of view up is the top edge");
  Check(Near(ViewFade(p), 1.0, 1e-12), "the view fade is full at the edge");
  const double right_edge[3] = {tan_x / std::sqrt(1 + tan_x * tan_x), 0, 1 / std::sqrt(1 + tan_x * tan_x)};
  p = Place(right_edge, tan_x, tan_y);
  Check(Near(p.ndc[0], 1.0, 1e-9) && Near(p.uv[0], 1.0, 1e-9), "the half field of view right is the right edge");
  // behind, and at the sideways limit of "in front"
  const double behind[3] = {0, 0, -1};
  p = Place(behind, tan_x, tan_y);
  Check(!p.in_front && Near(p.uv[0], 0.5, 1e-12) && Near(p.uv[1], 0.5, 1e-12) && Near(ViewFade(p), 0.0, 1e-12),
        "behind the camera: no place, no light (the centre, as the census prints it)");
  const double edge_on[3] = {1, 0, 0.04};
  p = Place(edge_on, tan_x, tan_y);
  Check(!p.in_front, "a direction almost 90 degrees to the side is not in front");
  // the view fade between the edge and one and a half screens beyond it
  Placement q;
  q.in_front = true;
  q.away = 2.5;
  Check(Near(ViewFade(q), 0.5, 1e-12), "the view fade is half at 2.5 half-widths");
  q.away = 4.0;
  Check(Near(ViewFade(q), 0.0, 1e-12), "and none at 4 half-widths");
  q.away = 0.0;
  Check(Near(ViewFade(q), 1.0, 1e-12), "and full at the centre");
}

// The 17.7 h survey (w1s-survey-h17_7-both, census frame 3000): the light's sun in view space (+0.012, +0.362,
// +0.932) was logged at uv (0.52, -0.38), ndc (+0.03, +1.75): the same numbers here.
void SurveyLightPosition() {
  const double tan_y = 0.222;
  const double tan_x = tan_y * 16.0 / 9.0;
  const double light_view[3] = {0.012, 0.362, 0.932};
  const Placement p = Place(light_view, tan_x, tan_y);
  Check(Near(p.ndc[0], 0.03, 0.01) && Near(p.ndc[1], 1.75, 0.02), "the survey's light lands at ndc (0.03, 1.75), as logged");
  Check(Near(p.uv[0], 0.52, 0.01) && Near(p.uv[1], -0.38, 0.02), "the survey's light lands at uv (0.52, -0.38), above the picture");
}

// The same frame's sun disc (tour_02_spot0-b.png): 4.8 degrees above the view axis and 3 degrees to the right, at
// about (0.57, 0.31) of the picture. A camera looking north and 2.5 degrees down; the disc at 2.3 degrees above the
// horizon and 3.2 degrees east of north gives that place; the light at 18.7 degrees, the survey's, does not.
void SurveyDiscPosition() {
  const double tan_y = 0.222;
  const double tan_x = tan_y * 16.0 / 9.0;
  const Camera cam = MakeCamera(0.0, -2.5);
  double toward_disc[3];
  Toward(3.2, 2.3, toward_disc);
  double view[3];
  Check(WorldToView(cam.rotation, toward_disc, view), "the disc's direction maps to the view");
  const Placement disc = Place(view, tan_x, tan_y);
  Check(disc.in_front && Near(disc.uv[0], 0.57, 0.02) && Near(disc.uv[1], 0.31, 0.02),
        "a disc 2.3 degrees above the horizon, seen from 2.5 degrees down, sits at (0.57, 0.31) as in the picture");
  double toward_light[3];
  Toward(0.7, 18.7, toward_light);
  Check(WorldToView(cam.rotation, toward_light, view), "the light's direction maps to the view");
  const Placement light = Place(view, tan_x, tan_y);
  Check(light.uv[1] < -0.3 && Near(light.uv[0], 0.52, 0.02), "the light at 18.7 degrees lands above the picture (uv y under -0.3)");
  double a[3], b[3];
  Toward(3.2, 2.3, a);
  Toward(0.7, 18.7, b);
  Check(Near(AngleDegrees(a, b), 16.5, 0.5), "the disc and the light are about 16 degrees apart");
  const SkyJudgement j = JudgeSky(true, a, b);
  Check(j.state == SkyState::kUsed && Near(j.elevation, 2.3, 1e-6) && Near(j.light_angle, 16.5, 0.5),
        "the survey's disc is used (16 degrees from the light: inside the cross-check)");
}

void CameraTurns() {
  // however the camera turns, a sun straight ahead of it is at the centre of the picture, and a sun 5 degrees above
  // its axis is the same place whatever the heading
  const double tan_y = 0.222;
  const double tan_x = tan_y * 16.0 / 9.0;
  double first_uv[2] = {};
  for (int yaw = -170; yaw <= 180; yaw += 35) {
    const Camera cam = MakeCamera(double(yaw), 0.0);
    double toward[3];
    Toward(double(yaw), 0.0, toward);
    double view[3];
    Check(WorldToView(cam.rotation, toward, view), "maps");
    Placement p = Place(view, tan_x, tan_y);
    Check(p.in_front && Near(p.ndc[0], 0, 1e-9) && Near(p.ndc[1], 0, 1e-9), "a sun straight ahead is the centre at every heading");
    Toward(double(yaw), 5.0, toward);
    WorldToView(cam.rotation, toward, view);
    p = Place(view, tan_x, tan_y);
    if (yaw == -170) {
      first_uv[0] = p.uv[0];
      first_uv[1] = p.uv[1];
    }
    Check(Near(p.uv[0], first_uv[0], 1e-9) && Near(p.uv[1], first_uv[1], 1e-9),
          "a sun 5 degrees above the axis is at the same place at every heading");
    Toward(double(yaw) + 180.0, 0.0, toward);
    WorldToView(cam.rotation, toward, view);
    p = Place(view, tan_x, tan_y);
    Check(!p.in_front, "a sun behind the camera has no place at every heading");
  }
}

void ElevationAndAngles() {
  const double up[3] = {0, 1, 0};
  const double down[3] = {0, -1, 0};
  const double east[3] = {1, 0, 0};
  const double zero[3] = {0, 0, 0};
  Check(Near(ElevationDegrees(up), 90.0, 1e-9) && Near(ElevationDegrees(down), -90.0, 1e-9) && Near(ElevationDegrees(east), 0.0, 1e-9),
        "elevations of up, down and the horizon");
  Check(Near(ElevationDegrees(zero), 0.0, 1e-12), "no direction: elevation 0");
  const double tilted[3] = {0, 3, 3};
  Check(Near(ElevationDegrees(tilted), 45.0, 1e-9), "a vector of any length has its direction's elevation");
  Check(Near(AngleDegrees(up, east), 90.0, 1e-9) && Near(AngleDegrees(up, up), 0.0, 1e-6) && Near(AngleDegrees(up, down), 180.0, 1e-9),
        "angles of 90, 0 and 180 degrees");
  Check(Near(AngleDegrees(up, zero), 180.0, 1e-12), "an angle to nothing is 180");
  // the light's c16 is the direction the light travels (y -0.867 at noon): minus it is toward the sun
  const double c16[3] = {0.0, -0.866, 0.5};
  const double toward[3] = {-c16[0], -c16[1], -c16[2]};
  Check(Near(ElevationDegrees(toward), 60.0, 0.05), "the noon light's c16 (y -0.866) is a sun 60 degrees up");
}

void JudgingTheSky() {
  const double light[3] = {0.0, 0.32, 0.95};
  const double sky_ok[3] = {0.05, 0.04, 0.99};
  Check(JudgeSky(false, sky_ok, light).state == SkyState::kNotSeen, "no sky draw: not seen");
  Check(JudgeSky(true, sky_ok, light).state == SkyState::kUsed, "a sky sun near the light is used");
  const double nan[3] = {std::numeric_limits<double>::quiet_NaN(), 0, 1};
  Check(JudgeSky(true, nan, light).state == SkyState::kUnusable, "a NaN constant is unusable");
  const double inf[3] = {std::numeric_limits<double>::infinity(), 0, 0};
  Check(JudgeSky(true, inf, light).state == SkyState::kUnusable, "an infinite constant is unusable");
  const double zero[3] = {0, 0, 0};
  Check(JudgeSky(true, zero, light).state == SkyState::kUnusable, "an all-zero constant (a draw that left nothing there) is unusable");
  const double huge[3] = {10, 0, 0};
  Check(JudgeSky(true, huge, light).state == SkyState::kUnusable, "a constant of length 10 is not a direction");
  const double half[3] = {0.5, 0.0, 0.5};
  Check(JudgeSky(true, half, light).state == SkyState::kUsed || JudgeSky(true, half, light).state == SkyState::kFarFromLight,
        "a constant that is not of unit length is judged as a direction (the shader normalises it)");
  const double sideways[3] = {1.0, 0.0, 0.0};
  SkyJudgement j = JudgeSky(true, sideways, light);
  Check(j.state == SkyState::kFarFromLight && j.light_angle > 70.0, "a sky sun 71 degrees from the light is not the same sun");
  // the limit
  double edge[3];
  Toward(0.0, 0.0, edge);
  double near_limit[3];
  Toward(0.0, 48.0, near_limit);
  double over_limit[3];
  Toward(0.0, 52.0, over_limit);
  Check(JudgeSky(true, near_limit, edge).state == SkyState::kUsed, "48 degrees from the light: used");
  Check(JudgeSky(true, over_limit, edge).state == SkyState::kFarFromLight, "52 degrees from the light: not used");
  // the sky's own elevation is reported
  Check(Near(JudgeSky(true, near_limit, edge).elevation, 48.0, 1e-6), "the sky's elevation is reported");
}

void ElevationFades() {
  // without a sky sun: the light's alone, as before this change
  for (double e = -10.0; e <= 80.0; e += 2.5) {
    const double old_formula = Smooth(0.0, 2.5, e) * (1.0 - Smooth(27.0, 45.0, e));
    Check(Near(ElevationFade(e, false, 99.0), old_formula, 1e-12), "no sky sun: the fade is the light's own");
  }
  Check(Near(ElevationFade(18.7, false, 0.0), 1.0, 1e-12), "the survey's light (18.7) gives full strength");
  Check(Near(ElevationFade(60.0, false, 0.0), 0.0, 1e-12), "noon (60) gives none");
  Check(Near(ElevationFade(-5.0, false, 0.0), 0.0, 1e-12), "a light under the horizon gives none");
  // with the survey's disc at 2.3 degrees: nearly full
  const double survey = ElevationFade(18.7, true, 2.3);
  Check(survey > 0.97 && survey < 1.0, "the survey's disc at 2.3 degrees: nearly full strength");
  Check(Near(ElevationFade(18.7, true, 0.0), 0.0, 1e-12), "a sun at the horizon gives none even with the light high");
  Check(Near(ElevationFade(18.7, true, -3.0), 0.0, 1e-12), "a sun under the horizon gives none even with the light high");
  Check(Near(ElevationFade(18.7, true, 10.0), 1.0, 1e-12), "a sun well above the horizon: full");
  Check(Near(ElevationFade(60.0, true, 60.0), 0.0, 1e-12), "noon with both suns high: none");
  // the fade out follows the light: a high light gives none whatever the disc says
  Check(Near(ElevationFade(60.0, true, 5.0), 0.0, 1e-12), "the light's fade out applies with the sky's sun used");
  const double mid = ElevationFade(40.0, true, 30.0);
  Check(Near(mid, 1.0 - Smooth(27.0, 45.0, 40.0), 1e-12), "the fade out is the light's: 40 degrees gives its share");
  // never above 1 nor below 0
  for (double l = -20.0; l <= 90.0; l += 5.0) {
    for (double s = -20.0; s <= 90.0; s += 5.0) {
      const double f = ElevationFade(l, true, s);
      Check(f >= 0.0 && f <= 1.0, "the elevation fade stays within 0 and 1");
    }
  }
}

// SKY-ID: which draws tell the sun constant. The first table held the five pixel shaders of xrage_atmoscatt_clouds; the
// main view draws the city's xSkyhat (census: the dome is VS 7E5802436ED6C520 with PS D3B9FE4A90C0EF08).
void SkyDrawTables() {
  // the identities are distinct and not zero, in both tables and across them, and every one has a name
  constexpr size_t kPixel = sizeof(kSkyPixelShaders) / sizeof(kSkyPixelShaders[0]);
  constexpr size_t kVertex = sizeof(kSkyVertexShaders) / sizeof(kSkyVertexShaders[0]);
  bool distinct = true;
  bool named = true;
  for (size_t i = 0; i < kPixel; ++i) {
    named = named && kSkyPixelShaders[i].identity != 0 && kSkyPixelShaders[i].name && kSkyPixelShaders[i].name[0];
    for (size_t j = i + 1; j < kPixel; ++j) distinct = distinct && kSkyPixelShaders[i].identity != kSkyPixelShaders[j].identity;
    for (size_t j = 0; j < kVertex; ++j) distinct = distinct && kSkyPixelShaders[i].identity != kSkyVertexShaders[j].identity;
  }
  for (size_t i = 0; i < kVertex; ++i) {
    named = named && kSkyVertexShaders[i].identity != 0 && kSkyVertexShaders[i].name && kSkyVertexShaders[i].name[0];
    for (size_t j = i + 1; j < kVertex; ++j) distinct = distinct && kSkyVertexShaders[i].identity != kSkyVertexShaders[j].identity;
  }
  Check(distinct, "no sky shader identity is listed twice");
  Check(named, "every listed sky shader has an identity and a name");
  Check(kPixel == 11 && kVertex == 4, "six xSkyhat and five xrage_atmoscatt_clouds pixel shaders, four dome vertex shaders");

  // the six pixel shaders of the city's sky (the pack builder's names, shafts_log.py ids checks them against its table)
  struct { uint64_t id; const char* name; } city[] = {
      {0xC7D5174E04D352E4ull, "xSkyhat ps_main"},
      {0x37BB7CE96694769Eull, "xSkyhat ps_main_cheap"},
      {0x45484886A2840899ull, "xSkyhat ps_main_withmoon"},
      {0xD3B9FE4A90C0EF08ull, "xSkyhat ps_main_withstarfield"},
      {0xCE703509D1285287ull, "xSkyhat ps_main_withstarfield_withmoon"},
      {0x347BAEE87E5AE632ull, "xSkyhat ps_sunonly"},
  };
  const uint64_t dome_vs = 0x7E5802436ED6C520ull;  // xSkyhat vs_main
  for (const auto& c : city) {
    const char* name = SkyPixelShaderName(c.id);
    Check(name && std::string(name) == c.name, std::string("the pixel shader ") + c.name + " is a sky shader");
    Check(ClassifySkyDraw(dome_vs, c.id) == SkyDraw::kTells, std::string(c.name) + " with the dome's vertex shader tells the sun");
    Check(ClassifySkyDraw(0, c.id) == SkyDraw::kTells, std::string(c.name) + " tells the sun whatever the vertex shader (the pixel shader is the key)");
  }
  // the census' own main-view sky draw
  Check(ClassifySkyDraw(0x7E5802436ED6C520ull, 0xD3B9FE4A90C0EF08ull) == SkyDraw::kTells, "the census' dome (VS 7E58..., PS D3B9...) tells the sun");
  // the old table is still recognised (the register is the same)
  Check(SkyPixelShaderName(0xA6755E24615016F2ull) && SkyPixelShaderName(0xF1CFDA6F8EC76266ull), "xrage_atmoscatt_clouds ps_main and ps_sunonly stay in the table");

  // what is not the sky: the dome's stencil mask (VS only), the environment map's and the mini sky's shaders, the ground
  // dome, the anchor, the tone map, the shadow collector, a draw without shaders
  struct { uint64_t vs, ps; const char* what; } others[] = {
      {0x892C3FF3002828BDull, 0ull, "the dome's stencil mask (xSkyhat vs_stencil, no pixel shader)"},
      {0x5A2CA13B2568F03Bull, 0x1B0336CB63F48F9Eull, "the environment map's sky (xSkyhat vs_DpBackMain + ps_dpenvmapsky)"},
      {0xB869978B200BFF5Bull, 0xC68BEA0366BA9DE0ull, "the mini sky (xSkyhat vs_MiniSky + ps_miniSky)"},
      {0xAEAF86AD49C20C9Aull, 0xE1C19293E5FB9F5Bull, "the blurred sky (xSkyhat vs_BlurSky + ps_BlurSky)"},
      {0xCFBFDB994A1B3D35ull, 0x5F6C293FBD9AE1B8ull, "the ground dome (xGroundHat)"},
      {0x95C121D46C9C1B5Full, 0x0F681CC854B766D4ull, "the anchor (the motion blur draw)"},
      {0xF097B3E9236CB05Aull, 0xBC5F0EED0A746DEAull, "the tone map"},
      {0x03540FDC130BD4DAull, 0x2CDEA0E25045AD9Eull, "the shadow collector"},
      {0, 0, "nothing"},
  };
  for (const auto& o : others) {
    Check(ClassifySkyDraw(o.vs, o.ps) == SkyDraw::kNone, std::string(o.what) + " is no sky draw");
  }

  // the dome's vertex shader with a pixel shader the table does not have: counted and logged, tells nothing
  Check(ClassifySkyDraw(0x7E5802436ED6C520ull, 0x1234567812345678ull) == SkyDraw::kUnknownPs, "xSkyhat vs_main with an unlisted pixel shader is an unknown sky draw");
  Check(ClassifySkyDraw(0xBA77A519EF33AFBDull, 0x1234567812345678ull) == SkyDraw::kUnknownPs, "xSkyhat vs_main_fast with an unlisted pixel shader is an unknown sky draw");
  Check(SkyPixelShaderName(0x1234567812345678ull) == nullptr && SkyVertexShaderName(0x1234567812345678ull) == nullptr, "an unlisted identity has no name");
  Check(SkyVertexShaderName(0x7E5802436ED6C520ull) && std::string(SkyVertexShaderName(0x7E5802436ED6C520ull)) == "xSkyhat vs_main", "the dome's vertex shader is named");
}

}  // namespace

int main() {
  WorldToViewAxes();
  PlacementBasics();
  SurveyLightPosition();
  SurveyDiscPosition();
  CameraTurns();
  ElevationAndAngles();
  JudgingTheSky();
  ElevationFades();
  SkyDrawTables();
  std::printf("shafts_sun_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
