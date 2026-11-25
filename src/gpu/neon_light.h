#pragma once
// ===========================================================================
// MCLA GPU renderer -- a car's neon underglow as a light: what the game holds, how it is read, and the tube the light pass lights from (GB-NEON; perf/LIGHT-UNIFY.md, feature-parity/NEON-PULSE.md,
// NEON-COLOUR.md)
// ===========================================================================
// WHAT THE GAME DOES (sub_823553F8, the car effect object's light update, once an update; [guest] = the recompiled code of default.xex, native\tree-k1\generated, larecomp_recomp.44.cpp). The
// effect object `this` holds the car's neon as a LIGHT OBJECT at this + 560 (the exterior underglow; this + 568 is the interior neon, which lights the cabin and is not read here). Each update:
//
//     intensity = ExtNeonIntensity (config + 368) x pulse (this[404]) x fade       fade = ((y + 1) x K) x this[268] x a visibility clamp x (0.2 at night, 0.125 by day)
//     colour    = ExtNeonColor (config + 80) x intensity                           the vector register v124, stored at light + 144 with the enable byte (light + 4) set to 1; a car
//                                                                                  with no neon (intensity 0) or one that is hidden stores the enable byte 0
//
// `pulse` is sub_82353E38's (sin(10 x rate x T) + 1) / 2, with the console's 30 Hz hold (hooks/neon_pulse.cpp, mcla_fix_neon_30hz) when that fix is on. The light object's layout is sub_823511F8's
// (the geometry) and sub_823553F8's (the rest):
//
//     +4          the enable byte
//     +16 +32 +48 +64   four points, xyz, a tube: A, A + (B - A) c1, A + (B - A) c2, B (sub_823511F8 lerps the two between A = *r4 and B = *r5); each point's w (+28 +44 +60 +76) is
//                 (half extent x K)^2 x fade: the plateau radius squared of the game's own distance law, saturate(w / d^2)
//     +80         a 4x4 matrix, rows right, up, back, position as row vectors: the car's body matrix (this + 4512 rows) as sub_823553F8 builds it with scale 1
//     +144        the colour (xyz; the vector's w is the config colour's)
//
// THE LIGHT PASS lights nearby surfaces with it (owner, 2026-11-21: the underglow lights the ground, the kerbs, the walls and the other cars within a short range, ON by default): the colour
// and the pulse are the game's own, read from the object AFTER its update, so the 30 Hz hold, the day and night constants and the fade are in it exactly as the game's own underglow has them
// (the twin below says how they are made, and the hook checks the stored colour against it). The tube's points make a few neon law lights (local_lights_sources.h MakeNeonTubeLights): the colour
// is read as a hue and a strength, not as a light intensity (it is the glow shader's HDR multiplier: 30 for a red neon), and the light is a few street lamps' worth near the tube with a smooth
// reach of 5 to 6 m (GB-NEON-TUNE, perf/GB-NEON-TUNE.md).
//
// WHAT IS NOT KNOWN WITHOUT A RUN (every line is a CHECK in the run plan, h11-work\gb-neon-notes\RUNPLAN-gb-neon.txt):
//   N1  whether the four points are in WORLD space or in the car's (the code adds the car's position row to the tube's ends, which says world): ResolveTube accepts the reading whose
//       middle is within kMaxMidFromCar of the car's own position and says which one it took (the log's first line); a tube neither reading puts there is not lit, and the line prints the
//       raw numbers.
//   N2  whether the game's multi-light renderer already lights the road with this very object (the red pool under the car in NEON-COLOUR.md's pictures says it does): the pass then ADDS
//       a second pool; the run plan's A/B (the pass's neon off / on) and mcla_gpu_neon_light_gain decide the strength and the plateau.
//   N3  the units: the game's colour here is NOT the "rgb" of its lamp law: it is the glow shader's HDR multiplier (30 for a red neon; the pass used it as a lamp's colour and lit a road 7 m round the
//       car). GB-NEON-TUNE reads it as a hue and a strength (over kNeonFullColour = 32) and gives the tube kNeonIntensity street lamp units; the pass multiplies that by the exposure like every game lamp.
//
// Pure: no guest, no renderer, no SDK (the hook's decode is a template over a memory reader, so a test lays out a fake effect object). neon_light_test.cpp holds all of it.
// ===========================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace larecomp::gpu::neon_light {

// ---------------------------------------------------------------------------
// the guest's layout
// ---------------------------------------------------------------------------

inline constexpr uint32_t kThisRecord = 20;        // this + 20: the vehicle record (hooks/neon_pulse.cpp)
inline constexpr uint32_t kRecordConfig = 132;     // record + 132: the car's config (mcCarConfig)
inline constexpr uint32_t kThisExtPulse = 404;     // this + 404: the exterior pulse (sub_82353E38)
inline constexpr uint32_t kThisExtLight = 560;     // this + 560: the exterior neon's light object (a pointer)
inline constexpr uint32_t kThisCarMatrix = 4512;   // this + 4512: the car body's matrix, four row vectors (right, up, back, position)
inline constexpr uint32_t kLightEnable = 4;        // the light object: the enable byte
inline constexpr uint32_t kLightPoints = 16;       // the first tube point; the others follow at 16 byte steps
inline constexpr uint32_t kLightPointStride = 16;
inline constexpr uint32_t kLightPointW = 12;       // a point's w, in its row
inline constexpr uint32_t kLightMatrix = 80;       // the 4x4 matrix
inline constexpr uint32_t kLightColour = 144;      // the colour
inline constexpr uint32_t kCfgColour = 80;         // config + 80: ExtNeonColor (a vector)
inline constexpr uint32_t kCfgIntensity = 368;     // config + 368: ExtNeonIntensity (a float)
inline constexpr uint32_t kCfgRate = 396;          // config + 396: ExtNeonFlashRate (a signed byte)
inline constexpr uint32_t kTubePoints = 4;

inline constexpr bool PlausiblePtr(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

// ---------------------------------------------------------------------------
// what the hook publishes
// ---------------------------------------------------------------------------

struct NeonCar {
  uint32_t object = 0;      // the car effect object (the key: it stays with the car)
  uint32_t record = 0;      // this[20]
  bool on = false;          // the light object's enable byte
  bool player = false;      // the record is the player's vehicle record  // CHECK N4: that the record is the vehicle (the light points' `vehicle` is [[object] + 12] - 16)
  float colour[3] = {};     // the light object's colour: ExtNeonColor x intensity x pulse x fade, as the game stored it (finite, 0 or more)
  float pulse = 0.0f;       // this[404], what the colour was made with (with the 30 Hz hold on, the held one)
  float fade = -1.0f;       // the fade the stored colour implies (ImpliedFade); < 0 when it cannot be told
  bool twin_ok = true;      // the colour is what the game's own arithmetic makes of the config and the pulse (ColourFollowsTwin)
  float points[kTubePoints][3] = {};
  float point_w[kTubePoints] = {};
  float matrix[16] = {};       // the light object's matrix, +80
  float car_matrix[16] = {};   // the effect object's body matrix, this + 4512
  float age_ms = 0.0f;         // filled by Table::Recent
};

// ---------------------------------------------------------------------------
// the colour: the game's arithmetic, and the check against it
// ---------------------------------------------------------------------------

// sub_823553F8's colour in the game's order, single precision (fmuls f11 = intensity x pulse, fmuls f10 = that x f0 where f0 is the fade with its day or night constant, vmulfp128
// colour x splat(f10)). `fade` is f0.
inline void GameColour(const float cfg_rgb[3], float intensity, float pulse, float fade, float out[3]) {
  const float scale = (intensity * pulse) * fade;
  for (int i = 0; i < 3; ++i) {
    out[i] = cfg_rgb[i] * scale;
  }
}

// The fade the stored colour implies: from the channel the config colour is largest in. < 0 when the config colour, the intensity or the pulse is 0 (a dark half of the pulse: nothing to say).
inline float ImpliedFade(const float colour[3], const float cfg_rgb[3], float intensity, float pulse) {
  int best = 0;
  for (int i = 1; i < 3; ++i) {
    if (cfg_rgb[i] > cfg_rgb[best]) {
      best = i;
    }
  }
  const float denom = cfg_rgb[best] * intensity * pulse;
  if (!(denom > 1.0e-6f) || !std::isfinite(denom)) {
    return -1.0f;
  }
  return colour[best] / denom;
}

// Whether the stored colour is the game's arithmetic of the config's colour, intensity and pulse at one fade (the same for the three channels, 0 to 1.5, within a percent and a hair of
// float noise): the check the hook logs. A colour that fails it is still lit (the object's colour is the game's own, whatever made it); the log says it, so a wrong offset shows at once.
// `fade` gets the implied value. True when the fade cannot be told (nothing to compare).
inline bool ColourFollowsTwin(const float colour[3], const float cfg_rgb[3], float intensity, float pulse, float* fade) {
  const float f = ImpliedFade(colour, cfg_rgb, intensity, pulse);
  if (fade != nullptr) {
    *fade = f;
  }
  if (f < 0.0f) {
    return true;
  }
  if (!(f <= 1.5f)) {
    return false;
  }
  float twin[3];
  GameColour(cfg_rgb, intensity, pulse, f, twin);
  for (int i = 0; i < 3; ++i) {
    if (!(std::fabs(twin[i] - colour[i]) <= 0.01f * std::max(std::fabs(colour[i]), std::fabs(twin[i])) + 1.0e-5f)) {
      return false;
    }
  }
  return true;
}

inline float Brightest(const float colour[3]) { return std::max({colour[0], colour[1], colour[2], 0.0f}); }

// Whether the neon has a reason to ask for the G-buffer's planes this frame (NEONALL-REVIEW M2): a car with its neon lit is in the table (`lit_car`) AND one of its lights can reach a pixel of the
// picture (`light_seen`: local_lights_sources.h AnyNeonLightSeen, against the last frame's camera). Without that camera (`camera_known` false: the first frame, a pause or garage view, a cut) the
// second condition cannot be tested and a lit car alone asks, as it always did. The planes are shared (every consumer has its own want: the reflections' and the others' stay whatever this says).
inline constexpr bool LitNow(bool lit_car, bool camera_known, bool light_seen) {
  return lit_car && (!camera_known || light_seen);
}

// The frames the pass keeps asking for the G-buffer's planes after the last frame with a lit neon car in reach (1 s at 60 FPS): the pulse's dark half (the colour passes under kMinColour twice a
// cycle at a high flash rate) and a car passing in and out of view must not make and unmake the sidecar.
inline constexpr uint32_t kHoldFrames = 60;
// The hold's next value: full while NEON LIGHT is on and a lit car is in the table, else counting down to 0 (0 at once when NEON LIGHT is off).
inline constexpr uint32_t NextHold(bool neon_on, bool lit_now, uint32_t hold) {
  return !neon_on ? 0u : (lit_now ? kHoldFrames : (hold > 0 ? hold - 1 : 0u));
}

// ---------------------------------------------------------------------------
// the tube
// ---------------------------------------------------------------------------

// How near the tube's middle must be to the car's own position for a reading of the four points to count (m): the car is about 4.5 m long and the tube lies under it.
inline constexpr float kMaxMidFromCar = 3.5f;
inline constexpr float kMaxTube = 10.0f;     // longer than this is not a car's underglow
inline constexpr float kPointTube = 0.25f;   // a tube this short is one point

enum class Space : uint8_t { kNone = 0, kWorld = 1, kCar = 2 };
inline constexpr const char* SpaceName(Space s) { return s == Space::kWorld ? "world" : (s == Space::kCar ? "car" : "none"); }

struct Tube {
  Space space = Space::kNone;
  float a[3] = {};
  float b[3] = {};
  float mid_from_car = 0.0f;  // m, the middle of the tube to the car's position
  float length = 0.0f;        // m
};

// A point through a 4x4 of row vectors (the game's: p x M, the fourth row the translation).
inline void TransformPoint(const float m[16], const float p[3], float out[3]) {
  for (int c = 0; c < 3; ++c) {
    out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
  }
}

namespace detail {
inline bool Finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
inline float Dist3(const float a[3], const float b[3]) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
inline bool Consider(const float a[3], const float b[3], const float origin[3], Space space, Tube* best) {
  if (!Finite3(a) || !Finite3(b)) {
    return false;
  }
  const float mid[3] = {0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2])};
  const float from_car = Dist3(mid, origin);
  const float length = Dist3(a, b);
  if (!(from_car <= kMaxMidFromCar) || !(length <= kMaxTube)) {
    return false;
  }
  if (best->space == Space::kNone || from_car < best->mid_from_car) {
    best->space = space;
    for (int i = 0; i < 3; ++i) {
      best->a[i] = a[i];
      best->b[i] = b[i];
    }
    best->mid_from_car = from_car;
    best->length = length;
  }
  return true;
}
}  // namespace detail

// The tube's ends in the world (the object's first and last point), N1: the stored points as world positions, or put through the object's matrix as car space positions; the reading whose
// middle is nearer the car's own position (the body matrix's fourth row) and within kMaxMidFromCar of it counts. False (and `out->space` kNone) when neither does.
inline bool ResolveTube(const NeonCar& car, Tube* out) {
  *out = Tube{};
  const float* a = car.points[0];
  const float* b = car.points[kTubePoints - 1];
  const float origin[3] = {car.car_matrix[12], car.car_matrix[13], car.car_matrix[14]};
  if (!detail::Finite3(origin)) {
    return false;
  }
  detail::Consider(a, b, origin, Space::kWorld, out);
  float wa[3], wb[3];
  TransformPoint(car.matrix, a, wa);
  TransformPoint(car.matrix, b, wb);
  detail::Consider(wa, wb, origin, Space::kCar, out);
  return out->space != Space::kNone;
}

// ---------------------------------------------------------------------------
// the hook's decode
// ---------------------------------------------------------------------------

// Reads the car effect object `self` after the game's update (sub_823553F8): `mem` has U8(addr), U32(addr) and F32(addr) as the guest sees them (big-endian values read). False when a pointer
// is not plausible (nothing is noted). `player_record` is the player's vehicle record (0: none known).
template <class Mem>
inline bool ReadNeonCar(const Mem& mem, uint32_t self, uint32_t player_record, NeonCar* out) {
  if (!PlausiblePtr(self)) {
    return false;
  }
  const uint32_t record = mem.U32(self + kThisRecord);
  if (!PlausiblePtr(record)) {
    return false;
  }
  const uint32_t cfg = mem.U32(record + kRecordConfig);
  const uint32_t light = mem.U32(self + kThisExtLight);
  if (!PlausiblePtr(cfg) || !PlausiblePtr(light)) {
    return false;
  }
  NeonCar c;
  c.object = self;
  c.record = record;
  c.player = player_record != 0 && record == player_record;
  c.on = mem.U8(light + kLightEnable) != 0;
  float cfg_rgb[3];
  for (uint32_t i = 0; i < 3; ++i) {
    const float v = mem.F32(light + kLightColour + 4u * i);
    c.colour[i] = std::isfinite(v) && v > 0.0f ? v : 0.0f;
    cfg_rgb[i] = mem.F32(cfg + kCfgColour + 4u * i);
  }
  c.pulse = mem.F32(self + kThisExtPulse);
  const float intensity = mem.F32(cfg + kCfgIntensity);
  c.twin_ok = ColourFollowsTwin(c.colour, cfg_rgb, intensity, c.pulse, &c.fade);
  for (uint32_t p = 0; p < kTubePoints; ++p) {
    for (uint32_t k = 0; k < 3; ++k) {
      c.points[p][k] = mem.F32(light + kLightPoints + kLightPointStride * p + 4u * k);
    }
    c.point_w[p] = mem.F32(light + kLightPoints + kLightPointStride * p + kLightPointW);
  }
  for (uint32_t i = 0; i < 16; ++i) {
    c.matrix[i] = mem.F32(light + kLightMatrix + 4u * i);
    c.car_matrix[i] = mem.F32(self + kThisCarMatrix + 4u * i);
  }
  *out = c;
  return true;
}

// ---------------------------------------------------------------------------
// the census line (the log's first line a session: what N1 and the twin say)
// ---------------------------------------------------------------------------

// One line for a car: its neon's state, the colour and what made it, the tube as stored and how it was read (or why it was not).
inline std::string FormatLine(const NeonCar& car, const Tube& tube, bool placed) {
  char buf[1024];
  int n = std::snprintf(buf, sizeof(buf),
                        "[neon-light] car effect object 0x%08x (record 0x%08x%s): neon %s, colour (%.4f %.4f %.4f), pulse %.3f, fade %.4f (the colour %s the game's arithmetic of the config); stored tube "
                        "A (%.2f %.2f %.2f) B (%.2f %.2f %.2f), w %.3f %.3f %.3f %.3f; the car's position (%.2f %.2f %.2f); ",
                        car.object, car.record, car.player ? ", the player's" : "", car.on ? "on" : "off", double(car.colour[0]), double(car.colour[1]), double(car.colour[2]), double(car.pulse),
                        double(car.fade), car.twin_ok ? "follows" : "DOES NOT follow", double(car.points[0][0]), double(car.points[0][1]), double(car.points[0][2]),
                        double(car.points[kTubePoints - 1][0]), double(car.points[kTubePoints - 1][1]), double(car.points[kTubePoints - 1][2]), double(car.point_w[0]), double(car.point_w[1]),
                        double(car.point_w[2]), double(car.point_w[3]), double(car.car_matrix[12]), double(car.car_matrix[13]), double(car.car_matrix[14]));
  std::string line(buf, size_t(std::max(n, 0)));
  if (placed) {
    std::snprintf(buf, sizeof(buf), "read as %s space: the tube runs from (%.2f %.2f %.2f) to (%.2f %.2f %.2f), %.2f m long, its middle %.2f m from the car", SpaceName(tube.space), double(tube.a[0]),
                  double(tube.a[1]), double(tube.a[2]), double(tube.b[0]), double(tube.b[1]), double(tube.b[2]), double(tube.length), double(tube.mid_from_car));
  } else {
    std::snprintf(buf, sizeof(buf), "NOT PLACED: neither the world nor the car's space puts the tube within %.1f m of the car (N1: the layout of the light object needs a look)", double(kMaxMidFromCar));
  }
  line += buf;
  return line;
}

// ---------------------------------------------------------------------------
// who wants it, and the table
// ---------------------------------------------------------------------------

// The hook runs its reads only while the light pass takes neon (NEON LIGHT on, or the pass's neon source): one relaxed load otherwise.
inline std::atomic<bool>& WantedFlag() {
  static std::atomic<bool> wanted{false};
  return wanted;
}
inline void Want(bool wanted) { WantedFlag().store(wanted, std::memory_order_relaxed); }
inline bool Wanted() { return WantedFlag().load(std::memory_order_relaxed); }

// The cars the hook has seen, one entry a car effect object: the latest reading and when. A mutex: the game thread writes (a few cars a frame), the render thread reads once a frame.
class Table {
 public:
  static constexpr size_t kCapacity = 32;  // cars with an effect object in sight; the game keeps a few dozen alive at most
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kMaxAge{100};  // the update runs every guest frame; 3 frames at 30 FPS

  void Note(const NeonCar& car, Clock::time_point at = Clock::now()) {
    std::lock_guard<std::mutex> guard(lock_);
    size_t slot = kCapacity, free_slot = kCapacity, oldest = 0;
    for (size_t i = 0; i < kCapacity; ++i) {
      const Entry& e = entries_[i];
      if (!e.used) {
        if (free_slot == kCapacity) free_slot = i;
        continue;
      }
      if (e.car.object == car.object) {
        slot = i;
        break;
      }
      if (!entries_[oldest].used || e.at < entries_[oldest].at) oldest = i;
    }
    if (slot == kCapacity) slot = free_slot != kCapacity ? free_slot : oldest;
    entries_[slot] = {true, at, car};
    ++notes_;
  }

  // The cars seen within `max_age`, in no particular order, with their age.
  void Recent(std::vector<NeonCar>* out, std::chrono::milliseconds max_age = kMaxAge, Clock::time_point now = Clock::now()) const {
    out->clear();
    std::lock_guard<std::mutex> guard(lock_);
    for (const Entry& e : entries_) {
      if (e.used && now - e.at <= max_age) {
        out->push_back(e.car);
        out->back().age_ms = std::chrono::duration<float, std::milli>(now - e.at).count();
      }
    }
  }

  // Whether some car seen within `max_age` has its neon on and bright enough to show (the pass asks, to know whether the G-buffer's planes are worth making).
  bool AnyLit(float floor, std::chrono::milliseconds max_age = kMaxAge, Clock::time_point now = Clock::now()) const {
    std::lock_guard<std::mutex> guard(lock_);
    for (const Entry& e : entries_) {
      if (e.used && now - e.at <= max_age && e.car.on && Brightest(e.car.colour) >= floor) {
        return true;
      }
    }
    return false;
  }

  uint64_t notes() const {
    std::lock_guard<std::mutex> guard(lock_);
    return notes_;
  }
  void Clear() {
    std::lock_guard<std::mutex> guard(lock_);
    for (Entry& e : entries_) e = {};
    notes_ = 0;
  }

 private:
  struct Entry {
    bool used = false;
    Clock::time_point at{};
    NeonCar car;
  };
  mutable std::mutex lock_;
  std::array<Entry, kCapacity> entries_{};
  uint64_t notes_ = 0;
};

inline Table& Global() {
  static Table table;
  return table;
}

}  // namespace larecomp::gpu::neon_light
