// Check of the NaN guard (hlsl/nan_guard.hlsli) and of the places that use it (CITY-RT review, NAN-CLEANUP). A program of its own: it needs no renderer, no game files and no GPU, and exits 0 when every
// check holds.
//
//   build: cmake --build <dir> --target larecomp_nan_guard_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src/gpu src/gpu/nan_guard_test.cpp
//   run: larecomp_nan_guard_test [<src/gpu dir>]   (without the path the shaders are found from this file's own place)
//
// Why: D3D12's DXIL max / min / clamp / saturate return the other operand for a NaN, so `min(max(v, 0), x)` drops a NaN there; SPIR-V's FMax / FMin / FClamp leave which operand a NaN gives open, so on
// Vulkan the same text can let it through, into an image that is kept (a history, the probes' atlas, the froxel volume) or into the scene. The helpers decide the NaN by its bits first. It checks:
//   the helpers, the .hlsli compiled as C++ (through hlsl_cpp_shim.h), against the standard library on every kind of NaN (either sign, any payload, quiet or signalling), the infinities, the zeros, the
//   denormals, and two million random bit patterns: what they make of a number is what max / min / clamp / saturate make of it (nothing changes for numbers), what they make of a NaN or an infinity is
//   the same on both APIs (and is not left to them);
//   the places: each shader that reads or makes a colour, a light or a factor that is kept or added to the scene calls the helper where it clamps, and no longer clamps a value that can be NaN with
//   max / min / saturate alone (the text of each place, and that the file includes the helper).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "hlsl_cpp_shim.h"

namespace hlsl_shim {
#include "hlsl/nan_guard.hlsli"
}  // namespace hlsl_shim

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

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream s;
  s << f.rdbuf();
  *out = s.str();
  return true;
}

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

float F(uint32_t bits) { return std::bit_cast<float>(bits); }

// every kind of float the shaders can meet
std::vector<float> Kinds() {
  std::vector<float> v;
  const uint32_t nans[] = {0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFF800001u, 0x7FBFFFFFu, 0xFFBFFFFFu, 0x7FC12345u, 0xFFC12345u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x7FA00000u};
  for (const uint32_t b : nans) v.push_back(F(b));
  const float inf = std::numeric_limits<float>::infinity();
  const float ordinary[] = {inf, -inf, 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, 60000.0f, 65504.0f, 1.0e30f, -1.0e30f, std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(), 1.0e-20f, 3.0f, 123.456f};
  for (const float f : ordinary) v.push_back(f);
  return v;
}

// the number's max, min, clamp and saturate as D3D12 makes them (the shim's: a comparison, so a NaN gives the other operand)
float RefHeld(float v, float hi) {
  if (std::isnan(v)) return 0.0f;
  const float m = v > 0.0f ? v : 0.0f;
  return m < hi ? m : hi;
}
float RefSaturate(float v) {
  if (std::isnan(v)) return 0.0f;
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
float RefMax(float a, float b) {
  if (std::isnan(a)) return b;
  if (std::isnan(b)) return a;
  return a > b ? a : b;
}
bool Same(float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

void TestHelpers() {
  using namespace hlsl_shim;
  const std::vector<float> kinds = Kinds();
  bool nan_ok = true, number_ok = true, or_ok = true, finite_ok = true, held_ok = true, max_ok = true, sat_ok = true, vec_ok = true;
  for (const float a : kinds) {
    if (NgIsNaN(a) != std::isnan(a)) nan_ok = false;
    if (NgIsNumber(a) != std::isfinite(a)) number_ok = false;
    if (!Same(NgNumberOr(a, 7.0f), std::isfinite(a) ? a : 7.0f)) or_ok = false;
    if (!Same(NgFinite(a), std::isfinite(a) ? a : 0.0f)) finite_ok = false;
    for (const float hi : {1.0f, 64.0f, 60000.0f}) {
      if (!Same(NgHeld(a, hi), RefHeld(a, hi))) held_ok = false;
    }
    if (!Same(NgSaturate(a), RefSaturate(a))) sat_ok = false;
    for (const float b : kinds) {
      if (!Same(NgMax(a, b), (std::isnan(a) && std::isnan(b)) ? a : RefMax(a, b))) max_ok = false;
    }
    if (NgIsNumber3(float3(a, 1.0f, 2.0f)) != std::isfinite(a) || NgIsNumber3(float3(1.0f, a, 2.0f)) != std::isfinite(a) || NgIsNumber3(float3(1.0f, 2.0f, a)) != std::isfinite(a) ||
        NgIsNumber4(float4(a, 1.0f, 2.0f, 3.0f)) != std::isfinite(a) || NgIsNumber4(float4(1.0f, 2.0f, 3.0f, a)) != std::isfinite(a) || !NgIsNumber4(float4(1.0f, 2.0f, 3.0f, 4.0f)) ||
        NgIsNumber4(float4(1.0f, a, a, 3.0f)) != std::isfinite(a))
      vec_ok = false;
    const float3 v3 = NgFinite3(float3(a, 1.0f, a));
    if (!Same(v3.x, std::isfinite(a) ? a : 0.0f) || !Same(v3.z, std::isfinite(a) ? a : 0.0f) || v3.y != 1.0f) vec_ok = false;
    const float4 h4 = NgHeld4(float4(a, 2.0f, -1.0f, a), 64.0f);
    if (!Same(h4.x, RefHeld(a, 64.0f)) || h4.y != 2.0f || h4.z != 0.0f || !Same(h4.w, RefHeld(a, 64.0f))) vec_ok = false;
    const float3 o3 = NgNumberOr3(float3(a, 0.25f, a), 1.0f);
    if (!Same(o3.x, std::isfinite(a) ? a : 1.0f) || o3.y != 0.25f || !Same(o3.z, std::isfinite(a) ? a : 1.0f)) vec_ok = false;
  }
  Check(nan_ok, "NgIsNaN is true for every NaN (either sign, any payload, quiet or signalling) and for nothing else");
  Check(number_ok, "NgIsNumber is false for a NaN and for an infinity of either sign, true for every other float (zeros and denormals too)");
  Check(or_ok, "NgNumberOr keeps a number and gives the other value for a NaN or an infinity");
  Check(finite_ok, "NgFinite keeps a number and gives 0 for a NaN or an infinity");
  Check(held_ok, "NgHeld is min(max(v, 0), hi) for a number (infinity is the cap) and 0 for a NaN");
  Check(max_ok, "NgMax is max for numbers, drops a NaN operand (D3D12's rule), and is NaN only when both are");
  Check(sat_ok, "NgSaturate is saturate for a number and 0 for a NaN");
  Check(vec_ok, "the 3 and 4 component forms work channel by channel");

  // two million random bit patterns: the helpers against the references (numbers and NaNs of every payload alike)
  std::mt19937 rng(20261121u);
  bool random_ok = true;
  for (int i = 0; i < 2000000 && random_ok; ++i) {
    const float a = F(uint32_t(rng()));
    const float b = F(uint32_t(rng()));
    if (NgIsNaN(a) != std::isnan(a) || NgIsNumber(a) != std::isfinite(a)) random_ok = false;
    if (!Same(NgHeld(a, 60000.0f), RefHeld(a, 60000.0f)) || !Same(NgSaturate(a), RefSaturate(a)) || !Same(NgFinite(a), std::isfinite(a) ? a : 0.0f)) random_ok = false;
    if (!Same(NgMax(a, b), (std::isnan(a) && std::isnan(b)) ? a : RefMax(a, b))) random_ok = false;
  }
  Check(random_ok, "the helpers equal the references on two million random bit patterns");

  // for numbers nothing changes against the text the places had: min(max(v, 0), hi) and saturate(v)
  bool same_ok = true;
  for (int i = 0; i < 200000 && same_ok; ++i) {
    const float a = (float(rng() % 2000001u) - 1000000.0f) * 0.001f;
    if (NgHeld(a, 64.0f) != min(max(a, 0.0f), 64.0f) || NgSaturate(a) != saturate(a) || NgMax(a, 0.5f) != max(a, 0.5f) || NgNumberOr(a, 1.0f) != a) same_ok = false;
  }
  Check(same_ok, "for a number the helpers are the old text (min(max(v, 0), hi), saturate, max): nothing changes");

  // the rules of the places, as numbers: a visibility or a factor that is NaN is no change (1), a light that is NaN is none (0), an infinity never reaches a weight of 0
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  Check(saturate(NgNumberOr(nan, 1.0f)) == 1.0f && saturate(NgNumberOr(inf, 1.0f)) == 1.0f && saturate(NgNumberOr(0.25f, 1.0f)) == 0.25f, "a visibility that is NaN or infinite is no occlusion (1)");
  Check(NgFinite(nan * 0.0f) == 0.0f && NgFinite(inf * 0.0f) == 0.0f && NgFinite(inf) == 0.0f, "an infinity times a weight of 0 is NaN, and both are none (0)");
  Check(NgHeld(inf, 60000.0f) == 60000.0f && NgHeld(-inf, 60000.0f) == 0.0f && NgHeld(nan, 60000.0f) == 0.0f, "a colour held for a half float: infinity is the cap, minus infinity and NaN are 0");
}

// ---- the places ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Place {
  const char* file;            // under hlsl/
  std::vector<const char*> has;
  std::vector<const char*> hasnt;
  const char* why;
};

void TestPlaces(const std::string& gpu) {
  const std::vector<Place> places = {
      {"nan_guard.hlsli",
       {"bool NgIsNaN(float v)", "bool NgIsNumber(float v)", "float NgHeld(float v, float hi)", "float NgMax(float a, float b)", "float NgSaturate(float v)", "#define NG_BITS(v) asuint(v)", "#define NG_SELECT(condition, yes, no) select((condition), (yes), (no))"},
       {},
       "the helpers (the HLSL branch reads the bits with asuint)"},
      // the screen-space reflections: the trace (and the ray traced fallback) before the cap and the history, the history's reads, the output kept, the term added to the scene
      {"ssr_common.hlsli", {"#include \"nan_guard.hlsli\""}, {}, "the SSR passes include the guard through ssr_common"},
      {"ssr_trace_ps.hlsl",
       {"fallback = min(NgFinite3(SsrRoadViewAt(road_view, screen_uv)), float3(cap, cap, cap)) * share;", "float3 rgb = NgFinite3(sum / float(rays));"},
       {"fallback = min(SsrRoadViewAt(road_view, screen_uv), float3(cap, cap, cap)) * share;", "float3 rgb = sum / float(rays);"},
       "the trace: the game's road view and the sum are numbers before the cap (infinity times cap / infinity is NaN)"},
      {"ssr_rt_trace_cs.hlsl",
       {"fallback = min(NgFinite3(SsrRoadViewAt(road_view, screen_uv)), float3(cap, cap, cap)) * share;", "float3 rgb = NgFinite3(SsrRtBlend(current.xyz, confidence, sum / float(rays), weight));"},
       {"float3 rgb = SsrRtBlend("},
       "the ray traced fallback: the same two places"},
      {"ssr_temporal_ps.hlsl",
       {"const float4 current = float4(NgFinite3(trace_raw.xyz), trace_raw.w);", "history = NgFinite3(g_textures[history_slot].SampleLevel(", "const float4 t = float4(NgFinite3(traced.xyz), traced.w);",
        "return float4(NgFinite3(lerp(current.xyz, history, weight)), z_block);"},
       {"const float4 current = g_textures[trace_slot]", "return float4(lerp(current.xyz, history, weight), z_block);"},
       "the accumulation: this frame's trace, the history read, the neighbourhood and the image that becomes the next history are numbers"},
      {"ssr_apply_ps.hlsl", {"float3 term = NgFinite3(sum / total * fresnel * (gain * ramp * SsrF(SSR_W_STRENGTH)));"}, {"float3 term = sum / total"}, "the term added to the scene is a number"},
      // the hit shading: every consumer of a hit's colour
      {"city_hit_shade.hlsli", {"#include \"nan_guard.hlsli\"", "float3 shaded = NgHeld3(result * p.units, kCityHitMaxColour);", "static const float kCityHitMaxColour = 60000.0;"},
       {"max(result, 0.0) * p.units"}, "the city hit's colour (the reflections, the bounce light, the mirror): not negative, a half float's most, NaN black"},
      {"car_rt_hit_shade.hlsli", {"#include \"nan_guard.hlsli\"", "colour = NgHeld3(result * (f.gain * units), kCarHitMaxColour);"}, {"colour = max(result, 0.0) * (f.gain * units);"}, "the car hit's colour"},
      {"car_rt_refl_cs.hlsl", {"seen = NgHeld3(cityColour, kMaxColour);", "const float3 colour = NgHeld3(mixed, kMaxColour);"}, {"min(max(cityColour, 0.0), kMaxColour)", "min(max(mixed, 0.0), kMaxColour)"},
       "the player's car reflection: the city hit's colour and the image the paint reads"},
      {"car_rt_refl_opp.hlsli", {"const float3 colour = NgHeld3(mixed, kMaxColour);"}, {"min(max(mixed, 0.0), kMaxColour)"}, "the opponents' variant"},
      {"car_rt_refl_glass.hlsli", {"min(NgHeld3(colour, kMaxColour) * (GlassFresnel(nv) * weight * strength), float3(cap, cap, cap))"}, {"min(max(colour, 0.0) * ("}, "the glass variant"},
      {"car_glass_apply_ps.hlsl", {"#include \"nan_guard.hlsli\"", "sum += w * NgFinite4(g_textures[g_push.image.x].Load(int3(texel, 0)));"}, {"sum += w * g_textures[g_push.image.x].Load(int3(texel, 0));"},
       "the glass term added to the scene"},
      // the volumetric light: the volume is its own history
      {"volumetrics_inject_cs.hlsl",
       {"#include \"nan_guard.hlsli\"", "float4 now = NgHeld4(float4(scatter, sigma), 60000.0);", "old = select(NgIsNumber4(old), old, now);"},
       {"float4 now = float4(scatter, sigma);"}, "the froxel volume: this frame's light is a number and a history texel that is not one is no history"},
      // the occlusion and the bounce light
      {"ao_main_ps.hlsl",
       {"#include \"nan_guard.hlsli\"", "visibility = saturate(NgNumberOr(visibility * inv_open, 1.0f));", "bounce = NgHeld3(bounce * inv_open, 60000.0f);",
        "visibility = saturate(NgNumberOr(visibility / max(open_sum, 1e-3f), 1.0f));"},
       {"visibility = saturate(visibility * inv_open);", "min(max(bounce * inv_open, 0.0f), 60000.0f)", "visibility = saturate(visibility / max(open_sum, 1e-3f));"},
       "the occlusion's result: a NaN is no occlusion and no bounce light"},
      {"ao_temporal_ps.hlsl",
       {"#include \"nan_guard.hlsli\"", "return float4(saturate(NgNumberOr(result.x, 1.0f)), NgHeld3(result.yzw, 60000.0f));", "return float4(saturate(NgNumberOr(result, 1.0f)), depth_out, 0.0f, 0.0f);"},
       {"return float4(saturate(result.x), clamp(result.yzw, 0.0f, 60000.0f));", "return float4(saturate(result), depth_out, 0.0f, 0.0f);"},
       "the occlusion's history (both passes): always numbers"},
      {"ao_apply_ps.hlsl",
       {"#include \"nan_guard.hlsli\"", "const float3 factor3 = NgNumberOr3(ao_factor * contact_factor3, 1.0f);", "const float3 gi_factor = NgNumberOr3(factor3 + bounce, 1.0f);"},
       {"const float3 factor3 = ao_factor * contact_factor3;", "const float3 gi_factor = factor3 + bounce;"}, "the factor the picture is multiplied by: a NaN is no change"},
      {"ao_color_ps.hlsl", {"#include \"nan_guard.hlsli\"", "colour = NgHeld3(colour, kColourMax);"}, {"colour = min(max(colour, 0.0f), kColourMax);"}, "the bounce light's colour capture of the game's scene"},
      // the lights and the bounce light added to the scene, the denoiser's guides
      {"local_light_ps.hlsl", {"#include \"nan_guard.hlsli\"", "float3 add = NgFinite3(LpPixelLight(l, g, world, normal, albedo, rt2.w));"}, {"float3 add = LpPixelLight(l, g, world, normal, albedo, rt2.w);"},
       "a local light's term added to the scene"},
      {"gi_apply_ps.hlsl", {"#include \"nan_guard.hlsli\"", "float3 add = NgFinite3(GiaBounceAdd("}, {"float3 add = GiaBounceAdd("}, "the bounce light added to the scene"},
      {"rr_guides_common.hlsli", {"#include \"nan_guard.hlsli\"", "return NgSaturate(NgMax(reactive, transparency));"}, {"return saturate(max(reactive, transparency));"}, "the responsivity guide"},
      {"rr_guides_cs.hlsl", {"source = NgMax(source, g_textures[g_push.slots2.w].Load(int3(p, 0)).x);"}, {"source = max(source, g_textures[g_push.slots2.w]"}, "the hit distance guide's second writer"},
  };
  for (const Place& p : places) {
    std::string text;
    if (!Slurp(gpu + "/hlsl/" + p.file, &text)) {
      Check(false, std::string("hlsl/") + p.file + " is readable");
      continue;
    }
    bool ok = true;
    std::string missing;
    for (const char* h : p.has) {
      if (!Has(text, h)) {
        ok = false;
        missing += std::string(" has:") + h;
      }
    }
    for (const char* n : p.hasnt) {
      if (Has(text, n)) {
        ok = false;
        missing += std::string(" still:") + n;
      }
    }
    Check(ok, std::string("hlsl/") + p.file + ": " + p.why + missing);
  }

  // the places reach the helper: a shader that uses NgXxx includes nan_guard.hlsli itself or through a header it includes
  const std::vector<std::pair<const char*, std::vector<const char*>>> includes = {
      {"ssr_trace_ps.hlsl", {"ssr_pass.hlsli"}},  {"ssr_rt_trace_cs.hlsl", {"ssr_pass.hlsli"}}, {"ssr_temporal_ps.hlsl", {"ssr_pass.hlsli"}},    {"ssr_apply_ps.hlsl", {"ssr_pass.hlsli"}},
      {"car_rt_refl_cs.hlsl", {"car_rt_hit_shade.hlsli"}}, {"rr_guides_cs.hlsl", {"rr_guides_common.hlsli"}}, {"ssr_pass.hlsli", {"ssr_common.hlsli"}},
  };
  for (const auto& [file, via] : includes) {
    std::string text;
    bool ok = Slurp(gpu + "/hlsl/" + file, &text);
    for (const char* v : via) ok = ok && Has(text, std::string("#include \"") + v + "\"");
    Check(ok, std::string("hlsl/") + file + " reaches nan_guard.hlsli through " + via[0]);
  }

  // the shaders that decided a NaN by the bits before this one keep doing so, with their own text (the review's: screen colour, car history, probe update, RtDenoise)
  std::string held_a, held_b, giu, rtd;
  Check(Slurp(gpu + "/hlsl/screen_colour_ps.hlsl", &held_a) && Slurp(gpu + "/hlsl/car_history_ps.hlsl", &held_b) && Has(held_a, "return ((asuint(v) & 0x7FFFFFFFu) <= 0x7F800000u) ? min(max(v, 0.0f), kMaxColour) : 0.0f;") &&
            Has(held_b, "return ((asuint(v) & 0x7FFFFFFFu) <= 0x7F800000u) ? min(max(v, 0.0f), kMaxColour) : 0.0f;"),
        "the colour captures still decide a NaN by the bits (HeldChannel)");
  Check(Slurp(gpu + "/hlsl/gi_update_common.hlsli", &giu) && Has(giu, "const bool nan = (asuint(v) & 0x7FFFFFFFu) > 0x7F800000u;") && Has(giu, "bool GiuFinite(float v)"),
        "the probe update still decides a NaN by the bits (GiuRadianceChannel, GiuFinite)");
  Check(Slurp(gpu + "/hlsl/rt_denoise_common.hlsli", &rtd) && Has(rtd, "bool RtDnIsNumber(float v) { return (asuint(v) & 0x7F800000u) != 0x7F800000u; }"), "RtDenoise still decides a number by the bits (RtDnIsNumber)");

  // the CMake line of this test
  std::string cmake;
  Check(Slurp(gpu + "/CMakeLists.txt", &cmake) && Has(cmake, "add_executable(larecomp_nan_guard_test EXCLUDE_FROM_ALL nan_guard_test.cpp)"), "CMakeLists.txt builds this test (larecomp_nan_guard_test)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  if (argc > 1) {
    gpu = argv[1];
  } else {
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    gpu = slash == std::string::npos ? "." : here.substr(0, slash);
  }
  TestHelpers();
  TestPlaces(gpu);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
