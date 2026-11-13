#pragma once
// ===========================================================================
// MCLA GPU renderer -- the before/after split: an effect on left of a position in the picture, off right of it
// ===========================================================================
// Trailer toolkit M5b (feature-parity/CINEMATIC-PLAN.md, Phase 4). A before/after clip of a per-pixel effect needs the
// same frame twice, with and without the effect. A second take cannot give it for a world that moves (a driven car
// does not repeat), so the renderer can do it in one frame: the debug-only cvar mcla_gpu_split is a position from 0 to
// 1 across the picture's width, and the effects that honour it (mcla_gpu_split_effects: ambient occlusion, the
// present's dither, or both) are applied only left of it. Right of it the picture is what the game draws without the
// effect. A scene moves it with the track `split <a>..<b> over <s>` (scene.h); a compositor (ports\scripts\scene_cut.py)
// adds the divider line and the labels.
//
// How an effect honours it, so that nothing is added to the effect's own shader:
//   ambient occlusion  the apply pass multiplies the scene colour by the occlusion factor; its scissor is cut at the
//                      split column, so the pixels right of it are not drawn at all (ao.cpp RecordApply)
//   present dither     the pass that writes the back buffer is drawn in two parts, one scissor each: left of the split
//                      with the dither's code size, right of it with 0 (present.cpp RecordFitted and the RCAS draw)
// The same position holds for both, as a share of the picture's width (of the render target for the occlusion, of the
// letterboxed picture for the dither), so the edges agree to a pixel at the picture's scale.
//
// Cost: with the cvar at its default (-1) PositionFor is one cvar read and a compare, no draw changes and nothing is
// allocated. A position of 1 or more is "everywhere" and is treated as off. While the split is on the occlusion's
// apply is one draw with a narrower scissor and the dither is two draws that cover the same pixels as one.
//
// This file is the pure half (the columns, the spans, the effect list: split_test.cpp builds it alone) plus the one
// function the renderer calls, which reads the cvars (split.cpp).
// ===========================================================================

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mcla::gpu::split {

// The cvar's value for no split.
constexpr double kOff = -1.0;

// The effects that can honour the split (a bit each).
enum Effect : uint32_t {
  kEffectAo = 1u << 0,      // ambient occlusion's apply pass
  kEffectDither = 1u << 1,  // the dither of the pass that writes the back buffer
  kEffectsAll = kEffectAo | kEffectDither,
};

// What a cvar value stands for: a split at `position` (0 = the effect nowhere, then up to just short of 1) or none. Off
// is anything below 0 (the default is -1) and NaN; 1 and above is the effect everywhere, which needs no work.
inline bool Active(double position) {
  return position >= 0.0 && position < 1.0;  // false for NaN
}

// The columns, counted from the left, of a picture `width` wide that carry the effect: the position's share of the
// width rounded to the nearest column, 0..width. An inactive position is the whole width.
inline uint32_t EffectColumns(double position, uint32_t width) {
  if (!Active(position)) {
    return width;
  }
  const double columns = std::floor(position * double(width) + 0.5);
  if (columns <= 0.0) {
    return 0;
  }
  return columns >= double(width) ? width : uint32_t(columns);
}

// The two parts of a draw across the picture: `effect` columns from the left, then `plain` columns; they add up to the
// width. The plain part is empty when the split is off.
struct Spans {
  uint32_t effect = 0;
  uint32_t plain = 0;
};
inline Spans SpansOf(double position, uint32_t width) {
  Spans spans;
  spans.effect = EffectColumns(position, width);
  spans.plain = width - spans.effect;
  return spans;
}

// mcla_gpu_split_effects: "all" (the default), "ao" or "dither". Anything else counts as all, which is what the cvar's
// allowed list keeps out anyway.
inline uint32_t EffectsOf(std::string_view text) {
  if (text == "ao") {
    return kEffectAo;
  }
  if (text == "dither") {
    return kEffectDither;
  }
  return kEffectsAll;
}

// For the frame being recorded (render thread): the split position when the split is on and `effect` honours it, else
// nothing: the effect is applied whole, as the game draws it. Logs once each time the split goes on or off.
std::optional<double> PositionFor(Effect effect);

}  // namespace mcla::gpu::split
