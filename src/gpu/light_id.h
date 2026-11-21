#pragma once
// ===========================================================================
// MCLA GPU renderer -- a light's identity: ONE function for every key a light had (perf/LIGHT-UNIFY.md section 2.3, package U1)
// ===========================================================================
// A light's `id` (light_frame.h Light::id) is 64 bits and stays the same while the light lives, from frame to frame, whoever made it: the picker's hysteresis, the shadow
// atlas's slot that stays with a light, the volume's moving-beam history and the ray traced shadows' temporal state all key on it. Before this there were three keys for
// the same things: a street lamp had TWO (volumetrics_lamps.h `LampKey` and lamp_shadow_block.h `KeyOf` on feat/lamp-shadows, held equal by a unit test) and a car had a
// third (the rig's `car_key`, a 32-bit track number). The id of a street lamp IS that key (bit for bit, light_frame_test.cpp holds it to both), so nothing that already
// keys by it changes; every other id is made from its source's own number, in a domain of its own so that two sources never meet.
//
//   street lamp (the game's list)  its place to an eighth of a metre: LampId(pos), the old LampKey / KeyOf (the list has no number, and street lamps do not move)
//   a car's lamp                   CarLampId(car_key, pair | left | right): `car_key` is the rig's track key (it stays with a car from frame to frame)
//   a mod's lamp (lights.bin)      ModLampId(mod_key, the record's u32 id): `mod_key` = ModKey(the mod folder's name)
//   a police / brake / tail point  PointId(vehicle, point index): the guest address of the vehicle and the point's place in its set (headlight_points.h)
//   neon                           a car's underglow: NeonCarId(effect object, point of its tube) (GB-NEON: the car effect object is the game's own light holder and stays with the car);
//                                  NeonId(place) is a place's key in the same domain (kept, nothing makes one)
//
// 0 is no light (kNoLightId). Pure: no renderer, no guest.
// ===========================================================================

#include <cmath>
#include <cstdint>
#include <string_view>

namespace larecomp::gpu {

inline constexpr uint64_t kNoLightId = 0;

namespace light_id {

// A car's lamps: the pair (the light of the two headlamps together: one Light for a car, the rig's way), or one lamp of it.
enum class CarLamp : uint32_t { kPair = 0, kLeft = 1, kRight = 2 };

// The domains: the high byte of the seed of every id that is not a street lamp's.
inline constexpr uint64_t kDomainCar = 0x01, kDomainMod = 0x02, kDomainPoint = 0x03, kDomainNeon = 0x04;

// splitmix64's finaliser: a bijection on 64 bits that spreads every input bit over every output bit.
inline constexpr uint64_t Mix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// (domain, a, b) -> id, never 0.
inline constexpr uint64_t Make(uint64_t domain, uint64_t a, uint64_t b) {
  const uint64_t h = Mix64(Mix64((domain << 56) ^ 0xA076D1E5F00DBEEFull) ^ Mix64(a + 0x2545F4914F6CDD1Dull) ^ (Mix64(b ^ 0xE7037ED1A0B428DBull) << 1));
  return h != kNoLightId ? h : 1ull;
}

// A street lamp: its place to an eighth of a metre. THE SAME FUNCTION as volumetrics_lamps.h LampKey(GameLamp) and lamp_shadow_block.h KeyOf(pos): the same seed, the
// same step, the same floor, so an id made here finds the views and the tracks that were made with either of the old keys.
inline uint64_t LampId(const float pos[3]) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (int i = 0; i < 3; ++i) {
    const int64_t q = int64_t(std::floor(double(pos[i]) * 8.0));
    h ^= uint64_t(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  }
  return h;
}
// Any record with a `pos[3]` (the game's list's GameLamp, a mod's record).
template <class LampT>
  requires requires(const LampT& l) { l.pos; }
inline uint64_t LampId(const LampT& lamp) {
  return LampId(lamp.pos);
}

// A car's lamp (or its pair).
inline constexpr uint64_t CarLampId(uint32_t car_key, CarLamp which = CarLamp::kPair) { return Make(kDomainCar, car_key, uint64_t(which)); }

// A mod's lamp: the mod's key (ModKey of its folder's name) and the lamp's u32 id from lights.bin.
inline constexpr uint64_t ModLampId(uint32_t mod_key, uint32_t light_id) { return Make(kDomainMod, mod_key, light_id); }
// FNV-1a of a name: the number a mod's lamps are keyed under (stable across runs and machines; the name is the mod's folder).
inline constexpr uint32_t ModKey(std::string_view name) {
  uint32_t h = 2166136261u;
  for (const char c : name) {
    h ^= uint32_t(uint8_t(c));
    h *= 16777619u;
  }
  return h;
}

// A light point of the game's vehicle light submit (headlight_points.h Point: the vehicle's guest address and the point's index in its set): police, brake, tail, turn.
inline constexpr uint64_t PointId(uint32_t vehicle, uint32_t index) { return Make(kDomainPoint, vehicle, index); }

// Neon: a place (to a quarter of a metre) and a number the source gives it. Nothing makes one (a car's underglow moves with the car: NeonCarId below).
inline uint64_t NeonId(const float pos[3], uint32_t n = 0) {
  uint64_t h = 0;
  for (int i = 0; i < 3; ++i) {
    h = Mix64(h ^ uint64_t(int64_t(std::floor(double(pos[i]) * 4.0))));
  }
  return Make(kDomainNeon, h, n);
}

// A car's underglow (GB-NEON): the guest address of the car's effect object (neon_light.h: the object that holds the neon's light) and the number of the point along its tube.
inline constexpr uint64_t NeonCarId(uint32_t effect_object, uint32_t point) { return Make(kDomainNeon, 0x10000000ull | effect_object, point); }

}  // namespace light_id
}  // namespace larecomp::gpu
