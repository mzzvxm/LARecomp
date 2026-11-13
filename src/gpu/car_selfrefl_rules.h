#pragma once
// ===========================================================================
// MCLA GPU renderer -- car self-reflections (G8b prototype): the rules that need no renderer
// ===========================================================================
// The test cvar's modes, the tuning numbers, the history matrix and the 112 bytes the car paint variant reads
// (shaderpack/variants/car_selfrefl.h), as pure functions on plain numbers, so that car_selfrefl_rules_test.cpp builds
// on its own. The renderer's half is car_selfrefl.h. This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "temporal_math.h"

namespace mcla::gpu::car_selfrefl {

// mcla_gpu_car_selfrefl_test: 0 the game's paint, 1 the reflection darkened where the car's own geometry blocks it, 2 the
// darkening and the car's own colour in the reflection.
enum class Mode : uint8_t { kOff = 0, kOcclusion = 1, kReflection = 2 };

constexpr Mode ModeOfTest(int value) {
  return value <= 0 ? Mode::kOff : (value == 1 ? Mode::kOcclusion : Mode::kReflection);
}

// The 112 bytes after the SharedConstants of a draw of the paint variant: g_CarReflM0..M3, g_CarReflA, g_CarReflB and
// g_CarReflC of shaderpack/variants/car_selfrefl.cpp, byte 608 on.
struct CarSelfReflConstants {
  // The world position of this frame, as the paint's vertex shader gives it, to the clip space of the frame the history
  // image was made from: the rows of a row-vector matrix (clip = p.x m[0..3] + p.y m[4..7] + p.z m[8..11] + m[12..15]),
  // the vehicle's own motion in it.
  float m[16];
  float a[4];     // x mode, y ray length (m), z thickness (m), w strength
  float b[4];     // x steps, y debug view, z colour gain, w noise frame
  uint32_t c[4];  // x the history image's bindless slot, y a linear clamp sampler's slot, zw reserved
};
static_assert(sizeof(CarSelfReflConstants) == 112);
static_assert(offsetof(CarSelfReflConstants, a) == 64 && offsetof(CarSelfReflConstants, b) == 80 &&
              offsetof(CarSelfReflConstants, c) == 96);

// The tuning cvars (mcla_gpu_car_selfrefl_*), already read.
struct Tuning {
  float length = 2.0f;      // the ray's length in the game's world units (about metres)
  float thickness = 0.25f;  // how far behind a surface a point still hits it
  float strength = 1.0f;    // the most the hit takes of the reflection, 0 to 1
  int steps = 12;           // points on the ray
  int view = 0;             // the debug view, 0 none
  float gain = 1.0f;        // times the exponent bias's, for the history's colour units (CHECK)
};

inline constexpr float kMinLength = 0.2f;
inline constexpr float kMaxLength = 6.0f;
inline constexpr float kMinThickness = 0.02f;
inline constexpr float kMaxThickness = 1.0f;
inline constexpr int kMinSteps = 4;
inline constexpr int kMaxSteps = 24;
inline constexpr int kMaxView = 4;
inline constexpr float kMaxGain = 16.0f;

inline Tuning Clamped(Tuning t) {
  t.length = std::clamp(t.length, kMinLength, kMaxLength);
  t.thickness = std::clamp(t.thickness, kMinThickness, kMaxThickness);
  t.strength = std::clamp(t.strength, 0.0f, 1.0f);
  t.steps = std::clamp(t.steps, kMinSteps, kMaxSteps);
  t.view = std::clamp(t.view, 0, kMaxView);
  t.gain = std::clamp(t.gain, 0.0f, kMaxGain);
  return t;
}

// What the history's colour is multiplied by to be in the paint's units. The history is the scene colour as the target
// holds it, which is the shaders' output times 2^bias (RT0's exponent bias: constants.cpp, the hardware multiplies the
// colour by it after the shader), and the paint's own colours (and the environment reads it mixes with) are the output.
// MCLA's main target has a bias of 4 (the paint's gInvColorExpBias.x is 0.0625). CHECK in a picture: if the self-reflection
// is too bright or too dark, mcla_gpu_car_selfrefl_gain moves it.
inline float ExpBiasGain(int32_t exp_bias) {
  return std::ldexp(1.0f, -std::clamp<int32_t>(exp_bias, -24, 24));
}

// The matrix a vertex of the draw is taken into the history frame's clip space with: the vehicle's own motion (this
// frame's world position to its world position one frame ago, vehicle_motion.h; null: the identity) and then the
// history frame's view-projection, row vectors, written row by row.
inline void HistoryMatrix(const temporal::Mat4& view_projection, const temporal::Mat4* motion, float out[16]) {
  const temporal::Mat4 m = motion ? temporal::Mul(*motion, view_projection) : view_projection;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = float(m.m[r][c]);
    }
  }
}

// The constants of a draw that has no history: the game's paint (mode 0) and a slot that is never read.
inline CarSelfReflConstants OffConstants() {
  CarSelfReflConstants c = {};
  return c;
}

inline CarSelfReflConstants MakeConstants(Mode mode, const Tuning& tuning, const float matrix[16],
                                          uint32_t history_slot, uint32_t sampler_slot, float exp_gain,
                                          float noise_frame) {
  CarSelfReflConstants c = {};
  for (int i = 0; i < 16; ++i) {
    c.m[i] = matrix[i];
  }
  const Tuning t = Clamped(tuning);
  c.a[0] = float(uint8_t(mode));
  c.a[1] = t.length;
  c.a[2] = t.thickness;
  c.a[3] = t.strength;
  c.b[0] = float(t.steps);
  c.b[1] = float(t.view);
  c.b[2] = exp_gain * t.gain;
  c.b[3] = noise_frame;
  c.c[0] = history_slot;
  c.c[1] = sampler_slot;
  return c;
}

// The view distance a stored depth (1 near, 0 far) stands for under the projection `p` (row vectors, view space to clip,
// the camera looking down -z): ao_common.hlsli AoLinearDepth's formula, which car_history_ps.hlsl uses. For a perspective
// projection with p[2][3] = -1 and p[3][3] = 0 it is the w of the clip position, which the paint variant divides by.
inline double LinearDepth(double stored, const temporal::Mat4& p) {
  const double ndc = 1.0 - std::clamp(stored, 0.0, 1.0);
  const double den = p.m[2][2] - ndc * p.m[2][3];
  return den != 0.0 ? (p.m[3][2] - ndc * p.m[3][3]) / den : 0.0;
}

}  // namespace mcla::gpu::car_selfrefl
