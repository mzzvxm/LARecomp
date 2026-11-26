#pragma once
// ===========================================================================
// MCLA GPU renderer -- the game's per-frame street lamp list, as the volumetric light reads it (perf/VOLUMETRICS-SURVEY.md sections 1.2 and 2.2)
// ===========================================================================
// WHAT THE GAME HOLDS. The prop lamp pass (sub_82729438 -> AddLight sub_823029D0) keeps every spot lamp within 300 m of the eye in a list at guest
// address 0x8288B8D8 ({u32 array pointer, u16 count, u16 capacity}; up to 256 records of 128 bytes), and the cone drawer (sub_82302810) sorts it, draws
// one cone mesh per record (in rain only: a weather gate, perf/out/NIGHT-PROBE.txt C3) and empties it. The list is filled every night frame (mean 64
// lamps, at most 83 in a clear night) and holds nothing by day (the prop pass runs between the lamps' on hours). A record (the night probe's reading,
// test/night-probe 3d3bb584 src/gpu/night_probe_rules.h; all of it big-endian floats in guest memory):
//     +0    float  distance to the eye when the record was made
//     +16   the game's unpacked 96-byte light:  +0 position xyz, +16 colour rgb (0..1), +32 direction xyz, +64 intensity (the game's units: 20 to 100,
//           a Los Angeles street lamp is 55 to 70), +68 cone angle (degrees, 360 = omni), +72 cone spread (degrees), +88 type (0 omni, 1 spot), +89 on
//     +112  float  the scale the prop pass gave (its fade by distance and by the lamps' hours, 0..1)
//
// HOW IT REACHES THE RENDER THREAD. The guest builds the list on its own thread and the host's render thread records a frame later (the guest runs
// ahead), so the list cannot be read at the render thread's trigger (it would be another frame's, or half filled). A read-only guest hook at the
// cone drawer's entry (src/mc_engine/hooks/vol_lamps.cpp, a [[midasm_hook]] one instruction after the night probe's own at 0x82302810: a hook is
// keyed by its address, so that both can run) copies the COMPLETE list into this table once a frame, and the render thread takes the newest copy (as the headlight rig's
// headlight_points.h does for the light points: a mutex, a timestamp, nothing written into the guest). Street lamps do not move, only their fade and
// the set in range change, so a copy a frame or two old is as good as the current one.
//
// Header only, pure (no guest, no renderer, no SDK): volumetrics_lamps_test.cpp checks the decoder and the table.
// ===========================================================================

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include "settings/immortal.h"

namespace mcla::gpu::vol_lamps {

inline constexpr uint32_t kListHeaderAddress = 0x8288B8D8u;  // the per-frame list: array pointer +0, u16 count +4, u16 capacity +6
inline constexpr uint32_t kRecordBytes = 128;
inline constexpr uint32_t kRecordLight = 16;   // the unpacked light inside a record
inline constexpr uint32_t kRecordScale = 112;  // AddLight's scale (the prop pass's fade)
inline constexpr uint32_t kMaxRecords = 256;   // the list's capacity as far as it has been seen (83 at most in the probe)
// Offsets inside the unpacked light.
inline constexpr uint32_t kLightPosition = 0, kLightColour = 16, kLightDirection = 32, kLightIntensity = 64, kLightCone = 68, kLightSpread = 72,
                          kLightType = 88, kLightOn = 89;

// One lamp of the list, in host byte order and the game's units.
struct GameLamp {
  float pos[3] = {};
  float colour[3] = {};      // rgb, 0..1 (the intensity is separate)
  float dir[3] = {0.0f, -1.0f, 0.0f};  // the axis of the cone (the way the light points)
  float intensity = 0.0f;    // the game's units
  float cone_deg = 360.0f;   // the full angle of the cone's lit part, degrees (360: omni)
  float spread_deg = 0.0f;   // the width of the soft edge, degrees (0 counts as 1; negative: the edge is inside the angle)
  float scale = 1.0f;        // the prop pass's fade, 0..1
  uint8_t type = 1;          // 0 omni, 1 spot
  uint8_t on = 1;
};

inline uint32_t BeU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint16_t BeU16(const uint8_t* p) { return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1])); }
inline float BeF32(const uint8_t* p) {
  const uint32_t bits = BeU32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// A record (128 big-endian bytes at `rec`) as a lamp.
inline GameLamp DecodeRecord(const uint8_t* rec) {
  GameLamp l;
  const uint8_t* light = rec + kRecordLight;
  for (int i = 0; i < 3; ++i) {
    l.pos[i] = BeF32(light + kLightPosition + 4 * i);
    l.colour[i] = BeF32(light + kLightColour + 4 * i);
    l.dir[i] = BeF32(light + kLightDirection + 4 * i);
  }
  l.intensity = BeF32(light + kLightIntensity);
  l.cone_deg = BeF32(light + kLightCone);
  l.spread_deg = BeF32(light + kLightSpread);
  l.type = light[kLightType];
  l.on = light[kLightOn];
  l.scale = BeF32(rec + kRecordScale);
  return l;
}

// Whether every number of the lamp is a number a light can have (a garbage record is left out, never drawn).
inline bool Plausible(const GameLamp& l) {
  const auto ok = [](float v) { return std::isfinite(v) && std::fabs(v) < 1.0e6f; };
  for (int i = 0; i < 3; ++i) {
    if (!ok(l.pos[i]) || !ok(l.colour[i]) || !ok(l.dir[i])) {
      return false;
    }
  }
  return ok(l.intensity) && ok(l.cone_deg) && ok(l.spread_deg) && ok(l.scale);
}

// ---------------------------------------------------------------------------
// the table the hook fills (game thread) and the producer reads (render thread)
// ---------------------------------------------------------------------------

// Whether the hook is to do anything: the readers' mask (one relaxed load in the hook otherwise). A reader sets its bit while it is on and one reader's switch never takes another's away: the
// volumetric light's SetWanted (the original call, unchanged), the ray tracing world frame's (rt_world_frame.h; it reads the lamps for the hit shading's lamp term) and the street lamps' shadows'
// (lamp_shadows.h) and the ray traced lights' (rt_lights.h: the nearest lamps get a visibility ray). The hook runs while any bit is set.
inline constexpr uint32_t kWantedByVolume = 1u << 0;
inline constexpr uint32_t kWantedByWorldFrame = 1u << 1;
inline constexpr uint32_t kWantedByLampShadows = 1u << 2;
inline constexpr uint32_t kWantedByRtLights = 1u << 3;
inline std::atomic<uint32_t> g_wanted_by{0};
inline bool Wanted() { return g_wanted_by.load(std::memory_order_relaxed) != 0; }
inline void SetWantedBy(uint32_t who, bool wanted) {
  if (wanted) {
    g_wanted_by.fetch_or(who, std::memory_order_relaxed);
  } else {
    g_wanted_by.fetch_and(~who, std::memory_order_relaxed);
  }
}
inline void SetWanted(bool wanted) { SetWantedBy(kWantedByVolume, wanted); }

class Table {
 public:
  using Clock = std::chrono::steady_clock;

  // Game thread: the complete list of a frame (`count` lamps), seen now (or at `at`, for tests). An empty list is a list too: a day, or a menu.
  void Publish(const GameLamp* lamps, uint32_t count, Clock::time_point at = Clock::now(), bool garage = false) {
    std::lock_guard<std::mutex> guard(lock_);
    count = count > kMaxRecords ? kMaxRecords : count;
    if (lamps != nullptr && count != 0) {
      lamps_.assign(lamps, lamps + count);
    } else {
      lamps_.clear();
    }
    at_ = at;
    garage_ = garage;
    ++seq_;
  }

  // Render thread: the newest list, if it is newer than `*seen` (the sequence number the caller has) and at most `max_age` old. Copies it into `out`
  // and moves `*seen` to its number. False: nothing new (`out` untouched), or none was published in time (`*stale` is set then, so that the caller
  // can drop the lamps it still holds: the hook is not running, a menu, a day).
  bool Latest(std::vector<GameLamp>* out, uint64_t* seen, bool* stale, std::chrono::milliseconds max_age, Clock::time_point now = Clock::now(), bool* garage = nullptr) const {
    std::lock_guard<std::mutex> guard(lock_);
    if (seq_ == 0 || now - at_ > max_age) {
      *stale = true;
      return false;
    }
    *stale = false;
    if (seq_ == *seen) {
      return false;
    }
    *out = lamps_;
    if (garage) *garage = garage_;
    *seen = seq_;
    return true;
  }

  uint64_t published() const {
    std::lock_guard<std::mutex> guard(lock_);
    return seq_;
  }
  void Clear() {
    std::lock_guard<std::mutex> guard(lock_);
    lamps_.clear();
    seq_ = 0;
  }

 private:
  mutable std::mutex lock_;
  std::vector<GameLamp> lamps_;
  Clock::time_point at_{};
  uint64_t seq_ = 0;
  bool garage_ = false;
};

inline Table& Global() {
  static Table& table = larecomp::Leak<Table>();
  return table;
}

}  // namespace mcla::gpu::vol_lamps
