#pragma once
// ===========================================================================
// MCLA GPU renderer -- the forward headlight stage against the deferred local light pass: the A/B, the picture-identity rule and the retirement gate
// (perf/LIGHT-UNIFY.md 6.3 and 11.2, package GB-H2)
// ===========================================================================
// THE TWO WAYS TO LIGHT WITH A CAR'S HEADLIGHTS (the same lights: the frame's, the rig's four, each with its views of the shadow atlas):
//
//   forward   the lit shaders' headlight variant (shaderpack/variants/headlight.cpp, feature 9): 1072 bytes of constants on every lit draw, the term added in the shader to the sum the shader
//             multiplies by its albedo. It reaches the 58 of the 120 lit shaders the edits know (retail census, headlight_test.cpp); the other 62 (18 read no light grid, 44 add their ambient in a shape
//             the edits do not know: the interiors and a few more), the cars' paint (the player's, the racers'), the pedestrians and everything the game draws with another family of shaders get
//             NOTHING. It reads the maps of the LAST frame (the lit draws of frame N are recorded before the replay's trigger of N).
//   pass      the deferred local light pass (local_lights.h): one scissored additive draw per light from the G-buffer (normal, albedo, lit weight, class) and the depth, the light a row of the frame's
//             GpuLight buffer, the term the one law (light_laws.h, hlsl/light_laws.hlsli), the shadow the ONE lookup (hlsl/light_shadow_lookup.hlsli, the box kernel of the lit shaders) read from the
//             atlas through the frame's view table. It lights every pixel the G-buffer holds a normal and an albedo for: the shaders the forward stage edits AND, where the G-buffer's stage writes
//             them (GB-W1: an albedo from the diffuse texel on 71 of the 100 night and 25 of the 40 day shaders that have one), the ones it leaves out, the cars (73 car, traffic and character
//             shaders: GB-G1, GB-G2) and the pedestrians; and it reads THIS frame's maps (the replay recorded them at the same trigger, before the pass).
//
// THE SWITCH (mcla_gpu_headlight_pass, a hidden debug setting, live): which of the two light the frame's headlights.
//   forward   the default, the game as it is: the variant lights, the pass takes no headlight of the frame (unless mcla_gpu_local_lights_sources says so, which is the old double lighting test).
//   pass      the pass lights the frame's headlights, and the variant is kept ONLY for the draws the pass does not light (see "THE DRAWS IN `pass`" below): a pixel is lit once.
//   both      the variant lights and the pass adds the same lights again (the covered surfaces are lit twice: a diagnostic, the sum of the two).
// THE DRAWS IN `pass` (GB-H2-FOLLOW, from the review's S4). The first version of `pass` took the variant away from EVERY draw, so what the pass does not light had no headlight at all: the neon, the
// store sign and the lamp glass (the 7 emissive shaders of the 8 the night stage holds out: their lit weight is 0 and the pass skips them), and every draw that does not write the planes (a pipeline still
// being made, a view the G-buffer does not carry, a draw whose shader has no G-buffer word). Now the variant stays in `pass` and the Scene drops it per draw (Scene::GBufferKey) exactly for the draws
// whose pixels the pass lights, so every pixel is lit once, by one of the two:
//   the pass lights a draw   its planes are carried by the draw (the draw WRITES them, or is a blend that MASKS them: the pass then lights the surface under it, and a forward term on top would light that
//                            share twice), the frame has the normal and the albedo planes, and the shader's class and lit weight are lit (the material table: not emissive, not none, weight over 0).
//   the variant stays        everything else: a draw the G-buffer does not carry (not the main view, a pipeline pending, no planes this frame, the planes' channels not writable), a draw that CLEARS the
//                            planes under it (they say "no data": the pass skips those pixels), and a shader the pass skips by its class (the 7 emissive ones).
// mcla_gpu_headlight_pass_keep_forward (debug only, default on) is the A/B of this: off, `pass` is the first version again (no headlight on what the pass does not light).
//
// The pass runs for `pass` and `both` whatever mcla_gpu_local_lights says (the G-buffer's normal and albedo planes are wanted for it while HEADLIGHTS is on). HEADLIGHTS (mcla_gpu_headlights) stays the
// master of both: with it off the rig has no light and neither way lights anything.
//
// THE PICTURE-IDENTITY RULE (where both cover the surface, the two pictures are the same light). Three pictures of the same scene time, same frozen cars: BASE (HEADLIGHTS off: the lamps and the ambient
// alone), FORWARD (route forward), PASS (route pass). `forward - base` and `pass - base` are each path's headlight light, free of everything else, in linear light (a display picture is decoded
// from sRGB first; the pixels a channel of which is saturated are left out: the tone map compresses). Over the pixels where BOTH are over a floor:
//   ratio       sum(pass light) / sum(forward light) inside 0.95 .. 1.05
//   same share  the share of those pixels where |pass - forward| <= 10 per cent of forward (a floor of 2 codes of mid grey) is at least 0.90
// that is the lamp A/B's rule (LIGHT-UNIFY section 8 item 7) and the rule here, JudgeIdentity (headlight_ab_compare.py is the picture tool, the same numbers). The pixels only one path lights are
// COVERAGE, not identity: `pass only` is the new light (the left-out shaders, the cars, the pedestrians), `forward only` is what the pass fails to cover (it must be about nothing, and its places are
// the list of what the G-buffer does not hold: glass, emissive, the unlit rage and power-up shaders, a shader with no albedo plane). The two cannot be bit for bit: the forward stage lights by the
// geometric normal from the derivatives and the interpolated world position, the pass by the G-buffer's exported normal and the position rebuilt from the depth, and at a shadow edge a sample's
// place differs by a texel's depth bias; the lamp law and the lookup ARE bit for bit (the golden tests of light_frame_test.cpp and shadow_atlas_test.cpp, and the pixel identity test of
// headlight_pass_test.cpp that puts them together on random surfaces).
//
// THE RETIREMENT (the forward stage is deleted only after the owner has seen the A/B pictures and said so: nothing is deleted in this package). THE GATE (MayRetire): on every scene of the plan the
// identity rule passes, `forward only` is under 2 per cent of the lit pixels, the pass's GPU time with the G-buffer's planes is no more than 0.3 ms over the forward stage's at 4K and 0.15 ms at
// 1080p on the reference GPU (the stage's cost is 1072 bytes of upload and 4 lights x a 16 tap lookup on every lit draw: it is paid by the render thread too, which the pass is not), and the owner
// looked. THE STEPS after the gate: (1) the default of mcla_gpu_headlight_pass becomes `pass` and the pass runs while HEADLIGHTS is on (the planes are wanted for it); one release; (2) the variant is
// deleted: shaderpack/variants/headlight.{h,cpp} and its test, kFeatureHeadlight and its row in variant_chain.h (and the two composes_with entries that name it), the pack's version and stamp, the
// starter list's words, scene.cpp's FillVariantBlock case for it and `headlights_->Block()` with the rig's block code (headlight_rig_rules.h HeadlightConstants, MakeConstants: the LU-4a projection
// ProjectHeadlightBlock goes with it), pipelines.cpp's WantedFeatures line, VariantSelected, the manager's kFlipHeadlightBlock and its self-check, and the G-buffer's composed words with the
// headlight (the chain's 1680 byte offset goes back to 608); (3) the headlight SHADOW providers STAY (the replay, the atlas, the one lookup: the pass and the volume read them). The 1072 bytes a lit
// draw uploads, the 4 lights' loop and the 16 tap lookup in 58 shaders and their pipeline permutations go; the pack shrinks by the headlight words.
//
// Pure: no renderer, no guest, no SDK. headlight_pass_test.cpp holds it.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "local_lights_rules.h"

namespace larecomp::gpu::headlight_pass {

// ---------------------------------------------------------------------------
// the switch
// ---------------------------------------------------------------------------

enum class Route : uint8_t { kForward = 0, kPass = 1, kBoth = 2 };

inline constexpr Route ParseRoute(std::string_view word) {
  if (word == "pass") {
    return Route::kPass;
  }
  if (word == "both") {
    return Route::kBoth;
  }
  return Route::kForward;  // "forward", and a word that is no value
}
inline constexpr const char* RouteName(Route r) { return r == Route::kPass ? "pass" : (r == Route::kBoth ? "both" : "forward"); }

// The lit shaders' headlight variant is asked for by the draws (pipelines.cpp WantedFeatures): in `forward` and `both` always; in `pass` only with the A/B switch mcla_gpu_headlight_pass_keep_forward on
// (the default), and then the Scene drops it again for the draws the pass lights (DropsForwardStage below). With the switch off `pass` is the pass alone.
inline constexpr bool ForwardStageOn(Route r, bool keep_in_pass = false) { return r != Route::kPass || keep_in_pass; }
// The deferred pass lights the frame's headlights (and runs for them whatever mcla_gpu_local_lights says).
inline constexpr bool PassLightsHeadlights(Route r) { return r != Route::kForward; }
// The sources mask the pass runs with: the setting's, and the frame's surface lights when the route says the pass lights the headlights.
inline constexpr uint32_t SourcesFor(Route r, uint32_t sources) { return PassLightsHeadlights(r) ? (sources | local_lights::kSourceFrame) : sources; }
// Whether the pass is on: its own setting, or a route that gives it the headlights.
inline constexpr bool PassOn(Route r, bool setting) { return setting || PassLightsHeadlights(r); }

// How the G-buffer carries a draw (gbuffer_rules.h PlaneMode, kept apart so that this header does not need the planes' rules): not at all (no sidecar on the draw), clearing the planes under it, masked
// (the draw binds the planes with a write mask of 0: they keep what the surface under it wrote: the blends), or writing them.
enum class Carry : uint8_t { kNone = 0, kClear = 1, kMask = 2, kWrite = 3 };

// What the Scene knows of one draw when it decides whether the draw keeps the forward headlight in route `pass`.
struct DrawCover {
  Carry carry = Carry::kNone;
  bool planes_ready = false;  // the frame attaches the normal AND the albedo plane (the pass needs both: local_lights.cpp OnResolve)
  bool shader_lit = true;     // the pass lights what the shader leaves in the planes: its class is not emissive or none and its lit weight is over 0 (gbuffer MaterialTable::PassLights)
};

// The deferred pass lights the pixels of the draw: the draw puts its own surface in the planes, or is a blend over a surface that did (the planes under it are lit), the planes exist, and the pass does
// not skip the shader's class. (The pass's own skips of a whole frame, no camera or no target, are not known at the draw: that frame has no headlights in `pass`, as before.)
inline constexpr bool PassLightsDraw(const DrawCover& c) { return c.planes_ready && c.shader_lit && (c.carry == Carry::kWrite || c.carry == Carry::kMask); }
// The draw's forward headlight is dropped: the route is `pass` and the pass lights its pixels. In every other case the draw keeps the variant it asked for (always in `forward` and `both`).
inline constexpr bool DropsForwardStage(Route r, const DrawCover& c) { return r == Route::kPass && PassLightsDraw(c); }

// The retail census of the forward stage (headlight_test.cpp, the game's 120 lit pixel shaders): what it edits and what it leaves out and the pass does not.
inline constexpr uint32_t kLitShaders = 120;
inline constexpr uint32_t kForwardEdited = 58;
inline constexpr uint32_t kLeftOutNoGrid = 18;   // they read no light grid: no place in the sum to add to
inline constexpr uint32_t kLeftOutAmbient = 44;  // their ambient is not added in a statement of a shape the edits know (the interiors and a few more)
inline constexpr uint32_t kForwardLeftOut = kLeftOutNoGrid + kLeftOutAmbient;
static_assert(kForwardEdited + kForwardLeftOut == kLitShaders, "every lit shader is edited or left out");

// ---------------------------------------------------------------------------
// the picture-identity rule
// ---------------------------------------------------------------------------

inline constexpr float kRatioLow = 0.95f;       // sum(pass) / sum(forward) over the pixels both light
inline constexpr float kRatioHigh = 1.05f;
inline constexpr float kPixelTolerance = 0.10f; // a pixel is the same when |pass - forward| <= this share of forward
inline constexpr float kSameShareMin = 0.90f;   // ... and this share of the pixels both light must be the same
inline constexpr float kFloorOfSameLuma = 2.0f / 255.0f * 0.2f;  // the least forward light a pixel's share is taken of: 2 codes of mid grey in linear light, about
inline constexpr uint32_t kMinBothPixels = 200; // fewer pixels lit by both and there is nothing to judge (the light is in the wrong place)

struct IdentityVerdict {
  uint64_t pixels = 0;
  uint64_t forward_lit = 0;
  uint64_t pass_lit = 0;
  uint64_t both = 0;
  uint64_t forward_only = 0;  // the pass fails to cover them
  uint64_t pass_only = 0;     // the new light: the left-out shaders, the cars, the pedestrians
  double ratio = 0.0;         // sum(pass) / sum(forward) over `both`
  double same_share = 0.0;
  bool enough = false;        // `both` has kMinBothPixels
  bool ok = false;            // the verdict: enough, the ratio in range and the same share reached
};

// `forward[i]` and `pass[i]` are the light each path adds at pixel i: the luma of (picture - base) in linear light, 0 or more; `lit_floor` the least that counts as lit (default 0.01).
inline IdentityVerdict JudgeIdentity(const float* forward, const float* pass, size_t n, float lit_floor = 0.01f) {
  IdentityVerdict v;
  v.pixels = n;
  double sum_f = 0.0, sum_p = 0.0;
  uint64_t same = 0;
  for (size_t i = 0; i < n; ++i) {
    const bool f = forward[i] > lit_floor;
    const bool p = pass[i] > lit_floor;
    v.forward_lit += f ? 1u : 0u;
    v.pass_lit += p ? 1u : 0u;
    if (f && p) {
      ++v.both;
      sum_f += double(forward[i]);
      sum_p += double(pass[i]);
      same += std::fabs(double(pass[i]) - double(forward[i])) <= double(kPixelTolerance) * std::max(double(forward[i]), double(kFloorOfSameLuma)) ? 1u : 0u;
    } else if (f) {
      ++v.forward_only;
    } else if (p) {
      ++v.pass_only;
    }
  }
  v.enough = v.both >= kMinBothPixels;
  if (v.both != 0) {
    v.ratio = sum_f > 0.0 ? sum_p / sum_f : 0.0;
    v.same_share = double(same) / double(v.both);
  }
  v.ok = v.enough && v.ratio >= double(kRatioLow) && v.ratio <= double(kRatioHigh) && v.same_share >= double(kSameShareMin);
  return v;
}

// ---------------------------------------------------------------------------
// the retirement gate
// ---------------------------------------------------------------------------

// CHECK: the three limits below are [I] (the rule's 5 / 10 per cent are the lamp A/B's; these are set to be confirmed by run R2 and R4 of RUNPLAN-gb-h2.txt, not measured).
inline constexpr double kForwardOnlyShareMax = 0.02;  // of the pixels either path lights, the share only the forward stage lights
inline constexpr double kCostOver4kMs = 0.3;          // the pass's GPU time over the forward stage's, 4K, the reference GPU
inline constexpr double kCostOver1080Ms = 0.15;       // ... at 1080p

// What the runs say, per scene (the identity) and over all (the cost and the owner).
struct GateInputs {
  uint32_t scenes = 0;           // the scenes of the plan that were judged
  uint32_t scenes_identical = 0; // of those, the ones whose JudgeIdentity is ok
  double forward_only_share = 0; // the largest over the scenes: forward_only / (forward_lit + pass_only)
  double cost_over_4k_ms = 0.0;  // pass (with the planes) minus forward, GPU ms, 4K
  double cost_over_1080_ms = 0.0;
  bool owner_saw = false;        // the owner looked at the A/B pictures and said the forward stage may go
};

inline bool MayRetire(const GateInputs& in) {
  return in.scenes != 0 && in.scenes_identical == in.scenes && in.forward_only_share <= kForwardOnlyShareMax && in.cost_over_4k_ms <= kCostOver4kMs &&
         in.cost_over_1080_ms <= kCostOver1080Ms && in.owner_saw;
}

}  // namespace larecomp::gpu::headlight_pass
