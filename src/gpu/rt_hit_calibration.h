#pragma once
// ===========================================================================
// MCLA GPU renderer -- the exposure of a shaded hit, measured against the game's own picture (CITY-RT M2 package H4; shared by the car hit shading and the city hit library)
// ===========================================================================
// WHY. A hit that is shaded from its material (the car's paint, the city's wall) is lit by a model: the sun's colour, the ambient, the albedo. The game's own lit result is not that model
// (vertex lighting, baked occlusion, the exposure and the way the light was tuned), and the model is out by one to three stops where it is dark (RT-HIT-SHADING-3's census). But wherever a
// hit also lands on a pixel the camera SEES, the game's picture holds the true colour of that very surface in that very light: the pass shades the hit by the model as well and adds the two
// luminances' logs to counters. The host reads the counters back, turns the mean difference into a gain on the model, smooths it, and the hits the picture does not show are shaded
// with it. The same counters are the census of the pass. This header is the host's half of it: the fixed point log the counters sum, the mean difference in stops, the smoothed gain,
// and, for the city, one such gain PER CLASS (the G-buffer's 16: a wall, a road and a lamp lens are lit by different rules and drift apart).
//
// THE COUNTERS. A frame's stats buffer holds, per class, three words: the number of samples, the sum of the fixed point log of the picture's luminance and the sum of the model's; the
// shader's own copy of the numbers is hlsl/city_hit_shade.hlsli (CITY_HIT_STAT_*) and, for the car's single paint gain, hlsl/car_rt_refl_cs.hlsl kStat*. A sample counts only where both
// luminances are over Params::floor (a black part has no brightness to compare) and the pass says the frame is a stats frame (a frame in twenty: the counters are atomics).
//
// THE GAIN. residual = (picture's mean log - model's mean log) / kLogScale stops over a window with at least Params::min_samples samples; the shader shaded the samples WITH the gain in
// force, so the residual is what is left to correct: the gain moves Params::rate of the way to cancel it (NextStops), settling where the residual is 0 and following the hour as it
// changes. The range is Params::min_stops .. max_stops (the car: 1/8 to 8; the city: 0.5 to 3, M2 section 4.2: a model that needs more than that is wrong, not mis-exposed).
//
// The car's census (car_rt_hit_rules.h census::) keeps its names and numbers and forwards here with the car's parameters: tests/car_rt_hit_rules_test.cpp holds both to the same values.
// This header includes nothing of the renderer.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::rt_hit_cal {

// A luminance as a fixed-point log: (log2(max(l, kLogFloor)) + kLogBias) x kLogScale, clamped to 0 .. kLogMax (a sum over a million samples stays under 2^32; a step is a sixty-fourth of a stop).
inline constexpr float kLogFloor = 1e-4f;
inline constexpr float kLogBias = 16.0f;
inline constexpr float kLogScale = 64.0f;
inline constexpr uint32_t kLogMax = 2047;
inline uint32_t FixedLog2(float luminance) {
  const float v = (std::log2(std::max(luminance, kLogFloor)) + kLogBias) * kLogScale;
  return uint32_t(std::clamp(v, 0.0f, float(kLogMax)));
}
inline float FromFixedLog2(float mean_fixed) { return std::exp2(mean_fixed / kLogScale - kLogBias); }

inline constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};
inline float Luminance(const float rgb[3]) { return kLuma[0] * rgb[0] + kLuma[1] * rgb[1] + kLuma[2] * rgb[2]; }

// What a calibration counts and how it moves.
struct Params {
  float floor;            // a sample counts only where both luminances are over this (in the picture's units)
  uint32_t min_samples;   // fewer samples in a window and the gain stays where it was
  float min_stops;        // the gain's range, in stops
  float max_stops;
  float rate;             // the share of the way to the new measurement a window moves the gain (in stops)
};
inline constexpr Params kCarParams = {0.02f, 48, -3.0f, 3.0f, 0.5f};
inline constexpr Params kCityParams = {0.02f, 48, -1.0f, 1.5849625f, 0.5f};  // x0.5 .. x3

// The mean of the log2 luminance difference (picture over model) of a calibration's samples in stops, and whether there were enough of them to say.
inline bool Stops(const Params& p, uint64_t count, uint64_t seen_sum, uint64_t model_sum, float* stops) {
  if (count < p.min_samples) {
    return false;
  }
  const float diff = (float(double(seen_sum) - double(model_sum)) / float(count)) / kLogScale;
  *stops = std::clamp(diff, p.min_stops, p.max_stops);
  return true;
}
// The new gain (in stops) from the old one and a window's residual.
inline float NextStops(const Params& p, float previous, float residual) { return std::clamp(previous + residual * p.rate, p.min_stops, p.max_stops); }
inline float GainOf(const Params& p, float stops) { return std::exp2(std::clamp(stops, p.min_stops, p.max_stops)); }

// ---------------------------------------------------------------------------
// the city's counters: one row of three words a class of the G-buffer
// ---------------------------------------------------------------------------

inline constexpr uint32_t kClasses = 16;     // gbuffer_types.h SurfaceClass: none .. water
inline constexpr uint32_t kRowWords = 3;     // count, the picture's summed log, the model's summed log
inline constexpr uint32_t kStatsWords = kClasses * kRowWords;
inline constexpr uint32_t kStatsBytes = kStatsWords * 4;
inline constexpr uint32_t kCountWord = 0;
inline constexpr uint32_t kSeenWord = 1;
inline constexpr uint32_t kModelWord = 2;
constexpr uint32_t StatsWord(uint32_t cls, uint32_t which) { return (cls < kClasses ? cls : kClasses - 1) * kRowWords + which; }

// The gains of every class, from the windows of stats frames the host reads back. One per class: a class with too few samples in a window keeps its gain.
struct ClassGains {
  Params params = kCityParams;
  float stops[kClasses] = {};
  uint64_t count[kClasses] = {};
  uint64_t seen[kClasses] = {};
  uint64_t model[kClasses] = {};
  uint32_t windows = 0;

  // A stats frame's counters (kStatsWords words) added into the open window.
  void Add(const uint32_t words[kStatsWords]) {
    for (uint32_t c = 0; c < kClasses; ++c) {
      count[c] += words[StatsWord(c, kCountWord)];
      seen[c] += words[StatsWord(c, kSeenWord)];
      model[c] += words[StatsWord(c, kModelWord)];
    }
  }
  // Closes the window: every class that has enough samples moves its gain; the window is cleared. Returns how many classes moved.
  uint32_t Close() {
    uint32_t moved = 0;
    for (uint32_t c = 0; c < kClasses; ++c) {
      float residual = 0.0f;
      if (Stops(params, count[c], seen[c], model[c], &residual)) {
        stops[c] = NextStops(params, stops[c], residual);
        ++moved;
      }
      count[c] = seen[c] = model[c] = 0;
    }
    ++windows;
    return moved;
  }
  float Gain(uint32_t cls) const { return GainOf(params, stops[cls < kClasses ? cls : kClasses - 1]); }
  // The table the shader reads (hlsl/city_hit_shade.hlsli CITY_HIT_GAIN): `out[c]` is class c's gain.
  void Table(float out[kClasses]) const {
    for (uint32_t c = 0; c < kClasses; ++c) {
      out[c] = Gain(c);
    }
  }
};

}  // namespace mcla::gpu::rt_hit_cal
