#pragma once
// ===========================================================================
// MCLA GPU renderer -- a draw whose pipeline has a fallback never waits for it on the render thread
// ===========================================================================
// A guest draw whose pipeline is not made yet is made in the background only when the game's rule lets the draw be left out
// (Scene::MaySkipForPipeline: targets drawn into in each of the last 30 frames, multisampled, no stencil writes; pipelines.h,
// "Background builds"): a draw that is simply missing from a frame must not be. Every other miss is made on the render thread, one
// after the other, 20 to 150 ms each when the driver has not compiled the shader before.
//
// The lit world draws of the city all WRITE THE STENCIL (REPLACE on a pass, write mask 0xFF; `pipeline_list.py lacks` on the runs' pipeline files of
// 2026-11-21/22: 52 of the 53 keys the shipped list lacked at the first night, 291 of the 294 the neon light's first G-buffer frames made), so the
// rule refused every one of them, and the light stack's new variants of those draws were all made on the render thread:
//   * the HEADLIGHT variant (word bit 1024, the alpha test twin 1025) on the first night, the first time a car's lamp is lit: 41 to 52
//     pipelines in one frame (3.1 s on D3D12, 5 to 7 s on Vulkan with the driver's cache cold: the freeze watchdog's
//     `frame stalled 5s at phase 0x20000000` in the night runs of the stack's executables, neon off too; main's night run r5c-high-night-d3d12 made 41 here);
//   * the G-buffer's variants (the pixel word 2048 / 3072, the vertex word 8192, and the game's own shader with the sidecar's planes as extra
//     colour targets) the first frames the G-buffer is on: 211 to 276 pipelines (5 to 17.6 s).
// Neither of them leaves anything out of the picture: the game's own pipeline of the same draw (the same shaders without the extra) is the
// fallback, and the draw already writes its stencil there. The headlight is missing from the car's beam and the planes keep an old
// value for the few frames the build takes; the next frame that finds the build ready takes it.
//
// A key with the G-buffer's planes, the road mirror blur or the headlight may defer its pipeline build
// whatever the game's rule says (HasFallback). Scene::Prepare walks these rungs in order: remove the planes,
// then the road mirror blur, then the headlight. The final key keeps the game's rule: a pipeline of the game's
// own that nobody made is a gap of the starter list. While a take runs (sync_builds.h) nothing waits, as before: the
// lookup makes the pipeline or waits for the builder, and the wait is counted.
//
// A variant whose build FAILS (the driver refuses the extra colour targets' blend state, the pack's word does not compile) takes the same way down, for good: the
// lookup answers "no pipeline, not pending", the draw takes the key below, and only a failure of the game's own key at the end drops the draw (NextStep). Before,
// only "pending" took the rung down, so a failed variant left its draw out of every frame (a car body, a road piece) for the rest of the session. A key that carries
// an extra says so in its Deferral (has_fallback): its failure is final in every path (a precache entry that failed on a worker is not made again on the render
// thread) and is logged once per key.
//
// Pure: no renderer, no ReXGlue. variant_wait_test.cpp checks it alone.
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>

#include "shaderpack/variant_chain.h"

namespace mcla::gpu::variant_wait {

// What the draw's pipeline key carries on top of the game's own shader.
struct Extras {
  bool planes = false;     // the G-buffer's planes are attached: extra colour targets, usually with the G-buffer's word
  bool headlight = false;  // the pixel word has the headlight feature
  bool road_mirror_blur = false;
};

constexpr bool Any(const Extras& e) { return e.planes || e.headlight || e.road_mirror_blur; }

// Whether a key with these extras has a fallback key that draws the same thing without them: then the lookup may be answered "pending" whatever the game's rule says.
constexpr bool HasFallback(const Extras& e) { return Any(e); }
constexpr bool HasFallback(bool planes, bool headlight) { return HasFallback(Extras{planes, headlight}); }

// Who answers "may this draw be left out for now" on the lookup of a rung of the ladder.
enum class Ask : uint8_t {
  kGameRule,  // Scene::MaySkipForPipeline
  kAlways,    // the draw is not left out, its fallback draws: pending is always the answer (a take still waits: sync_builds.h)
};
constexpr Ask AskFor(const Extras& e) { return HasFallback(e) ? Ask::kAlways : Ask::kGameRule; }

// How the lookup of one rung ended, and what the draw does next. `extras` are those of the key just asked for; `have_pipeline`: Pipelines::Get gave one;
// `pending`: it gave none because a builder thread is still making it. Neither means the build FAILED: Get remembers that for the key (a failed key is never tried
// again, the failure is logged once, pipelines.cpp LogFailure), so a key that failed stays out for the session and every draw with it takes the rung below.
enum class Next : uint8_t {
  kDraw,      // the rung's pipeline is there
  kFallBack,  // there is none (being made, or its build failed) and the key has a fallback key: the draw takes the key one rung down
  kWait,      // being made, and there is no rung below: the draw is left out for now, as the game's rule allowed it (Skip::kPipelinePending)
  kDrop,      // its build failed, and there is no rung below: the game's own pipeline failed too, so the draw is skipped (Skip::kPipeline)
};
constexpr Next NextStep(const Extras& extras, bool have_pipeline, bool pending) {
  if (have_pipeline) {
    return Next::kDraw;
  }
  if (HasFallback(extras)) {
    return Next::kFallBack;
  }
  return pending ? Next::kWait : Next::kDrop;
}

// The pixel word of the fallback that drops a feature (the pack's word for the other features the draw has when it has one, else the game's own
// shader in the draw's spec variant: the alpha test twin stays). `pack_has(word)` says whether the pixel shader has that word in the variant pack.
template <class Has>
constexpr uint32_t WordWithoutFeature(uint32_t word, uint32_t feature, Has&& pack_has) {
  namespace chain = larecomp::shaderpack::chain;
  const uint32_t rest = chain::Word(chain::FeaturesOf(word) & ~chain::FeatureBit(feature), chain::SpecOf(word));
  if (chain::FeaturesOf(rest) != 0 && !pack_has(rest)) {
    return chain::SpecOf(rest);
  }
  return rest;
}

template <class Has>
constexpr uint32_t WordWithoutHeadlight(uint32_t word, Has&& pack_has) {
  return WordWithoutFeature(word, larecomp::shaderpack::kFeatureHeadlight, pack_has);
}

constexpr bool HasHeadlight(uint32_t ps_word) {
  return larecomp::shaderpack::chain::HasFeature(ps_word, larecomp::shaderpack::kFeatureHeadlight);
}

constexpr bool HasRoadMirrorBlur(uint32_t ps_word) {
  return larecomp::shaderpack::chain::HasFeature(ps_word, larecomp::shaderpack::kFeatureRoadMirrorBlur);
}

// The bloom variant's unit-scale tap is the guest tap. Keep its alpha-test
// specialization and any word composed with other features.
constexpr uint32_t BloomWord(uint32_t word, bool unit_scale, bool skip_identity) {
  namespace sp = larecomp::shaderpack;
  return skip_identity && unit_scale && sp::chain::FeaturesOf(word) == sp::chain::FeatureBit(sp::kFeatureBloomGauss)
             ? sp::chain::SpecOf(word) : word;
}

// The ladder of one draw: the rungs in the order Scene::Prepare asks for them, each with who answers "pending". `extras` are those of the full key
// (planes attached, features in its pixel word). The final rung has no extras.
struct Rung {
  Extras extras;
  Ask ask = Ask::kGameRule;
};
struct Ladder {
  std::array<Rung, 4> rungs{};
  size_t count = 0;
};
constexpr Ladder LadderOf(const Extras& full) {
  Ladder ladder;
  Extras at = full;
  while (true) {
    ladder.rungs[ladder.count++] = Rung{at, AskFor(at)};
    if (!HasFallback(at)) {
      break;
    }
    if (at.planes) {
      at.planes = false;
    } else if (at.road_mirror_blur) {
      at.road_mirror_blur = false;
    } else {
      at.headlight = false;
    }
  }
  return ladder;
}

}  // namespace mcla::gpu::variant_wait
